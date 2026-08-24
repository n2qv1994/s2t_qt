// Typed façade over one gRPC channel: every RPC in ProductASRService and
// SpeakerRegistryService, with the method paths spelled exactly as the two
// .proto files declare them.
//
// Both services live on the same host, port and token (see
// speaker_registry.proto's header comment), so one channel serves both.
#ifndef ASRCLIENT_H
#define ASRCLIENT_H

#include "GrpcChannel.h"
#include "../proto/AsrSession.h"
#include "../proto/SpeakerRegistry.h"

class AsrClient
{
public:
    AsrClient(const QString &target, const QString &token);

    void setTarget(const QString &target, const QString &token);
    QString target() const { return m_channel.target(); }

    // Drops the TCP connection; the next call dials again.  Used by the audio
    // path so a recovered network is observed on the very next retry instead
    // of after HTTP/2's own backoff.
    void reset() { m_channel.reset(); }
    grpc::Status ping(int timeoutMs, double *latencyMs) { return m_channel.ping(timeoutMs, latencyMs); }

    // ---- ProductASRService -------------------------------------------------
    grpc::Status startSession(const asr::StartSessionRequest &req, asr::StartSessionResponse *out,
                              int timeoutMs);
    grpc::Status pushAudio(const asr::PushAudioRequest &req, asr::PushAudioResponse *out,
                           int timeoutMs);
    grpc::Status getLiveState(const asr::SessionRequest &req, asr::StateResponse *out, int timeoutMs);
    grpc::Status getReviewState(const asr::ReviewRequest &req, asr::StateResponse *out, int timeoutMs);
    grpc::Status getAudioRange(const asr::AudioRangeRequest &req, asr::AudioRangeResponse *out,
                               int timeoutMs);
    grpc::Status applyTextEdit(const asr::TextEditRequest &req, asr::ReviewEditResponse *out,
                               int timeoutMs);
    grpc::Status stopSession(const asr::SessionRequest &req, asr::StopSessionResponse *out,
                             int timeoutMs);
    grpc::Status listSessions(const asr::ListSessionsRequest &req, asr::ListSessionsResponse *out,
                              int timeoutMs);
    grpc::Status renameSpeaker(const asr::RenameSpeakerRequest &req, asr::ReviewEditResponse *out,
                               int timeoutMs);
    grpc::Status getPipelineTrace(const asr::PipelineTraceRequest &req,
                                  asr::PipelineTraceResponse *out, int timeoutMs);
    grpc::Status getAuditHistory(const asr::AuditHistoryRequest &req, asr::AuditHistoryResponse *out,
                                 int timeoutMs);
    grpc::Status getModelStatus(asr::ModelStatusResponse *out, int timeoutMs);

    // ---- SpeakerRegistryService --------------------------------------------
    grpc::Status getEnrollmentScript(reg::GetEnrollmentScriptResponse *out, int timeoutMs);
    grpc::Status enrollSpeaker(const reg::EnrollSpeakerRequest &req, reg::EnrollSpeakerResponse *out,
                               int timeoutMs);
    grpc::Status listSessionSpeakers(const reg::ListSessionSpeakersRequest &req,
                                     reg::ListSessionSpeakersResponse *out, int timeoutMs);
    grpc::Status saveSessionSpeakers(const reg::SaveSessionSpeakersRequest &req,
                                     reg::SaveSessionSpeakersResponse *out, int timeoutMs);
    grpc::Status getSpeakerRegistryStatus(const reg::GetSpeakerRegistryStatusRequest &req,
                                          reg::GetSpeakerRegistryStatusResponse *out, int timeoutMs);

private:
    template <typename Req, typename Resp>
    grpc::Status call(const char *method, const Req &request, Resp *out, int timeoutMs)
    {
        QByteArray payload;
        grpc::Status status = m_channel.invoke(QString::fromLatin1(method), request.serialize(),
                                               timeoutMs, &payload);
        if (!status.ok())
            return status;
        pw::Reader reader(payload);
        out->parse(reader);
        if (!reader.ok()) {
            status.code = grpc::Internal;
            status.message = QStringLiteral("could not decode the %1 reply")
                                 .arg(QString::fromLatin1(method));
        }
        return status;
    }

    grpc::Channel m_channel;
};

#endif // ASRCLIENT_H
