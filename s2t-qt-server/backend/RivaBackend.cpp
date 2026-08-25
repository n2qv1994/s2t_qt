#include "RivaBackend.h"

#include "core/Logger.h"
#include "grpc/Methods.h"
#include "grpc/UnaryCall.h"
#include "proto/RivaAsr.h"

#include <QElapsedTimer>

namespace {

// How long finish() will wait for Riva to answer everything still in flight.
// The far side has to run its endpointer over the tail of the meeting, which is
// slower than a live chunk but nowhere near a minute.
const int kDrainMs = 30000;

// A live push never blocks: it sends, then takes whatever has already arrived.
// A few milliseconds of polling costs nothing at a 160 ms cadence and catches
// the common case where the answer to the previous packet landed while this one
// was being serialised.
const int kPollMs = 5;

double secondsFromMs(qint32 ms)
{
    return double(ms) / 1000.0;
}

// The client parses DisplayRow.speaker with toInt() and renders "speaker_N"
// itself, so a diarization cluster has to travel as a bare number.  Anything
// else shows up in the UI as speaker_0 for every single row - see
// speakerIndexOf() in s2t-qt-client/core/TranscriptModel.cpp.
QString speakerLabel(qint32 tag)
{
    return tag > 0 ? QString::number(tag - 1) : QString::number(0);
}

} // namespace

// One meeting on one StreamingRecognize stream.
class RivaSession : public BackendSession
{
public:
    RivaSession(const QString &sessionId, const QString &target, const QString &token,
                const BackendSessionConfig &config, const QString &model, const QString &language,
                int timeoutMs)
        : m_sessionId(sessionId), m_config(config), m_model(model), m_language(language),
          m_timeoutMs(timeoutMs), m_channel(target, token), m_stream(&m_channel)
    {
    }

    ~RivaSession() override { m_stream.cancel(); }

    // Opens the stream and sends the one config message that has to precede any
    // audio.  Separate from the constructor so the caller gets a status rather
    // than a half-built object.
    grpc::Status begin()
    {
        // No grpc-timeout: the stream lives as long as the meeting does, and a
        // deadline here would end a long meeting mid-sentence.
        grpc::Status status = m_stream.start(QString::fromLatin1(rpcpath::RivaStreamingRecognize), 0);
        if (!status.ok())
            return status;

        riva::StreamingRecognizeRequest first;
        first.hasStreamingConfig = true;
        riva::RecognitionConfig &cfg = first.streamingConfig.config;
        cfg.encoding = riva::LinearPcm;
        cfg.sampleRateHertz = qint32(m_config.sampleRate);
        cfg.audioChannelCount = qint32(m_config.channels);
        cfg.languageCode = m_config.language.isEmpty() ? m_language : m_config.language;
        cfg.maxAlternatives = 1;
        cfg.enableWordTimeOffsets = m_config.wordTimeOffsets;
        cfg.enableAutomaticPunctuation = m_config.punctuation;
        cfg.model = m_config.model.isEmpty() ? m_model : m_config.model;
        cfg.diarizationConfig.enableSpeakerDiarization = m_config.diarization;
        cfg.diarizationConfig.maxSpeakerCount = m_config.maxSpeakers;
        // Interim results are the whole point of a streaming demo: without them
        // nothing appears until the endpointer closes a segment.
        first.streamingConfig.interimResults = m_config.interimResults;

        status = m_stream.send(first.serialize(), m_timeoutMs);
        if (!status.ok())
            return status;

        LOG_INFO(applog::cat::Session)
            << "Riva stream open for" << m_sessionId << "- model"
            << (cfg.model.isEmpty() ? QStringLiteral("(mặc định)") : cfg.model) << "language"
            << cfg.languageCode << "at" << cfg.sampleRateHertz << "Hz,"
            << (m_config.diarization ? "diarization on" : "diarization off");
        return status;
    }

    grpc::Status push(const asr::PushAudioRequest &request, asr::PushAudioResponse *out) override
    {
        out->sessionId = m_sessionId;

        // LINEAR_PCM is exactly what the client already captures, so the audio
        // goes out untouched.  Re-encoding here would cost quality to save
        // bandwidth we are not short of.
        riva::StreamingRecognizeRequest message;
        message.audioContent = request.pcm;
        grpc::Status status = m_stream.send(message.serialize(), m_timeoutMs);
        if (!status.ok())
            return status;

        m_bytesSent += quint64(request.pcm.size());
        return collect(out, kPollMs);
    }

    grpc::Status finish(asr::PushAudioResponse *out) override
    {
        out->sessionId = m_sessionId;
        if (!m_stream.active()) {
            // Already torn down by a failure; the caller still gets whatever the
            // session knew, rather than an error about a stream that is gone.
            fillProgress(out);
            return grpc::Status();
        }

        grpc::Status status = m_stream.closeSend(m_timeoutMs);
        if (!status.ok())
            return status;

        // Now the deadline does apply: the far side has been told there is no
        // more audio, so it owes an answer.
        QElapsedTimer clock;
        clock.start();
        while (!m_stream.ended() && clock.elapsed() < kDrainMs) {
            status = collect(out, 200);
            if (!status.ok())
                return status;
        }
        if (!m_stream.ended()) {
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("Riva không kết thúc luồng sau %1 ms").arg(kDrainMs);
            return status;
        }
        out->events.final = true;
        fillProgress(out);
        return m_stream.finish();
    }

    void abandon() override { m_stream.cancel(); }

    void reset() override
    {
        // A dead stream cannot be resumed - Riva keeps its decoder state on the
        // stream, and there is no way to tell it "carry on from where we were".
        // So this opens a *new* one and sends the config again.  Riva loses the
        // context it had built, which costs accuracy across the seam; the
        // alternative is ending the meeting on a network blip, and the client
        // is still recording.
        m_stream.cancel();
        m_channel.reset();
        const grpc::Status status = begin();
        if (!status.ok()) {
            LOG_WARN(applog::cat::Session)
                << "re-opening the Riva stream for" << m_sessionId
                << "failed:" << status.toString() << "- the next push will report it";
            return;
        }
        LOG_INFO(applog::cat::Session)
            << "Riva stream for" << m_sessionId
            << "re-opened after a transport failure - decoder context was lost at"
            << m_audioProcessedSec << "s";
    }

private:
    // Takes every complete response that has arrived and folds it into `out`.
    grpc::Status collect(asr::PushAudioResponse *out, int waitMs)
    {
        for (;;) {
            QByteArray raw;
            bool have = false;
            const grpc::Status status = m_stream.receive(&raw, &have, waitMs);
            if (!status.ok())
                return status;
            if (!have)
                break;

            riva::StreamingRecognizeResponse response;
            pw::Reader reader(raw);
            response.parse(reader);
            if (!reader.ok()) {
                grpc::Status bad;
                bad.code = grpc::Internal;
                bad.message = QStringLiteral("không giải mã được StreamingRecognizeResponse");
                return bad;
            }
            for (const riva::StreamingRecognitionResult &result : response.results)
                apply(result, out);
            // Only the first read may wait; anything already buffered behind it
            // is taken without blocking again.
            waitMs = 0;
        }
        fillProgress(out);
        return grpc::Status();
    }

    void apply(const riva::StreamingRecognitionResult &result, asr::PushAudioResponse *out)
    {
        if (result.alternatives.isEmpty())
            return;
        const riva::SpeechRecognitionAlternative &best = result.alternatives.first();

        // audio_processed is Riva's own count of how far into the stream it has
        // got, which is exactly what source_seen_sec means to the client - and
        // is better than our byte count, because it reflects the pipeline
        // rather than the socket.
        if (result.audioProcessed > 0.0f)
            m_audioProcessedSec = double(result.audioProcessed);

        if (result.isFinal) {
            // A final segment appends to the meeting; an interim one replaces
            // the tail.  Keeping them in separate fields is what lets the
            // client show settled text and a moving edge at the same time.
            out->text = best.transcript;
            out->events.final = true;
            out->asrConfidence = best.confidence;
            for (const riva::WordInfo &word : best.words) {
                asr::Word converted;
                converted.w = word.word;
                converted.c = word.confidence;
                converted.startSec = secondsFromMs(word.startTime);
                converted.endSec = secondsFromMs(word.endTime);
                converted.speaker = speakerLabel(word.speakerTag);
                out->asrWords.append(converted);
                out->asrWordConfidence.append(word.confidence);
            }
            if (!best.words.isEmpty()) {
                out->chunkStartSec = secondsFromMs(best.words.first().startTime);
                out->chunkEndSec = secondsFromMs(best.words.last().endTime);
                out->chunkStartMs = best.words.first().startTime;
                out->speaker = speakerLabel(best.words.last().speakerTag);
            }
        } else {
            out->streamingText = best.transcript;
            out->events.streaming = true;
        }
    }

    void fillProgress(asr::PushAudioResponse *out) const
    {
        // Falls back to the byte count only while Riva has not yet said
        // anything: a meeting that starts with silence would otherwise report
        // no progress at all and the client's queue reading would sit at 0.
        const double bytesPerSecond =
            double(qMax(1u, m_config.sampleRate) * qMax(1u, m_config.channels) * 2u);
        const double fromBytes = double(m_bytesSent) / bytesPerSecond;
        out->sourceSeenSec = m_audioProcessedSec > 0.0 ? m_audioProcessedSec : fromBytes;
        // Riva reports no separate speech figure, and inventing one would make
        // the "speech seen" reading a lie.  It stays at the source figure,
        // which is what a pipeline without VAD gating honestly means.
        out->speechSeenSec = out->sourceSeenSec;
    }

    QString m_sessionId;
    BackendSessionConfig m_config;
    QString m_model;
    QString m_language;
    int m_timeoutMs = 30000;
    quint64 m_bytesSent = 0;
    double m_audioProcessedSec = 0.0;
    grpc::Channel m_channel;
    grpc::ClientStream m_stream;
};

// ---- backend ---------------------------------------------------------------

RivaBackend::RivaBackend(const QString &target, const QString &token, const QString &model,
                         const QString &language, int timeoutMs)
    : m_target(target), m_token(token), m_model(model), m_language(language),
      m_timeoutMs(timeoutMs), m_admin(QStringLiteral("riva-admin"), target, token)
{
    LOG_INFO(applog::cat::Rpc) << "inference backend: Riva at" << target << "- model"
                               << (model.isEmpty() ? QStringLiteral("(mặc định)") : model)
                               << "language" << language;
}

RivaBackend::~RivaBackend() = default;

grpc::Status RivaBackend::ping(int timeoutMs, double *latencyMs)
{
    QElapsedTimer clock;
    clock.start();
    // GetRivaSpeechRecognitionConfig rather than a bare TCP probe: it proves the
    // ASR service is actually there, which is the question the badge is really
    // asking.  A Riva that is up but still loading models answers this.
    riva::RivaSpeechRecognitionConfigRequest request;
    riva::RivaSpeechRecognitionConfigResponse response;
    const grpc::Status status = m_admin.call([&](grpc::Channel &channel) {
        return grpc::unaryCall(channel, rpcpath::RivaGetConfig, request, &response, timeoutMs);
    });
    if (latencyMs)
        *latencyMs = double(clock.nsecsElapsed()) / 1e6;
    return status;
}

grpc::Status RivaBackend::models(asr::ModelStatusResponse *out, int timeoutMs)
{
    riva::RivaSpeechRecognitionConfigRequest request;
    riva::RivaSpeechRecognitionConfigResponse response;
    const grpc::Status status = m_admin.call([&](grpc::Channel &channel) {
        return grpc::unaryCall(channel, rpcpath::RivaGetConfig, request, &response, timeoutMs);
    });
    if (!status.ok())
        return status;

    for (const riva::ModelConfig &config : response.modelConfig) {
        asr::ModelStatusEntry entry;
        entry.name = config.modelName;
        // Riva reports a model's streaming/offline flavour and its language in
        // the parameter map rather than as a version, so the most useful thing
        // to put in `version` is the language it will actually decode.
        entry.version = config.parameters.value(QStringLiteral("language_code"));
        if (entry.version.isEmpty())
            entry.version = config.parameters.value(QStringLiteral("type"));
        // Anything Riva lists is loaded; it does not advertise models it cannot
        // serve.  Saying READY here keeps the column meaning what it means for
        // Triton, where the state is explicit.
        entry.state = QStringLiteral("READY");
        out->models.append(entry);
    }
    return status;
}

std::unique_ptr<BackendSession> RivaBackend::open(const QString &sessionId, qint64 streamId,
                                                  const BackendSessionConfig &config,
                                                  grpc::Status *status)
{
    // Riva has no use for the buffer's stream id: the HTTP/2 stream itself is
    // the session, so the meeting's identity is the connection, not a tensor.
    Q_UNUSED(streamId);
    auto session = std::make_unique<RivaSession>(sessionId, m_target, m_token, config, m_model,
                                                 m_language, m_timeoutMs);
    *status = session->begin();
    if (!status->ok()) {
        LOG_WARN(applog::cat::Session)
            << "opening a Riva stream for" << sessionId << "failed:" << status->toString();
        return nullptr;
    }
    return session;
}
