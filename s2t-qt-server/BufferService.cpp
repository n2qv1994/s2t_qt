#include "BufferService.h"

#include "core/Logger.h"

#include <QJsonArray>
#include <QSet>
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
                    if (!session) {
                        // Not live any more - but it may still be in the
                        // archive.  Reviewing a meeting from last week is the
                        // normal case for this RPC, not the exception.
                        //
                        // The window is applied to the stored state too.  It
                        // was not until 2026-09-24, so asking an archived
                        // meeting for [20, 30] s returned all 152 words of it,
                        // 117 of them outside the window - which defeats the
                        // one thing a review window is for.
                        if (hub->store().loadState(req.sessionId, resp)) {
                            if (req.hasViewStartSec || req.hasViewEndSec) {
                                LiveTranscript view;
                                view.restore(*resp);
                                *resp = view.snapshot(
                                    resp->sessionId, resp->streamId,
                                    req.hasViewStartSec ? req.viewStartSec : -1.0,
                                    req.hasViewEndSec ? req.viewEndSec : -1.0);
                            }
                            return grpc::Status();
                        }
                        return noSuchSession(req.sessionId, durable);
                    }
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
                    if (session)
                        return session->applyTextEdit(req, resp);
                    // Not live any more - which is the normal state of a
                    // meeting somebody is reviewing.  It used to answer
                    // NOT_FOUND while the client still offered an edit button,
                    // so correcting last week's transcript was impossible and
                    // looked like a bug in the client.
                    if (hub->store().hasSession(req.sessionId))
                        return hub->editArchived(req, resp);
                    return noSuchSession(req.sessionId, durable);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::DeleteSession),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::DeleteSessionRequest, asr::DeleteSessionResponse>(
                call, out,
                [&](const asr::DeleteSessionRequest &req, asr::DeleteSessionResponse *resp) {
                    if (req.editorId.trimmed().isEmpty()) {
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message = QStringLiteral("thiếu editor_id - xoá một cuộc họp là thao "
                                                     "tác không thể hoàn tác và phải ghi rõ ai làm");
                        return bad;
                    }
                    // A running meeting is refused rather than torn out from
                    // under its own forwarder: the thread is still archiving
                    // audio into the file this would delete.
                    const SessionRef session = hub->find(req.sessionId);
                    if (session && !session->isFinished()) {
                        grpc::Status bad;
                        bad.code = grpc::FailedPrecondition;
                        bad.message = QStringLiteral("phiên '%1' đang chạy - hãy dừng phiên trước "
                                                     "khi xoá")
                                          .arg(req.sessionId);
                        return bad;
                    }
                    if (!hub->store().hasSession(req.sessionId))
                        return noSuchSession(req.sessionId, durable);
                    // The audit row goes in BEFORE the delete, so the
                    // tombstone survives even if the removal half-fails.
                    hub->store().appendAudit(
                        req.sessionId, QStringLiteral("delete_session"),
                        QStringLiteral("{\"editor\":\"%1\"}").arg(req.editorId.trimmed()));
                    if (session)
                        hub->forget(req.sessionId);
                    quint64 bytes = 0;
                    QString error;
                    if (!hub->store().deleteSession(req.sessionId, &bytes, &error)) {
                        grpc::Status bad;
                        bad.code = grpc::Internal;
                        bad.message = error;
                        return bad;
                    }
                    resp->sessionId = req.sessionId;
                    resp->bytesRemoved = bytes;
                    resp->deletedAt = nowSeconds();
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::RenameSpeaker),
        [hub, durable](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::RenameSpeakerRequest, asr::ReviewEditResponse>(
                call, out, [&](const asr::RenameSpeakerRequest &req, asr::ReviewEditResponse *resp) {
                    const SessionRef session = hub->find(req.sessionId);
                    if (session)
                        return session->renameSpeaker(req, resp);
                    if (hub->store().hasSession(req.sessionId))
                        return hub->renameArchived(req, resp);
                    return noSuchSession(req.sessionId, durable);
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListSessions),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::ListSessionsRequest, asr::ListSessionsResponse>(
                call, out,
                [&](const asr::ListSessionsRequest &req, asr::ListSessionsResponse *resp) {
                    // The archive is the list; a live meeting only overwrites
                    // its own row in it.
                    //
                    // It used to be the other way round - live entries pushed
                    // in front of the first page - and that made the paging
                    // arithmetic wrong in two directions at once: the first
                    // page carried more rows than the caller asked for, and a
                    // running meeting appeared a second time, as an archived
                    // row, on whichever later page its timestamp fell on.
                    QHash<QString, asr::SessionSummary> live;
                    for (const asr::SessionSummary &item : hub->summaries(0))
                        live.insert(item.sessionId, item);

                    QString nextCursor;
                    const QList<asr::SessionSummary> stored =
                        hub->store().listSessions(int(req.limit), req.cursor, &nextCursor);
                    for (const asr::SessionSummary &item : stored) {
                        const auto it = live.constFind(item.sessionId);
                        // The live entry wins where there is one: it is the
                        // only one with a true `running` flag and a duration
                        // that is up to this second.
                        resp->sessions.append(it != live.constEnd() ? it.value() : item);
                    }
                    resp->nextCursor = nextCursor;

                    // A server with no archive at all still has to answer.
                    if (!hub->store().enabled() && req.cursor.isEmpty())
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
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::AudioRangeRequest, asr::AudioRangeResponse>(
                call, out, [&](const asr::AudioRangeRequest &req, asr::AudioRangeResponse *resp) {
                    QString error;
                    if (hub->store().audioRange(req.sessionId, req.startSec, req.endSec, resp,
                                                &error)) {
                        return grpc::Status();
                    }
                    grpc::Status status;
                    // A bad range is the caller's mistake; a missing archive is
                    // the deployment's. The client shows the message either
                    // way, so the codes are what tell them apart.
                    status.code = hub->store().enabled() ? grpc::InvalidArgument
                                                         : grpc::FailedPrecondition;
                    status.message = error;
                    return status;
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetPipelineTrace),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::PipelineTraceRequest, asr::PipelineTraceResponse>(
                call, out,
                [&](const asr::PipelineTraceRequest &req, asr::PipelineTraceResponse *resp) {
                    resp->sessionId = req.sessionId;
                    // enabled reports whether this deployment collects traces
                    // at all, which is the same question as whether there is a
                    // store to collect them into.  It answered a flat false
                    // until 2026-09-21 - the contract allows that, but it cost
                    // the pipeline team the one tool they use to trace a wrong
                    // punctuation mark back to the window that decided it.
                    resp->enabled = hub->store().enabled();
                    if (!resp->enabled)
                        return grpc::Status();
                    bool hasMore = false;
                    resp->events = hub->store().traceHistory(req.sessionId, req.afterSeq,
                                                             int(req.limit), req.stages, &hasMore);
                    resp->hasMore = hasMore;
                    // The cursor for the next poll.  Where nothing came back
                    // it stays where the caller left it, so an idle session
                    // does not rewind them to the start of the meeting.
                    resp->nextSeq = resp->events.isEmpty() ? req.afterSeq
                                                           : resp->events.last().seq;
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetAuditHistory),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<asr::AuditHistoryRequest, asr::AuditHistoryResponse>(
                call, out,
                [&](const asr::AuditHistoryRequest &req, asr::AuditHistoryResponse *resp) {
                    resp->sessionId = req.sessionId;
                    resp->events = hub->store().auditHistory(req.sessionId, int(req.limit));
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

    // ---- the global registry's lifecycle -----------------------------------
    //
    // An enrolment used to be one-way from here: the five RPCs that let an
    // operator look at the shared database, take a voice out of it, or hear
    // what a recording would become before committing it, existed in the
    // .proto and in the reference adapter and nowhere in this server.  The
    // practical result was a database nobody could tidy - 62 speakers on the
    // deployed host as of 2026-09-21, including `5`, `A` and `a1`, every one
    // of them something a meeting can be matched against.

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::PreviewEnrollment),
        [campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::PreviewEnrollmentRequest, reg::PreviewEnrollmentResponse>(
                call, out,
                [&](const reg::PreviewEnrollmentRequest &req, reg::PreviewEnrollmentResponse *resp) {
                    if (req.wav.isEmpty()) {
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message = QStringLiteral("thiếu dữ liệu WAV");
                        return bad;
                    }
                    // No editor_id, deliberately, and it is not an oversight:
                    // a preview writes nothing - no database row, no catalogue
                    // entry, no audit line - so there is nothing to attribute.
                    // /enroll_preview on the far side requires none either.
                    QList<QPair<QByteArray, QByteArray>> headers;
                    headers.append({QByteArrayLiteral("X-Speaker-Name"),
                                    CampPlusClient::urlEncode(req.displayName.trimmed())});
                    headers.append({QByteArrayLiteral("Content-Type"),
                                    QByteArrayLiteral("application/octet-stream")});
                    if (req.allowBelowPolicy)
                        headers.append({QByteArrayLiteral("X-Allow-Below-Policy"),
                                        QByteArrayLiteral("1")});

                    QByteArray body;
                    const grpc::Status status =
                        campp->post(QStringLiteral("/enroll_preview"), req.wav, headers, &body, ms);
                    if (!status.ok()) {
                        // Same rule as EnrollSpeaker: a rejected sample is
                        // advice for the operator, not a broken server.
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
                    resp->policyCompliant =
                        object.value(QStringLiteral("policy_compliant")).toBool();
                    resp->warning = object.value(QStringLiteral("warning")).toString();
                    // base64 on the wire because the service answers JSON; the
                    // proto field is bytes, so it is decoded here and the
                    // client gets a playable WAV rather than a string.
                    resp->trimmedWav = QByteArray::fromBase64(
                        object.value(QStringLiteral("trimmed_wav_b64")).toString().toLatin1());
                    return status;
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListGlobalSpeakers),
        [campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::ListGlobalSpeakersRequest, reg::ListGlobalSpeakersResponse>(
                call, out,
                [&](const reg::ListGlobalSpeakersRequest &, reg::ListGlobalSpeakersResponse *resp) {
                    QByteArray body;
                    const grpc::Status status =
                        campp->get(QStringLiteral("/speakers"), &body, ms);
                    if (!status.ok())
                        return status;
                    const QJsonObject object = QJsonDocument::fromJson(body).object();
                    // Tombstones included on purpose - see the .proto.  A
                    // listing that hid the inactive and deleted rows would
                    // make reactivating somebody impossible and would hide who
                    // removed them.
                    for (const QJsonValue &value :
                         object.value(QStringLiteral("speakers")).toArray()) {
                        const QJsonObject item = value.toObject();
                        reg::GlobalSpeakerEntry entry;
                        entry.spkId = item.value(QStringLiteral("spk_id")).toString();
                        entry.spkName = item.value(QStringLiteral("spk_name")).toString();
                        entry.status = item.value(QStringLiteral("status")).toString();
                        entry.sampleCount =
                            quint32(qMax(0, item.value(QStringLiteral("sample_count")).toInt()));
                        entry.usableSampleCount = quint32(
                            qMax(0, item.value(QStringLiteral("usable_sample_count")).toInt()));
                        entry.createdAt = item.value(QStringLiteral("created_at")).toString();
                        entry.lastUpdated = item.value(QStringLiteral("last_updated")).toString();
                        entry.reviewedBy = item.value(QStringLiteral("reviewed_by")).toString();
                        entry.reviewReason =
                            item.value(QStringLiteral("review_reason")).toString();
                        resp->speakers.append(entry);
                    }
                    return status;
                });
        });

    // Three verbs, one shape.  Registered from a table rather than written out
    // three times: they differ only in which word the enrol service is asked
    // for, and three near-identical 60-line lambdas is how one of them ends up
    // quietly sending a different payload than the other two.
    struct GlobalAction
    {
        const char *path;
        const char *verb;
        bool needsReason;
    };
    static const GlobalAction kGlobalActions[] = {
        {rpcpath::DeactivateGlobalSpeaker, "deactivate", true},
        {rpcpath::ActivateGlobalSpeaker, "activate", false},
        {rpcpath::DeleteGlobalSpeaker, "delete", true},
    };

    for (const GlobalAction &action : kGlobalActions) {
        const QString verb = QString::fromLatin1(action.verb);
        const bool needsReason = action.needsReason;
        m_server->registerMethod(
            QString::fromLatin1(action.path),
            [campp, deadline, verb, needsReason](const grpc::ServerCall &call, QByteArray *out) {
                const int ms = deadline(call);
                return serve<reg::GlobalSpeakerActionRequest, reg::GlobalSpeakerActionResponse>(
                    call, out,
                    [&](const reg::GlobalSpeakerActionRequest &req,
                        reg::GlobalSpeakerActionResponse *resp) {
                        if (req.spkId.trimmed().isEmpty()) {
                            grpc::Status bad;
                            bad.code = grpc::InvalidArgument;
                            bad.message = QStringLiteral("thiếu spk_id");
                            return bad;
                        }
                        if (req.editorId.trimmed().isEmpty()) {
                            grpc::Status bad;
                            bad.code = grpc::InvalidArgument;
                            bad.message = QStringLiteral(
                                "thiếu editor_id - mỗi thay đổi trên DB giọng chung đều được "
                                "ghi nhật ký kèm người thao tác");
                            return bad;
                        }
                        // Refused here as well as on the far side.  A voice
                        // removed from the shared database with no stated
                        // reason is unreviewable six months later, and by then
                        // the person who did it does not remember either.
                        if (needsReason && req.reason.trimmed().isEmpty()) {
                            grpc::Status bad;
                            bad.code = grpc::InvalidArgument;
                            bad.message = QStringLiteral(
                                "thiếu lý do - thao tác này gỡ một giọng khỏi DB chung và "
                                "phải nói rõ vì sao");
                            return bad;
                        }

                        QJsonObject payload;
                        payload.insert(QStringLiteral("spk_id"), req.spkId.trimmed());
                        payload.insert(QStringLiteral("editor_id"), req.editorId.trimmed());
                        payload.insert(QStringLiteral("reason"), req.reason.trimmed());
                        QList<QPair<QByteArray, QByteArray>> headers;
                        headers.append({QByteArrayLiteral("Content-Type"),
                                        QByteArrayLiteral("application/json")});

                        QByteArray body;
                        const grpc::Status status = campp->post(
                            QStringLiteral("/speakers/") + verb,
                            QJsonDocument(payload).toJson(QJsonDocument::Compact), headers, &body,
                            ms);
                        if (!status.ok()) {
                            resp->ok = false;
                            resp->spkId = req.spkId.trimmed();
                            resp->error = status.message;
                            return grpc::Status();
                        }
                        const QJsonObject object = QJsonDocument::fromJson(body).object();
                        // `changed` is the field worth reading: deactivating
                        // somebody already inactive succeeds and changes
                        // nothing, which is not the same as having done it.
                        resp->ok = !object.contains(QStringLiteral("error"))
                            || object.value(QStringLiteral("error")).toString().isEmpty();
                        resp->error = object.value(QStringLiteral("error")).toString();
                        resp->changed = object.value(QStringLiteral("changed")).toBool();
                        resp->spkId = object.value(QStringLiteral("spk_id")).toString();
                        if (resp->spkId.isEmpty())
                            resp->spkId = req.spkId.trimmed();
                        resp->spkName = object.value(QStringLiteral("spk_name")).toString();
                        resp->status = object.value(QStringLiteral("status")).toString();
                        resp->samplesRetired =
                            quint32(qMax(0, object.value(QStringLiteral("samples_retired")).toInt()));
                        resp->deleteEventsReleased = quint32(
                            qMax(0, object.value(QStringLiteral("delete_events_released")).toInt()));
                        resp->deleteDispatch =
                            object.value(QStringLiteral("delete_dispatch")).toString();
                        resp->message = object.value(QStringLiteral("message")).toString();
                        LOG_INFO(applog::cat::Session)
                            << "global speaker" << verb << resp->spkId << "by"
                            << req.editorId.trimmed() << "- changed=" << resp->changed
                            << "status=" << resp->status;
                        return status;
                    });
            });
    }

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::GetSpeakerRegistryStatus),
        [campp, deadline, hub](const grpc::ServerCall &call, QByteArray *out) {
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
                    // The per-session counters come out of this server's own
                    // registry, not out of CAM++.  They answered a flat zero
                    // until 2026-09-24 - two voices waiting for review were
                    // reported as none waiting, and a voice already written to
                    // the shared database was reported as not published, which
                    // is the reading an operator uses to decide whether there
                    // is anything left to do.
                    if (!req.sessionId.isEmpty()) {
                        hub->store().speakerCounts(req.sessionId, &resp->sessionPendingCount,
                                                   &resp->sessionPublishedCount,
                                                   &resp->sessionFailedCount);
                    }
                    return status;
                });
        });

    // ---- SpeakerRegistryService: the per-session registry ------------------
    //
    // These two are not pass-throughs.  The registry lives in this server's own
    // store, and publishing a speaker means collecting the audio behind its
    // staged evidence spans and posting that to /enroll_from_pcm.

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::ListSessionSpeakers),
        [hub](const grpc::ServerCall &call, QByteArray *out) {
            return serve<reg::ListSessionSpeakersRequest, reg::ListSessionSpeakersResponse>(
                call, out,
                [&](const reg::ListSessionSpeakersRequest &req,
                    reg::ListSessionSpeakersResponse *resp) {
                    resp->sessionId = req.sessionId;
                    resp->speakers = hub->store().listSpeakers(req.sessionId);
                    return grpc::Status();
                });
        });

    m_server->registerMethod(
        QString::fromLatin1(rpcpath::SaveSessionSpeakers),
        [hub, campp, deadline](const grpc::ServerCall &call, QByteArray *out) {
            const int ms = deadline(call);
            return serve<reg::SaveSessionSpeakersRequest, reg::SaveSessionSpeakersResponse>(
                call, out,
                [&](const reg::SaveSessionSpeakersRequest &req,
                    reg::SaveSessionSpeakersResponse *resp) {
                    resp->sessionId = req.sessionId;
                    if (req.editorId.trimmed().isEmpty()) {
                        grpc::Status bad;
                        bad.code = grpc::InvalidArgument;
                        bad.message = QStringLiteral("thiếu editor_id - mỗi quyết định publish đều "
                                                     "được ghi nhật ký kèm người thao tác");
                        return bad;
                    }

                    for (const reg::SpeakerSelection &selection : req.selections) {
                        reg::SaveSpeakerResult result;
                        result.sessionSpeakerId = selection.sessionSpeakerId;

                        reg::SessionSpeakerEntry entry;
                        if (!hub->store().speaker(req.sessionId, selection.sessionSpeakerId,
                                                  &entry)) {
                            result.ok = false;
                            result.status = QStringLiteral("pending");
                            result.error = QStringLiteral("phiên không có giọng '%1'")
                                               .arg(selection.sessionSpeakerId);
                            resp->results.append(result);
                            continue;
                        }

                        // SESSION_ONLY is a decision, not a publish: it is
                        // recorded and nothing leaves this server.
                        if (selection.destination != reg::GlobalShared) {
                            hub->store().updateSpeakerStatus(req.sessionId,
                                                             selection.sessionSpeakerId,
                                                             QStringLiteral("session_only"),
                                                             QString(), QString());
                            result.ok = true;
                            result.status = QStringLiteral("session_only");
                            resp->results.append(result);
                            continue;
                        }

                        const QString globalName = selection.globalName.trimmed().isEmpty()
                            ? entry.verifiedName.trimmed()
                            : selection.globalName.trimmed();
                        if (globalName.isEmpty()) {
                            result.ok = false;
                            result.status = entry.status;
                            result.error = QStringLiteral(
                                "chưa có tên để publish (global_name trống và giọng này cũng "
                                "chưa được đặt verified_name)");
                            resp->results.append(result);
                            continue;
                        }

                        const QList<QPair<double, double>> spans =
                            hub->store().evidenceSpans(req.sessionId, selection.sessionSpeakerId);
                        if (spans.isEmpty()) {
                            result.ok = false;
                            result.status = entry.status;
                            result.error = QStringLiteral(
                                "giọng này chưa có bằng chứng nào được ghim - hãy dùng "
                                "rename_speaker để gán tên cho nó trước");
                            resp->results.append(result);
                            continue;
                        }

                        // The audio behind the spans, concatenated.  This is
                        // the same shape /enroll_from_pcm expects: raw s16le,
                        // not a WAV.
                        QByteArray pcm;
                        quint32 sampleRate = 16000;
                        for (const auto &span : spans) {
                            asr::AudioRangeResponse chunk;
                            QString error;
                            if (!hub->store().audioRange(req.sessionId, span.first, span.second,
                                                         &chunk, &error))
                                continue;
                            if (chunk.sampleRate)
                                sampleRate = chunk.sampleRate;
                            pcm.append(chunk.pcm);
                        }
                        if (pcm.isEmpty()) {
                            result.ok = false;
                            result.status = QStringLiteral("publish_failed");
                            result.error = QStringLiteral(
                                "không lấy được audio cho các đoạn bằng chứng của giọng này");
                            hub->store().updateSpeakerStatus(req.sessionId,
                                                             selection.sessionSpeakerId,
                                                             QStringLiteral("publish_failed"),
                                                             QString(), result.error);
                            resp->results.append(result);
                            continue;
                        }

                        // Keyed by (session, speaker) and never by name, so
                        // re-publishing the same speaker under a different name
                        // later replaces this exact sample instead of leaving
                        // the old name's copy orphaned in the database.
                        const QString tag = QStringLiteral("sess-%1-reg-%2")
                                                .arg(req.sessionId, selection.sessionSpeakerId);
                        QList<QPair<QByteArray, QByteArray>> headers;
                        headers.append({QByteArrayLiteral("X-Speaker-Name"),
                                        CampPlusClient::urlEncode(globalName)});
                        headers.append({QByteArrayLiteral("X-Enroll-Write-Tag"),
                                        CampPlusClient::urlEncode(tag)});
                        headers.append({QByteArrayLiteral("X-Enroll-Search-Tag"),
                                        CampPlusClient::urlEncode(tag)});
                        headers.append({QByteArrayLiteral("X-Session-Id"),
                                        CampPlusClient::urlEncode(req.sessionId)});
                        headers.append({QByteArrayLiteral("X-Editor-Id"),
                                        CampPlusClient::urlEncode(req.editorId.trimmed())});
                        headers.append({QByteArrayLiteral("X-Sample-Rate"),
                                        QByteArray::number(sampleRate)});
                        headers.append({QByteArrayLiteral("Content-Type"),
                                        QByteArrayLiteral("application/octet-stream")});

                        QByteArray body;
                        const grpc::Status posted = campp->post(
                            QStringLiteral("/enroll_from_pcm"), pcm, headers, &body, ms);
                        if (!posted.ok()) {
                            result.ok = false;
                            result.status = QStringLiteral("publish_failed");
                            result.error = posted.message;
                            hub->store().updateSpeakerStatus(req.sessionId,
                                                             selection.sessionSpeakerId,
                                                             QStringLiteral("publish_failed"),
                                                             QString(), posted.message);
                            resp->results.append(result);
                            continue;
                        }

                        const QJsonObject object = QJsonDocument::fromJson(body).object();
                        if (!object.value(QStringLiteral("enrolled")).toBool(true)) {
                            result.ok = false;
                            result.status = QStringLiteral("publish_failed");
                            result.error = object.value(QStringLiteral("reason"))
                                               .toString(QStringLiteral("mẫu không dùng được"));
                            hub->store().updateSpeakerStatus(req.sessionId,
                                                             selection.sessionSpeakerId,
                                                             QStringLiteral("publish_failed"),
                                                             QString(), result.error);
                            resp->results.append(result);
                            continue;
                        }

                        // "enrolled" with nothing enrolled is not a publish.
                        //
                        // enroll_service answers 200 with segments_enrolled=0
                        // when it will not add the sample - the case measured
                        // on 2026-09-24 was a name that had been DELETED from
                        // the shared database earlier: the tombstone stays,
                        // the new sample is dropped, and the operator is told
                        // the voice was published while the next meeting goes
                        // on not recognising it.  Reporting that as success is
                        // the worst kind of wrong answer here.
                        const quint32 enrolled =
                            quint32(qMax(0, object.value(QStringLiteral("segments_enrolled"))
                                                .toInt()));
                        if (enrolled == 0) {
                            result.ok = false;
                            result.status = QStringLiteral("publish_failed");
                            result.error = QStringLiteral(
                                "dịch vụ đăng ký nhận yêu cầu nhưng không ghi đoạn nào "
                                "(segments_enrolled=0) - thường là do tên '%1' đã từng bị xoá "
                                "khỏi DB chung; hãy dùng một tên khác hoặc kích hoạt lại giọng cũ")
                                               .arg(globalName);
                            hub->store().updateSpeakerStatus(req.sessionId,
                                                             selection.sessionSpeakerId,
                                                             QStringLiteral("publish_failed"),
                                                             QString(), result.error);
                            resp->results.append(result);
                            continue;
                        }

                        result.ok = true;
                        result.status = QStringLiteral("global_shared");
                        result.segmentsEnrolled = enrolled;
                        hub->store().updateSpeakerStatus(req.sessionId,
                                                         selection.sessionSpeakerId,
                                                         QStringLiteral("global_shared"),
                                                         globalName, QString());
                        hub->store().appendAudit(
                            req.sessionId, QStringLiteral("publish_speaker"),
                            QStringLiteral("{\"editor\":\"%1\",\"speaker\":\"%2\",\"name\":\"%3\"}")
                                .arg(req.editorId, selection.sessionSpeakerId, globalName));
                        resp->results.append(result);
                    }
                    return grpc::Status();
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
