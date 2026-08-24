#include "BufferService.h"

#include "core/Logger.h"
#include "grpc/Methods.h"

#include <QDateTime>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

grpc::Status invalidRequest(const QString &method)
{
    grpc::Status status;
    status.code = grpc::InvalidArgument;
    status.message = QStringLiteral("không giải mã được yêu cầu %1").arg(method);
    return status;
}

grpc::Status noSuchSession(const QString &sessionId)
{
    grpc::Status status;
    status.code = grpc::NotFound;
    // Says which of the two things went wrong.  A session the pipeline still
    // has but this buffer does not - after a server restart, say - is a
    // different problem from a typo, and an operator can act on the difference.
    status.message =
        QStringLiteral("máy chủ đệm không giữ phiên '%1' (phiên bắt đầu trước khi máy chủ này "
                       "khởi động lại sẽ không còn ở đây)")
            .arg(sessionId);
    return status;
}

// Decodes the request, runs `fn`, encodes the reply.  Every handler below is
// this plus one line, which is the point: the decode/encode pair is where a
// mistake would be silent, so it is written once.
template <typename Req, typename Resp, typename Fn>
grpc::Status serve(const grpc::ServerCall &call, QByteArray *out, Fn fn)
{
    Req request;
    pw::Reader reader(call.message);
    request.parse(reader);
    if (!reader.ok())
        return invalidRequest(call.method);

    Resp response;
    const grpc::Status status = fn(request, &response);
    if (!status.ok())
        return status;
    *out = response.serialize();
    return status;
}

} // namespace

BufferService::BufferService(BufferHub *hub, grpc::Server *server) : m_hub(hub), m_server(server) {}

void BufferService::registerMethods()
{
    BufferHub *hub = m_hub;

    // The client's own deadline, when it sent one.  Passing it upstream rather
    // than substituting our own means a caller that has already given up is
    // not still being waited for on the far side.
    const auto deadline = [hub](const grpc::ServerCall &call) {
        return call.deadlineMs > 0 ? call.deadlineMs : hub->config().upstreamTimeoutMs;
    };

    // One relay: take a lane, make the call, and let the probe know at once if
    // the transport failed, so the client's badge moves now and not at the next
    // tick.
    const auto relay = [hub](const std::function<grpc::Status(AsrClient &)> &work) {
        const grpc::Status status = hub->pool().call(work);
        if (status.isTransport()) {
            hub->noteUpstream(false, 0.0, status.toString());
            hub->pokeProbe();
        } else {
            hub->noteUpstream(true, 0.0, QString());
        }
        return status;
    };

    // ---- ProductASRService: answered by the buffer -------------------------

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::StartSession),
        [hub, deadline](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::StartSessionRequest, asr::StartSessionResponse>(
                call, out, [&](const asr::StartSessionRequest &req, asr::StartSessionResponse *resp) {
                    return hub->startSession(req, call.peer, deadline(call), resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::PushAudio),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::PushAudioRequest, asr::PushAudioResponse>(
                call, out, [&](const asr::PushAudioRequest &req, asr::PushAudioResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId);
                    return session->push(req, resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetLiveState),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::SessionRequest, asr::StateResponse>(
                call, out, [&](const asr::SessionRequest &req, asr::StateResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId);
                    return session->liveState(resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::StopSession),
        [hub, deadline](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::SessionRequest, asr::StopSessionResponse>(
                call, out, [&](const asr::SessionRequest &req, asr::StopSessionResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId);
                    // The drain barrier: everything the client sent reaches the
                    // pipeline before this returns.
                    return session->stop(deadline(call), resp);
                });
        });

    // ---- ProductASRService: relayed ----------------------------------------

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetReviewState),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::ReviewRequest, asr::StateResponse>(
                call, out, [&](const asr::ReviewRequest &req, asr::StateResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getReviewState(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetAudioRange),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::AudioRangeRequest, asr::AudioRangeResponse>(
                call, out, [&](const asr::AudioRangeRequest &req, asr::AudioRangeResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getAudioRange(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ApplyTextEdit),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::TextEditRequest, asr::ReviewEditResponse>(
                call, out, [&](const asr::TextEditRequest &req, asr::ReviewEditResponse *resp) {
                    return relay([&](AsrClient &c) { return c.applyTextEdit(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListSessions),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::ListSessionsRequest, asr::ListSessionsResponse>(
                call, out,
                [&](const asr::ListSessionsRequest &req, asr::ListSessionsResponse *resp) {
                    return relay([&](AsrClient &c) { return c.listSessions(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::RenameSpeaker),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::RenameSpeakerRequest, asr::ReviewEditResponse>(
                call, out, [&](const asr::RenameSpeakerRequest &req, asr::ReviewEditResponse *resp) {
                    return relay([&](AsrClient &c) { return c.renameSpeaker(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetPipelineTrace),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::PipelineTraceRequest, asr::PipelineTraceResponse>(
                call, out,
                [&](const asr::PipelineTraceRequest &req, asr::PipelineTraceResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getPipelineTrace(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetAuditHistory),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::AuditHistoryRequest, asr::AuditHistoryResponse>(
                call, out,
                [&](const asr::AuditHistoryRequest &req, asr::AuditHistoryResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getAuditHistory(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetModelStatus),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::ModelStatusRequest, asr::ModelStatusResponse>(
                call, out, [&](const asr::ModelStatusRequest &, asr::ModelStatusResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getModelStatus(resp, ms); });
                });
        });

    // ---- SpeakerRegistryService: relayed in full ---------------------------
    //
    // Enrolment writes to the global CAM++ database, which is shared state
    // that belongs to the pipeline, not to any one buffer.  Caching or
    // reordering it here would let two servers publish conflicting speakers.

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetEnrollmentScript),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::GetEnrollmentScriptRequest, reg::GetEnrollmentScriptResponse>(
                call, out,
                [&](const reg::GetEnrollmentScriptRequest &, reg::GetEnrollmentScriptResponse *resp) {
                    return relay([&](AsrClient &c) { return c.getEnrollmentScript(resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::EnrollSpeaker),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            // Enrolment runs CAM++ over a whole recording and legitimately
            // takes minutes; the client already sends a long deadline, and
            // this passes it on instead of cutting it short.
            const int ms = deadline(call);
            return serve<reg::EnrollSpeakerRequest, reg::EnrollSpeakerResponse>(
                call, out, [&](const reg::EnrollSpeakerRequest &req, reg::EnrollSpeakerResponse *resp) {
                    return relay([&](AsrClient &c) { return c.enrollSpeaker(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListSessionSpeakers),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::ListSessionSpeakersRequest, reg::ListSessionSpeakersResponse>(
                call, out,
                [&](const reg::ListSessionSpeakersRequest &req,
                    reg::ListSessionSpeakersResponse *resp) {
                    return relay([&](AsrClient &c) { return c.listSessionSpeakers(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::SaveSessionSpeakers),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::SaveSessionSpeakersRequest, reg::SaveSessionSpeakersResponse>(
                call, out,
                [&](const reg::SaveSessionSpeakersRequest &req,
                    reg::SaveSessionSpeakersResponse *resp) {
                    return relay([&](AsrClient &c) { return c.saveSessionSpeakers(req, resp, ms); });
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetSpeakerRegistryStatus),
        [relay, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::GetSpeakerRegistryStatusRequest, reg::GetSpeakerRegistryStatusResponse>(
                call, out,
                [&](const reg::GetSpeakerRegistryStatusRequest &req,
                    reg::GetSpeakerRegistryStatusResponse *resp) {
                    return relay(
                        [&](AsrClient &c) { return c.getSpeakerRegistryStatus(req, resp, ms); });
                });
        });

    // ---- BufferAdminService: this process, not the pipeline ----------------

    grpc::Server *server = m_server;

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::BufferPing),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<buf::PingRequest, buf::PingResponse>(
                call, out, [&](const buf::PingRequest &req, buf::PingResponse *resp) {
                    // The caller's own timestamp goes back untouched, so a
                    // round trip can be measured without the two clocks having
                    // to agree.
                    resp->clientTs = req.clientTs;
                    resp->serverTs = nowSeconds();
                    resp->serverVersion = QStringLiteral(S2T_SERVER_VERSION);
                    resp->upstreamReady = hub->upstreamReady();
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetBufferStatus),
        [hub, server](const grpc::ServerCall &call, QByteArray *out) {
            return serve<buf::BufferStatusRequest, buf::BufferStatusResponse>(
                call, out, [&](const buf::BufferStatusRequest &req, buf::BufferStatusResponse *resp) {
                    resp->serverVersion = QStringLiteral(S2T_SERVER_VERSION);
                    resp->uptimeSec = hub->uptimeSec();
                    resp->upstream = hub->upstreamStatus();
                    resp->activeConnections = quint32(server->activeConnections());
                    resp->totalConnections = server->totalConnections();
                    resp->totalCalls = server->totalRequests();
                    resp->rejectedCalls = server->rejectedCalls();
                    resp->queueCapacityBytes = hub->queueCapacityBytes();
                    resp->queueUsedBytes = hub->queueUsedBytes();
                    resp->spoolDir = hub->config().spoolDir;
                    resp->spoolEnabled = !hub->config().spoolDir.trimmed().isEmpty();
                    if (req.sessionId.isEmpty()) {
                        resp->sessions = hub->snapshots(true, 0);
                    } else {
                        const SessionRef session = hub->find(req.sessionId);
                        if (!session)
                            return noSuchSession(req.sessionId);
                        resp->sessions.append(session->snapshot());
                    }
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListBufferedSessions),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<buf::BufferSessionsRequest, buf::BufferSessionsResponse>(
                call, out,
                [&](const buf::BufferSessionsRequest &req, buf::BufferSessionsResponse *resp) {
                    resp->sessions = hub->snapshots(req.includeFinished, int(req.limit));
                    return grpc::Status();
                });
        });

    LOG_INFO(applog::cat::Grpc) << m_server->methods().size() << "RPC methods registered";
}
