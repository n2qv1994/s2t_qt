// The meeting's state, built here rather than asked for.
//
// This class exists because of the 2026-08-25 change of direction.  Until then
// the Server buffer relayed get_live_state to a Python adapter that owned the
// transcript; now the buffer talks to an inference tier (Riva or Triton) that
// answers per chunk and remembers nothing, so the thing the client reads -
// asr::SessionState, with its rows, phrases, speaker lanes and counters - has
// to be accumulated on this side.
//
// The shape it produces is not new: it is exactly what s2t-qt-client's
// TranscriptModel already knows how to render, down to DisplayRow.speaker being
// a bare integer string.  That was the constraint, not a choice - the client is
// deployed and its parsing rules are in
// s2t-qt-client/core/TranscriptModel.cpp.
//
// Threading: no locking of its own.  SessionBuffer owns one of these and guards
// it with the same mutex that guards the queue, because a state read and a
// forward completing are exactly the two things that must not interleave.
#ifndef LIVETRANSCRIPT_H
#define LIVETRANSCRIPT_H

#include "proto/AsrSession.h"

#include <QList>
#include <QString>

class LiveTranscript
{
public:
    void configure(const QString &title, quint32 sampleRate, quint32 channels,
                   double sourceTotalSec);

    // Folds one backend answer into the meeting.  Returns the new state
    // version, which is what the client uses to tell a changed transcript from
    // an unchanged one without diffing it.
    quint64 apply(const asr::PushAudioResponse &response);

    // The meeting is over: the provisional row is settled and `done` goes true.
    void markDone();

    // What get_live_state and get_review_state answer with.  `viewStart`/
    // `viewEnd` below zero mean "everything", which is what a live read wants.
    asr::StateResponse snapshot(const QString &sessionId, qint64 streamId,
                                double viewStartSec = -1.0, double viewEndSec = -1.0) const;

    // The flat, canonical form an editor works against.
    asr::CanonicalTranscript transcript() const;

    // Replaces every word inside [startSec, endSec) with `words`.  Returns
    // false when `baseRevision` is not the current one - a concurrent edit,
    // which the caller reports as ABORTED so the client can re-read and retry
    // rather than silently overwrite someone else's correction.
    bool applyEdit(quint64 baseRevision, double startSec, double endSec,
                   const QList<asr::Word> &words);

    // Moves every word and row from one diarization slot to another, and
    // optionally attaches a verified name.  An empty name clears it, which is
    // deliberate: proto3 cannot tell "unset" from "empty", and the client's
    // rename dialog uses blank to mean "forget the name".
    void renameSpeaker(const QString &from, const QString &to, const QString &verifiedName);

    quint64 revision() const { return m_revision; }
    quint64 version() const { return m_version; }
    double commitBoundarySec() const { return m_commitBoundarySec; }
    bool done() const { return m_done; }
    double sourceSeenSec() const { return m_sourceSeenSec; }
    double speechSeenSec() const { return m_speechSeenSec; }
    QString title() const { return m_title; }
    QList<QString> speakerIds() const;

private:
    // A word joins the open row when it is from the same speaker and close
    // enough in time; otherwise it starts a new one.  That is the whole
    // turn-detection rule, and it is deliberately simple - the tier already did
    // the diarization, so guessing again here would only disagree with it.
    // Replaces the time span the incoming words cover with those words.  This
    // is the normal path, because both asr_words and the correction's
    // merged_words are rolling windows the tier re-sends rather than deltas.
    void replaceSpan(const QList<asr::Word> &words, const QString &speaker, float speakerProb,
                     const QString &verifiedName);
    void appendWords(const QList<asr::Word> &words, const QString &speaker, float speakerProb,
                     const QString &verifiedName);
    void rebuildPhrases(asr::DisplayRow *row) const;
    void recount();

    QString m_title;
    quint32 m_sampleRate = 16000;
    quint32 m_channels = 1;

    QList<asr::DisplayRow> m_rows;
    // The moving edge: interim text that has not been committed by the tier.
    // Kept apart from m_rows so a re-decode replaces it wholesale instead of
    // leaving half a sentence behind.
    asr::DisplayRow m_provisional;
    bool m_haveProvisional = false;

    quint64 m_version = 0;
    quint64 m_revision = 0;
    double m_commitBoundarySec = 0.0;
    double m_sourceSeenSec = 0.0;
    double m_speechSeenSec = 0.0;
    double m_sourceTotalSec = 0.0;
    double m_startedAt = 0.0;
    double m_lastChunkMs = 0.0;
    bool m_done = false;

    quint32 m_nPhrases = 0;
    quint32 m_nLow = 0;
    int m_nextRowId = 1;

    // One sample per accepted chunk, for the amplitude strip under the
    // transcript.  Bounded: a three-hour meeting must not grow this without
    // limit, and the strip is drawn a few hundred pixels wide anyway.
    QList<float> m_ampTrace;
    double m_ampStepSec = 0.0;
};

#endif // LIVETRANSCRIPT_H
