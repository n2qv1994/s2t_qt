// Everything about a meeting that has to outlive the process.
//
// The journal (SessionJournal.h) is a *queue*: it holds audio that has not
// reached the inference tier yet and is deleted once it has.  This is the other
// half - the meeting itself, kept so that `list_sessions` can list a meeting
// from last week, `get_review_state` can show its transcript, `get_audio_range`
// can play a sentence back, and the speaker registry can remember which voices
// a reviewer decided to publish.
//
// Two stores, deliberately, and the split is the same one
// ui_client/session_store.py made:
//
//   SQLite  metadata, the transcript snapshot, the audit log, the per-session
//           speaker registry - small, structured, queried by id.
//   a flat  the audio itself, one `<handle>.s16le` per meeting, appended and
//   file    read back by sample offset.
//
// Audio does not go in the database.  A three-hour meeting is ~350 MB of PCM;
// as a BLOB that is rewritten on every append and read whole to serve a
// two-second range, while a plain file seeks straight to the byte.
//
// Threading: one connection, one mutex, and every call takes it.  QSqlDatabase
// has thread affinity, and the alternative - a connection per connection-thread
// - would multiply open handles by the number of operators for writes that are
// a few hundred bytes each.  Audio append bypasses the lock's contention
// because it writes to the flat file, which is the only hot path here.
#ifndef SESSIONSTORE_H
#define SESSIONSTORE_H

#include "proto/AsrSession.h"
#include "proto/SpeakerRegistry.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>

class SessionStore
{
public:
    SessionStore();
    ~SessionStore();

    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;

    // Opens (and migrates) the database under `dir`.  An empty `dir` leaves the
    // store disabled: every read then answers empty and every write is a no-op,
    // which is how a deployment that does not want a meeting archive behaves.
    bool open(const QString &dir, QString *error);
    bool enabled() const { return m_enabled; }
    QString directory() const { return m_dir; }

    // ---- the meeting -------------------------------------------------------
    void createSession(const QString &sessionId, const QString &title, const QString &client,
                       const QString &configJson, quint32 sampleRate, quint32 channels);
    // The transcript snapshot.  Written at stop and on a timer, not per packet:
    // a long meeting's state is megabytes and the journal already covers crash
    // recovery of the audio behind it.
    void saveState(const QString &sessionId, const asr::StateResponse &state);
    bool loadState(const QString &sessionId, asr::StateResponse *out);
    void markFinished(const QString &sessionId, double durationSec);

    QList<asr::SessionSummary> listSessions(int limit, const QString &cursor,
                                            QString *nextCursor);
    bool hasSession(const QString &sessionId);

    // ---- audio -------------------------------------------------------------
    // Appends to the flat file, then records the new length.  In that order,
    // always: the count may lag what is on disk but must never overstate it,
    // because audioRange() clamps to the count and would otherwise read past
    // the end of a file that is still being written.
    void appendAudio(const QString &sessionId, const QByteArray &pcm);
    // A range that runs past the end is clamped, not refused - a reviewer
    // dragging over the live edge should get what exists.  A session with no
    // audio yet answers empty rather than NOT_FOUND: start_session without a
    // successful push is a real state, not an error.
    bool audioRange(const QString &sessionId, double startSec, double endSec,
                    asr::AudioRangeResponse *out, QString *error);

    // ---- audit -------------------------------------------------------------
    void appendAudit(const QString &sessionId, const QString &event, const QString &payloadJson);
    QList<asr::AuditEvent> auditHistory(const QString &sessionId, int limit);

    // ---- the per-session speaker registry ----------------------------------
    //
    // Identity and metadata are one concern; the publish decision is another.
    // syncSpeakers() writes the first and never touches the second, because the
    // export it carries comes from the tier's automatic resolution and knows
    // nothing about a reviewer who already renamed someone by hand.
    void syncSpeakers(const QString &sessionId, const QList<reg::SessionSpeakerEntry> &entries);
    QList<reg::SessionSpeakerEntry> listSpeakers(const QString &sessionId);
    bool speaker(const QString &sessionId, const QString &speakerId,
                 reg::SessionSpeakerEntry *out);
    // A manual rename outranks any later sync, so the source is recorded with
    // the name rather than inferred from which call last ran.
    void setVerifiedName(const QString &sessionId, const QString &speakerId, const QString &name,
                         bool manual);
    void stageEvidence(const QString &sessionId, const QString &speakerId,
                       const QList<QPair<double, double>> &spans, const QString &sourceName);
    QList<QPair<double, double>> evidenceSpans(const QString &sessionId, const QString &speakerId);
    void updateSpeakerStatus(const QString &sessionId, const QString &speakerId,
                             const QString &status, const QString &publishedName,
                             const QString &publishError);

private:
    bool migrate(QString *error);
    QString audioPath(const QString &sessionId) const;
    // The sample rate and channel count a session was created with, for turning
    // a time range into a byte offset.  Cached read; the DB is the source.
    bool audioFormat(const QString &sessionId, quint32 *sampleRate, quint32 *channels,
                     qint64 *samples);

    mutable QMutex m_mutex;
    QSqlDatabase m_db;
    QString m_dir;
    QString m_connectionName;
    bool m_enabled = false;
};

#endif // SESSIONSTORE_H
