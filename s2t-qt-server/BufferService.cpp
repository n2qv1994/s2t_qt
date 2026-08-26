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
                        if (hub->store().loadState(req.sessionId, resp))
                            return grpc::Status();
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
                    // Live meetings first, then the archive.  A meeting that is
                    // running exists in both - it was written to the store at
                    // start - so the live entry wins: it is the one with a true
                    // `running` flag and an up-to-the-second duration.
                    QSet<QString> seen;
                    if (req.cursor.isEmpty()) {
                        for (const asr::SessionSummary &live : hub->summaries(0)) {
                            resp->sessions.append(live);
                            seen.insert(live.sessionId);
                        }
                    }
                    QString nextCursor;
                    for (const asr::SessionSummary &stored :
                         hub->store().listSessions(int(req.limit), req.cursor, &nextCursor)) {
                        if (!seen.contains(stored.sessionId))
                            resp->sessions.append(stored);
                    }
                    resp->nextCursor = nextCursor;
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

                        result.ok = true;
                        result.status = QStringLiteral("global_shared");
                        result.segmentsEnrolled =
                            quint32(object.value(QStringLiteral("segments_enrolled")).toInt());
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
