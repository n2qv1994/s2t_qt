#include "BufferService.h"

#include "core/Logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "grpc/Methods.h"
#include "proto/SpeakerRegistry.h"

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

// A session the pipeline still has but this buffer does not is a different
// problem from a typo, and an operator can act on the difference - so the
// message says which.  What "different" means depends on whether journalling is
// on: with it, a restart no longer loses meetings and the honest explanations
// are retention or a clean stop.  Claiming otherwise would send someone looking
// for a restart that never happened.
grpc::Status noSuchSession(const QString &sessionId, bool durable)
{
    grpc::Status status;
    status.code = grpc::NotFound;
    if (durable) {
        status.message = QStringLiteral("máy chủ đệm không giữ phiên '%1' (phiên đã kết thúc, "
                                        "hoặc đã quá hạn giữ sau khi dừng)")
                             .arg(sessionId);
    } else {
        status.message = QStringLiteral("máy chủ đệm không giữ phiên '%1' (nhật ký phiên đang "
                                        "TẮT, nên phiên bắt đầu trước khi máy chủ này khởi động "
                                        "lại sẽ không còn ở đây - xem buffer/journal_dir)")
                             .arg(sessionId);
    }
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

BufferService::BufferService(BufferHub *hub, grpc::Server *server, CampPlusClient *campp)
    : m_hub(hub), m_server(server), m_campp(campp)
{
}

void BufferService::registerMethods()
{
    BufferHub *hub = m_hub;
    // Fixed for the life of the process, so it is read once here rather than on
    // every miss.
    const bool durable = !m_hub->config().journalDir.trimmed().isEmpty();

    // The client's own deadline, when it sent one.  Passing it upstream rather
    // than substituting our own means a caller that has already given up is
    // not still being waited for on the far side.
    const auto deadline = [hub](const grpc::ServerCall &call) {
        return call.deadlineMs > 0 ? call.deadlineMs : hub->config().upstreamTimeoutMs;
    };

    // A backend call, with the probe told at once if the transport failed so
    // the client's badge moves now and not at the next tick.  Only two RPCs
    // still reach the tier from a connection thread - the rest are answered
    // from this process now.
    const auto tier = [hub](const std::function<grpc::Status()> &work) {
        const grpc::Status status = work();
        if (status.isTransport()) {
            hub->noteUpstream(false, 0.0, status.toString());
            hub->pokeProbe();
        } else if (status.ok()) {
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
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::PushAudioRequest, asr::PushAudioResponse>(
                call, out, [&](const asr::PushAudioRequest &req, asr::PushAudioResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    return session->push(req, resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetLiveState),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::SessionRequest, asr::StateResponse>(
                call, out, [&](const asr::SessionRequest &req, asr::StateResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    return session->liveState(resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::StopSession),
        [hub, deadline, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::SessionRequest, asr::StopSessionResponse>(
                call, out, [&](const asr::SessionRequest &req, asr::StopSessionResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    // The drain barrier: everything the client sent reaches the
                    // pipeline before this returns.
                    return session->stop(deadline(call), resp);
                });
        });

    // ---- ProductASRService: answered from this process ---------------------
    //
    // These twelve used to be relayed verbatim to a Python adapter that owned
    // the transcript, the audio archive and the speaker database.  That adapter
    // is gone: the buffer now drives Riva or Triton directly, and neither keeps
    // a meeting.  So the ones that are *about* a meeting are answered from the
    // SessionBuffer that owns it, and the ones that need a store this server
    // does not have yet say so plainly rather than returning an empty success -
    // an empty transcript that looks like a real answer is the worse failure.

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetReviewState),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::ReviewRequest, asr::StateResponse>(
                call, out, [&](const asr::ReviewRequest &req, asr::StateResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    // Below zero means "no bound", which is what an unset
                    // has_view_* field has always meant on this RPC.
                    return session->reviewState(req.hasViewStartSec ? req.viewStartSec : -1.0,
                                                req.hasViewEndSec ? req.viewEndSec : -1.0, resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ApplyTextEdit),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::TextEditRequest, asr::ReviewEditResponse>(
                call, out, [&](const asr::TextEditRequest &req, asr::ReviewEditResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    return session->applyTextEdit(req, resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::RenameSpeaker),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::RenameSpeakerRequest, asr::ReviewEditResponse>(
                call, out, [&](const asr::RenameSpeakerRequest &req, asr::ReviewEditResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (!session)
                        return noSuchSession(req.sessionId, durable);
                    return session->renameSpeaker(req, resp);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListSessions),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::ListSessionsRequest, asr::ListSessionsResponse>(
                call, out,
                [&](const asr::ListSessionsRequest &req, asr::ListSessionsResponse *resp) {
                    // Only what this process is holding.  Once a meeting ages
                    // past finished_retention_sec it is gone from here, and
                    // there is no archive behind it yet - see SessionStore in
                    // the handover notes.
                    resp->sessions = hub->summaries(int(req.limit));
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetModelStatus),
        [hub, tier, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<asr::ModelStatusRequest, asr::ModelStatusResponse>(
                call, out, [&](const asr::ModelStatusRequest &, asr::ModelStatusResponse *resp) {
                    return tier([&] { return hub->backend().models(resp, ms); });
                });
        });

    // ---- ProductASRService: not answerable yet -----------------------------
    //
    // Three RPCs need stores this server does not have. They are registered
    // rather than left unregistered on purpose: UNIMPLEMENTED with a sentence
    // an operator can act on beats the client's generic "method not found".

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetAudioRange),
        [](const grpc::ServerCall &call, QByteArray *out) {
            Q_UNUSED(out);
            Q_UNUSED(call);
            grpc::Status status;
            status.code = grpc::Unimplemented;
            status.message = QStringLiteral(
                "máy chủ này chưa lưu kho audio để phát lại. Nhật ký phiên là hàng đợi, "
                "không phải bản lưu - nghe lại một đoạn cần kho audio riêng, chưa làm.");
            return status;
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetPipelineTrace),
        [](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::PipelineTraceRequest, asr::PipelineTraceResponse>(
                call, out,
                [&](const asr::PipelineTraceRequest &req, asr::PipelineTraceResponse *resp) {
                    // enabled=false is the contract's own way of saying "this
                    // deployment does not collect traces", and the client
                    // already draws that case. Better than UNIMPLEMENTED here.
                    resp->sessionId = req.sessionId;
                    resp->enabled = false;
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetAuditHistory),
        [](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::AuditHistoryRequest, asr::AuditHistoryResponse>(
                call, out,
                [&](const asr::AuditHistoryRequest &req, asr::AuditHistoryResponse *resp) {
                    // An empty history is honest: nothing is recorded yet. The
                    // edits themselves are logged, so nothing is silently lost.
                    resp->sessionId = req.sessionId;
                    return grpc::Status();
                });
        });

    // ---- SpeakerRegistryService: straight to CAM++ -------------------------
    //
    // The adapter presented a gRPC face over campp_native/enroll_service.py, a
    // plain HTTP service on :8790.  That translation lives here now.
    //
    // Note what it is NOT: the inference tier.  rebuild_db needs docker exec
    // access the Triton container deliberately does not have, so the enrolment
    // service runs on the host beside it and is reached separately.  Riva has
    // no enrolment RPC at all, so this path is the same either way.

    CampPlusClient *campp = m_campp;

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetEnrollmentScript),
        [campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::GetEnrollmentScriptRequest, reg::GetEnrollmentScriptResponse>(
                call, out,
                [&](const reg::GetEnrollmentScriptRequest &,
                    reg::GetEnrollmentScriptResponse *resp) {
                    QByteArray body;
                    const grpc::Status status =
                        campp->get(QStringLiteral("/enroll_script"), &body, ms);
                    if (!status.ok())
                        return status;
                    const QJsonObject object = QJsonDocument::fromJson(body).object();
                    // The service spells it `text`; the proto spells it
                    // script_text.  Same field, and this is the only place that
                    // has to know both spellings.
                    resp->scriptText = object.value(QStringLiteral("text")).toString();
                    resp->sampleRate =
                        quint32(object.value(QStringLiteral("sample_rate")).toInt(16000));
                    resp->recommendedDurationSec =
                        object.value(QStringLiteral("recommended_duration_sec")).toDouble(25.0);
                    resp->targetSegments =
                        quint32(object.value(QStringLiteral("target_segments")).toInt(10));
                    return status;
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::EnrollSpeaker),
        [campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            // Enrolment reruns rebuild_db over every speaker on file and
            // legitimately takes minutes; the client sends a long deadline and
            // this passes it on rather than cutting it short.
            const int ms = deadline(call);
            return serve<reg::EnrollSpeakerRequest, reg::EnrollSpeakerResponse>(
                call, out,
                [&](const reg::EnrollSpeakerRequest &req, reg::EnrollSpeakerResponse *resp) {
                    if (req.displayName.trimmed().isEmpty()) {
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message = QStringLiteral("thiếu tên người nói");
                        return bad;
                    }
                    if (req.editorId.trimmed().isEmpty()) {
                        // enroll_service.py audits this header all the way to
                        // the database write, so an anonymous enrolment is
                        // refused here rather than there.
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message =
                            QStringLiteral("thiếu editor_id - mỗi lần đăng ký đều được ghi nhật ký "
                                           "kèm người thao tác");
                        return bad;
                    }
                    if (req.wav.isEmpty()) {
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message = QStringLiteral("thiếu dữ liệu WAV");
                        return bad;
                    }

                    // Percent-encoded because an HTTP header value is Latin-1
                    // and these two are free text that routinely carries
                    // Vietnamese diacritics.  enroll_service.py calls unquote()
                    // on both.
                    QList<QPair<QByteArray, QByteArray>> headers;
                    headers.append({QByteArrayLiteral("X-Speaker-Name"),
                                    CampPlusClient::urlEncode(req.displayName.trimmed())});
                    headers.append({QByteArrayLiteral("X-Editor-Id"),
                                    CampPlusClient::urlEncode(req.editorId.trimmed())});
                    headers.append({QByteArrayLiteral("Content-Type"),
                                    QByteArrayLiteral("application/octet-stream")});
                    if (req.allowBelowPolicy)
                        headers.append({QByteArrayLiteral("X-Allow-Below-Policy"),
                                        QByteArrayLiteral("1")});

                    QByteArray body;
                    const grpc::Status status =
                        campp->post(QStringLiteral("/enroll"), req.wav, headers, &body, ms);
                    if (!status.ok()) {
                        // A rejected sample is not a broken server: report it
                        // in the message field the client already renders, so
                        // "quá ngắn" reaches the operator as advice.
                        resp->ok = false;
                        resp->error = status.message;
                        return grpc::Status();
                    }
                    const QJsonObject object = QJsonDocument::fromJson(body).object();
                    resp->ok = true;
                    resp->speakerId = object.value(QStringLiteral("spk_id")).toString();
                    resp->rawSeconds = object.value(QStringLiteral("raw_seconds")).toDouble();
                    resp->speechSecondsAfterVad =
                        object.value(QStringLiteral("speech_seconds_after_vad")).toDouble();
                    resp->segmentsEnrolled =
                        quint32(object.value(QStringLiteral("segments_enrolled")).toInt());
                    resp->targetSegments =
                        quint32(object.value(QStringLiteral("target_segments")).toInt());
                    // Never swallowed: a sample stored below policy has to stay
                    // visibly in need of a proper re-enrolment.
                    resp->warning = object.value(QStringLiteral("warning")).toString();
                    resp->dbMtime = object.value(QStringLiteral("mtime")).toDouble();
                    return status;
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetSpeakerRegistryStatus),
        [campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::GetSpeakerRegistryStatusRequest, reg::GetSpeakerRegistryStatusResponse>(
                call, out,
                [&](const reg::GetSpeakerRegistryStatusRequest &req,
                    reg::GetSpeakerRegistryStatusResponse *resp) {
                    QByteArray body;
                    const grpc::Status status = campp->get(QStringLiteral("/status"), &body, ms);
                    if (!status.ok())
                        return status;
                    const QJsonObject object = QJsonDocument::fromJson(body).object();
                    resp->sessionId = req.sessionId;
                    resp->globalDbMtime = object.value(QStringLiteral("mtime")).toDouble();
                    resp->globalDbRevision =
                        object.value(QStringLiteral("revision")).toString();
                    resp->globalSpeakerCount =
                        quint32(object.value(QStringLiteral("speaker_count")).toInt());
                    resp->sidecarReachable =
                        object.value(QStringLiteral("sidecar_reachable")).toBool(true);
                    for (const QJsonValue &name :
                         object.value(QStringLiteral("speaker_names")).toArray()) {
                        resp->globalSpeakerNames.append(name.toString());
                    }
                    for (const QJsonValue &item :
                         object.value(QStringLiteral("below_policy")).toArray()) {
                        const QJsonObject entry = item.toObject();
                        reg::SpeakerBelowPolicy weak;
                        weak.spkId = entry.value(QStringLiteral("spk_id")).toString();
                        weak.spkName = entry.value(QStringLiteral("spk_name")).toString();
                        weak.sampleCount =
                            quint32(entry.value(QStringLiteral("sample_count")).toInt());
                        weak.longestSampleSec =
                            entry.value(QStringLiteral("longest_sample_sec")).toDouble();
                        resp->speakersBelowPolicy.append(weak);
                    }
                    // The per-session counters stay zero: they come from the
                    // session speaker registry, which needs the store this
                    // server does not have yet.  Zero is the honest answer -
                    // nothing has been staged here.
                    return status;
                });
        });

    // ---- SpeakerRegistryService: still needs a session store ---------------
    //
    // These two are not pass-throughs.  ListSessionSpeakers reads the staged
    // per-session registry, and SaveSessionSpeakers publishes a speaker from a
    // meeting by collecting the audio behind its evidence spans and posting it
    // to /enroll_from_pcm.  Both need the session store and the audio archive
    // this server has yet to grow, so they say so rather than half-working.

    const auto needsStore = [](const grpc::ServerCall &, QByteArray *) {
        grpc::Status status;
        status.code = grpc::Unimplemented;
        status.message = QStringLiteral(
            "danh sách người nói theo phiên chưa có ở máy chủ này - nó cần kho phiên và kho "
            "audio chưa được làm. Đăng ký trực tiếp (EnrollSpeaker) thì đã dùng được.");
        return status;
    };

    for (const char *method : {rpcpath::ListSessionSpeakers, rpcpath::SaveSessionSpeakers})
        m_server->registerMethod(QString::fromLatin1(method), needsStore);

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
        [hub, server, durable](const grpc::ServerCall &call, QByteArray *out) {
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
                    resp->spoolDir = hub->config().journalDir;
                    resp->spoolEnabled = !hub->config().journalDir.trimmed().isEmpty();
                    if (req.sessionId.isEmpty()) {
                        resp->sessions = hub->snapshots(true, 0);
                    } else {
                        const SessionRef session = hub->find(req.sessionId);
                        if (!session)
                            return noSuchSession(req.sessionId, durable);
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
