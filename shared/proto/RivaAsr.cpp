#include "RivaAsr.h"

namespace riva {

using pw::Reader;
using pw::WireType;
using pw::Writer;

// ---------------------------------------------------------------- config ----

QByteArray SpeechContext::serialize() const
{
    Writer w;
    w.putRepeatedString(1, phrases);
    w.putFloat(4, boost);
    return w.take();
}

void SpeechContext::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: phrases.append(r.readString()); break;
        case 4: boost = r.readFloat(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray SpeakerDiarizationConfig::serialize() const
{
    Writer w;
    w.putBool(1, enableSpeakerDiarization);
    w.putInt32(2, maxSpeakerCount);
    return w.take();
}

void SpeakerDiarizationConfig::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: enableSpeakerDiarization = r.readBool(); break;
        case 2: maxSpeakerCount = r.readInt32(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray EndpointingConfig::serialize() const
{
    Writer w;
    // Every field here is `optional` upstream, so presence is the message: a
    // threshold deliberately set to 0 must go on the wire, which is what the
    // putOptional* pair is for.
    if (hasStartHistory)
        w.putOptionalInt32(1, startHistory);
    if (hasStartThreshold)
        w.putOptionalFloat(2, startThreshold);
    if (hasStopHistory)
        w.putOptionalInt32(3, stopHistory);
    if (hasStopThreshold)
        w.putOptionalFloat(4, stopThreshold);
    if (hasStopHistoryEou)
        w.putOptionalInt32(5, stopHistoryEou);
    if (hasStopThresholdEou)
        w.putOptionalFloat(6, stopThresholdEou);
    return w.take();
}

void EndpointingConfig::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: startHistory = r.readInt32(); hasStartHistory = true; break;
        case 2: startThreshold = r.readFloat(); hasStartThreshold = true; break;
        case 3: stopHistory = r.readInt32(); hasStopHistory = true; break;
        case 4: stopThreshold = r.readFloat(); hasStopThreshold = true; break;
        case 5: stopHistoryEou = r.readInt32(); hasStopHistoryEou = true; break;
        case 6: stopThresholdEou = r.readFloat(); hasStopThresholdEou = true; break;
        default: r.skip(type); break;
        }
    }
}

QByteArray RecognitionConfig::serialize() const
{
    Writer w;
    w.putEnum(1, encoding);
    w.putInt32(2, sampleRateHertz);
    w.putString(3, languageCode);
    w.putInt32(4, maxAlternatives);
    w.putBool(5, profanityFilter);
    w.putRepeatedMessage(6, speechContexts);
    w.putInt32(7, audioChannelCount);
    w.putBool(8, enableWordTimeOffsets);
    w.putBool(11, enableAutomaticPunctuation);
    w.putBool(12, enableSeparateRecognitionPerChannel);
    w.putString(13, model);
    w.putBool(14, verbatimTranscripts);
    // A submessage whose fields are all default still has to go out when
    // diarization is on, and must not when it is off: Riva reads presence.
    if (diarizationConfig.enableSpeakerDiarization || diarizationConfig.maxSpeakerCount != 0)
        w.putSubMessage(19, diarizationConfig.serialize());
    w.putStringMap(24, customConfiguration);
    if (endpointingConfig.any())
        w.putSubMessage(25, endpointingConfig.serialize());
    return w.take();
}

void RecognitionConfig::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: encoding = r.readEnum(); break;
        case 2: sampleRateHertz = r.readInt32(); break;
        case 3: languageCode = r.readString(); break;
        case 4: maxAlternatives = r.readInt32(); break;
        case 5: profanityFilter = r.readBool(); break;
        case 6: r.appendMessage(&speechContexts); break;
        case 7: audioChannelCount = r.readInt32(); break;
        case 8: enableWordTimeOffsets = r.readBool(); break;
        case 11: enableAutomaticPunctuation = r.readBool(); break;
        case 12: enableSeparateRecognitionPerChannel = r.readBool(); break;
        case 13: model = r.readString(); break;
        case 14: verbatimTranscripts = r.readBool(); break;
        case 19: r.readMessageInto(&diarizationConfig); break;
        case 24: r.readStringMapEntry(&customConfiguration); break;
        case 25: r.readMessageInto(&endpointingConfig); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray StreamingRecognitionConfig::serialize() const
{
    Writer w;
    w.putSubMessage(1, config.serialize());
    w.putBool(2, interimResults);
    return w.take();
}

void StreamingRecognitionConfig::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readMessageInto(&config); break;
        case 2: interimResults = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray StreamingRecognizeRequest::serialize() const
{
    Writer w;
    // The oneof: never both.  An audio message with an empty payload is legal
    // and meaningful (a keep-alive), so the flag decides, not the size.
    if (hasStreamingConfig)
        w.putSubMessage(1, streamingConfig.serialize());
    else
        w.putBytes(2, audioContent);
    w.putStringMap(3, runtimeConfig);
    return w.take();
}

void StreamingRecognizeRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readMessageInto(&streamingConfig); hasStreamingConfig = true; break;
        case 2: audioContent = r.readBytes(); hasStreamingConfig = false; break;
        case 3: r.readStringMapEntry(&runtimeConfig); break;
        default: r.skip(type); break;
        }
    }
}

// --------------------------------------------------------------- results ----

QByteArray WordInfo::serialize() const
{
    Writer w;
    w.putInt32(1, startTime);
    w.putInt32(2, endTime);
    w.putString(3, word);
    w.putFloat(4, confidence);
    w.putInt32(5, speakerTag);
    w.putString(6, languageCode);
    return w.take();
}

void WordInfo::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: startTime = r.readInt32(); break;
        case 2: endTime = r.readInt32(); break;
        case 3: word = r.readString(); break;
        case 4: confidence = r.readFloat(); break;
        case 5: speakerTag = r.readInt32(); break;
        case 6: languageCode = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray SpeechRecognitionAlternative::serialize() const
{
    Writer w;
    w.putString(1, transcript);
    w.putFloat(2, confidence);
    w.putRepeatedMessage(3, words);
    w.putRepeatedString(4, languageCode);
    return w.take();
}

void SpeechRecognitionAlternative::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: transcript = r.readString(); break;
        case 2: confidence = r.readFloat(); break;
        case 3: r.appendMessage(&words); break;
        case 4: languageCode.append(r.readString()); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray PipelineStates::serialize() const
{
    Writer w;
    w.putPackedFloat(1, vadProbabilities);
    return w.take();
}

void PipelineStates::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readPackedFloat(type, &vadProbabilities); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray StreamingRecognitionResult::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, alternatives);
    w.putBool(2, isFinal);
    w.putFloat(3, stability);
    w.putInt32(5, channelTag);
    w.putFloat(6, audioProcessed);
    if (hasPipelineStates)
        w.putSubMessage(7, pipelineStates.serialize());
    return w.take();
}

void StreamingRecognitionResult::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&alternatives); break;
        case 2: isFinal = r.readBool(); break;
        case 3: stability = r.readFloat(); break;
        case 5: channelTag = r.readInt32(); break;
        case 6: audioProcessed = r.readFloat(); break;
        case 7: r.readMessageInto(&pipelineStates); hasPipelineStates = true; break;
        default: r.skip(type); break;
        }
    }
}

QByteArray StreamingRecognizeResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, results);
    return w.take();
}

void StreamingRecognizeResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&results); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray SpeechRecognitionResult::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, alternatives);
    w.putInt32(2, channelTag);
    w.putFloat(3, audioProcessed);
    return w.take();
}

void SpeechRecognitionResult::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&alternatives); break;
        case 2: channelTag = r.readInt32(); break;
        case 3: audioProcessed = r.readFloat(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray RecognizeRequest::serialize() const
{
    Writer w;
    w.putSubMessage(1, config.serialize());
    w.putBytes(2, audio);
    return w.take();
}

void RecognizeRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.readMessageInto(&config); break;
        case 2: audio = r.readBytes(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray RecognizeResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, results);
    return w.take();
}

void RecognizeResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&results); break;
        default: r.skip(type); break;
        }
    }
}

// ------------------------------------------------------------ model list ----

QByteArray RivaSpeechRecognitionConfigRequest::serialize() const
{
    Writer w;
    w.putString(1, modelName);
    return w.take();
}

void RivaSpeechRecognitionConfigRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: modelName = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray ModelConfig::serialize() const
{
    Writer w;
    w.putString(1, modelName);
    w.putStringMap(2, parameters);
    return w.take();
}

void ModelConfig::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: modelName = r.readString(); break;
        case 2: r.readStringMapEntry(&parameters); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray RivaSpeechRecognitionConfigResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, modelConfig);
    return w.take();
}

void RivaSpeechRecognitionConfigResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&modelConfig); break;
        default: r.skip(type); break;
        }
    }
}

} // namespace riva
