#include "SubtitleWindow.h"

#include "audio/MediaDecode.h"
#include "core/AppConfig.h"
#include "core/Logger.h"
#include "core/SessionController.h"

#include <QAudioOutput>
#include <QBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSlider>
#include <QTextLayout>
#include <QUrl>
#include <QVideoSink>

#include <algorithm>

namespace {

// How far either side of the playhead a word still counts as "being said now".
// A caption that vanished the instant a word ended would flicker between words;
// half a second is long enough to read and short enough to stay on the frame.
const double kCaptionTailSec = 0.6;

// Longest caption drawn over the picture.  Past this it stops being a subtitle
// and starts being a wall of text over someone's face - the reading pane beside
// it is where the whole transcript belongs.
const int kCaptionChars = 140;

// Behind the picture, and behind the "no picture" plate.  Deliberately dark
// whatever the desktop scheme is: a white rectangle where a film should be is
// read as a fault, not as "this source has no picture".
const QColor kStageBackground(0x10, 0x10, 0x14);

QString formatClock(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    const int total = int(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ---------------------------------------------------------------------------
// SubtitleStage
// ---------------------------------------------------------------------------

SubtitleStage::SubtitleStage(QWidget *parent) : QWidget(parent)
{
    // Every pixel is painted here, so let Qt skip erasing the widget first.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);

    m_sink = new QVideoSink(this);
    // The decoder emits from its own thread; an auto connection to a sink that
    // lives here queues the frame onto the GUI thread, which is the only place
    // it may be painted.
    connect(m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        m_frame = frame;
        update();
    });
}

void SubtitleStage::setHasPicture(bool hasPicture)
{
    m_hasPicture = hasPicture;
    // Drop the last frame of the previous file, or an audio-only source would
    // play under a still of the one before it.
    if (!hasPicture)
        m_frame = QVideoFrame();
    update();
}

void SubtitleStage::setCaption(const QString &settled, const QString &moving)
{
    if (settled == m_settled && moving == m_moving)
        return;
    m_settled = settled;
    m_moving = moving;
    update();
}

void SubtitleStage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), kStageBackground);

    if (m_frame.isValid()) {
        QVideoFrame::PaintOptions options;
        options.backgroundColor = kStageBackground;
        options.aspectRatioMode = Qt::KeepAspectRatio;
        m_frame.paint(&painter, rect(), options);
    } else if (!m_hasPicture) {
        painter.setPen(QColor(0x8a, 0x8a, 0x95));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("Nguồn chỉ có âm thanh."));
    }

    // Caption last, in the same painter as the picture.  That ordering is the
    // whole fix: nothing can stack above it because there is nothing else.
    paintCaption(painter);
}

void SubtitleStage::paintCaption(QPainter &painter)
{
    if (m_settled.isEmpty() && m_moving.isEmpty())
        return;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter.font();
    // Scales with the surface: the same caption has to be readable in a small
    // docked window and on a projector.
    font.setPixelSize(qBound(14, height() / 22, 40));
    font.setBold(true);
    painter.setFont(font);

    const int margin = qMax(12, width() / 40);
    const QRect area(margin, margin, width() - 2 * margin, height() - 2 * margin);

    const QString whole =
        m_moving.isEmpty() ? m_settled
                           : (m_settled.isEmpty() ? m_moving : m_settled + QLatin1Char(' ') + m_moving);

    // One layout, two colours - not two drawText() passes over each other.
    //
    // The old code painted the whole string in the edge colour and then the
    // settled prefix in white on top of it, on the theory that a prefix wraps
    // the same way as the string it starts.  It does not, once the box is
    // bottom-aligned: a prefix with fewer lines is pinned to the *bottom* of
    // the box, so the white words landed across the yellow last line and the
    // caption read as two sentences printed on top of one another.  Nobody saw
    // it while the caption itself was invisible on the deployed host.
    //
    // QTextLayout wraps once and colours by character range, so the two parts
    // cannot disagree about where a line breaks.
    QTextLayout layout(whole, font);
    QTextOption option(Qt::AlignHCenter);
    option.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(option);

    // Default pen (white) is the settled part; only the moving edge is
    // recoloured, and when there is nothing settled the whole string is edge.
    const int edgeStart = m_settled.isEmpty() ? 0 : m_settled.size() + 1;
    if (edgeStart < whole.size() && !m_moving.isEmpty()) {
        QTextLayout::FormatRange edge;
        edge.start = edgeStart;
        edge.length = whole.size() - edgeStart;
        edge.format.setForeground(QColor(255, 214, 120));
        layout.setFormats({edge});
    }

    layout.beginLayout();
    qreal used = 0.0;
    qreal widest = 0.0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(area.width());
        line.setPosition(QPointF(0.0, used));
        used += line.height();
        widest = qMax(widest, line.naturalTextWidth());
    }
    layout.endLayout();

    const QPointF origin(area.x(), area.bottom() - used);

    // A plate behind the text, because a subtitle over a bright frame is
    // otherwise unreadable and outlining every glyph costs more than it buys.
    const QRectF plate =
        QRectF(area.x() + (area.width() - widest) / 2.0, origin.y(), widest, used)
            .adjusted(-14, -8, 14, 8);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 165));
    painter.drawRoundedRect(plate, 8, 8);

    painter.setPen(Qt::white);
    layout.draw(&painter, origin);
    painter.restore();
}

// ---------------------------------------------------------------------------
// SubtitleWindow
// ---------------------------------------------------------------------------

SubtitleWindow::SubtitleWindow(SessionController *controller, AppConfig *config, QWidget *parent)
    : QWidget(parent, Qt::Window), m_controller(controller), m_config(config)
{
    setWindowTitle(QStringLiteral("Phụ đề trực tiếp"));
    // Derived from the font, not hard-coded: Vietnamese labels measure wider
    // than the English they were laid out against.
    resize(fontMetrics().averageCharWidth() * 130, fontMetrics().height() * 34);

    auto *root = new QVBoxLayout(this);

    // ---- toolbar -----------------------------------------------------------
    auto *bar = new QHBoxLayout;
    m_fileButton = new QPushButton(QStringLiteral("Mở tệp âm thanh / video…"), this);
    m_micButton = new QPushButton(QStringLiteral("Thu từ micro"), this);
    m_stopButton = new QPushButton(QStringLiteral("Dừng"), this);
    m_status = new QLabel(this);
    m_clock = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    bar->addWidget(m_fileButton);
    bar->addWidget(m_micButton);
    bar->addWidget(m_stopButton);
    bar->addSpacing(fontMetrics().averageCharWidth() * 2);
    bar->addWidget(m_status, 1);
    bar->addWidget(m_clock);
    root->addLayout(bar);

    // ---- stage: the picture and the caption on it ---------------------------
    auto *split = new QHBoxLayout;

    m_stage = new SubtitleStage(this);
    split->addWidget(m_stage, 3);

    m_transcript = new QPlainTextEdit(this);
    m_transcript->setReadOnly(true);
    m_transcript->setPlaceholderText(
        QStringLiteral("Bản chép sẽ hiện ở đây trong lúc phát."));
    split->addWidget(m_transcript, 2);
    root->addLayout(split, 1);

    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setRange(0, 0);
    root->addWidget(m_seek);

    // ---- playback ----------------------------------------------------------
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoSink(m_stage->sink());

    connect(m_fileButton, &QPushButton::clicked, this, &SubtitleWindow::openFile);
    connect(m_micButton, &QPushButton::clicked, this, &SubtitleWindow::startMicrophone);
    connect(m_stopButton, &QPushButton::clicked, this, &SubtitleWindow::stopEverything);
    connect(m_player, &QMediaPlayer::positionChanged, this, &SubtitleWindow::onPlayerPosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
        m_durationSec = double(ms) / 1000.0;
        m_seek->setRange(0, int(ms));
    });
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_seeking = false;
        m_player->setPosition(m_seek->value());
    });
    connect(m_seek, &QSlider::valueChanged, this, &SubtitleWindow::onSeek);
    connect(m_controller, &SessionController::modelUpdated, this,
            &SubtitleWindow::onModelUpdated);
    connect(m_controller, &SessionController::statusUpdated, this,
            &SubtitleWindow::refreshStatus);

    setMode(Mode::Idle);
}

SubtitleWindow::~SubtitleWindow() = default;

void SubtitleWindow::setMode(Mode mode)
{
    m_mode = mode;
    const bool busy = mode != Mode::Idle;
    m_fileButton->setEnabled(!busy);
    m_micButton->setEnabled(!busy);
    m_stopButton->setEnabled(busy);
    m_seek->setEnabled(mode == Mode::File);
    refreshStatus();
}

void SubtitleWindow::refreshStatus()
{
    QString text;
    switch (m_mode) {
    case Mode::Idle:
        text = QStringLiteral("Sẵn sàng. Chọn một tệp hoặc bật micro.");
        break;
    case Mode::File:
        text = QStringLiteral("Đang phát: %1").arg(m_sourceName);
        break;
    case Mode::Microphone:
        text = QStringLiteral("Đang thu từ micro");
        break;
    }
    if (!m_controller->upstreamReady() && m_mode != Mode::Idle) {
        // Worth saying out loud during a demo: audio is still being accepted
        // and queued, so the silence is not a bug in the client.
        text += QStringLiteral("  —  tầng suy luận chưa sẵn sàng, audio đang được đệm");
    }
    m_status->setText(text);
}

void SubtitleWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Chọn tệp âm thanh hoặc video"), QString(),
        QStringLiteral("Media (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.mp4 *.mkv *.mov *.avi);;"
                       "Tất cả (*)"));
    if (path.isEmpty())
        return;
    loadFile(path);
}

void SubtitleWindow::loadFile(const QString &path)
{

    // Decode first, play second.  If the audio cannot be read there is nothing
    // to transcribe, and starting the picture anyway would demonstrate a
    // subtitle feature with no subtitles.
    QString error;
    const wav::Pcm pcm = audio::decodeMedia(path, &error);
    if (!pcm.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Không dùng được tệp này"), error);
        return;
    }

    m_sourceName = QFileInfo(path).fileName();
    m_words.clear();
    m_movingText.clear();
    m_transcript->clear();
    m_stage->setCaption(QString(), QString());
    const bool video = audio::hasVideoTrack(path);
    m_stage->setHasPicture(video);

    SessionMeta meta;
    meta.title = m_sourceName;
    // Paced to the source clock, always. The subtitles are placed against the
    // player's position, so sending faster than real time would only mean the
    // transcript is ready before the frames that need it - harmless - but
    // pacing also keeps the pipeline's own windowing honest.
    m_controller->startPcm(pcm, m_sourceName, false, QStringList(), meta, true);

    m_player->setSource(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
    m_player->play();
    setMode(Mode::File);
    LOG_INFO(applog::cat::Ui) << "subtitle demo started for" << m_sourceName
                              << (video ? "(có hình)" : "(chỉ tiếng)") << pcm.durationSec() << "s";
}

void SubtitleWindow::startMicrophone()
{
    m_sourceName = QStringLiteral("micro");
    m_words.clear();
    m_movingText.clear();
    m_transcript->clear();
    m_stage->setCaption(QString(), QString());
    m_stage->setHasPicture(false);
    m_durationSec = 0.0;

    SessionMeta meta;
    meta.title = QStringLiteral("Phụ đề trực tiếp");
    m_controller->startMicrophone(false, QStringList(), meta);
    setMode(Mode::Microphone);
}

void SubtitleWindow::stopEverything()
{
    m_player->stop();
    if (m_controller->isRunning())
        m_controller->stop();
    setMode(Mode::Idle);
}

void SubtitleWindow::onSeek(int value)
{
    if (!m_seeking)
        return;
    // Move the caption with the handle while dragging, so scrubbing shows what
    // was said there rather than freezing on the last painted line.
    renderAt(double(value) / 1000.0);
}

void SubtitleWindow::onPlayerPosition(qint64 ms)
{
    if (m_mode != Mode::File)
        return;
    if (!m_seeking)
        m_seek->setValue(int(ms));
    m_clock->setText(QStringLiteral("%1 / %2")
                         .arg(formatClock(double(ms) / 1000.0), formatClock(m_durationSec)));
    renderAt(double(ms) / 1000.0);
}

void SubtitleWindow::rebuildWords()
{
    m_words.clear();
    const TranscriptModel &model = m_controller->model();
    for (const asr::DisplayRow &row : model.rows()) {
        const QList<WordItem> items = TranscriptModel::rowWordItems(row);
        m_words.append(items);
    }
    std::sort(m_words.begin(), m_words.end(),
              [](const WordItem &a, const WordItem &b) {
                  return a.startSec < b.startSec;
              });

    // The moving edge is whatever the pipeline has not committed yet.  It has
    // no reliable timing - that is what makes it provisional - so it is kept
    // as text and only shown at the live end of the recording.
    m_movingText.clear();
    for (const asr::DisplayRow &row : model.provisionalRows()) {
        const QString text = row.updatingText.isEmpty() ? row.mergedText : row.updatingText;
        if (text.isEmpty())
            continue;
        if (!m_movingText.isEmpty())
            m_movingText += QLatin1Char(' ');
        m_movingText += text;
    }
}

void SubtitleWindow::onModelUpdated()
{
    rebuildWords();

    // The reading pane always shows everything settled so far, newest at the
    // bottom, and follows unless the operator has scrolled up to read.
    QString whole;
    for (const asr::DisplayRow &row : m_controller->model().rows()) {
        const QString line = row.mergedText.trimmed();
        if (line.isEmpty())
            continue;
        // "unknown" is what the pipeline returns when nobody was verified, and
        // it is not empty - printing it raw puts the word "unknown" where a
        // person's name goes.  TranscriptModel owns that rule for every pane.
        const QString who = TranscriptModel::isRealName(row.verifiedName)
            ? row.verifiedName.trimmed()
            : QStringLiteral("Người %1").arg(row.speaker.toInt() + 1);
        whole += QStringLiteral("[%1] %2: %3\n").arg(formatClock(row.startSec), who, line);
    }
    const bool atBottom =
        m_transcript->verticalScrollBar()->value() >= m_transcript->verticalScrollBar()->maximum() - 4;
    if (whole != m_transcript->toPlainText()) {
        m_transcript->setPlainText(whole);
        if (atBottom)
            m_transcript->verticalScrollBar()->setValue(
                m_transcript->verticalScrollBar()->maximum());
    }

    if (m_mode == Mode::Microphone) {
        // No media clock to place anything against: the live end *is* the
        // playhead, so the caption is simply the newest thing there is.
        const double edge = m_words.isEmpty() ? 0.0 : m_words.last().endSec;
        renderAt(edge);
        m_clock->setText(formatClock(m_controller->model().sourceSeenSec()));
    }
}

void SubtitleWindow::renderAt(double atSec)
{
    // Everything being said at `atSec`, plus a short tail so a caption does not
    // blink out between two words of the same sentence.
    QString settled;
    for (const WordItem &word : m_words) {
        if (word.endSec + kCaptionTailSec < atSec)
            continue;
        if (word.startSec > atSec)
            break; // sorted: nothing later can qualify either
        if (!settled.isEmpty())
            settled += QLatin1Char(' ');
        settled += word.text;
    }

    // Keep the tail rather than the head: the words nearest the playhead are
    // the ones being spoken now.
    if (settled.size() > kCaptionChars)
        settled = QStringLiteral("… ") + settled.right(kCaptionChars);

    // The interim edge belongs only at the live end.  Painting it while the
    // operator scrubs through the middle would put tomorrow's text on
    // yesterday's frame.
    const bool atLiveEdge =
        m_mode == Mode::Microphone
        || (!m_words.isEmpty() && atSec >= m_words.last().endSec - kCaptionTailSec);
    m_stage->setCaption(settled, atLiveEdge ? m_movingText : QString());
}
