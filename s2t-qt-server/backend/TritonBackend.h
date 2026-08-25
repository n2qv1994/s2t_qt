// The inference tier as it is deployed today: Triton, spoken to directly.
//
// This is what grpc_session_adapter.py used to do from Python.  The adapter is
// out of the picture, so the mapping between one audio packet and one
// ModelInfer call lives here instead - and it is close to one for one, because
// asr_diar_session is a BLS model whose outputs were already shaped for this
// UI.  See the tensor table in the .cpp; it was read off the running server
// (`/v2/models/asr_diar_session/config`), not guessed.
//
// What Triton does NOT do, and this class therefore does not pretend to:
//
//   - it has no session store.  The meeting's transcript, its revisions and
//     its audio live in s2t-qt-server now (SessionStore), not upstream.
//   - it has no notion of `seq` idempotency.  The adapter replayed a stored
//     answer for a seq it had already processed, which is what made a retry
//     after DEADLINE_EXCEEDED safe.  Triton has no such memory, so a push that
//     fails after the model may have consumed the audio is NOT retried - the
//     same rule SessionBuffer already applies, for the same reason.
#ifndef TRITONBACKEND_H
#define TRITONBACKEND_H

#include "InferenceBackend.h"
#include "RpcLane.h"

#include <QString>

class TritonBackend : public InferenceBackend
{
public:
    TritonBackend(const QString &target, const QString &token, const QString &model,
                  int timeoutMs);
    ~TritonBackend() override;

    QString name() const override { return QStringLiteral("triton"); }
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
    int m_timeoutMs = 30000;
    // ping() and models() are called from the probe thread and from connection
    // threads respectively, and a Channel has thread affinity - so they go
    // through a lane rather than touching a socket of their own.
    RpcLane m_admin;
};

#endif // TRITONBACKEND_H
