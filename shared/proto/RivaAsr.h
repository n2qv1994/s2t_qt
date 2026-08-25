// C++ mirror of NVIDIA Riva's riva/proto/riva_asr.proto (package
// nvidia.riva.asr) and the AudioEncoding enum from riva/proto/riva_audio.proto
// (package nvidia.riva).
//
// Same rule as shared/proto/AsrSession.h: the field numbers below are the
// contract, copied verbatim from the .proto, and must only change together
// with it.  There is no protoc on either build machine - see
// shared/proto/ProtoWire.h for why the whole codec is hand written.
//
// Only what s2t-qt-server actually calls is mirrored:
//
//   Recognize                       offline re-decode of a finished meeting
//   StreamingRecognize              the live audio path
//   GetRivaSpeechRecognitionConfig  what get_model_status reports
//
// `RequestId id = 100` is present on four of these messages.  It is optional
// tracing metadata, so it is never written here and simply skipped on read -
// which is exactly what a protobuf runtime does with a field it does not know.
#ifndef RIVAASR_H
#define RIVAASR_H

#include "ProtoWire.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace riva {

// nvidia.riva.AudioEncoding.  LINEAR_PCM is the only one this project sends:
// the client already captures 16-bit little-endian PCM and re-encoding it on
// the way past would cost quality for bandwidth we are not short of.
enum AudioEncoding {
    EncodingUnspecified = 0,
    LinearPcm = 1,
    Flac = 2,
    Mulaw = 3,
    OggOpus = 4,
    Alaw = 20,
};

struct SpeechContext
{
    QList<QString> phrases; // 1
    float boost = 0.0f;     // 4
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SpeakerDiarizationConfig
{
    bool enableSpeakerDiarization = false; // 1
    qint32 maxSpeakerCount = 0;            // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// Every field here is `optional` in the .proto, which on the wire means proto3
// field presence: an unset field is absent, not zero.  A `has` flag per field
// is how that is expressed without protoc - and it matters, because 0 is a
// meaningful threshold and writing it when the operator set nothing would
// silently change Riva's endpointing.
struct EndpointingConfig
{
    bool hasStartHistory = false;
    qint32 startHistory = 0; // 1
    bool hasStartThreshold = false;
    float startThreshold = 0.0f; // 2
    bool hasStopHistory = false;
    qint32 stopHistory = 0; // 3
    bool hasStopThreshold = false;
    float stopThreshold = 0.0f; // 4
    bool hasStopHistoryEou = false;
    qint32 stopHistoryEou = 0; // 5
    bool hasStopThresholdEou = false;
    float stopThresholdEou = 0.0f; // 6

    bool any() const
    {
        return hasStartHistory || hasStartThreshold || hasStopHistory || hasStopThreshold
            || hasStopHistoryEou || hasStopThresholdEou;
    }
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// Note the gaps: 9 and 10 are not ours to use, they are reserved in the
// upstream .proto (they were Google's enable_speaker_diarization /
// diarization_speaker_count before Riva moved diarization into its own
// message).  15..18 and 20..23 are likewise unused there.
struct RecognitionConfig
{
    int encoding = LinearPcm;                    // 1
    qint32 sampleRateHertz = 0;                  // 2
    QString languageCode;                        // 3
    qint32 maxAlternatives = 0;                  // 4
    bool profanityFilter = false;                // 5
    QList<SpeechContext> speechContexts;         // 6
    qint32 audioChannelCount = 0;                // 7
    bool enableWordTimeOffsets = false;          // 8
    bool enableAutomaticPunctuation = false;     // 11
    bool enableSeparateRecognitionPerChannel = false; // 12
    QString model;                               // 13
    bool verbatimTranscripts = false;            // 14
    SpeakerDiarizationConfig diarizationConfig;  // 19
    QMap<QString, QString> customConfiguration;  // 24
    EndpointingConfig endpointingConfig;         // 25 (optional)
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct StreamingRecognitionConfig
{
    RecognitionConfig config;    // 1
    bool interimResults = false; // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// A oneof: exactly one of the two is set on any given message.  The first
// message on a StreamingRecognize stream carries the config and no audio;
// every one after it carries audio and no config.
struct StreamingRecognizeRequest
{
    bool hasStreamingConfig = false;
    StreamingRecognitionConfig streamingConfig; // 1
    QByteArray audioContent;                    // 2
    QMap<QString, QString> runtimeConfig;       // 3
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// start_time and end_time are milliseconds from the start of the stream, as
// int32 - not the seconds this project's own Word carries.  The conversion
// lives in RivaBackend and nowhere else.
struct WordInfo
{
    qint32 startTime = 0;   // 1
    qint32 endTime = 0;     // 2
    QString word;           // 3
    float confidence = 0.0f; // 4
    qint32 speakerTag = 0;  // 5
    QString languageCode;   // 6
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SpeechRecognitionAlternative
{
    QString transcript;      // 1
    float confidence = 0.0f; // 2
    QList<WordInfo> words;   // 3
    QList<QString> languageCode; // 4
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct PipelineStates
{
    QList<float> vadProbabilities; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct StreamingRecognitionResult
{
    QList<SpeechRecognitionAlternative> alternatives; // 1
    bool isFinal = false;                             // 2
    float stability = 0.0f;                           // 3
    qint32 channelTag = 0;                            // 5
    float audioProcessed = 0.0f;                      // 6
    bool hasPipelineStates = false;
    PipelineStates pipelineStates;                    // 7 (optional)
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct StreamingRecognizeResponse
{
    QList<StreamingRecognitionResult> results; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SpeechRecognitionResult
{
    QList<SpeechRecognitionAlternative> alternatives; // 1
    qint32 channelTag = 0;                            // 2
    float audioProcessed = 0.0f;                      // 3
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RecognizeRequest
{
    RecognitionConfig config; // 1
    QByteArray audio;         // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RecognizeResponse
{
    QList<SpeechRecognitionResult> results; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RivaSpeechRecognitionConfigRequest
{
    QString modelName; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ModelConfig
{
    QString modelName;                    // 1
    QMap<QString, QString> parameters;    // 2
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct RivaSpeechRecognitionConfigResponse
{
    QList<ModelConfig> modelConfig; // 1
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

} // namespace riva

#endif // RIVAASR_H
