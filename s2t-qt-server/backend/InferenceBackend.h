// What the Server buffer needs from an inference tier, and nothing more.
//
// Until 2026-08-25 there was no interface here at all: the buffer relayed
// asr.ui.v1.ProductASRService straight through to a Python adapter that owned
// the whole pipeline.  The adapter is gone from the deployment picture, so the
// buffer now talks to the inference tier itself - and there are two of them:
//
//   TritonBackend  inference.GRPCInferenceService (KServe v2) against the
//                  model repository that is deployed today: asr_diar_session
//                  and the ten models behind it.  This is what runs now.
//   RivaBackend    nvidia.riva.asr.RivaSpeechRecognition against a Riva
//                  server.  Selected by configuration; see ServerConfig.
//
// Nothing above this header knows which one it got.  That is the point: the
// two tiers answer with very different shapes, and the translation into
// asr::PushAudioResponse belongs on the far side of this line so that
// SessionBuffer's queueing, journalling and drain barrier stay backend-blind.
//
// Threading: a BackendSession owns a socket and therefore has thread affinity.
// It must be created, used and destroyed on one thread - the SessionBuffer
// forwarder that drives it.  See RpcLane.h for the same rule stated for the
// lanes that genuinely do have to cross threads.
#ifndef INFERENCEBACKEND_H
#define INFERENCEBACKEND_H

#include "grpc/GrpcChannel.h"
#include "proto/AsrSession.h"

#include <QString>

#include <memory>

// Everything the tier needs to know about a meeting before the first packet.
//
// start_session hands the client's wishes over as a JSON blob - that is the
// shape the .proto defines and it is not ours to change - so this is the parsed
// form, with the defaults a Vietnamese meeting actually wants rather than
// whatever a missing key would otherwise mean.
struct BackendSessionConfig
{
    QString title;
    // A hint only.  The client does not put the audio format in config_json -
    // it puts it on every push_audio - so the real rate arrives with the first
    // packet and overrides this.  Getting that wrong silently multiplies
    // source_seen_sec by the ratio, which reads as a pipeline three times
    // faster or slower than it is.
    quint32 sampleRate = 16000;
    quint32 channels = 1;
    // Total length of the source when a file is being played back, for the
    // client's progress bar.  Zero while recording live: the meeting has no
    // known end.
    double sourceTotalSec = 0.0;
    QString audioFormat = QStringLiteral("pcm_s16le");
    QString language = QStringLiteral("vi-VN");
    // Empty means "the backend's own default model", which for Triton is
    // asr_diar_session and for Riva is whatever the server was deployed with.
    QString model;
    bool diarization = true;
    int maxSpeakers = 0; // 0 = let the tier decide
    bool punctuation = true;
    bool wordTimeOffsets = true;
    bool interimResults = true;
    quint32 vadChunkMs = 0;
    // Passed to Triton's expected_speakers_json input, which is how a meeting
    // with a known attendee list gets verified names instead of "Người 1".
    QString expectedSpeakersJson;
    // The request exactly as it arrived.  Kept so the journal and the audit log
    // record what was asked for, not what we made of it.
    QString rawJson;

    // Never fails: an unreadable or absent config yields the defaults above and
    // explains itself through `warning`, because refusing to start a meeting
    // over a malformed optional field would be the worse failure.
    static BackendSessionConfig fromJson(const QString &json, QString *warning);
};

// One meeting's worth of inference.
class BackendSession
{
public:
    virtual ~BackendSession() = default;

    // One chunk of audio in, whatever the tier produced for it out.  `out` is
    // filled with as much of asr::PushAudioResponse as the tier can support;
    // fields it has no equivalent for are left at their defaults rather than
    // invented.
    virtual grpc::Status push(const asr::PushAudioRequest &request,
                              asr::PushAudioResponse *out) = 0;

    // No more audio is coming.  Anything still in flight is collected into
    // `out`.  This is the drain barrier's far end: when it returns OK the tier
    // has seen every byte the client sent.
    virtual grpc::Status finish(asr::PushAudioResponse *out) = 0;

    // Drops the session without a flush - used when the whole server is going
    // down, where pretending to have closed the meeting cleanly would be a lie.
    virtual void abandon() = 0;

    // Drops the transport so the next push dials again.  Called after a
    // transport failure instead of waiting out HTTP/2's own backoff.
    virtual void reset() = 0;
};

class InferenceBackend
{
public:
    virtual ~InferenceBackend() = default;

    // "triton" or "riva".  Reported in get_buffer_status so an operator can see
    // which tier a running server is actually pointed at.
    virtual QString name() const = 0;
    virtual QString target() const = 0;

    // Reachability, on a channel of the caller's own.  Called from the probe
    // thread, so it must not touch anything a session owns.
    virtual grpc::Status ping(int timeoutMs, double *latencyMs) = 0;

    // What get_model_status answers with.
    virtual grpc::Status models(asr::ModelStatusResponse *out, int timeoutMs) = 0;

    // Opens a session.  Must be called on the thread that will drive it.
    // `streamId` is the buffer's own handle for the meeting; Triton takes it as
    // a tensor, Riva has no use for it.
    virtual std::unique_ptr<BackendSession> open(const QString &sessionId, qint64 streamId,
                                                 const BackendSessionConfig &config,
                                                 grpc::Status *status) = 0;
};

#endif // INFERENCEBACKEND_H
