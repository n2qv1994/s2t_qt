#include "mainwindow.h"

#include "core/Logger.h"
#include "ui/Dialogs.h"
#include "ui/DiagnosticsWindow.h"
#include "ui/EnrollDialog.h"
#include "ui/EvidenceWindow.h"
#include "ui/ReviewPanel.h"
#include "ui/StatusPanel.h"
#include "ui/Theme.h"
#include "ui/TimelineView.h"
#include "ui/SubtitleWindow.h"
#include "ui/TraceWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSignalBlocker>
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
        m_sessionLabel->setText(id);
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
    setWindowTitle(QStringLiteral("S2T · Ghi âm & chuyển văn bản trực tiếp"));

    buildActions();
    buildMenus();
    buildToolBar();
    buildCentral();
    buildStatusBar();

    m_reviewPanel = new ReviewPanel(m_controller, this);
    connect(m_reviewPanel, &ReviewPanel::statusMessage, this,
            [this](const QString &message) { statusBar()->showMessage(message, 10000); });
    // "&&", not "&": a QDockWidget title goes through mnemonic processing the
    // same way an action's text does, and a lone ampersand is swallowed.
    m_reviewDock = new QDockWidget(QStringLiteral("Soát && sửa bản chép"), this);
    m_reviewDock->setWidget(m_reviewPanel);
    m_reviewDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_reviewDock);
    m_reviewDock->hide();
    // The toolbar button and the dock's own close box are the same state, so
    // closing the dock has to un-press the button or the two disagree.
    connect(m_reviewDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        QSignalBlocker blocker(m_reviewAction);
        m_reviewAction->setChecked(visible);
    });

    // The window opens wide enough for its own toolbar and never wider than
    // the screen.  A hard-coded width fits exactly one font stack: the same
    // Vietnamese labels measure about 90 px wider on RHEL than under MinGW,
    // and QToolBar lays items out at their size hint and runs off the edge
    // rather than shrinking them.  Since the tool windows moved into the menu
    // bar the row is much shorter than it was, but the rule still holds.
    QToolBar *bar = findChild<QToolBar *>();
    int width = qMax(1440, bar ? bar->sizeHint().width() + 48 : 0);
    int height = 900;
    // theme::screenRoom(), not QScreen::availableGeometry() - see its comment.
    // The deployed RHEL host reports its screen as 0x0, and the old code here
    // clamped with qMin(width, 0), asking for a zero-wide window and getting
    // whatever the layout's minimum happened to be.  It looked like it worked
    // because nobody had measured the window on that host.
    const QSize room = theme::screenRoom(this);
    if (room.isValid()) {
        width = qMin(width, room.width());
        height = qMin(height, room.height());
    }
    resize(width, height);

    statusBar()->showMessage(QStringLiteral("Sẵn sàng."));
}

// ---------------------------------------------------------------------------
// Actions
//
// Created once, here, and referenced from both the menu bar and the toolbar.
// Two QActions doing the same job is how a checkable item ends up out of sync
// with the thing it controls.
// ---------------------------------------------------------------------------

void MainWindow::buildActions()
{
    m_micAction = new QAction(theme::icon(theme::Glyph::Record, theme::Tone::Danger),
                              QStringLiteral("Ghi âm từ micro"), this);
    m_micAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    m_micAction->setToolTip(QStringLiteral(
        "Thu microphone trên máy này và gửi lên Server buffer bằng gRPC (Ctrl+R)"));
    connect(m_micAction, &QAction::triggered, this, &MainWindow::startMicrophone);

    m_pauseAction = new QAction(theme::icon(theme::Glyph::Pause),
                                QStringLiteral("Tạm dừng"), this);
    m_pauseAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
    m_pauseAction->setToolTip(QStringLiteral(
        "Tạm dừng gửi microphone, không kết thúc phiên. Audio nói lúc tạm dừng "
        "không được gửi sau khi tiếp tục. (Ctrl+P)"));
    connect(m_pauseAction, &QAction::triggered, this, &MainWindow::togglePause);

    m_stopAction = new QAction(theme::icon(theme::Glyph::Stop), QStringLiteral("Dừng phiên"), this);
    m_stopAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+.")));
    m_stopAction->setToolTip(
        QStringLiteral("Kết thúc audio hiện tại và flush correction (Ctrl+.)"));
    connect(m_stopAction, &QAction::triggered, this, &MainWindow::stopSession);

    m_fileAction = new QAction(theme::icon(theme::Glyph::File),
                               QStringLiteral("Chạy tệp audio..."), this);
    m_fileAction->setShortcut(QKeySequence::Open);
    m_fileAction->setToolTip(
        QStringLiteral("Chạy lại một tệp âm thanh hoặc video qua đúng pipeline (Ctrl+O)"));
    connect(m_fileAction, &QAction::triggered, this, &MainWindow::startFileReplay);

    m_quitAction = new QAction(QStringLiteral("Thoát"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, this, &MainWindow::close);

    m_liveAction = new QAction(theme::icon(theme::Glyph::Live), QStringLiteral("Bám trực tiếp"),
                               this);
    m_liveAction->setCheckable(true);
    m_liveAction->setChecked(true);
    m_liveAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    m_liveAction->setToolTip(QStringLiteral(
        "Timeline tự cuộn theo audio đang tới. Cuộn tay sẽ tự tắt chế độ này. (Ctrl+L)"));
    // toggled, not triggered.  On `triggered` a second press unchecked the
    // button and then turned following straight back on, because
    // setFollowEnabled() returns early when nothing changed and never emitted
    // followChanged() to put the tick back - so the button read "off" while
    // the timeline was still following.
    connect(m_liveAction, &QAction::toggled, this, [this](bool on) {
        if (on)
            m_timeline->setFollowTarget(TimelineView::FollowTarget::Audio);
        m_timeline->setFollowEnabled(on);
    });

    m_textAction = new QAction(theme::icon(theme::Glyph::JumpText),
                               QStringLiteral("Tới chữ mới nhất"), this);
    m_textAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
    m_textAction->setToolTip(QStringLiteral("Nhảy tới từ mới nhất đã nhận được (Ctrl+J)"));
    connect(m_textAction, &QAction::triggered, this, [this]() { m_timeline->jumpToLatestText(); });

    m_tickerAction = new QAction(theme::icon(theme::Glyph::Ticker),
                                 QStringLiteral("Chữ chạy"), this);
    m_tickerAction->setCheckable(true);
    m_tickerAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    m_tickerAction->setToolTip(QStringLiteral(
        "Đổi timeline sang một dòng chữ chạy, cỡ lớn, để chiếu lên màn hình chung (Ctrl+T)"));
    connect(m_tickerAction, &QAction::toggled, this, &MainWindow::toggleTicker);

    m_lowConfAction = new QAction(theme::icon(theme::Glyph::LowConf),
                                  QStringLiteral("Hiện từ yếu"), this);
    m_lowConfAction->setCheckable(true);
    m_lowConfAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    m_lowConfAction->setToolTip(QStringLiteral(
        "Mặc định các từ dưới ngưỡng tin cậy bị ẩn khỏi timeline (giống UI cũ). "
        "Bật để xem tất cả, kể cả từ độ tin cậy thấp. (Ctrl+W)"));
    connect(m_lowConfAction, &QAction::toggled, this, [this](bool on) {
        m_controller->model().setShowLowConfidence(on);
        m_timeline->refresh();
    });

    // A lone "&" is Qt's mnemonic marker and is eaten from the label, so any
    // ampersand meant to be read has to be doubled.  setWindowTitle() is the
    // one place that does not apply.
    m_reviewAction = new QAction(theme::icon(theme::Glyph::Review),
                                 QStringLiteral("Soát && sửa"), this);
    m_reviewAction->setCheckable(true);
    m_reviewAction->setShortcut(QKeySequence(QStringLiteral("F9")));
    m_reviewAction->setToolTip(QStringLiteral(
        "Mở bảng soát bản chép ở dưới: nghe lại từng câu, sửa chữ, đặt tên người nói (F9)"));
    connect(m_reviewAction, &QAction::triggered, this, &MainWindow::openReview);

    m_subtitleAction = new QAction(theme::icon(theme::Glyph::Subtitle),
                                   QStringLiteral("Phụ đề"), this);
    m_subtitleAction->setShortcut(QKeySequence(QStringLiteral("F6")));
    m_subtitleAction->setToolTip(QStringLiteral(
        "Cửa sổ phụ đề cỡ lớn, không viền, để đưa sang màn hình thứ hai (F6)"));
    connect(m_subtitleAction, &QAction::triggered, this, &MainWindow::openSubtitles);

    m_historyAction = new QAction(theme::icon(theme::Glyph::History),
                                  QStringLiteral("Lịch sử hiệu chỉnh..."), this);
    m_historyAction->setShortcut(QKeySequence(QStringLiteral("F7")));
    connect(m_historyAction, &QAction::triggered, this, &MainWindow::openAuditHistory);

    m_traceAction = new QAction(theme::icon(theme::Glyph::Trace),
                                QStringLiteral("Pipeline trace..."), this);
    m_traceAction->setShortcut(QKeySequence(QStringLiteral("F8")));
    connect(m_traceAction, &QAction::triggered, this, &MainWindow::openTrace);

    m_evidenceAction = new QAction(theme::icon(theme::Glyph::Evidence),
                                   QStringLiteral("Nghiệm thu pipeline..."), this);
    m_evidenceAction->setShortcut(QKeySequence(QStringLiteral("F10")));
    connect(m_evidenceAction, &QAction::triggered, this, &MainWindow::openEvidence);

    m_enrollAction = new QAction(theme::icon(theme::Glyph::Enroll),
                                 QStringLiteral("Đăng ký giọng nói..."), this);
    m_enrollAction->setShortcut(QKeySequence(QStringLiteral("F5")));
    connect(m_enrollAction, &QAction::triggered, this, &MainWindow::openEnrollment);

    m_logAction = new QAction(theme::icon(theme::Glyph::Log),
                              QStringLiteral("Nhật ký && chẩn đoán"), this);
    m_logAction->setShortcut(QKeySequence(QStringLiteral("F12")));
    m_logAction->setToolTip(QStringLiteral(
        "Xem nhật ký hoạt động ngay trong ứng dụng và chạy các phép chẩn đoán "
        "(probe máy chủ, self-test giao thức) mà không cần cửa sổ lệnh. (F12)"));
    connect(m_logAction, &QAction::triggered, this, &MainWindow::openDiagnostics);

    m_settingsAction = new QAction(theme::icon(theme::Glyph::Settings),
                                   QStringLiteral("Cấu hình..."), this);
    m_settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    m_denoiseOnAction = new QAction(theme::icon(theme::Glyph::Denoise),
                                    QStringLiteral("Bật lọc nhiễu"), this);
    connect(m_denoiseOnAction, &QAction::triggered, this,
            [this]() { m_controller->setDenoise(true); });
    m_denoiseOffAction = new QAction(QStringLiteral("Tắt lọc nhiễu"), this);
    connect(m_denoiseOffAction, &QAction::triggered, this,
            [this]() { m_controller->setDenoise(false); });

    // Six of these actions are only ever shown inside a menu.  Without an
    // application-wide context their shortcuts would do nothing until that
    // menu happened to be open, which is not what a shortcut is for.
    for (QAction *action : findChildren<QAction *>())
        action->setShortcutContext(Qt::ApplicationShortcut);
}

void MainWindow::buildMenus()
{
    QMenuBar *menu = menuBar();

    QMenu *session = menu->addMenu(QStringLiteral("Phiên"));
    session->addAction(m_micAction);
    session->addAction(m_fileAction);
    session->addSeparator();
    session->addAction(m_pauseAction);
    session->addAction(m_stopAction);
    session->addSeparator();
    session->addAction(m_enrollAction);
    session->addSeparator();
    session->addAction(m_quitAction);

    QMenu *view = menu->addMenu(QStringLiteral("Hiển thị"));
    view->addAction(m_liveAction);
    view->addAction(m_textAction);
    view->addSeparator();
    view->addAction(m_tickerAction);
    view->addAction(m_lowConfAction);
    view->addSeparator();
    view->addAction(m_reviewAction);
    view->addAction(m_subtitleAction);

    QMenu *mic = menu->addMenu(QStringLiteral("Micro"));
    mic->addAction(m_denoiseOnAction);
    mic->addAction(m_denoiseOffAction);

    QMenu *tools = menu->addMenu(QStringLiteral("Công cụ"));
    tools->addAction(m_historyAction);
    tools->addAction(m_traceAction);
    tools->addAction(m_evidenceAction);
    tools->addSeparator();
    tools->addAction(m_logAction);
    tools->addAction(m_settingsAction);
}

void MainWindow::buildToolBar()
{
    auto *bar = addToolBar(QStringLiteral("Chính"));
    bar->setObjectName(QStringLiteral("s2tMainToolBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // Icon size follows the font, so a desktop running at 125% does not get a
    // 16 px icon next to 20 px text.
    const int glyph = qMax(16, QFontMetrics(bar->font()).height());
    bar->setIconSize(QSize(glyph, glyph));

    // Group 1 - the transport.  What starts and stops audio, nothing else.
    bar->addAction(m_micAction);
    bar->addAction(m_pauseAction);
    bar->addAction(m_stopAction);
    bar->addAction(m_fileAction);
    bar->addSeparator();

    // Group 2 - what the timeline shows.  Every item here is a view toggle and
    // none of them touch the session.
    bar->addAction(m_liveAction);
    bar->addAction(m_textAction);
    bar->addAction(m_tickerAction);
    bar->addAction(m_lowConfAction);
    bar->addSeparator();

    // Group 3 - the two panels reached for mid-meeting.  The other six windows
    // are in the menu bar only: putting all of them here is what used to push
    // "Cấu hình" into the "»" overflow exactly when the connection went bad
    // and an operator needed it to fix the server address.
    bar->addAction(m_reviewAction);
    bar->addAction(m_subtitleAction);

    // This one button changes its own label at run time ("Bám trực tiếp" when
    // it is following, "Đang xem lại" when the operator has scrolled away), and
    // a toolbar item that resizes re-flows every item after it.  Pin it to the
    // wider of its two states, measured rather than guessed - the two strings
    // differ by more on the RHEL font stack than on the MinGW one.
    if (QWidget *liveButton = bar->widgetForAction(m_liveAction)) {
        const QFontMetrics metrics(liveButton->font());
        const int text = qMax(metrics.horizontalAdvance(QStringLiteral("Bám trực tiếp")),
                              metrics.horizontalAdvance(QStringLiteral("Đang xem lại")));
        liveButton->setMinimumWidth(text + bar->iconSize().width() + 6 * theme::kGap);
    }

    auto *spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    // The right end of the bar is state, not verbs: the two indicators an
    // operator glances at without stopping what they are doing.
    m_deviceLabel = new QLabel(bar);
    m_deviceLabel->setProperty("s2tMuted", true);
    m_deviceLabel->setMargin(theme::kGapTight);
    bar->addWidget(m_deviceLabel);

    m_connectionPill = new QLabel(QStringLiteral("● ĐANG KIỂM TRA"), bar);
    // Fixed to the widest of its four states, measured rather than guessed.
    // Without this the label resizes when the connection state changes and the
    // whole toolbar re-flows under it - and it does that exactly when the
    // state goes bad, which is when the operator is reading it.
    {
        const QStringList states{QStringLiteral("● ĐANG KIỂM TRA"),
                                 QStringLiteral("● ĐÃ KẾT NỐI AI"),
                                 QStringLiteral("● ĐANG ĐỆM"),
                                 QStringLiteral("● MẤT KẾT NỐI")};
        QFont bold = m_connectionPill->font();
        bold.setBold(true);
        const QFontMetrics metrics(bold);
        int widest = 0;
        for (const QString &text : states)
            widest = qMax(widest, metrics.horizontalAdvance(text));
        m_connectionPill->setMinimumWidth(widest + 6 * theme::kGapTight);
    }
    bar->addWidget(m_connectionPill);
    // A spacer the width of the bar's own padding, so the pill does not sit
    // flush against the window edge.
    auto *tail = new QWidget(bar);
    tail->setFixedWidth(theme::kGapTight);
    bar->addWidget(tail);
}

void MainWindow::buildStatusBar()
{
    // The bottom bar carries identity and the session id, which change at most
    // once per meeting; the top bar carries the two things that change while
    // the meeting runs.  Splitting them that way is most of why the toolbar no
    // longer overflows.
    auto *operatorLabel = new QLabel(QStringLiteral("Người thao tác:"), this);
    operatorLabel->setProperty("s2tMuted", true);

    m_operatorId = new QLineEdit(this);
    m_operatorId->setMaxLength(80);
    m_operatorId->setPlaceholderText(QStringLiteral("tên của bạn"));
    m_operatorId->setClearButtonEnabled(true);
    // Deliberately not remembered between runs: this name is recorded as the
    // person answerable for an edit, and a field that refills itself with
    // whoever used this machine last files one person's work under another's.
    m_operatorId->setToolTip(QStringLiteral(
        "Ghi vào lịch sử hiệu chỉnh. Phải nhập lại mỗi lần mở ứng dụng."));
    m_operatorId->setFixedWidth(
        qMax(160, QFontMetrics(m_operatorId->font()).horizontalAdvance(QLatin1Char('m')) * 18));

    m_sessionLabel = new QLabel(QStringLiteral("chưa có phiên"), this);
    m_sessionLabel->setProperty("s2tMuted", true);
    m_sessionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sessionLabel->setToolTip(QStringLiteral("Mã phiên hiện tại trên Server buffer."));

    // QStatusBar packs permanent widgets with no gap at all, so the session id
    // ran straight into the next label.  A hairline separator between the two
    // groups is cheaper to read than more whitespace.
    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("border:none; background:%1; margin:3px %2px;")
                               .arg(theme::color(theme::Role::Border).name())
                               .arg(theme::kGap));

    statusBar()->addPermanentWidget(m_sessionLabel);
    statusBar()->addPermanentWidget(divider);
    statusBar()->addPermanentWidget(operatorLabel);
    statusBar()->addPermanentWidget(m_operatorId);
    statusBar()->setSizeGripEnabled(true);
}

void MainWindow::buildCentral()
{
    m_timeline = new TimelineView(this);
    m_timeline->setModel(&m_controller->model());
    connect(m_timeline, &TimelineView::wordActivated, this, &MainWindow::onWordActivated);
    connect(m_timeline, &TimelineView::followChanged, this, [this](bool enabled) {
        QSignalBlocker blocker(m_liveAction);
        m_liveAction->setChecked(enabled);
        // The label is the state, not a second button: while following is off
        // the timeline is a document being read, and saying so is clearer than
        // leaving an unpressed "Bám trực tiếp" and no explanation.
        m_liveAction->setText(enabled ? QStringLiteral("Bám trực tiếp")
                                      : QStringLiteral("Đang xem lại"));
    });

    m_ticker = new QTextEdit(this);
    m_ticker->setReadOnly(true);
    m_ticker->setFrameShape(QFrame::NoFrame);
    {
        // Projection mode: sized off the desktop font so it stays readable at
        // the back of a room without pinning a pixel size that only suits one
        // of the two font stacks.
        QFont large = m_ticker->font();
        large.setPointSizeF(qMax(13.0, large.pointSizeF() * 2.0));
        m_ticker->setFont(large);
    }

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_timeline);
    m_stack->addWidget(m_ticker);

    m_status = new StatusPanel(this);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(m_stack);
    splitter->addWidget(m_status);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    // The panel needs enough room for "Tiếng nói · văn bản" plus its value on
    // one line; asking it rather than guessing keeps it whole on both kits.
    const int sidebar = qMax(300, m_status->sizeHint().width());
    splitter->setSizes({1200, sidebar});
    setCentralWidget(splitter);
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
        this, QStringLiteral("Chọn tệp audio hoặc video"), QString(),
        QStringLiteral("Âm thanh / video (*.wav *.m4a *.mp3 *.aac *.flac *.ogg *.mp4 *.mkv "
                       "*.mov *.avi);;WAV (*.wav);;Tất cả (*)"));
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
    // Driven by the action's own checked state now that the toolbar button
    // and the dock's close box are the same switch; reading the dock instead
    // would invert it on the way in from the menu.
    const bool show = m_reviewAction->isChecked();
    LOG_INFO(applog::cat::Ui) << "action: review panel ->" << (show ? "open" : "closed");
    m_reviewDock->setVisible(show);
    if (!show)
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

void MainWindow::openSubtitles()
{
    LOG_INFO(applog::cat::Ui) << "action: open the live subtitle window";
    if (!m_subtitleWindow)
        m_subtitleWindow = new SubtitleWindow(m_controller, &m_config, this);
    m_subtitleWindow->show();
    m_subtitleWindow->raise();
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
    statusBar()->showMessage(QStringLiteral("Đã cập nhật cấu hình; đang kết nối lại Server buffer."),
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
            label += QStringLiteral(" · %1:%2")
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
    // A microphone that has stopped or is re-opening is a state the operator
    // has to act on, so it gets the danger colour; anything else stays in the
    // muted role and does not compete with the connection pill next to it.
    const bool bad = status == MicStatus::Error || status == MicStatus::DeviceReconnecting;
    m_deviceLabel->setStyleSheet(
        QStringLiteral("color:%1;%2")
            .arg(theme::color(bad ? theme::Role::Danger : theme::Role::TextMuted).name(),
                 bad ? QStringLiteral(" font-weight:700;") : QString()));

    const QString session = m_controller->sessionId();
    // Only a real id gets the monospace face: it is there to be compared with
    // a log line character by character.  The placeholder is prose and reads
    // badly with the wide fixed advance.
    m_sessionLabel->setFont(session.isEmpty() ? font() : theme::mono());
    m_sessionLabel->setText(session.isEmpty() ? QStringLiteral("chưa có phiên")
                                              : QStringLiteral("phiên %1").arg(session));
}

void MainWindow::onConnectionUpdated()
{
    const bool connected = m_controller->connected();
    const bool upstream = m_controller->upstreamReady();
    // Three states, not two.  A reachable buffer in front of a down pipeline
    // still accepts and queues audio, so calling that "mất kết nối" would tell
    // an operator to stop recording when the right answer is to keep going.
    if (!connected) {
        m_connectionPill->setText(QStringLiteral("● MẤT KẾT NỐI"));
        theme::stylePill(m_connectionPill, theme::Tone::Danger);
    } else if (!upstream) {
        m_connectionPill->setText(QStringLiteral("● ĐANG ĐỆM"));
        theme::stylePill(m_connectionPill, theme::Tone::Warn);
    } else {
        m_connectionPill->setText(QStringLiteral("● ĐÃ KẾT NỐI AI"));
        theme::stylePill(m_connectionPill, theme::Tone::Ok);
    }
    m_connectionPill->setToolTip(m_controller->connectionDetail());
}

void MainWindow::refreshActions()
{
    const bool running = m_controller->isRunning();
    const MicStatus status = m_controller->micStatus();
    m_micAction->setEnabled(!running);
    m_micAction->setText(running && status == MicStatus::Recording
                             ? QStringLiteral("Đang ghi")
                             : QStringLiteral("Ghi âm từ micro"));
    m_fileAction->setEnabled(!running);
    m_stopAction->setEnabled(running);
    const bool pausable = running
        && (status == MicStatus::Recording || status == MicStatus::Paused);
    m_pauseAction->setEnabled(pausable);
    m_pauseAction->setText(status == MicStatus::Paused ? QStringLiteral("Tiếp tục")
                                                       : QStringLiteral("Tạm dừng"));
    m_pauseAction->setIcon(theme::icon(status == MicStatus::Paused ? theme::Glyph::Record
                                                                  : theme::Glyph::Pause));
    const bool haveControlApp = !m_config.micControlApp.trimmed().isEmpty();
    m_denoiseOnAction->setEnabled(haveControlApp);
    m_denoiseOffAction->setEnabled(haveControlApp);
    m_historyAction->setEnabled(!m_controller->sessionId().isEmpty());
}

void MainWindow::refreshHighlights()
{
    m_status->setHighlights(m_controller->model().state().highlights,
                            m_controller->model().state().confThresholdPct);
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

    StatusReadout readout;
    readout.running = m_controller->isRunning();
    readout.done = state.done;
    readout.hasText = textSec > 0.0;
    readout.audioSec = audioSec;
    readout.wallSec = wallSec;
    readout.speed = speed;
    readout.speechSec = speechSec;
    readout.textSec = textSec;
    readout.wallScale = speed > 1.05 ? speed : 1.0;
    readout.accelerated = speed > 1.2;
    if (readout.hasText) {
        // Deliberately excludes trailing VAD silence: the durable ingress
        // queue is a different measurement and gets its own row, so a 0.15 s
        // word gap cannot hide a many-minute server backlog.
        readout.freshnessSec = qMax(0.0, speechSec - textSec);
        readout.rawDelaySec = qMax(0.0, audioSec - textSec);
    }
    readout.haveBacklog = m_controller->isRunning() || telemetry.sentSec > 0.0;
    readout.backlogLocalSec = telemetry.localQueueSec;
    readout.backlogServerSec = telemetry.serverQueueSec;
    readout.ackLastMs = telemetry.rpcLastMs;
    readout.ackMaxMs = telemetry.rpcMaxMs;
    readout.aiWaitMs = telemetry.aiWaitLastMs;

    // Everything below is for whoever is diagnosing the pipeline, not for
    // whoever is running the meeting, so it goes behind the fold.
    QStringList detail;
    if (readout.hasText) {
        detail << QStringLiteral("delay(raw)      %1 s")
                      .arg(readout.rawDelaySec, 0, 'f', 2);
    }
    if (readout.haveBacklog) {
        detail << QStringLiteral("ACK max         %1 ms").arg(telemetry.rpcMaxMs, 0, 'f', 0);
        detail << QStringLiteral("network+gRPC    %1 ms  (một chiều ≈%2 ms)")
                      .arg(telemetry.transportRoundTripMs, 0, 'f', 0)
                      .arg(telemetry.transportOneWayEstMs, 0, 'f', 0);
    }
    const asr::LatencyServer &server = state.latency.server;
    if (server.sumP50 > 0) {
        detail << QStringLiteral("server p50/p95  %1 / %2 ms")
                      .arg(server.sumP50, 0, 'f', 0)
                      .arg(server.sumP95, 0, 'f', 0);
        detail << QStringLiteral("  asr %1 · diar %2 · verify %3 · itn %4 ms")
                      .arg(server.asrP50, 0, 'f', 0)
                      .arg(server.diarP50, 0, 'f', 0)
                      .arg(server.verifyP50, 0, 'f', 0)
                      .arg(server.itnP50, 0, 'f', 0);
    }
    const asr::LatencyClient &client = state.latency.client;
    if (client.e2eP50 > 0) {
        detail << QStringLiteral("client p50/p95  %1 / %2 ms")
                      .arg(client.e2eP50, 0, 'f', 0)
                      .arg(client.e2eP95, 0, 'f', 0);
        detail << QStringLiteral("  prep %1 · wait %2 · parse %3 ms")
                      .arg(client.prepareP50, 0, 'f', 0)
                      .arg(client.waitP50, 0, 'f', 0)
                      .arg(client.parseP50, 0, 'f', 0);
    }
    // The model counters used to sit in the operator's line of sight, where
    // they said nothing an operator can act on.  They are still worth having
    // when a transcript looks wrong, so they moved here rather than away.
    detail << QStringLiteral("rows %1 · tạm %2 · cụm %3 · yếu %4 · làn %5 · vẽ %6/%7 %8")
                  .arg(model.rows().size())
                  .arg(model.provisionalRows().size())
                  .arg(state.nPhrases)
                  .arg(state.nLow)
                  .arg(model.lanes().size())
                  .arg(m_timeline->visibleWordCount())
                  .arg(m_timeline->totalWordCount())
                  .arg(state.done ? QStringLiteral("[XONG]") : QStringLiteral("[TRỰC TIẾP]"));
    readout.detail = detail;

    QStringList problems;
    if (!telemetry.pollError.isEmpty())
        problems << telemetry.pollError;
    if (!m_controller->errorText().isEmpty())
        problems << m_controller->errorText();
    readout.error = problems.join(QStringLiteral("\n"));

    m_status->setReadout(readout);
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
            speakerIndex = int(qHash(lane));
        // Provisional words are drawn in a lighter shade of the same speaker
        // colour rather than at reduced opacity: QTextEdit's HTML subset
        // ignores `opacity`, so the old rule rendered every word identically
        // and nothing on screen said which text was still going to change.
        QColor ink = theme::laneColor(speakerIndex);
        if (word.provisional)
            ink = theme::isDark() ? ink.darker(135) : ink.lighter(155);
        html += QStringLiteral("<span style=\"color:%1;\">%2 </span>")
                    .arg(ink.name(), word.text.toHtmlEscaped());
    }
    m_ticker->setHtml(html);
    m_ticker->moveCursor(QTextCursor::End);
}
