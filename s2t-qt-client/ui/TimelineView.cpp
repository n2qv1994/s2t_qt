#include "TimelineView.h"

#include "Theme.h"

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
// Floor for the speaker column, not its width - see TimelineView::gutterWidth().
const int kGutterMinWidth = 128;
// Dot, the gap after it, and the padding either side of the label.
const int kGutterPadding = 34;
// The waveform is context and the lanes are the content, so the waveform gets
// the smaller share.  At 118 px it took a seventh of the window to say
// "somebody is talking", and the two rows of transcript underneath it - the
// reason the window exists - had to share what was left.
const int kWaveHeight = 76;
const int kRulerHeight = 26;
// One lane was 96 px tall to hold a chip about 30 px tall.  62 keeps the rows
// separable without spending half the window on air; the chips are centred in
// it rather than pinned to the top.
const int kLaneHeight = 62;
const double kFollowEase = 0.35;
const double kFollowMaxStepPx = 180.0;
const double kOverscanSec = 2.0;
const int kWordPositionCacheMax = 8000;

// Type sizes are stated as a multiple of whatever the desktop font is rather
// than in points.  The same Vietnamese word measures about 90 px wider under
// the RHEL font stack than under MinGW, so a fixed 11.5 pt word chip is
// legible on one kit and cramped on the other.
QFont scaled(const QFont &base, double factor, bool bold)
{
    QFont font = base;
    const double point = base.pointSizeF() > 0 ? base.pointSizeF() : 9.0;
    font.setPointSizeF(qMax(6.0, point * factor));
    font.setBold(bold);
    return font;
}

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
    palette.setColor(QPalette::Window, theme::color(theme::Role::Surface));
    viewport()->setPalette(palette);
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(24);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        if (!m_programmaticScroll)
            noteUserInteraction();
        viewport()->update();
    });
}

int TimelineView::gutterWidth() const
{
    int widest = 0;
    if (m_model) {
        const QFontMetrics metrics(scaled(font(), 1.1, true));
        for (const Lane &lane : m_model->lanes())
            widest = qMax(widest, metrics.horizontalAdvance(lane.label));
    }
    // Clamped to a third of the view: a pasted-in name of unreasonable length
    // must not squeeze the transcript itself off the screen.  Past that point
    // the label still elides, which is the right answer for one outlier.
    return qBound(kGutterMinWidth, widest + kGutterPadding,
                  qMax(kGutterMinWidth, viewport()->width() / 3));
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
    const double trackWidth = qMax(120, viewport()->width() - gutterWidth());
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
    const int trackWidth = qMax(120, viewport()->width() - gutterWidth());
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
    const double trackWidth = qMax(120, viewport()->width() - gutterWidth());
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
    painter.fillRect(band, theme::color(theme::Role::SurfaceSunken));
    const double mid = band.top() + band.height() * 0.5;
    painter.setPen(theme::color(theme::Role::Border));
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

    const double amplitude = band.height() * 0.38;
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
    // Filled in the accent rather than in grey: the waveform is the one thing
    // on this band that carries information, and a neutral fill made it read
    // as chrome.
    QColor fill = theme::color(theme::Role::Accent);
    fill.setAlpha(theme::isDark() ? 110 : 70);
    painter.fillPath(path, fill);
    QColor edge = theme::color(theme::Role::Accent);
    edge.setAlpha(170);
    painter.strokePath(path, QPen(edge, 1.0));
}

void TimelineView::paintRuler(QPainter &painter, const QRect &band, double scrollX) const
{
    painter.fillRect(band, theme::color(theme::Role::SurfaceAlt));
    painter.setPen(theme::color(theme::Role::Border));
    painter.drawLine(band.bottomLeft(), band.bottomRight());

    painter.setFont(scaled(painter.font(), 0.92, true));

    const int firstTick = int(std::floor(scrollX / kPxPerSec));
    const int lastTick = int(std::ceil((scrollX + band.width()) / kPxPerSec));
    const QColor major = theme::color(theme::Role::BorderStrong);
    QColor minor = theme::color(theme::Role::Border);
    minor.setAlpha(140);
    for (int tick = qMax(0, firstTick); tick <= lastTick; ++tick) {
        const double x = band.left() + tick * kPxPerSec - scrollX;
        // Quarter-second marks, so a two-second gap between words can be
        // judged by eye instead of counted off the labels.
        painter.setPen(minor);
        for (int sub = 1; sub < 4; ++sub) {
            const double subX = x + sub * kPxPerSec / 4.0;
            painter.drawLine(QPointF(subX, band.bottom() - 5), QPointF(subX, band.bottom()));
        }
        painter.setPen(major);
        painter.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
        painter.setPen(theme::color(theme::Role::TextMuted));
        painter.drawText(QPointF(x + 6, band.center().y() + 4), formatClock(tick));
    }
}

void TimelineView::paintLane(QPainter &painter, const Lane &lane, const QRect &band, double scrollX,
                             double visibleStartSec, double visibleEndSec)
{
    // Alternating bands, so two speakers talking over each other stay visually
    // separate even where neither row has a word under the cursor.
    painter.fillRect(band, theme::color(lane.colorIndex % 2 == 0 ? theme::Role::Surface
                                                                 : theme::Role::SurfaceAlt));
    painter.setPen(theme::color(theme::Role::Border));
    painter.drawLine(band.bottomLeft(), band.bottomRight());

    // Baseline the words sit on, so an empty stretch still reads as "this
    // speaker's row continues here" rather than as a gap in the layout.
    QColor guide = theme::laneColor(lane.colorIndex);
    guide.setAlpha(70);
    painter.setPen(QPen(guide, 1, Qt::DashLine));
    const double baseline = band.top() + band.height() * 0.62;
    painter.drawLine(QPointF(band.left(), baseline), QPointF(band.right(), baseline));

    const QFont wordFont = scaled(painter.font(), 1.25, true);
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
        // Centred on the lane's baseline rather than pinned 10 px under its
        // top edge: the chip and the dashed guide then read as one row, and
        // the lane can be made shorter without the words drifting to its top.
        const QRectF rect(screenX, baseline - height * 0.72, width, height);

        // A low-confidence word used to be a solid red block with near-black
        // text on it, which is both hard to read and shouts louder than the
        // word itself.  Tinted fill plus a coloured border says the same thing
        // and leaves the word legible - which matters, because the point of
        // flagging it is that somebody has to read it and decide.
        QColor background = theme::color(theme::Role::Surface);
        QColor border = theme::color(theme::Role::Border);
        QColor text = theme::color(theme::Role::Text);
        if (word.low) {
            background = theme::color(theme::Role::DangerSoft);
            border = theme::color(theme::Role::Danger);
            text = theme::color(theme::Role::Danger);
        }
        painter.setPen(QPen(border, 1, word.provisional ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(background);
        painter.setOpacity(word.provisional ? 0.82 : 1.0);
        painter.drawRoundedRect(rect, 5, 5);
        painter.setOpacity(1.0);

        const bool active = m_activeCentreSec >= word.startSec - 0.03
            && m_activeCentreSec <= word.endSec + 0.03;
        if (active) {
            painter.setPen(QPen(theme::color(theme::Role::Accent), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect, 5, 5);
        }

        painter.setFont(wordFont);
        painter.setPen(text);
        painter.drawText(rect, Qt::AlignCenter, word.text);

        // The percentage is a low-confidence warning, not a second line under
        // every streamed word.
        if (word.low) {
            painter.setFont(scaled(wordFont, 0.65, false));
            painter.setPen(theme::color(theme::Role::Danger));
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
    const QRect trackRect(gutterWidth(), 0, qMax(0, width - gutterWidth()), height);

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
        // Below the last speaker there are no more lanes, and leaving that as
        // the same white as a lane made the window look like one empty row
        // running to the bottom of the screen.  Sinking it says where the
        // transcript ends.
        const int bottom = laneTop + lanes.size() * kLaneHeight - scrollY;
        if (!lanes.isEmpty() && bottom < height) {
            painter.fillRect(QRect(laneArea.left(), bottom, laneArea.width(), height - bottom),
                             theme::color(theme::Role::SurfaceSunken));
        }
    }
    painter.restore();

    // Guides drawn over the track, under the gutter.
    painter.save();
    painter.setClipRect(trackRect);
    if (m_model) {
        // Two heads, and they mean different things: where the audio has got
        // to, and which word is being looked at.  Giving them the same weight
        // in different colours made the pair read as one decoration, so the
        // audio head is now a quiet hairline and the cursor keeps the accent.
        const double audioX = trackRect.left() + m_model->sourceSeenSec() * kPxPerSec - scrollX;
        QColor head = theme::color(theme::Role::Text);
        head.setAlpha(150);
        painter.setPen(QPen(head, 1.5));
        painter.drawLine(QPointF(audioX, 0), QPointF(audioX, height));
        if (m_activeCentreSec >= 0.0) {
            const double textX = trackRect.left() + m_activeCentreSec * kPxPerSec - scrollX;
            painter.setPen(QPen(theme::color(theme::Role::Accent), 2.5));
            painter.drawLine(QPointF(textX, 0), QPointF(textX, height));
        }
    }
    painter.restore();

    // Gutter last: it is pinned and must cover whatever scrolled under it.
    painter.fillRect(QRect(0, 0, gutterWidth(), height), theme::color(theme::Role::SurfaceAlt));
    painter.setPen(theme::color(theme::Role::Border));
    painter.drawLine(gutterWidth() - 1, 0, gutterWidth() - 1, height);

    const QFont laneFont = scaled(painter.font(), 1.1, true);
    painter.setFont(laneFont);
    if (m_model) {
        const QList<Lane> &lanes = m_model->lanes();
        for (int i = 0; i < lanes.size(); ++i) {
            const int y = laneTop + i * kLaneHeight - scrollY;
            if (y + kLaneHeight < laneTop || y > height)
                continue;
            const Lane &lane = lanes.at(i);
            const QColor color = theme::laneColor(lane.colorIndex);
            // A drawn dot rather than "● " in the string: the bullet glyph is
            // missing from some of the fonts RHEL falls back to, and a missing
            // glyph box next to every speaker name is worse than no marker.
            const QRectF dot(10, y + kLaneHeight / 2.0 - 4, 8, 8);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(dot);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(theme::color(theme::Role::Text));
            const QRect labelRect(int(dot.right()) + 7, y, gutterWidth() - int(dot.right()) - 13,
                                  kLaneHeight);
            painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetricsF(laneFont)
                                 .elidedText(lane.label, Qt::ElideRight, labelRect.width()));
        }
        if (lanes.isEmpty()) {
            painter.setPen(theme::color(theme::Role::TextFaint));
            painter.setFont(scaled(painter.font(), 1.0, false));
            painter.drawText(QRect(gutterWidth(), laneTop, width - gutterWidth(), 90),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             QStringLiteral("Chưa có bản chép nào.\n"
                                            "Bấm \"Ghi âm từ micro\" hoặc \"Chạy tệp audio\" "
                                            "để bắt đầu."));
        }
    }

    // Time under the play head, in the gutter, where it does not collide with
    // the words themselves.
    if (m_model) {
        painter.setPen(theme::color(theme::Role::TextMuted));
        // Monospace: this counter ticks five times a second and a proportional
        // face makes the whole string shuffle sideways on every digit change.
        painter.setFont(theme::mono(-0.5));
        painter.drawText(QRect(8, kWaveHeight, gutterWidth() - 12, kRulerHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         formatPrecise(m_model->sourceSeenSec()));
    }
}
