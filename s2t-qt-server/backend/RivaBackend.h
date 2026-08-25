// The inference tier as NVIDIA Riva, spoken to over nvidia.riva.asr.
//
// One meeting is one StreamingRecognize call: a bidirectional stream that
// carries audio for as long as the meeting lasts and answers as it goes.  That
// is a different shape from Triton, where one packet is one ModelInfer, and it
// is why shared/grpc grew a streaming path - see Http2Connection::openStream.
//
// The two differences that matter to everything above this class:
//
//   - Riva answers when it has something to say, not once per packet.  A push
//     may therefore produce nothing at all, and a later one may produce three
//     results at once.  push() reports whatever had arrived by the time it was
//     called and never blocks waiting for more; finish() drains the rest.
//   - Riva has no speaker enrolment.  `speaker_tag` from its diarization is an
//     anonymous cluster number, so verified_name stays empty here and identity
//     comes from the server's own speaker table instead.
//
// Field numbers and message shapes are in shared/proto/RivaAsr.h, mirrored from
// riva/proto/riva_asr.proto.
#ifndef RIVABACKEND_H
#define RIVABACKEND_H

#include "InferenceBackend.h"
#include "RpcLane.h"

#include <QString>

class RivaBackend : public InferenceBackend
{
public:
    RivaBackend(const QString &target, const QString &token, const QString &model,
                const QString &language, int timeoutMs);
    ~RivaBackend() override;

    QString name() const override { return QStringLiteral("riva"); }
    QString target() const override { return m_target; }

    grpc::Status ping(int timeoutMs, double *latencyMs) override;
    grpc::Status models(asr::ModelStatusResponse *out, int timeoutMs) override;

    std::unique_ptr<BackendSession> open(const QString &sessionId, qint64 streamId,
                                         const BackendSessionConfig &config,
                                         grpc::Status *status) override;

private:
    QString m_target;
    QString m_token;
    QString m_model;
    QString m_language;
    int m_timeoutMs = 30000;
    // Same reason as TritonBackend's: ping() runs on the probe thread and
    // models() on a connection thread, and a Channel has thread affinity.
    RpcLane m_admin;
};

#endif // RIVABACKEND_H
