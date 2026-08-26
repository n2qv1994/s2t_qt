#include "TritonBackend.h"

#include "core/Logger.h"
#include "grpc/Methods.h"
#include "grpc/UnaryCall.h"
#include "proto/TritonInfer.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>

namespace {

// The tensor names of asr_diar_session, read off the running server with
// `curl :8010/v2/models/asr_diar_session/config` rather than guessed.  A typo
// here is an INVALID_ARGUMENT from Triton at run time, not a build error, which
// is why they are constants in one place.
namespace tensor {
constexpr const char *AudioChunk = "audio_chunk";
constexpr const char *StreamId = "stream_id";
constexpr const char *Reset = "reset";
constexpr const char *VadChunkMs = "vad_chunk_ms";
constexpr const char *IsFinal = "is_final";
constexpr const char *ValidSamples = "valid_samples";
constexpr const char *ExpectedSpeakers = "expected_speakers_json";
} // namespace tensor

// The outputs this server asks for.  Deliberately not "all of them":
// asr_diar_session also emits the mel spectrogram, the VAD-trimmed audio and
// several debug windows, which together are megabytes per packet and are of no
// use to a client.  Asking for only what is mapped keeps a 160 ms packet's
// answer in the kilobytes.
const char *const kWantedOutputs[] = {
    "text",           "streaming_text",   "itn_text",       "itn_full_text",
    "itn_correction_text", "speaker",     "speaker_prob",   "verified_name",
    "verify_score",   "chunk_start_ms",   "asr_confidence", "asr_words",
    "asr_words_conf", "asr_words_start_sec", "asr_words_end_sec",
    "diar_chunk_preds_flat", "diar_chunk_preds_shape",
    "diar_subframe_start_ms", "diar_subframe_end_ms",
    "timing_asr_ms",  "timing_diar_total_ms", "timing_verify_ms", "timing_itn_ms",
    "timing_vad_ms",  "timing_denoise_ms",
    "itn_committed_text", "itn_tail_text", "itn_merged_words_json",
    "itn_updated_indices_json", "itn_commit_boundary_sec",
    "itn_num_committed", "itn_num_tail", "itn_ms", "itn_ms_merge",
};

QByteArray rawFloats(const QList<float> &values)
{
    QByteArray out;
    out.resize(values.size() * 4);
    for (int i = 0; i < values.size(); ++i)
        std::memcpy(out.data() + i * 4, &values[i], 4);
    return out;
}

QByteArray rawInt32(qint32 value)
{
    QByteArray out;
    out.resize(4);
    std::memcpy(out.data(), &value, 4);
    return out;
}

QByteArray rawInt64(qint64 value)
{
    QByteArray out;
    out.resize(8);
    std::memcpy(out.data(), &value, 8);
    return out;
}

// A BYTES tensor element is a 4-byte little-endian length followed by the bytes.
QByteArray rawString(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    QByteArray out;
    const quint32 length = quint32(utf8.size());
    out.resize(4);
    std::memcpy(out.data(), &length, 4);
    out.append(utf8);
    return out;
}

// 16-bit little-endian PCM to the float32 in [-1, 1) that the model expects.
// Dividing by 32768 rather than 32767 keeps the mapping exact for the negative
// full-scale sample and matches what every NeMo front end does.
QList<float> pcmToFloat(const QByteArray &pcm)
{
    const int samples = pcm.size() / 2;
    QList<float> out;
    out.reserve(samples);
    const uchar *bytes = reinterpret_cast<const uchar *>(pcm.constData());
    for (int i = 0; i < samples; ++i) {
        const qint16 sample = qint16(quint16(bytes[i * 2]) | (quint16(bytes[i * 2 + 1]) << 8));
        out.append(float(sample) / 32768.0f);
    }
    return out;
}

void addInput(trt::ModelInferRequest *request, const char *name, const char *datatype,
              const QList<qint64> &shape, const QByteArray &raw)
{
    trt::InferInputTensor input;
    input.name = QString::fromLatin1(name);
    input.datatype = QString::fromLatin1(datatype);
    input.shape = shape;
    request->inputs.append(input);
    // Positional: Triton pairs raw_input_contents[i] with inputs[i], so these
    // two appends must stay together.
    request->rawInputContents.append(raw);
}

QList<asr::Word> wordsFrom(const trt::ModelInferResponse &response)
{
    QList<QString> texts;
    QList<float> confidences;
    QList<float> starts;
    QList<float> ends;
    response.stringsAt(QStringLiteral("asr_words"), &texts);
    response.floatsAt(QStringLiteral("asr_words_conf"), &confidences);
    response.floatsAt(QStringLiteral("asr_words_start_sec"), &starts);
    response.floatsAt(QStringLiteral("asr_words_end_sec"), &ends);

    QList<asr::Word> out;
    out.reserve(texts.size());
    for (int i = 0; i < texts.size(); ++i) {
        asr::Word word;
        word.w = texts.at(i);
        // The parallel arrays are supposed to be the same length.  When they
        // are not, the words still come through with whatever timing exists
        // rather than the whole packet being dropped over a ragged edge.
        word.c = i < confidences.size() ? confidences.at(i) : 0.0f;
        word.startSec = i < starts.size() ? double(starts.at(i)) : 0.0;
        word.endSec = i < ends.size() ? double(ends.at(i)) : 0.0;
        out.append(word);
    }
    return out;
}

QList<asr::Word> wordsFromJson(const QString &json)
{
    QList<asr::Word> out;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray())
        return out;
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        asr::Word word;
        word.w = object.value(QStringLiteral("w")).toString();
        if (word.w.isEmpty())
            word.w = object.value(QStringLiteral("word")).toString();
        word.c = float(object.value(QStringLiteral("c")).toDouble());
        word.startSec = object.value(QStringLiteral("start_sec")).toDouble();
        word.endSec = object.value(QStringLiteral("end_sec")).toDouble();
        word.speaker = object.value(QStringLiteral("speaker")).toString();
        out.append(word);
    }
    return out;
}

QList<qint32> int32sFromJson(const QString &json)
{
    QList<qint32> out;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isArray())
        return out;
    for (const QJsonValue &value : document.array())
        out.append(qint32(value.toInt()));
    return out;
}

} // namespace

// One meeting against asr_diar_session.  Stateless on this side: the model
// keeps its own per-stream state keyed by the stream_id tensor, which is why a
// session here is little more than a channel plus that id.
class TritonSession : public BackendSession
{
public:
    TritonSession(const QString &sessionId, qint64 streamId, const QString &target,
                  const QString &token, const BackendSessionConfig &config, const QString &model,
                  int timeoutMs)
        : m_sessionId(sessionId), m_streamId(streamId), m_config(config), m_model(model),
          m_timeoutMs(timeoutMs), m_channel(target, token)
    {
    }

    grpc::Status push(const asr::PushAudioRequest &request, asr::PushAudioResponse *out) override
    {
        // The packet carries the real audio format; config_json does not.  Take
        // it from the wire rather than trusting the hint, or source_seen_sec is
        // out by the ratio between the two.
        if (request.sampleRate != 0)
            m_config.sampleRate = request.sampleRate;
        if (request.channels != 0)
            m_config.channels = request.channels;
        return infer(request.pcm, request.reset || m_first, false,
                     request.vadChunkMs ? request.vadChunkMs : m_config.vadChunkMs, out);
    }

    grpc::Status finish(asr::PushAudioResponse *out) override
    {
        // An empty final chunk is how this model is told the meeting is over:
        // it flushes the endpointer and emits the last correction pass.
        const grpc::Status status = infer(QByteArray(), false, true, m_config.vadChunkMs, out);
        if (status.ok())
            out->events.final = true;
        return status;
    }

    void abandon() override { m_channel.reset(); }
    void reset() override { m_channel.reset(); }

private:
    grpc::Status infer(const QByteArray &pcm, bool reset, bool isFinal, quint32 vadChunkMs,
                       asr::PushAudioResponse *out)
    {
        const QList<float> samples = pcmToFloat(pcm);

        trt::ModelInferRequest request;
        request.modelName = m_model;
        request.id = m_sessionId;
        addInput(&request, tensor::AudioChunk, trt::dtype::Fp32, {qint64(samples.size())},
                 rawFloats(samples));
        addInput(&request, tensor::StreamId, trt::dtype::Int64, {1}, rawInt64(m_streamId));
        addInput(&request, tensor::Reset, trt::dtype::Int32, {1}, rawInt32(reset ? 1 : 0));
        addInput(&request, tensor::VadChunkMs, trt::dtype::Int32, {1},
                 rawInt32(qint32(vadChunkMs)));
        addInput(&request, tensor::IsFinal, trt::dtype::Int32, {1}, rawInt32(isFinal ? 1 : 0));
        addInput(&request, tensor::ValidSamples, trt::dtype::Int32, {1},
                 rawInt32(qint32(samples.size())));
        // Only on the first call: the model treats it as the attendee list for
        // the whole stream, and re-sending it mid-meeting would reset
        // verification against a list that has not changed.
        if (m_first && !m_config.expectedSpeakersJson.isEmpty()) {
            addInput(&request, tensor::ExpectedSpeakers, trt::dtype::Bytes, {1},
                     rawString(m_config.expectedSpeakersJson));
        }
        for (const char *name : kWantedOutputs) {
            trt::InferRequestedOutputTensor output;
            output.name = QString::fromLatin1(name);
            request.outputs.append(output);
        }

        trt::ModelInferResponse response;
        const grpc::Status status = grpc::unaryCall(m_channel, rpcpath::TritonModelInfer, request,
                                                    &response, m_timeoutMs);
        if (!status.ok())
            return status;

        m_first = false;
        m_samplesSent += quint64(samples.size());
        translate(response, out);
        return status;
    }

    void translate(const trt::ModelInferResponse &response, asr::PushAudioResponse *out)
    {
        out->sessionId = m_sessionId;
        out->streamId = m_streamId;

        QString text;
        if (response.stringAt(QStringLiteral("text"), &text))
            out->text = text;
        if (response.stringAt(QStringLiteral("streaming_text"), &text)) {
            out->streamingText = text;
            out->events.streaming = !text.isEmpty();
        }
        if (response.stringAt(QStringLiteral("itn_text"), &text))
            out->itnText = text;
        if (response.stringAt(QStringLiteral("itn_full_text"), &text))
            out->itnFullText = text;
        if (response.stringAt(QStringLiteral("itn_correction_text"), &text)) {
            out->itnCorrectionText = text;
            out->events.correction = !text.isEmpty();
        }
        if (response.stringAt(QStringLiteral("verified_name"), &text))
            out->verifiedName = text;

        qint32 speaker = 0;
        if (response.int32At(QStringLiteral("speaker"), &speaker)) {
            // The client parses this with toInt() and renders "speaker_N"
            // itself, so it must stay a bare number - see speakerIndexOf() in
            // s2t-qt-client/core/TranscriptModel.cpp.
            out->speaker = QString::number(speaker);
        }
        float value = 0.0f;
        if (response.floatAt(QStringLiteral("speaker_prob"), &value))
            out->speakerProb = value;
        if (response.floatAt(QStringLiteral("verify_score"), &value))
            out->verifyScore = value;
        if (response.floatAt(QStringLiteral("asr_confidence"), &value))
            out->asrConfidence = value;

        qint32 chunkStartMs = 0;
        if (response.int32At(QStringLiteral("chunk_start_ms"), &chunkStartMs)) {
            out->chunkStartMs = chunkStartMs;
            out->chunkStartSec = double(chunkStartMs) / 1000.0;
        }

        out->asrWords = wordsFrom(response);
        for (const asr::Word &word : out->asrWords)
            out->asrWordConfidence.append(word.c);
        if (!out->asrWords.isEmpty())
            out->chunkEndSec = out->asrWords.last().endSec;

        response.floatsAt(QStringLiteral("diar_chunk_preds_flat"), &out->diarization.flatScores);
        response.int32sAt(QStringLiteral("diar_chunk_preds_shape"), &out->diarization.shape);
        QList<qint32> starts;
        QList<qint32> ends;
        response.int32sAt(QStringLiteral("diar_subframe_start_ms"), &starts);
        response.int32sAt(QStringLiteral("diar_subframe_end_ms"), &ends);
        for (qint32 ms : starts)
            out->diarization.subframeStartMs.append(qint64(ms));
        for (qint32 ms : ends)
            out->diarization.subframeEndMs.append(qint64(ms));

        if (response.floatAt(QStringLiteral("timing_asr_ms"), &value))
            out->timing.asrMs = double(value);
        if (response.floatAt(QStringLiteral("timing_diar_total_ms"), &value))
            out->timing.diarMs = double(value);
        if (response.floatAt(QStringLiteral("timing_verify_ms"), &value))
            out->timing.verifyMs = double(value);
        if (response.floatAt(QStringLiteral("timing_itn_ms"), &value))
            out->timing.itnMs = double(value);
        if (response.floatAt(QStringLiteral("timing_vad_ms"), &value))
            out->timing.vadMs = double(value);
        if (response.floatAt(QStringLiteral("timing_denoise_ms"), &value))
            out->timing.denoiseMs = double(value);

        if (response.stringAt(QStringLiteral("itn_committed_text"), &text))
            out->correction.committedText = text;
        if (response.stringAt(QStringLiteral("itn_tail_text"), &text))
            out->correction.tailText = text;
        if (response.stringAt(QStringLiteral("itn_merged_words_json"), &text))
            out->correction.mergedWords = wordsFromJson(text);
        if (response.stringAt(QStringLiteral("itn_updated_indices_json"), &text))
            out->correction.updatedIndices = int32sFromJson(text);
        if (response.floatAt(QStringLiteral("itn_commit_boundary_sec"), &value))
            out->correction.commitBoundarySec = double(value);
        qint32 count = 0;
        if (response.int32At(QStringLiteral("itn_num_committed"), &count))
            out->correction.numCommitted = quint32(count);
        if (response.int32At(QStringLiteral("itn_num_tail"), &count))
            out->correction.numTail = quint32(count);
        if (response.floatAt(QStringLiteral("itn_ms"), &value))
            out->correction.itnMs = double(value);
        if (response.floatAt(QStringLiteral("itn_ms_merge"), &value))
            out->correction.mergeMs = double(value);
        out->correction.text = out->itnCorrectionText;
        out->correction.fullText = out->itnFullText;

        // Triton counts nothing for us, so progress is our own sample count.
        const double perSecond = double(qMax(1u, m_config.sampleRate) * qMax(1u, m_config.channels));
        out->sourceSeenSec = double(m_samplesSent) / perSecond;
        out->speechSeenSec = out->sourceSeenSec;
    }

    QString m_sessionId;
    qint64 m_streamId = 0;
    BackendSessionConfig m_config;
    QString m_model;
    int m_timeoutMs = 30000;
    bool m_first = true;
    quint64 m_samplesSent = 0;
    grpc::Channel m_channel;
};

// ---- backend ---------------------------------------------------------------

TritonBackend::TritonBackend(const QString &target, const QString &token, const QString &model,
                             int timeoutMs)
    : m_target(target), m_token(token), m_model(model), m_timeoutMs(timeoutMs),
      m_admin(QStringLiteral("triton-admin"), target, token)
{
    LOG_INFO(applog::cat::Rpc) << "inference backend: Triton at" << target << "- model" << model;
}

TritonBackend::~TritonBackend() = default;

grpc::Status TritonBackend::ping(int timeoutMs, double *latencyMs)
{
    QElapsedTimer clock;
    clock.start();
    trt::ServerLiveRequest request;
    trt::ServerLiveResponse response;
    grpc::Status status = m_admin.call([&](grpc::Channel &channel) {
        return grpc::unaryCall(channel, rpcpath::TritonServerLive, request, &response, timeoutMs);
    });
    if (latencyMs)
        *latencyMs = double(clock.nsecsElapsed()) / 1e6;
    if (status.ok() && !response.live) {
        // Reachable but not serving is a different problem from unreachable,
        // and an operator can act on the difference.
        status.code = grpc::Unavailable;
        status.message = QStringLiteral("Triton trả lời nhưng báo chưa sẵn sàng (live=false)");
    }
    return status;
}

grpc::Status TritonBackend::models(asr::ModelStatusResponse *out, int timeoutMs)
{
    trt::RepositoryIndexRequest request;
    trt::RepositoryIndexResponse response;
    const grpc::Status status = m_admin.call([&](grpc::Channel &channel) {
        return grpc::unaryCall(channel, rpcpath::TritonRepositoryIndex, request, &response,
                               timeoutMs);
    });
    if (!status.ok())
        return status;
    for (const trt::ModelIndex &model : response.models) {
        asr::ModelStatusEntry entry;
        entry.name = model.name;
        entry.version = model.version;
        entry.state = model.state.isEmpty() ? model.reason : model.state;
        out->models.append(entry);
    }
    return status;
}

std::unique_ptr<BackendSession> TritonBackend::open(const QString &sessionId, qint64 streamId,
                                                    const BackendSessionConfig &config,
                                                    grpc::Status *status)
{
    // Nothing to negotiate: the model has no open call, and the first infer
    // with reset=1 is what starts the stream on its side.
    *status = grpc::Status();
    LOG_INFO(applog::cat::Session) << "Triton session" << sessionId << "-> model" << m_model
                                   << "stream_id" << streamId << "at" << config.sampleRate << "Hz";
    return std::make_unique<TritonSession>(sessionId, streamId, m_target, m_token, config, m_model,
                                           m_timeoutMs);
}
