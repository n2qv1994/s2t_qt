// C++ mirror of ui_client/asr_session.proto (package asr.ui.v1).
//
// Field numbers are the contract; they are repeated here verbatim from the
// .proto and must be changed only together with it.  Reserved numbers 7 and 9
// on DisplayRow (the retired itn_text/stable_text) are deliberately absent.
#ifndef ASRSESSION_H
#define ASRSESSION_H

#include "ProtoWire.h"

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

namespace asr {

struct StartSessionRequest
{
    QString configJson;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct PushAudioRequest
{
    QString sessionId;
    QByteArray pcm;
    quint32 sampleRate = 0;
    quint32 channels = 0;
    QString audioFormat;
    bool reset = false;
    quint32 vadChunkMs = 0;
    // Monotonic per-session counter starting at 1.  The adapter replays the
    // stored response for a seq it already processed, which is the only thing
    // that makes a retry after DEADLINE_EXCEEDED safe on a stateful pipeline.
    quint64 seq = 0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SessionRequest
{
    QString sessionId;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ReviewRequest
{
    QString sessionId;
    bool hasViewStartSec = false;
    double viewStartSec = 0.0;
    bool hasViewEndSec = false;
    double viewEndSec = 0.0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct AudioRangeRequest
{
    QString sessionId;
    double startSec = 0.0;
    double endSec = 0.0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct AudioRangeResponse
{
    QString sessionId;
    QByteArray pcm;
    quint32 sampleRate = 0;
    quint32 channels = 0;
    QString audioFormat;
    double startSec = 0.0;
    double endSec = 0.0;
    double totalSec = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct Word
{
    QString w;
    float c = 0.0f;
    double startSec = 0.0;
    double endSec = 0.0;
    QString speaker;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct Phrase
{
    QString text;
    float avgConf = 0.0f;
    bool isLowConf = false;
    double startSec = 0.0;
    double endSec = 0.0;
    QList<Word> words;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct DisplayRow
{
    QString rowId;
    QString speaker;
    float speakerProb = 0.0f;
    QString verifiedName;
    double startSec = 0.0;
    double endSec = 0.0;
    QString mergedText;
    QString updatingText;
    quint32 stableTokenCount = 0;
    bool isProvisional = false;
    QList<Phrase> phrases;
    QList<Word> displayTokens;
    QList<Word> updatingTokens;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct Highlight
{
    double startSec = 0.0;
    QString speaker;
    QString text;
    quint32 confPct = 0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct LatencyClient
{
    double prepareP50 = 0.0;
    double prepareP95 = 0.0;
    double waitP50 = 0.0;
    double waitP95 = 0.0;
    double parseP50 = 0.0;
    double parseP95 = 0.0;
    double e2eP50 = 0.0;
    double e2eP95 = 0.0;
    double overheadVsServerSumP50 = 0.0;
    double overheadVsServerSumP95 = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct LatencyServer
{
    double asrP50 = 0.0;
    double asrP95 = 0.0;
    double diarP50 = 0.0;
    double diarP95 = 0.0;
    double verifyP50 = 0.0;
    double verifyP95 = 0.0;
    double itnP50 = 0.0;
    double itnP95 = 0.0;
    double vadP50 = 0.0;
    double vadP95 = 0.0;
    double denoiseP50 = 0.0;
    double denoiseP95 = 0.0;
    double sumP50 = 0.0;
    double sumP95 = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct LatencyUi
{
    double onChunkTotalP50 = 0.0;
    double onChunkTotalP95 = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct LatencySummary
{
    LatencyClient client;
    LatencyServer server;
    LatencyUi ui;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct SessionState
{
    QString title;
    quint32 confThresholdPct = 0;
    QList<DisplayRow> rows;
    QList<DisplayRow> provisionalRows;
    QList<QString> speakerIds;
    QList<Highlight> highlights;
    quint32 nPhrases = 0;
    quint32 nLow = 0;
    QList<float> ampTrace;
    double ampTraceStepSec = 0.0;
    double sourceTotalSec = 0.0;
    double sourceSeenSec = 0.0;
    double speechSeenSec = 0.0;
    double wallElapsedSec = 0.0;
    double playheadRatio = 0.0;
    bool done = false;
    double ts = 0.0;
    quint32 lastAsrChunkMs = 0;
    double inferP50Ms = 0.0;
    double inferP95Ms = 0.0;
    LatencySummary latency;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct EventFlags
{
    bool streaming = false;
    bool correction = false;
    bool final = false;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct Diarization
{
    QList<float> flatScores;
    QList<qint32> shape;
    QList<qint64> subframeStartMs;
    QList<qint64> subframeEndMs;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct CorrectionUpdate
{
    QString text;
    QString fullText;
    QString committedText;
    QString tailText;
    QList<Word> mergedWords;
    QList<qint32> updatedIndices;
    double updateStartSec = 0.0;
    double updateEndSec = 0.0;
    double commitBoundarySec = 0.0;
    quint32 numCommitted = 0;
    quint32 numTail = 0;
    double itnMs = 0.0;
    double mergeMs = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct Timing
{
    double clientPrepareMs = 0.0;
    double clientWaitMs = 0.0;
    double asrMs = 0.0;
    double diarMs = 0.0;
    double verifyMs = 0.0;
    double itnMs = 0.0;
    double vadMs = 0.0;
    double denoiseMs = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct StartSessionResponse
{
    QString sessionId;
    qint64 streamId = 0;
    quint64 stateVersion = 0;
    SessionState state;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct PushAudioResponse
{
    QString sessionId;
    qint64 streamId = 0;
    quint64 stateVersion = 0;
    EventFlags events;
    double sourceSeenSec = 0.0;
    double speechSeenSec = 0.0;
    QString streamingText;
    QString text;
    QString itnText;
    QString itnFullText;
    QString itnCorrectionText;
    QList<Word> asrWords;
    float asrConfidence = 0.0f;
    QList<float> asrWordConfidence;
    QString speaker;
    float speakerProb = 0.0f;
    QString verifiedName;
    float verifyScore = 0.0f;
    qint64 chunkStartMs = 0;
    double chunkStartSec = 0.0;
    double chunkEndSec = 0.0;
    Diarization diarization;
    CorrectionUpdate correction;
    Timing timing;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct StateResponse
{
    QString sessionId;
    qint64 streamId = 0;
    quint64 stateVersion = 0;
    SessionState state;
    quint64 transcriptRevision = 0;
    bool transcriptFinal = false;
    double commitBoundarySec = 0.0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct StopSessionResponse
{
    QString sessionId;
    qint64 streamId = 0;
    quint64 stateVersion = 0;
    EventFlags events;
    PushAudioResponse result;
    SessionState state;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct TextEditRequest
{
    QString sessionId;
    quint64 baseRevision = 0;
    double startSec = 0.0;
    double endSec = 0.0;
    QList<Word> replacementWords;
    QString editorId;
    QString note;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct CanonicalTranscript
{
    quint64 revision = 0;
    bool final = false;
    double commitBoundarySec = 0.0;
    QString text;
    QList<Word> words;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ReviewEditResponse
{
    QString sessionId;
    CanonicalTranscript transcript;
    SessionState state;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ListSessionsRequest
{
    quint32 limit = 0;
    QString cursor;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SessionSummary
{
    QString sessionId;
    QString title;
    double createdAt = 0.0;
    double updatedAt = 0.0;
    double durationSec = 0.0;
    bool final = false;
    bool running = false;
    QList<QString> participants;
    QString securityLevel;
    QString mode;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ListSessionsResponse
{
    QList<SessionSummary> sessions;
    QString nextCursor;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct RenameSpeakerRequest
{
    QString sessionId;
    QString fromSpeaker;
    QString toSpeaker;
    // Always applied, including when empty: proto3 cannot distinguish "not
    // set" from "set to empty", so blank is a deliberate "clear the name".
    QString verifiedName;
    QString editorId;
    QString note;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct PipelineTraceRequest
{
    QString sessionId;
    quint64 afterSeq = 0;
    quint32 limit = 0;
    QList<QString> stages;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct PipelineTraceEvent
{
    quint64 seq = 0;
    double ts = 0.0;
    QString stage;
    QString event;
    double audioStartSec = 0.0;
    double audioEndSec = 0.0;
    QString payloadJson;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct PipelineTraceResponse
{
    QString sessionId;
    QList<PipelineTraceEvent> events;
    quint64 nextSeq = 0;
    bool hasMore = false;
    bool enabled = false;
    bool truncated = false;
    quint64 maxBytes = 0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct AuditHistoryRequest
{
    QString sessionId;
    quint32 limit = 0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct AuditEvent
{
    double ts = 0.0;
    QString event;
    QString payloadJson;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct AuditHistoryResponse
{
    QString sessionId;
    QList<AuditEvent> events;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ModelStatusRequest
{
    QByteArray serialize() const { return QByteArray(); }
    // Nothing to read, but a handler still has to be able to say "this was a
    // well-formed request" without special-casing the empty message.
    void parse(pw::Reader &reader) { reader.skipRemaining(); }
};

struct ModelStatusEntry
{
    QString name;
    QString version;
    QString state;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ModelStatusResponse
{
    QList<ModelStatusEntry> models;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

} // namespace asr

// StateResponse crosses from the poller thread to the UI through a queued
// signal, so Qt needs to know how to copy it.
Q_DECLARE_METATYPE(asr::StateResponse)

#endif // ASRSESSION_H
