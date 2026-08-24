#include "mainwindow.h"

#include "core/Logger.h"
#include "ui/Dialogs.h"
#include "ui/DiagnosticsWindow.h"
#include "ui/EnrollDialog.h"
#include "ui/EvidenceWindow.h"
#include "ui/ReviewPanel.h"
#include "ui/TimelineView.h"
#include "ui/TraceWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString formatClock(double seconds)
{
    const double value = qMax(0.0, seconds);
    const int minutes = int(value) / 60;
    const double rest = value - minutes * 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(rest, 5, 'f', 2, QLatin1Char('0'));
}

const char *kSpeakerColors[] = {"#1a56db", "#c00030", "#1a7a2e", "#8b00a0", "#a04000"};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_config.load();
    // The stored preference is applied here, right after load and before any
    // flow starts, so the rest of the startup is already written wherever the
    // operator asked for it.  It is ignored when --log-mode / S2T_LOG_MODE
    // forced a mode for this run.
    applog::applyStoredPreference(m_config.logMode, m_config.logLevel);
    m_controller = new SessionController(&m_config, this);
    buildUi();

    connect(m_controller, &SessionController::modelUpdated, this, &MainWindow::onModelUpdated);
    connect(m_controller, &SessionController::statusUpdated, this, &MainWindow::onStatusUpdated);
    connect(m_controller, &SessionController::connectionUpdated, this,
            &MainWindow::onConnectionUpdated);
    connect(m_controller, &SessionController::errorMessage, this, [this](const QString &message) {
        statusBar()->showMessage(QStringLiteral("⚠ ") + message, 15000);
    });
    connect(m_controller, &SessionController::notice, this, [this](const QString &message) {
        statusBar()->showMessage(message, 6000);
    });
    connect(m_controller, &SessionController::sessionStarted, this, [this](const QString &id) {
        m_timeline->resetForSession();
        statusBar()->showMessage(QStringLiteral("Đã tạo phiên %1").arg(id), 8000);
        refreshActions();
    });
    connect(m_controller, &SessionController::sessionFinished, this,
            [this](const FinishedSession &summary) {
                statusBar()->showMessage(
                    QStringLiteral("Phiên %1 đã kết thúc (%2s, rev=%3)")
                        .arg(summary.sessionId)
                        .arg(summary.durationSec, 0, 'f', 1)
                        .arg(summary.revision),
                    15000);
                m_reviewPanel->refreshSessionList();
                refreshActions();
            });

    // The timeline and side panels are cheap to repaint but the status text
    // does not need to change 5x a second; keep them on separate cadences.
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(500);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshDelayBox);
    m_statusTimer->start();

    refreshActions();
    onConnectionUpdated();
    LOG_INFO(applog::cat::Ui) << "main window built";
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("S2T · Realtime ASR / Diarization"));

    auto *bar = addToolBar(QStringLiteral("Chính"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    m_connectionPill = new QLabel(QStringLiteral("● ĐANG KIỂM TRA AI"), this);
    m_connectionPill->setMargin(6);
    bar->addWidget(m_connectionPill);
    bar->addSeparator();

    m_micAction = bar->addAction(QStringLiteral("BẮT ĐẦU GHI ÂM"), this, &MainWindow::startMicrophone);
    m_micAction->setToolTip(QStringLiteral(
        "Thu microphone trên máy này và gửi lên AI server bằng gRPC"));
    m_pauseAction = bar->addAction(QStringLiteral("PAUSE"), this, &MainWindow::togglePause);
    m_pauseAction->setToolTip(QStringLiteral(
        "Tạm dừng gửi microphone, không kết thúc phiên. Audio nói lúc tạm dừng "
        "không được gửi sau khi tiếp tục."));
    m_stopAction = bar->addAction(QStringLiteral("STOP"), this, &MainWindow::stopSession);
    m_stopAction->setToolTip(QStringLiteral("Kết thúc audio hiện tại và flush correction"));
    m_fileAction = bar->addAction(QStringLiteral("AUDIO"), this, &MainWindow::startFileReplay);
    m_fileAction->setToolTip(QStringLiteral("Chạy lại một tệp WAV qua đúng pipeline"));
    bar->addSeparator();

    m_deviceLabel = new QLabel(this);
    m_deviceLabel->setMargin(6);
    bar->addWidget(m_deviceLabel);
    bar->addSeparator();

    m_denoiseOnAction = bar->addAction(QStringLiteral("DENOISE ON"), this,
                                       [this]() { m_controller->setDenoise(true); });
    m_denoiseOffAction = bar->addAction(QStringLiteral("DENOISE OFF"), this,
                                        [this]() { m_controller->setDenoise(false); });
    bar->addSeparator();

    m_textAction = bar->addAction(QStringLiteral("TEXT"), this,
                                  [this]() { m_timeline->jumpToLatestText(); });
    m_textAction->setToolTip(QStringLiteral("Nhảy tới chữ mới nhất"));
    m_liveAction = bar->addAction(QStringLiteral("LIVE"), this, [this]() {
        m_timeline->setFollowTarget(TimelineView::FollowTarget::Audio);
        m_timeline->setFollowEnabled(true);
    });
    m_liveAction->setCheckable(true);
    m_liveAction->setChecked(true);
    m_tickerAction = bar->addAction(QStringLiteral("TICKER"));
    m_tickerAction->setCheckable(true);
    m_tickerAction->setToolTip(QStringLiteral("Chuyển sang chế độ chữ chạy một dòng"));
    connect(m_tickerAction, &QAction::toggled, this, &MainWindow::toggleTicker);

    m_lowConfAction = bar->addAction(QStringLiteral("HIỆN TỪ YẾU"));
    m_lowConfAction->setCheckable(true);
    m_lowConfAction->setToolTip(QStringLiteral(
        "Mặc định các từ dưới ngưỡng tin cậy bị ẩn khỏi timeline (giống UI cũ). "
        "Bật để xem tất cả, kể cả từ độ tin cậy thấp."));
    connect(m_lowConfAction, &QAction::toggled, this, [this](bool on) {
        m_controller->model().setShowLowConfidence(on);
        m_timeline->refresh();
    });
    bar->addSeparator();

    bar->addAction(QStringLiteral("REVIEW"), this, &MainWindow::openReview);
    bar->addAction(QStringLiteral("LỊCH SỬ"), this, &MainWindow::openAuditHistory);
    bar->addAction(QStringLiteral("TRACE"), this, &MainWindow::openTrace);
    bar->addAction(QStringLiteral("NGHIỆM THU"), this, &MainWindow::openEvidence);
    bar->addAction(QStringLiteral("SETUP"), this, &MainWindow::openEnrollment);
    auto *logAction = bar->addAction(QStringLiteral("NHẬT KÝ"), this, &MainWindow::openDiagnostics);
    logAction->setToolTip(QStringLiteral(
        "Xem nhật ký hoạt động ngay trong ứng dụng và chạy các phép chẩn đoán "
        "(probe máy chủ, self-test giao thức) mà không cần cửa sổ lệnh."));
    bar->addAction(QStringLiteral("Cấu hình"), this, &MainWindow::openSettings);

    auto *spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    bar->addWidget(new QLabel(QStringLiteral("Người thao tác: "), this));
    m_operatorId = new QLineEdit(this);
    m_operatorId->setMaximumWidth(160);
    m_operatorId->setMaxLength(80);
    m_operatorId->setPlaceholderText(QStringLiteral("tên của bạn"));
    // Deliberately not remembered between runs: this name is recorded as the
    // person answerable for an edit, and a field that refills itself with
    // whoever used this machine last files one person's work under another's.
    m_operatorId->setToolTip(QStringLiteral(
        "Ghi vào lịch sử hiệu chỉnh. Phải nhập lại mỗi lần mở ứng dụng."));
    bar->addWidget(m_operatorId);

    m_timeline = new TimelineView(this);
    m_timeline->setModel(&m_controller->model());
    connect(m_timeline, &TimelineView::wordActivated, this, &MainWindow::onWordActivated);
    connect(m_timeline, &TimelineView::followChanged, this, [this](bool enabled) {
        m_liveAction->setChecked(enabled);
        m_liveAction->setText(enabled ? QStringLiteral("LIVE") : QStringLiteral("REVIEW"));
    });

    m_ticker = new QTextEdit(this);
    m_ticker->setReadOnly(true);
    m_ticker->setStyleSheet(QStringLiteral("font-size:20px; line-height:1.6;"));

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_timeline);
    m_stack->addWidget(m_ticker);

    auto *side = new QWidget(this);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 8, 8, 8);
    m_highlightTitle = new QLabel(QStringLiteral("Highlights (< 75%)"), side);
    sideLayout->addWidget(m_highlightTitle);
    m_statusLine = new QLabel(QStringLiteral("connecting..."), side);
    m_statusLine->setWordWrap(true);
    m_statusLine->setStyleSheet(QStringLiteral("font-family:monospace; font-size:11px;"));
    sideLayout->addWidget(m_statusLine);
    m_delayBox = new QLabel(QStringLiteral("delay: --"), side);
    m_delayBox->setWordWrap(true);
    m_delayBox->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_delayBox->setStyleSheet(QStringLiteral(
        "font-family:monospace; font-size:11px; border:1px solid #d0d0d0; padding:6px;"));
    sideLayout->addWidget(m_delayBox);
    m_highlights = new QListWidget(side);
    m_highlights->setWordWrap(true);
    sideLayout->addWidget(m_highlights, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_stack);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 1);
    splitter->setSizes({1200, 320});
    setCentralWidget(splitter);

    m_reviewPanel = new ReviewPanel(m_controller, this);
    connect(m_reviewPanel, &ReviewPanel::statusMessage, this,
            [this](const QString &message) { statusBar()->showMessage(message, 10000); });
    m_reviewDock = new QDockWidget(QStringLiteral("Review"), this);
    m_reviewDock->setWidget(m_reviewPanel);
    m_reviewDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_reviewDock);
    m_reviewDock->hide();

    // The toolbar is one single row of Vietnamese labels, so how wide it wants
    // to be is a function of the font the platform picked: MinGW and RHEL
    // disagree by roughly 90 px for the same text.  A hard-coded window width
    // therefore fits on one toolchain and clips the trailing "Người thao tác"
    // field on the other - QToolBar lays its items out at their size hint and
    // runs off the edge rather than shrinking them.  Ask the toolbar what it
    // needs instead, and never open narrower than that.
    int width = qMax(1560, bar->sizeHint().width());
    int height = 900;
    if (const QScreen *display = screen()) {
        const QRect room = display->availableGeometry();
        width = qMin(width, room.width());
        height = qMin(height, room.height());
    }
    // On a screen too narrow for the whole row, take the difference out of the
    // one item that can afford to give it back, rather than letting the layout
    // clip it off the edge where it cannot be read or clicked at all.
    const int shortfall = bar->sizeHint().width() - width;
    if (shortfall > 0)
        m_operatorId->setMaximumWidth(qMax(72, m_operatorId->maximumWidth() - shortfall));
    resize(width, height);

    statusBar()->showMessage(QStringLiteral("Sẵn sàng"));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    LOG_INFO(applog::cat::Ui) << "close requested - sessionRunning="
                              << m_controller->isRunning();
    if (m_controller->isRunning()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Đang ghi âm"),
            QStringLiteral("Phiên đang chạy. Đóng ứng dụng sẽ dừng phiên mà không hoàn tất "
                           "flush correction trên máy này.\n\nVẫn đóng?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            LOG_INFO(applog::cat::Ui) << "the operator cancelled the close";
            event->ignore();
            return;
        }
        LOG_WARN(applog::cat::Ui) << "closing with a session running - the session will be cut off";
    }
    m_config.save();
    event->accept();
}

void MainWindow::startMicrophone()
{
    LOG_INFO(applog::cat::Ui) << "action: open the start-recording dialog";
    StartSessionDialog dialog(m_controller, StartSessionDialog::Purpose::Microphone, this);
    if (dialog.exec() != QDialog::Accepted) {
        LOG_INFO(applog::cat::Ui) << "action: start-recording cancelled";
        return;
    }
    m_controller->startMicrophone(dialog.restrictSpeakers(), dialog.selectedSpeakers(),
                                  dialog.meta());
    refreshActions();
}

void MainWindow::startFileReplay()
{
    LOG_INFO(applog::cat::Ui) << "action: open the file-replay dialog";
    StartSessionDialog dialog(m_controller, StartSessionDialog::Purpose::File, this);
    if (dialog.exec() != QDialog::Accepted) {
        LOG_INFO(applog::cat::Ui) << "action: file replay cancelled";
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Chọn tệp audio"), QString(),
        QStringLiteral("Audio (*.wav *.m4a);;WAV (*.wav);;M4A/AAC (*.m4a)"));
    if (path.isEmpty()) {
        LOG_INFO(applog::cat::Ui) << "action: no file picked";
        return;
    }
    m_controller->startFile(path, dialog.restrictSpeakers(), dialog.selectedSpeakers(),
                            dialog.meta());
    refreshActions();
}

void MainWindow::stopSession()
{
    LOG_INFO(applog::cat::Ui) << "action: STOP pressed";
    m_controller->stop();
    refreshActions();
}

void MainWindow::togglePause()
{
    const bool pause = m_controller->micStatus() != MicStatus::Paused;
    LOG_INFO(applog::cat::Ui) << "action:" << (pause ? "PAUSE" : "RESUME") << "pressed";
    m_controller->setPaused(pause);
    refreshActions();
}

void MainWindow::openReview()
{
    LOG_INFO(applog::cat::Ui) << "action: toggle the review panel";
    m_reviewDock->setVisible(!m_reviewDock->isVisible());
    if (!m_reviewDock->isVisible())
        return;
    m_reviewPanel->setEditorId(m_operatorId->text().trimmed());
    m_reviewPanel->refreshSessionList();
    if (!m_controller->sessionId().isEmpty())
        m_reviewPanel->openSession(m_controller->sessionId());
}

void MainWindow::openAuditHistory()
{
    LOG_INFO(applog::cat::Ui) << "action: open the audit history of session"
                              << m_controller->sessionId();
    AuditHistoryDialog dialog(m_controller, m_controller->sessionId(), this);
    dialog.exec();
}

void MainWindow::openEnrollment()
{
    LOG_INFO(applog::cat::Ui) << "action: open speaker enrolment - editor="
                              << m_operatorId->text().trimmed();
    EnrollDialog dialog(m_controller, m_operatorId->text().trimmed(), this);
    dialog.exec();
}

void MainWindow::openTrace()
{
    LOG_INFO(applog::cat::Ui) << "action: open the pipeline trace window";
    if (!m_traceWindow)
        m_traceWindow = new TraceWindow(m_controller, this);
    m_traceWindow->setSession(m_controller->sessionId());
    m_traceWindow->show();
    m_traceWindow->raise();
}

void MainWindow::openEvidence()
{
    LOG_INFO(applog::cat::Ui) << "action: open the evidence window";
    if (!m_evidenceWindow)
        m_evidenceWindow = new EvidenceWindow(m_controller, this);
    m_evidenceWindow->setSession(m_controller->sessionId());
    m_evidenceWindow->show();
    m_evidenceWindow->raise();
}

void MainWindow::openDiagnostics()
{
    LOG_INFO(applog::cat::Ui) << "action: open the log and diagnostics window";
    if (!m_diagnosticsWindow)
        m_diagnosticsWindow = new DiagnosticsWindow(m_controller, &m_config, this);
    m_diagnosticsWindow->show();
    m_diagnosticsWindow->raise();
    m_diagnosticsWindow->activateWindow();
}

void MainWindow::openSettings()
{
    LOG_INFO(applog::cat::Ui) << "action: open settings";
    SettingsDialog dialog(&m_config, this);
    if (dialog.exec() != QDialog::Accepted) {
        LOG_INFO(applog::cat::Ui) << "action: settings cancelled, nothing changed";
        return;
    }
    dialog.applyToConfig();
    m_controller->applyConfig();
    statusBar()->showMessage(QStringLiteral("Đã cập nhật cấu hình; đang kết nối lại AI server."),
                             8000);
}

void MainWindow::toggleTicker(bool enabled)
{
    m_stack->setCurrentIndex(enabled ? 1 : 0);
    if (enabled)
        refreshTicker();
}

void MainWindow::onWordActivated(double startSec, double endSec, const QPoint &globalPos)
{
    asr::DisplayRow row;
    if (!m_controller->model().rowCovering(startSec, endSec, &row))
        return;
    const QString editorId = m_operatorId->text().trimmed();
    SentenceEditDialog dialog(m_controller, row, m_controller->model().sessionId(),
                              m_controller->model().revision(), m_controller->model().isFinal(),
                              m_controller->model().commitBoundarySec(), editorId, this);
    connect(&dialog, &SentenceEditDialog::transcriptChanged, this,
            [this](const asr::SessionState &state, quint64 revision) {
                m_controller->model().applyEditedState(state, revision);
                m_timeline->refresh();
            });
    dialog.move(globalPos);
    dialog.exec();
}

void MainWindow::onModelUpdated()
{
    m_timeline->refresh();
    refreshHighlights();
    if (m_tickerAction->isChecked())
        refreshTicker();

    const TranscriptModel &model = m_controller->model();
    m_highlightTitle->setText(
        QStringLiteral("Highlights (< %1%)").arg(model.state().confThresholdPct));
    m_statusLine->setText(
        QStringLiteral("rows=%1 prov=%2 phrases=%3 low=%4 lanes=%5 virt=%6/%7 %8")
            .arg(model.rows().size())
            .arg(model.provisionalRows().size())
            .arg(model.state().nPhrases)
            .arg(model.state().nLow)
            .arg(model.lanes().size())
            .arg(m_timeline->visibleWordCount())
            .arg(m_timeline->totalWordCount())
            .arg(model.state().done ? QStringLiteral("[DONE]") : QStringLiteral("[LIVE]")));
}

void MainWindow::onStatusUpdated()
{
    refreshActions();

    const MicStatus status = m_controller->micStatus();
    QString label = micStatusLabel(status);
    if (m_controller->micStatusSince() > 0 && status != MicStatus::Idle) {
        const qint64 elapsed =
            qint64(QDateTime::currentMSecsSinceEpoch() / 1000 - m_controller->micStatusSince());
        if (elapsed >= 0) {
            label += QStringLiteral(" (%1:%2)")
                         .arg(elapsed / 60, 2, 10, QLatin1Char('0'))
                         .arg(elapsed % 60, 2, 10, QLatin1Char('0'));
        }
    }
    m_deviceLabel->setText(label);
    QStringList tips;
    if (!m_controller->deviceWarning().isEmpty())
        tips << m_controller->deviceWarning();
    if (!m_controller->deviceName().isEmpty())
        tips << QStringLiteral("thiết bị: %1").arg(m_controller->deviceName());
    tips << QStringLiteral("lọc nhiễu: %1").arg(m_controller->denoiseStateKey());
    m_deviceLabel->setToolTip(tips.join(QStringLiteral(" · ")));
    m_deviceLabel->setStyleSheet(status == MicStatus::Error || status == MicStatus::DeviceReconnecting
                                     ? QStringLiteral("color:#a31515; font-weight:bold;")
                                     : QString());
}

void MainWindow::onConnectionUpdated()
{
    const bool connected = m_controller->connected();
    m_connectionPill->setText(connected ? QStringLiteral("● ĐÃ KẾT NỐI AI")
                                        : QStringLiteral("● MẤT KẾT NỐI AI"));
    m_connectionPill->setStyleSheet(
        connected ? QStringLiteral("color:#176524; font-weight:bold;")
                  : QStringLiteral("color:#a31515; font-weight:bold;"));
    m_connectionPill->setToolTip(m_controller->connectionDetail());
}

void MainWindow::refreshActions()
{
    const bool running = m_controller->isRunning();
    const MicStatus status = m_controller->micStatus();
    m_micAction->setEnabled(!running);
    m_micAction->setText(running && status == MicStatus::Recording ? QStringLiteral("ĐANG GHI")
                                                                  : QStringLiteral("BẮT ĐẦU GHI ÂM"));
    m_fileAction->setEnabled(!running);
    m_stopAction->setEnabled(running);
    const bool pausable = running
        && (status == MicStatus::Recording || status == MicStatus::Paused);
    m_pauseAction->setEnabled(pausable);
    m_pauseAction->setText(status == MicStatus::Paused ? QStringLiteral("RESUME")
                                                       : QStringLiteral("PAUSE"));
    const bool haveControlApp = !m_config.micControlApp.trimmed().isEmpty();
    m_denoiseOnAction->setEnabled(haveControlApp);
    m_denoiseOffAction->setEnabled(haveControlApp);
}

void MainWindow::refreshHighlights()
{
    const QList<asr::Highlight> &items = m_controller->model().state().highlights;
    // Rebuild only on an actual change.  Comparing counts alone would miss a
    // correction that rewrote a highlight's text without adding one, which is
    // exactly the case this panel exists to show.
    QString renderKey;
    for (const asr::Highlight &highlight : items) {
        renderKey += QStringLiteral("%1|%2|%3|%4\n")
                         .arg(highlight.startSec, 0, 'f', 2)
                         .arg(highlight.speaker)
                         .arg(highlight.confPct)
                         .arg(highlight.text);
    }
    if (renderKey == m_highlightsKey)
        return;
    m_highlightsKey = renderKey;
    m_highlights->clear();
    if (items.isEmpty()) {
        m_highlights->addItem(QStringLiteral("Không có low-confidence highlight."));
        return;
    }
    for (const asr::Highlight &highlight : items) {
        m_highlights->addItem(QStringLiteral("%1 · %2 · %3%\n%4")
                                  .arg(formatClock(highlight.startSec), highlight.speaker)
                                  .arg(highlight.confPct)
                                  .arg(highlight.text));
    }
}

void MainWindow::refreshDelayBox()
{
    const TranscriptModel &model = m_controller->model();
    const asr::SessionState &state = model.state();
    const SessionTelemetry &telemetry = m_controller->telemetry();

    const double sourceSeen = qMax(0.0, state.sourceSeenSec);
    const double sourceTotal = qMax(0.0, state.sourceTotalSec);
    // Stop flushing can advance the internal cursor with padded zeros; for a
    // file replay never display synthetic samples beyond the real file.
    const double audioSec = sourceTotal > 0 ? qMin(sourceSeen, sourceTotal) : sourceSeen;
    const double speechSec = qMax(0.0, state.speechSeenSec);
    const double wallSec = qMax(0.0, state.wallElapsedSec);
    const double speed = wallSec > 0.05 ? audioSec / wallSec : 0.0;
    const double textSec = model.latestTextEndSec();

    QStringList lines;
    if (textSec <= 0.0) {
        lines << QStringLiteral("<b>word freshness lag: --</b>");
    } else {
        // Deliberately excludes trailing VAD silence: the durable ingress
        // queue is a different measurement and gets its own line, so a green
        // 0.15 s word gap cannot hide a many-minute server backlog.
        const double rawDelay = qMax(0.0, audioSec - textSec);
        const double speechDelay = qMax(0.0, speechSec - textSec);
        const double replayScale = speed > 1.05 ? speed : 1.0;
        const bool accelerated = speed > 1.2;
        lines << QStringLiteral("<b>word freshness lag: %1</b>")
                     .arg(accelerated
                              ? QStringLiteral("%1 audio-s ≈ %2 wall-s")
                                    .arg(speechDelay, 0, 'f', 2)
                                    .arg(speechDelay / replayScale, 0, 'f', 2)
                              : QStringLiteral("%1s").arg(speechDelay, 0, 'f', 2));
        lines << QStringLiteral("delay(raw): %1")
                     .arg(accelerated
                              ? QStringLiteral("%1 audio-s ≈ %2 wall-s")
                                    .arg(rawDelay, 0, 'f', 2)
                                    .arg(rawDelay / replayScale, 0, 'f', 2)
                              : QStringLiteral("%1s").arg(rawDelay, 0, 'f', 2));
    }

    const double endToEnd = telemetry.localQueueSec + telemetry.serverQueueSec;
    if (m_controller->isRunning() || telemetry.sentSec > 0.0) {
        lines << QStringLiteral("<b>capture→AI backlog: %1s</b> (máy này %2s + server %3s)")
                     .arg(endToEnd, 0, 'f', 2)
                     .arg(telemetry.localQueueSec, 0, 'f', 2)
                     .arg(telemetry.serverQueueSec, 0, 'f', 2);
        lines << QStringLiteral("ACK RTT=%1ms (max %2) · AI wait=%3ms · network+gRPC≈%4ms · "
                                "one-way≈%5ms")
                     .arg(telemetry.rpcLastMs, 0, 'f', 0)
                     .arg(telemetry.rpcMaxMs, 0, 'f', 0)
                     .arg(telemetry.aiWaitLastMs, 0, 'f', 0)
                     .arg(telemetry.transportRoundTripMs, 0, 'f', 0)
                     .arg(telemetry.transportOneWayEstMs, 0, 'f', 0);
    }

    lines << QStringLiteral("audio=%1 wall=%2 speed=%3x")
                 .arg(formatClock(audioSec), formatClock(wallSec))
                 .arg(speed > 0 ? QString::number(speed, 'f', 2) : QStringLiteral("--"));
    lines << QStringLiteral("speech=%1 text=%2")
                 .arg(formatClock(speechSec),
                      textSec > 0 ? formatClock(textSec) : QStringLiteral("--"));

    const asr::LatencyServer &server = state.latency.server;
    if (server.sumP50 > 0) {
        lines << QStringLiteral("server sum=%1/%2ms asr=%3 diar=%4 verify=%5 itn=%6")
                     .arg(server.sumP50, 0, 'f', 0)
                     .arg(server.sumP95, 0, 'f', 0)
                     .arg(server.asrP50, 0, 'f', 0)
                     .arg(server.diarP50, 0, 'f', 0)
                     .arg(server.verifyP50, 0, 'f', 0)
                     .arg(server.itnP50, 0, 'f', 0);
    }
    const asr::LatencyClient &client = state.latency.client;
    if (client.e2eP50 > 0) {
        lines << QStringLiteral("client e2e=%1/%2ms prep=%3 wait=%4 parse=%5")
                     .arg(client.e2eP50, 0, 'f', 0)
                     .arg(client.e2eP95, 0, 'f', 0)
                     .arg(client.prepareP50, 0, 'f', 0)
                     .arg(client.waitP50, 0, 'f', 0)
                     .arg(client.parseP50, 0, 'f', 0);
    }
    if (!telemetry.pollError.isEmpty())
        lines << QStringLiteral("<span style='color:#a31515;'>poll: %1</span>").arg(telemetry.pollError);
    if (!m_controller->errorText().isEmpty())
        lines << QStringLiteral("<span style='color:#a31515;'>%1</span>").arg(m_controller->errorText());

    const bool ok = textSec > 0.0 && endToEnd <= 1.0;
    m_delayBox->setStyleSheet(QStringLiteral(
                                  "font-family:monospace; font-size:11px; border:1px solid %1; "
                                  "padding:6px; color:%2;")
                                  .arg(ok ? QStringLiteral("#63a96b") : QStringLiteral("#d0d0d0"),
                                       ok ? QStringLiteral("#166b22") : QStringLiteral("#1f1f1f")));
    m_delayBox->setText(lines.join(QStringLiteral("<br>")));
}

void MainWindow::refreshTicker()
{
    const TranscriptModel &model = m_controller->model();
    QList<WordItem> all;
    for (const Lane &lane : model.lanes()) {
        for (const WordItem &word : lane.words)
            all.append(word);
    }
    std::sort(all.begin(), all.end(),
              [](const WordItem &a, const WordItem &b) { return a.startSec < b.startSec; });
    // Bounded: a long meeting must not grow this document without limit.
    if (all.size() > 4000)
        all = all.mid(all.size() - 4000);

    QString html;
    html.reserve(all.size() * 48);
    for (const WordItem &word : all) {
        int speakerIndex = 0;
        const QString lane = word.laneKey;
        if (lane.startsWith(QLatin1String("sid:")))
            speakerIndex = lane.mid(4).toInt();
        else
            speakerIndex = qAbs(qHash(lane)) % 5;
        html += QStringLiteral("<span style=\"color:%1;opacity:%2;\">%3 </span>")
                    .arg(QString::fromLatin1(kSpeakerColors[speakerIndex % 5]),
                         word.provisional ? QStringLiteral("0.45") : QStringLiteral("1.0"),
                         word.text.toHtmlEscaped());
    }
    m_ticker->setHtml(html);
    m_ticker->moveCursor(QTextCursor::End);
}
