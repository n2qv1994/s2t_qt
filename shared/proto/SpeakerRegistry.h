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
    // Nothing to read, but a handler still has to be able to say "this was a
    // well-formed request" without special-casing the empty message.
    void parse(pw::Reader &reader) { reader.skipRemaining(); }
};

struct GetEnrollmentScriptResponse
{
    QString scriptText;
    quint32 sampleRate = 0;
    double recommendedDurationSec = 0.0;
    quint32 targetSegments = 0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
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
    void parse(pw::Reader &reader);
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
    QByteArray serialize() const;
};

struct SessionSpeakerEvidence
{
    double totalSpeechSec = 0.0;
    quint32 spanCount = 0;
    double stagedAt = 0.0;
    QString sourceVerifiedName;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
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
    QByteArray serialize() const;
};

struct ListSessionSpeakersRequest
{
    QString sessionId;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct ListSessionSpeakersResponse
{
    QString sessionId;
    QList<SessionSpeakerEntry> speakers;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct SpeakerSelection
{
    QString sessionSpeakerId;
    int destination = SpeakerDestinationUnspecified;
    // GLOBAL_SHARED only; empty means "use this entry's verified_name".
    QString globalName;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SaveSessionSpeakersRequest
{
    QString sessionId;
    QList<SpeakerSelection> selections;
    QString editorId;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct SaveSpeakerResult
{
    QString sessionSpeakerId;
    bool ok = false;
    QString status;
    QString error;
    quint32 segmentsEnrolled = 0;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct SaveSessionSpeakersResponse
{
    QString sessionId;
    QList<SaveSpeakerResult> results;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
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
    QByteArray serialize() const;
};

struct GetSpeakerRegistryStatusRequest
{
    QString sessionId;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
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
    QByteArray serialize() const;
};

// ---- the global registry's own lifecycle -----------------------------------
//
// Everything above this line either reads the global database or adds to it.
// What follows is the other half: seeing what is in it, and taking something
// back out.  It matters because the database is written to by hand over months
// and nothing else can tidy it - as of 2026-09-21 the deployed one holds 62
// speakers including `5`, `A`, `a1` and an SQL-injection probe string, every
// one of them a live candidate the verifier can match a meeting against.
//
// These went missing when the Python adapter left the deployment picture: it
// implements all ten RPCs, this server implemented five.  The five it did not
// are exactly the ones that let an operator clean up.

struct PreviewEnrollmentRequest
{
    QString displayName;
    QByteArray wav;
    bool allowBelowPolicy = false;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

// What EnrollSpeaker WOULD keep, without keeping it.  Nothing is written: no
// database row, no catalogue entry, no audit line - so there is deliberately
// no editor_id on the request, because there is nothing to attribute.
//
// This is the answer to the one-way problem.  An enrolment cannot be undone
// through any API, so the only safe way to find out whether a recording is
// good enough is to ask without committing: `trimmedWav` is the audio that
// would actually be enrolled, and playing it is how an operator hears that
// they recorded two people instead of one.
struct PreviewEnrollmentResponse
{
    bool ok = false;
    QString error;
    QString speakerId;
    double rawSeconds = 0.0;
    double speechSecondsAfterVad = 0.0;
    bool policyCompliant = false;
    QString warning;
    QByteArray trimmedWav;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct GlobalSpeakerEntry
{
    QString spkId;
    QString spkName;
    // "approved" | "inactive" | "deleted" | "pending" | "rejected".  The
    // listing includes the tombstones on purpose: an operator has to be able
    // to see who removed a speaker, and to put one back.
    QString status;
    quint32 sampleCount = 0;
    quint32 usableSampleCount = 0;
    // ISO-8601 strings, not doubles - that is what the .proto says and what
    // the enrol service sends.
    QString createdAt;
    QString lastUpdated;
    QString reviewedBy;
    QString reviewReason;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

struct ListGlobalSpeakersRequest
{
    QByteArray serialize() const { return QByteArray(); }
    void parse(pw::Reader &reader) { reader.skipRemaining(); }
};

struct ListGlobalSpeakersResponse
{
    QList<GlobalSpeakerEntry> speakers;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

// One request type for all three lifecycle actions - deactivate, activate and
// delete - because the .proto says so and because they differ only in which
// verb the enrol service is asked for.
struct GlobalSpeakerActionRequest
{
    QString spkId;
    // Required, and enforced twice: here, and again by the enrol service,
    // which is the final authority on every global database write and audits
    // this identity all the way to it.
    QString editorId;
    // Required for deactivate and delete, optional for activate.  A removal
    // with no stated reason is unreviewable six months later.
    QString reason;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct GlobalSpeakerActionResponse
{
    bool ok = false;
    QString error;
    // False when the action was a no-op - deactivating someone already
    // inactive.  Not an error, and worth telling apart from one.
    bool changed = false;
    QString spkId;
    QString spkName;
    QString status;
    quint32 samplesRetired = 0;
    quint32 deleteEventsReleased = 0;
    QString deleteDispatch;
    QString message;
    void parse(pw::Reader &reader);
    QByteArray serialize() const;
};

} // namespace reg

#endif // SPEAKERREGISTRY_H
