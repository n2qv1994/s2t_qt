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

#include <QHash>
#include <QList>
#include <QString>

class LiveTranscript
{
public:
    void configure(const QString &title, quint32 sampleRate, quint32 channels,
                   double sourceTotalSec);

    // Puts a saved snapshot back.  Two callers, and both matter:
    //
    //   - a meeting picked back up after the server restarted.  Until
    //     2026-09-24 the transcript was simply gone at that point - only the
    //     audio was journalled - so a restart in the middle of a meeting lost
    //     every word spoken before it.
    //   - an archived meeting somebody edits.  The edit runs against a
    //     LiveTranscript built from the stored state and is saved straight
    //     back, which is what makes a meeting that has left RAM editable.
    void restore(const asr::StateResponse &state);

    // Everything the tier says is offset by this many seconds before it is
    // placed.  A resumed meeting opens a NEW stream on the tier, and that
    // stream counts from zero; without the offset the second half of a
    // meeting lands on top of the first and every timestamp - the one a
    // reviewer clicks to hear a sentence - points at the wrong audio.
    void setTimeOffset(double seconds) { m_timeOffsetSec = seconds; }
    double timeOffset() const { return m_timeOffsetSec; }

    // Folds one backend answer into the meeting.  Returns the new state
    // version, which is what the client uses to tell a changed transcript from
    // an unchanged one without diffing it.
    quint64 apply(const asr::PushAudioResponse &response);

    // How much audio the meeting has taken in, when there is no tier answer to
    // read it from.  Only mode=record_only needs this: nothing is inferred
    // there, so the counters the client draws its progress from would
    // otherwise stay at zero for the whole recording.
    void noteProgress(double sourceSeenSec, double speechSeenSec);

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
    //
    // The span is remembered as a human decision: no later correction from the
    // tier may write over it.  Without that, an edit made during a meeting is
    // reverted by the next correction window a few seconds later while the
    // audit log still records it as applied - measured 2026-09-24.
    bool applyEdit(quint64 baseRevision, double startSec, double endSec,
                   const QList<asr::Word> &words);

    // True when [startSec, endSec] is entirely inside what the tier has
    // committed.  Editing past the boundary is refused, because the words
    // there are still being rewritten by the correction pass and the edit
    // would be silently undone.  A finished meeting has no moving edge left,
    // so everything in it is editable.
    bool isEditable(double startSec, double endSec) const;

    // True for every spelling the pipeline uses to mean "nobody was verified".
    // The client has the same rule in TranscriptModel::isRealName() and remains
    // the authority on what gets *drawn*; this copy exists because the server
    // now has to decide something the client cannot: whether an incoming name
    // is worth overwriting a name it already has.
    static bool isPlaceholderName(const QString &name);

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
    // The worker behind both applyEdit() and replaceSpan().  The three speaker
    // arguments are what an operator edit does NOT have and a tier chunk does:
    // an edit only moves text around and must leave the diarization alone,
    // while a chunk carries the answer to "who is talking" and would otherwise
    // have it dropped on the floor here.
    // `respectManual` is what tells the two apart at the one place it matters:
    // a tier correction must leave a span an operator edited alone, while the
    // operator's own edit is allowed to rewrite their previous one.
    bool spliceWords(quint64 baseRevision, double startSec, double endSec,
                     const QList<asr::Word> &words, const QString &speaker, float speakerProb,
                     const QString &verifiedName, bool respectManual = false);
public:
    // The stretches of audio this speaker is the one talking in, longest
    // first, stopping once `maxSec` of speech has been collected.
    //
    // This is the evidence behind publishing a voice to the shared CAM++
    // database, so it is deliberately built out of the words themselves: a
    // span is only as wide as words assigned to that speaker make it, and a
    // pause longer than a turn gap splits it rather than swallowing whoever
    // spoke in between.  Handing CAM++ a span with two voices in it produces a
    // blended embedding, which then mis-names people in later meetings and
    // does so silently.
    QList<QPair<double, double>> speakerSpans(const QString &speaker, double maxSec) const;

private:
    // `opensSentence` is the answer to "does a capital belong on the first
    // word of this row": true only when the row before it closed a sentence.
    // A row break is a pause, not a full stop - splitting on silence in the
    // middle of a sentence and capitalising what follows is how "trong khu
    // vực. Tăng đi" became "trong khu vực Tăng đi".
    void rebuildPhrases(asr::DisplayRow *row, bool opensSentence) const;
    void rebuildPhrasesAt(int index);
    bool rowOpensSentence(int index) const;
    // Folds one rolling window of streaming words into the provisional lane,
    // with the same replace-the-span rule the official lane uses.
    void mergePending(const QList<asr::Word> &words, const QString &speaker, float speakerProb,
                      const QString &verifiedName);
    // The moving edge, rebuilt from m_pending whenever it changes.
    void rebuildProvisional();
    // Words the tier has re-decided are dropped from the edge: the correction
    // owns them now, and keeping both copies is what puts "Hôm Hôm nay" on
    // screen.
    void dropPendingInSpan(double startSec, double endSec);
    // And the reverse: a correction window reaches a little past the boundary
    // it reports, so its last words are moved back to the edge until the
    // boundary catches up with them.
    void holdBackUncommitted();
    bool overlapsManualEdit(double startSec, double endSec) const;
    asr::PushAudioResponse shifted(const asr::PushAudioResponse &response) const;
    void recount();

    // Who was speaking when, accumulated from the per-subframe diarization the
    // tier sends with every chunk.  This is the only source that can place a
    // word correctly: PushAudioResponse.speaker is a single value describing
    // the chunk that has just been decoded, while the words in that chunk were
    // spoken seconds earlier and may belong to the other person entirely.
    struct DiarTurn
    {
        double startSec = 0.0;
        double endSec = 0.0;
        QString speaker;
    };
    void foldDiarization(const asr::Diarization &diarization);
    // The slot that covers most of [startSec, endSec], or empty when the
    // diarization has nothing to say about that stretch.
    QString speakerAt(double startSec, double endSec) const;

    // Consecutive subframes of the same speaker are merged, so this is bounded
    // by how often the speakers change hands rather than by the length of the
    // meeting: a two-minute interview folds ~1900 subframes into a few dozen.
    QList<DiarTurn> m_turns;

    QString m_title;
    quint32 m_sampleRate = 16000;
    quint32 m_channels = 1;

    // The official transcript: words the correction pass has settled, plus
    // whatever an operator edited by hand.  Streaming words do NOT come here -
    // see m_pending.
    QList<asr::DisplayRow> m_rows;
    // The moving edge: interim text that has not been committed by the tier.
    // Kept apart from m_rows so a re-decode replaces it wholesale instead of
    // leaving half a sentence behind.
    asr::DisplayRow m_provisional;
    bool m_haveProvisional = false;

    // Streaming words past the commit boundary, and the rows drawn from them.
    //
    // This lane exists because the tier re-decides everything inside its
    // correction window: a word that has arrived from `asr_words` but has not
    // been through a correction yet is a guess, and putting guesses in the
    // official transcript is what produced "Hôm Hôm nay" at every window edge
    // (62 of 63 live polls carried official words past the boundary, measured
    // 2026-09-24).  The design the pipeline team wrote down is explicit: past
    // commit_boundary_sec there is only the provisional lane, and the official
    // transcript takes words from the correction alone.
    QList<asr::Word> m_pending;
    QList<asr::DisplayRow> m_pendingRows;
    QString m_pendingName;
    float m_pendingProb = 0.0f;

    // The name the tier has verified for each diarization slot.
    //
    // It has to be kept here because of the two lanes: the correction pass is
    // what puts a word into the official transcript, and a correction carries
    // no speaker and no name - only the streaming chunk does.  Without this
    // map the whole speaker-verification feature stops at the edge: a meeting
    // started with expected_speakers matches CAM++, the tier answers with the
    // person's real name on every chunk, and the transcript still shows every
    // row as unnamed.
    //
    // Used ONLY for a word being placed for the first time.  A word that
    // already has a name keeps it, which is the same freeze rule that stops
    // the tier flipping between two candidates from relabelling history.
    QHash<QString, QString> m_nameBySlot;

    // Spans an operator edited by hand.  Not serialized: a manual edit is
    // written straight to the store, and the protection only has to outlive
    // the correction windows still in flight.
    QList<QPair<double, double>> m_manualSpans;

    // Added to every timestamp the tier reports.  Non-zero only for a meeting
    // resumed after a restart; see setTimeOffset().
    double m_timeOffsetSec = 0.0;

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
