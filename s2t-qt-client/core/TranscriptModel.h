// Turns the server's SessionState into the lane/word layout the timeline
// draws, and keeps it stable while the pipeline keeps revising it.
//
// This is the part of the old browser UI with real behaviour in it, and the
// rules are load bearing:
//
//  * get_live_state returns a bounded tail, not the meeting.  A local cache
//    keeps older rows mounted instead of destroying and rebuilding them every
//    time they fall out of that tail.
//  * The same logical word can come back with slightly shifted timestamps
//    after a correction, so words are clustered into slots by centre and
//    duration rather than keyed on exact times - otherwise one word renders
//    twice, on two lanes.
//  * A word's lane is frozen once it has been seen in a committed row, with
//    exactly one allowed upgrade from a diarization slot to a verified
//    identity.  Without that, words visibly jump between speakers every time
//    diarization refines its guess.
//  * Displayed text is frozen per slot too.  Provisional refinements are
//    ignored (they flicker); a newer transcript revision is authoritative and
//    is allowed through, as is a punctuation-only improvement.
#ifndef TRANSCRIPTMODEL_H
#define TRANSCRIPTMODEL_H

#include "proto/AsrSession.h"

#include <QHash>
#include <QList>
#include <QString>

struct WordItem
{
    QString text;
    float conf = 0.0f;
    double startSec = 0.0;
    double endSec = 0.0;
    bool provisional = false;
    bool low = false;
    QString slotKey;
    QString laneKey;
};

struct Lane
{
    QString key;   // "name:<verified>" once identified, else "sid:<n>"
    QString label; // what the gutter shows
    int colorIndex = 0;
    bool provisional = false;
    QList<WordItem> words;
};

class TranscriptModel
{
public:
    // Anything below this confidence is not drawn at all; anything below the
    // warn threshold is drawn as low-confidence.  Both come from the server
    // snapshot in the original UI (token_low_threshold / conf_threshold_pct).
    static constexpr double kDefaultDropThreshold = 0.75;

    void resetForSession(const QString &sessionKey);

    // Live tail: reconciled into the local row cache.
    void applyLiveState(const asr::StateResponse &response);
    // Review viewport: authoritative for the rows it returns, no caching.
    void applyReviewState(const asr::StateResponse &response);
    // Post-edit state that came back on a ReviewEditResponse.
    void applyEditedState(const asr::SessionState &state, quint64 revision);

    // When true, low-confidence words are drawn instead of dropped.  Off by
    // default, which is the behaviour the deployed UI has.
    void setShowLowConfidence(bool show);
    bool showLowConfidence() const { return m_showLowConfidence; }

    const QList<Lane> &lanes() const { return m_lanes; }
    const asr::SessionState &state() const { return m_state; }
    const QList<asr::DisplayRow> &rows() const { return m_rows; }
    const QList<asr::DisplayRow> &provisionalRows() const { return m_state.provisionalRows; }

    QString sessionId() const { return m_sessionId; }
    quint64 revision() const { return m_revision; }
    bool isFinal() const { return m_final; }
    double commitBoundarySec() const { return m_commitBoundarySec; }
    bool isReview() const { return m_review; }

    double latestTextEndSec() const { return m_latestTextEndSec; }
    double totalSec() const;
    double sourceSeenSec() const { return m_state.sourceSeenSec; }

    // Row whose time span overlaps [startSec, endSec] the most - what the
    // click-to-edit popup needs to know which sentence a word belongs to.
    // Returned by value: the popup outlives several polls, and a pointer into
    // the row list would dangle the moment the next one rebuilds it.
    bool rowCovering(double startSec, double endSec, asr::DisplayRow *out) const;

    // Every distinct raw speaker id present, for the merge dropdown.
    QStringList speakerIds() const;

    // True when `name` is a real identity rather than one of the placeholders
    // the pipeline uses for "nobody was verified" - the literal "unknown", a
    // question mark, or a bare "speaker_N".  Exposed because every pane that
    // shows a speaker has to agree on it: printing "unknown" as though it were
    // a person is the whole failure mode this guards.
    static bool isRealName(const QString &name);

    // Exposed for the timeline's own token extraction and for tests.
    static QList<WordItem> rowWordItems(const asr::DisplayRow &row);
    static QString normalizeForSlot(const QString &text);

private:
    void rebuild();
    void mergeLiveRows(const asr::SessionState &incoming);
    void assignSlotKeys(QList<WordItem> *words) const;

    struct LaneMeta
    {
        int index = 0;
        int speakerIndex = 0;
        QString label;
    };
    struct LaneLock
    {
        QString laneKey;
        bool committed = false;
    };
    struct DisplayLock
    {
        WordItem snapshot;
        bool committed = false;
        quint64 revision = 0;
    };

    QString m_sessionId;
    asr::SessionState m_state;
    QList<asr::DisplayRow> m_rows; // live cache, or the review viewport
    QList<Lane> m_lanes;
    quint64 m_revision = 0;
    bool m_final = false;
    bool m_review = false;
    double m_commitBoundarySec = -1.0;
    double m_latestTextEndSec = 0.0;
    bool m_showLowConfidence = false;

    // Live row cache, keyed by row_id (or speaker+start when the server has
    // not assigned one).
    QHash<QString, asr::DisplayRow> m_rowCache;
    QString m_rowCacheSession;

    QHash<QString, LaneMeta> m_laneMeta;
    int m_laneOrderCounter = 0;
    QHash<QString, LaneLock> m_laneLocks;
    QHash<QString, DisplayLock> m_displayLocks;
};

#endif // TRANSCRIPTMODEL_H
