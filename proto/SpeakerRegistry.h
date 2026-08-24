// C++ mirror of ui_client/speaker_registry.proto (package asr.ui.v1).
//
// Enrollment and CAM++ registry management live in their own service on the
// same host/port/token as ProductASRService, so this client never has to
// speak HTTP to the enroll sidecar - see the .proto's own header comment.
#ifndef SPEAKERREGISTRY_H
#define SPEAKERREGISTRY_H

#include "ProtoWire.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace reg {

enum SpeakerDestination {
    SpeakerDestinationUnspecified = 0,
    // Keep this identity local to the session; never touches the global DB.
    SessionOnly = 1,
    // Publish the staged evidence into the global CAM++ DB (rebuilding it).
    GlobalShared = 2,
};

struct GetEnrollmentScriptRequest
{
    QByteArray serialize() const { return QByteArray(); }
};

struct GetEnrollmentScriptResponse
{
    QString scriptText;
    quint32 sampleRate = 0;
    double recommendedDurationSec = 0.0;
    quint32 targetSegments = 0;
    void parse(pw::Reader &reader);
};

struct EnrollSpeakerRequest
{
    QString displayName;
    QByteArray wav;
    QString editorId;
    QString note;
    // Never silent: the sample is stored policy_compliant=false and the
    // response carries a warning, so the speaker stays visibly in need of a
    // proper re-enrolment.
    bool allowBelowPolicy = false;
    QByteArray serialize() const;
};

struct EnrollSpeakerResponse
{
    bool ok = false;
    QString error;
    QString speakerId;
    double rawSeconds = 0.0;
    double speechSecondsAfterVad = 0.0;
    quint32 segmentsEnrolled = 0;
    quint32 targetSegments = 0;
    QString warning;
    double dbMtime = 0.0;
    void parse(pw::Reader &reader);
};

struct SessionSpeakerEvidence
{
    double totalSpeechSec = 0.0;
    quint32 spanCount = 0;
    double stagedAt = 0.0;
    QString sourceVerifiedName;
    void parse(pw::Reader &reader);
};

struct SessionSpeakerEntry
{
    QString sessionSpeakerId;
    QList<QString> diarSlots;
    QString verifiedName;
    double score = 0.0;
    quint32 windows = 0;
    double createdAt = 0.0;
    double updatedAt = 0.0;
    // "pending" | "session_only" | "global_shared" | "publish_failed"
    QString status;
    bool hasEvidence = false;
    SessionSpeakerEvidence evidence;
    QString publishedName;
    double publishedAt = 0.0;
    QString publishError;
    void parse(pw::Reader &reader);
};

struct ListSessionSpeakersRequest
{
    QString sessionId;
    QByteArray serialize() const;
};

struct ListSessionSpeakersResponse
{
    QString sessionId;
    QList<SessionSpeakerEntry> speakers;
    void parse(pw::Reader &reader);
};

struct SpeakerSelection
{
    QString sessionSpeakerId;
    int destination = SpeakerDestinationUnspecified;
    // GLOBAL_SHARED only; empty means "use this entry's verified_name".
    QString globalName;
    QByteArray serialize() const;
};

struct SaveSessionSpeakersRequest
{
    QString sessionId;
    QList<SpeakerSelection> selections;
    QString editorId;
    QByteArray serialize() const;
};

struct SaveSpeakerResult
{
    QString sessionSpeakerId;
    bool ok = false;
    QString status;
    QString error;
    quint32 segmentsEnrolled = 0;
    void parse(pw::Reader &reader);
};

struct SaveSessionSpeakersResponse
{
    QString sessionId;
    QList<SaveSpeakerResult> results;
    void parse(pw::Reader &reader);
};

struct SpeakerBelowPolicy
{
    QString spkId;
    QString spkName;
    quint32 sampleCount = 0;
    double longestSampleSec = 0.0;
    QString reason;
    // "legacy" | "urgent" | "other" - without this every speaker on a freshly
    // migrated host reads identically and a real urgent enrolment is invisible.
    QString kind;
    void parse(pw::Reader &reader);
};

struct GetSpeakerRegistryStatusRequest
{
    QString sessionId;
    QByteArray serialize() const;
};

struct GetSpeakerRegistryStatusResponse
{
    double globalDbMtime = 0.0;
    QString globalDbRevision;
    quint32 globalSpeakerCount = 0;
    bool sidecarReachable = false;
    QString sessionId;
    quint32 sessionPendingCount = 0;
    quint32 sessionPublishedCount = 0;
    quint32 sessionFailedCount = 0;
    QList<QString> globalSpeakerNames;
    QList<SpeakerBelowPolicy> speakersBelowPolicy;
    void parse(pw::Reader &reader);
};

} // namespace reg

#endif // SPEAKERREGISTRY_H
