#include "TimelineView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>
#include <iterator>
#include <limits>

namespace {

// Vietnamese speech at a normal rate places consecutive words only tens of
// pixels apart; widening the whole shared time axis is what keeps a word
// under its own audio instead of shifting individual labels off their
// timestamps to make room.
const double kPxPerSec = 520.0;
const int kGutterWidth = 128;
const int kWaveHeight = 118;
const int kRulerHeight = 30;
const int kLaneHeight = 96;
const double kFollowEase = 0.35;
const double kFollowMaxStepPx = 180.0;
const double kOverscanSec = 2.0;
const int kWordPositionCacheMax = 8000;

const QColor kLaneColors[4] = {
    QColor(0x2e, 0xa6, 0x3e), QColor(0x1b, 0x7c, 0xf0),
    QColor(0xe6, 0x4e, 0x4e), QColor(0x8b, 0x61, 0xd8),
};

QString formatClock(double seconds)
{
    const double value = qMax(0.0, seconds);
    const int minutes = int(value) / 60;
    const int secs = int(value) % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString formatPrecise(double seconds)
{
    const double value = qMax(0.0, seconds);
    const int minutes = int(value) / 60;
    const double rest = value - minutes * 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(rest, 5, 'f', 2, QLatin1Char('0'));
}

} // namespace

TimelineView::TimelineView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setAutoFillBackground(true);
    QPalette palette = viewport()->palette();
    palette.setColor(QPalette::Window, QColor(0xf8, 0xf8, 0xf8));
    viewport()->setPalette(palette);
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(24);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!m_programmaticScroll)
            noteUserInteraction();
        viewport()->update();
    });
}

void TimelineView::setModel(TranscriptModel *model)
{
    m_model = model;
    resetForSession();
}

void TimelineView::resetForSession()
{
    m_wordX.clear();
    m_painted.clear();
    m_activeCentreSec = -1.0;
    m_programmaticScroll = true;
    horizontalScrollBar()->setValue(0);
    verticalScrollBar()->setValue(0);
    m_programmaticScroll = false;
    viewport()->update();
}

void TimelineView::setFollowEnabled(bool enabled)
{
    if (m_follow == enabled)
        return;
    m_follow = enabled;
    emit followChanged(m_follow);
    if (m_follow)
        applyFollow();
    viewport()->update();
}

void TimelineView::setFollowTarget(FollowTarget target)
{
    m_followTarget = target;
    if (m_follow)
        applyFollow();
}

void TimelineView::jumpToLatestText()
{
    setFollowTarget(FollowTarget::Text);
    setFollowEnabled(true);
    // A jump is an explicit request, so it goes straight there rather than
    // easing - the easing exists to smooth continuous following, not this.
    if (!m_model)
        return;
    const double target = m_model->latestTextEndSec() > 0.0 ? m_model->latestTextEndSec()
                                                            : m_model->sourceSeenSec();
    const double trackWidth = qMax(120, viewport()->width() - kGutterWidth);
    const double desired = target * kPxPerSec - trackWidth * 0.72;
    m_programmaticScroll = true;
    horizontalScrollBar()->setValue(int(qBound(0.0, desired, double(horizontalScrollBar()->maximum()))));
    m_programmaticScroll = false;
    viewport()->update();
}

void TimelineView::noteUserInteraction()
{
    // Any manual scroll means the operator is reading history.  Only the LIVE
    // button turns following back on - re-enabling it by direction would rip
    // the viewport away mid-read.
    if (m_follow)
        setFollowEnabled(false);
}

double TimelineView::contentWidthPx() const
{
    if (!m_model)
        return 800.0;
    return qMax(800.0, std::ceil(m_model->totalSec() * kPxPerSec));
}

void TimelineView::updateScrollRanges()
{
    const int trackWidth = qMax(120, viewport()->width() - kGutterWidth);
    const int maxH = qMax(0, int(contentWidthPx()) - trackWidth);
    horizontalScrollBar()->setRange(0, maxH);
    horizontalScrollBar()->setPageStep(trackWidth);

    const int laneAreaHeight = qMax(0, viewport()->height() - kWaveHeight - kRulerHeight);
    const int contentHeight = laneCount() * kLaneHeight;
    verticalScrollBar()->setRange(0, qMax(0, contentHeight - laneAreaHeight));
    verticalScrollBar()->setPageStep(qMax(1, laneAreaHeight));
}

double TimelineView::focusSeconds() const
{
    if (!m_model)
        return 0.0;
    if (m_followTarget == FollowTarget::Audio)
        return m_model->sourceSeenSec();
    const double text = m_model->latestTextEndSec();
    return text > 0.0 ? text : m_model->sourceSeenSec();
}

void TimelineView::applyFollow()
{
    if (!m_follow || !m_model)
        return;
    const double trackWidth = qMax(120, viewport()->width() - kGutterWidth);
    const double target = qBound(0.0, focusSeconds() * kPxPerSec, contentWidthPx());
    const double desired = qBound(0.0, target - trackWidth * 0.72,
                                  double(horizontalScrollBar()->maximum()));
    const double current = horizontalScrollBar()->value();
    const double delta = desired - current;
    if (std::abs(delta) <= 2.0)
        return;
    // Rate limited so a burst of newly committed text pulls the viewport
    // along smoothly instead of snapping it.
    const double step = (delta < 0 ? -1.0 : 1.0)
        * qMin(std::abs(delta), qMax(24.0, qMin(kFollowMaxStepPx, std::abs(delta) * kFollowEase)));
    m_programmaticScroll = true;
    horizontalScrollBar()->setValue(int(current + step));
    m_programmaticScroll = false;
}

void TimelineView::refresh()
{
    pruneWordPositions();
    updateScrollRanges();
    applyFollow();
    viewport()->update();
}

void TimelineView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRanges();
}

void TimelineView::wheelEvent(QWheelEvent *event)
{
    noteUserInteraction();
    QAbstractScrollArea::wheelEvent(event);
}

void TimelineView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        noteUserInteraction();
        break;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TimelineView::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    for (const PaintedWord &word : m_painted) {
        if (word.rect.contains(pos)) {
            emit wordActivated(word.startSec, word.endSec,
                               viewport()->mapToGlobal(pos.toPoint()));
            return;
        }
    }
    noteUserInteraction();
    QAbstractScrollArea::mousePressEvent(event);
}

double TimelineView::lockedX(const QString &slotKey, double baseX, double minX, double width)
{
    Q_UNUSED(width)
    const auto existing = m_wordX.constFind(slotKey);
    if (existing != m_wordX.constEnd())
        return *existing;
    // First placement wins for the life of the word.  Recomputing it later
    // would make the whole line shuffle sideways every time a neighbouring
    // word's text changed length.
    const double x = qMax(baseX, minX);
    m_wordX.insert(slotKey, x);
    return x;
}

void TimelineView::pruneWordPositions()
{
    if (!m_model || m_wordX.size() <= kWordPositionCacheMax)
        return;
    // Drop positions for words that have scrolled out of the model entirely,
    // rather than clearing the map - clearing would re-place every word still
    // on screen and visibly shift the line.
    QSet<QString> live;
    for (const Lane &lane : m_model->lanes()) {
        for (const WordItem &word : lane.words)
            live.insert(word.slotKey);
    }
    for (auto it = m_wordX.begin(); it != m_wordX.end();)
        it = live.contains(it.key()) ? std::next(it) : m_wordX.erase(it);
}

void TimelineView::paintWaveform(QPainter &painter, const QRect &band, double scrollX) const
{
    painter.fillRect(band, QColor(0xf3, 0xf3, 0xf3));
    const double mid = band.top() + band.height() * 0.5;
    painter.setPen(QColor(0xb0, 0xb0, 0xb0));
    painter.drawLine(QPointF(band.left(), mid), QPointF(band.right(), mid));

    if (!m_model)
        return;
    const asr::SessionState &state = m_model->state();
    const QList<float> &trace = state.ampTrace;
    if (trace.isEmpty())
        return;

    double stepSec = qMax(0.001, state.ampTraceStepSec > 0 ? state.ampTraceStepSec : 0.02);
    const double seen = qMax(0.0, state.sourceSeenSec);
    // A persisted session can carry an older, floor-rounded step.  A full
    // trace cannot legitimately end well before source_seen_sec, so recover
    // its axis from the known duration instead of drawing a blank waveform
    // right where the streaming head is.
    if (seen > 0.0 && trace.size() * stepSec < seen * 0.98)
        stepSec = seen / double(qMax(1, trace.size()));

    const double amplitude = band.height() * 0.42;
    QPainterPath path;
    bool started = false;
    QList<QPointF> lower;
    for (int i = 0; i < trace.size(); ++i) {
        const double x = band.left() + double(i) * stepSec * kPxPerSec - scrollX;
        if (x < band.left() - 40)
            continue;
        if (x > band.right() + 40)
            break;
        const double value = qBound(0.0, double(trace.at(i)), 1.0) * amplitude;
        const QPointF top(x, mid - value);
        if (!started) {
            path.moveTo(top);
            started = true;
        } else {
            path.lineTo(top);
        }
        lower.prepend(QPointF(x, mid + value));
    }
    if (!started)
        return;
    for (const QPointF &point : lower)
        path.lineTo(point);
    path.closeSubpath();
    painter.fillPath(path, QColor(95, 95, 95, 174));
}

void TimelineView::paintRuler(QPainter &painter, const QRect &band, double scrollX) const
{
    painter.fillRect(band, QColor(0xfa, 0xfa, 0xfa));
    painter.setPen(QColor(0xd7, 0xd7, 0xd7));
    painter.drawLine(band.bottomLeft(), band.bottomRight());

    QFont font = painter.font();
    font.setPointSizeF(8.5);
    font.setBold(true);
    painter.setFont(font);

    const int firstTick = int(std::floor(scrollX / kPxPerSec));
    const int lastTick = int(std::ceil((scrollX + band.width()) / kPxPerSec));
    for (int tick = qMax(0, firstTick); tick <= lastTick; ++tick) {
        const double x = band.left() + tick * kPxPerSec - scrollX;
        painter.setPen(QColor(0xd7, 0xd7, 0xd7));
        painter.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
        painter.setPen(QColor(0x55, 0x55, 0x55));
        painter.drawText(QPointF(x + 6, band.center().y() + 4), formatClock(tick));
    }
}

void TimelineView::paintLane(QPainter &painter, const Lane &lane, const QRect &band, double scrollX,
                             double visibleStartSec, double visibleEndSec)
{
    painter.fillRect(band, QColor(0xff, 0xff, 0xff));
    painter.setPen(QColor(0xe4, 0xe4, 0xe4));
    painter.drawLine(band.bottomLeft(), band.bottomRight());

    // Baseline the words sit on, so an empty stretch still reads as "this
    // speaker's row continues here" rather than as a gap in the layout.
    painter.setPen(QPen(QColor(120, 120, 120, 90), 1, Qt::DashLine));
    const double baseline = band.top() + band.height() * 0.62;
    painter.drawLine(QPointF(band.left(), baseline), QPointF(band.right(), baseline));

    QFont wordFont = painter.font();
    wordFont.setPointSizeF(11.5);
    wordFont.setBold(true);
    const QFontMetricsF metrics(wordFont);

    double lastRight = -1e9;
    const double clipStart = visibleStartSec - kOverscanSec;
    const double clipEnd = visibleEndSec + kOverscanSec;

    for (const WordItem &word : lane.words) {
        ++m_totalWords;
        if (word.endSec < clipStart || word.startSec > clipEnd) {
            // Still advance the anti-overlap cursor for words scrolled past,
            // or the first visible word would be placed as if the line were
            // empty and would jump when the operator scrolls back.
            const double baseX = word.startSec * kPxPerSec;
            const double width = metrics.horizontalAdvance(word.text) + 18.0;
            lastRight = qMax(lastRight, lockedX(word.slotKey, baseX, lastRight + 6.0, width) + width);
            continue;
        }
        ++m_visibleWords;
        const double baseX = word.startSec * kPxPerSec;
        const double width = qMax(26.0, metrics.horizontalAdvance(word.text) + 18.0);
        const double x = lockedX(word.slotKey, baseX, lastRight + 6.0, width);
        lastRight = x + width;

        const double screenX = band.left() + x - scrollX;
        if (screenX + width < band.left() || screenX > band.right())
            continue;

        const double height = metrics.height() + 8.0;
        const QRectF rect(screenX, band.top() + 10.0, width, height);

        QColor background = QColor(255, 255, 255, 235);
        QColor border(0, 0, 0, 40);
        QColor text(0x1f, 0x1f, 0x1f);
        if (word.low) {
            background = QColor(255, 78, 78, 215);
            border = QColor(0xb3, 0x33, 0x33);
            text = QColor(0x25, 0x00, 0x00);
        }
        painter.setPen(QPen(border, 1, word.provisional ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(background);
        painter.setOpacity(word.provisional ? 0.82 : 1.0);
        painter.drawRoundedRect(rect, 5, 5);
        painter.setOpacity(1.0);

        const bool active = m_activeCentreSec >= word.startSec - 0.03
            && m_activeCentreSec <= word.endSec + 0.03;
        if (active) {
            painter.setPen(QPen(QColor(20, 90, 220), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect, 5, 5);
        }

        painter.setFont(wordFont);
        painter.setPen(text);
        painter.drawText(rect, Qt::AlignCenter, word.text);

        // The percentage is a low-confidence warning, not a second line under
        // every streamed word.
        if (word.low) {
            QFont small = wordFont;
            small.setPointSizeF(7.5);
            painter.setFont(small);
            painter.setPen(QColor(0x2a, 0x00, 0x00));
            painter.drawText(QRectF(rect.left(), rect.bottom(), rect.width(), 12),
                             Qt::AlignCenter,
                             QStringLiteral("%1%").arg(int(std::lround(word.conf * 100.0))));
        }

        PaintedWord painted;
        painted.rect = rect;
        painted.startSec = word.startSec;
        painted.endSec = word.endSec;
        m_painted.append(painted);
    }
}

void TimelineView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    m_painted.clear();
    m_visibleWords = 0;
    m_totalWords = 0;

    const int width = viewport()->width();
    const int height = viewport()->height();
    const double scrollX = horizontalScrollBar()->value();
    const int scrollY = verticalScrollBar()->value();
    const QRect trackRect(kGutterWidth, 0, qMax(0, width - kGutterWidth), height);

    const double visibleStartSec = scrollX / kPxPerSec;
    const double visibleEndSec = (scrollX + trackRect.width()) / kPxPerSec;

    if (m_model) {
        // The active word is whichever committed token the play head is
        // nearest, which is what the guide line and the outline follow.
        const double focus = focusSeconds();
        double bestDistance = std::numeric_limits<double>::max();
        m_activeCentreSec = -1.0;
        for (const Lane &lane : m_model->lanes()) {
            for (const WordItem &word : lane.words) {
                if (word.provisional)
                    continue;
                const double centre = (word.startSec + word.endSec) * 0.5;
                const double distance = std::abs(focus - centre);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    m_activeCentreSec = centre;
                }
            }
        }
    }

    painter.save();
    painter.setClipRect(trackRect);
    paintWaveform(painter, QRect(trackRect.left(), 0, trackRect.width(), kWaveHeight), scrollX);
    paintRuler(painter, QRect(trackRect.left(), kWaveHeight, trackRect.width(), kRulerHeight),
               scrollX);

    const int laneTop = kWaveHeight + kRulerHeight;
    const QRect laneArea(trackRect.left(), laneTop, trackRect.width(), qMax(0, height - laneTop));
    painter.setClipRect(laneArea);
    if (m_model) {
        const QList<Lane> &lanes = m_model->lanes();
        for (int i = 0; i < lanes.size(); ++i) {
            const int y = laneTop + i * kLaneHeight - scrollY;
            if (y + kLaneHeight < laneTop || y > height)
                continue;
            paintLane(painter, lanes.at(i), QRect(laneArea.left(), y, laneArea.width(), kLaneHeight),
                      scrollX, visibleStartSec, visibleEndSec);
        }
    }
    painter.restore();

    // Guides drawn over the track, under the gutter.
    painter.save();
    painter.setClipRect(trackRect);
    if (m_model) {
        const double audioX = trackRect.left() + m_model->sourceSeenSec() * kPxPerSec - scrollX;
        painter.setPen(QPen(QColor(0, 0, 0, 224), 2));
        painter.drawLine(QPointF(audioX, 0), QPointF(audioX, height));
        if (m_activeCentreSec >= 0.0) {
            const double textX = trackRect.left() + m_activeCentreSec * kPxPerSec - scrollX;
            painter.setPen(QPen(QColor(255, 36, 36, 250), 3));
            painter.drawLine(QPointF(textX, 0), QPointF(textX, height));
        }
    }
    painter.restore();

    // Gutter last: it is pinned and must cover whatever scrolled under it.
    painter.fillRect(QRect(0, 0, kGutterWidth, height), QColor(0xf7, 0xf7, 0xf7));
    painter.setPen(QColor(0xd0, 0xd0, 0xd0));
    painter.drawLine(kGutterWidth - 1, 0, kGutterWidth - 1, height);

    QFont laneFont = painter.font();
    laneFont.setPointSizeF(10.5);
    laneFont.setBold(true);
    painter.setFont(laneFont);
    if (m_model) {
        const QList<Lane> &lanes = m_model->lanes();
        for (int i = 0; i < lanes.size(); ++i) {
            const int y = laneTop + i * kLaneHeight - scrollY;
            if (y + kLaneHeight < laneTop || y > height)
                continue;
            const Lane &lane = lanes.at(i);
            const QColor color = kLaneColors[lane.colorIndex % 4];
            painter.setPen(color);
            painter.drawText(QRect(10, y, kGutterWidth - 16, kLaneHeight),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("● ") + lane.label);
        }
        if (lanes.isEmpty()) {
            painter.setPen(QColor(0x8a, 0x8a, 0x8a));
            painter.drawText(QRect(0, laneTop, width, 60), Qt::AlignCenter,
                             QStringLiteral("— chưa có dữ liệu —"));
        }
    }

    // Time under the play head, in the gutter, where it does not collide with
    // the words themselves.
    if (m_model) {
        painter.setPen(QColor(0x33, 0x33, 0x33));
        QFont small = laneFont;
        small.setPointSizeF(9.0);
        small.setBold(false);
        painter.setFont(small);
        painter.drawText(QRect(8, kWaveHeight, kGutterWidth - 12, kRulerHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         formatPrecise(m_model->sourceSeenSec()));
    }
}
