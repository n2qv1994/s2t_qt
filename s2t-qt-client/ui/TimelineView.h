// The main timeline: waveform, time ruler, and one lane per speaker with
// every word drawn at its own timestamp.
//
// Words are positioned by time, not by flow, because the whole point of this
// view is that a word sits directly under the audio it came from.  A light
// anti-overlap nudge keeps neighbouring labels readable, and each word's x is
// then locked to its slot so later refinements do not make the line shuffle.
//
// Follow mode eases toward the target instead of teleporting: ASR releases
// text in bursts, and jumping the viewport on every burst makes the waveform
// look like it is stuttering.
#ifndef TIMELINEVIEW_H
#define TIMELINEVIEW_H

#include "core/TranscriptModel.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QList>
#include <QRectF>

class TimelineView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class FollowTarget { Audio, Text };

    explicit TimelineView(QWidget *parent = nullptr);

    void setModel(TranscriptModel *model);
    void refresh();
    void resetForSession();

    bool followEnabled() const { return m_follow; }
    void setFollowEnabled(bool enabled);
    FollowTarget followTarget() const { return m_followTarget; }
    void setFollowTarget(FollowTarget target);
    void jumpToLatestText();

    int visibleWordCount() const { return m_visibleWords; }
    int totalWordCount() const { return m_totalWords; }
    int laneCount() const { return m_model ? m_model->lanes().size() : 0; }

signals:
    // A word was clicked; the caller opens the sentence editor over it.
    void wordActivated(double startSec, double endSec, const QPoint &globalPos);
    void followChanged(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct PaintedWord
    {
        QRectF rect;
        double startSec = 0.0;
        double endSec = 0.0;
    };

    void updateScrollRanges();
    void applyFollow();
    double contentWidthPx() const;
    double focusSeconds() const;
    void paintWaveform(QPainter &painter, const QRect &band, double scrollX) const;
    void paintRuler(QPainter &painter, const QRect &band, double scrollX) const;
    void paintLane(QPainter &painter, const Lane &lane, const QRect &band, double scrollX,
                   double visibleStartSec, double visibleEndSec);
    double lockedX(const QString &slotKey, double baseX, double minX, double width);
    // How wide the speaker column has to be for the names actually in this
    // session.  A constant fitted one font stack: 128 px held "Nguyễn Văn An"
    // under MinGW and elided it to "Nguyễn Vă..." on RHEL, where the same
    // string measures wider - and two people whose names share a prefix then
    // become indistinguishable in the one column that identifies them.
    int gutterWidth() const;
    void pruneWordPositions();
    void noteUserInteraction();

    TranscriptModel *m_model = nullptr;
    bool m_follow = true;
    FollowTarget m_followTarget = FollowTarget::Audio;
    QHash<QString, double> m_wordX;
    QList<PaintedWord> m_painted;
    int m_visibleWords = 0;
    int m_totalWords = 0;
    double m_activeCentreSec = -1.0;
    bool m_programmaticScroll = false;
};

#endif // TIMELINEVIEW_H
