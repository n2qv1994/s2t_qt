#include "DiagnosticsWindow.h"

#include "LogControls.h"
#include "../core/SelfTest.h"
#include "../core/SessionController.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// Bounded so a session left running overnight at trace level cannot grow the
// widget without limit; the file sink is what keeps the full history.
const int kMaxRenderedBlocks = 5000;
const int kMaxHeldEntries = 20000;
// Trimming one entry per arriving line would memmove the whole vector on every
// line once the cap is reached.  Dropping a block at a time makes that cost
// amortised instead of per-line, which matters at trace level.
const int kTrimBlock = 2000;

// How long shutdown gives a running diagnostic to come back on its own.  A
// probe bails out after its first failed RPC (~12 s), but the full network
// suite keeps going through deadlines of up to 30 s each, so no fixed number
// is safe - hence the fallbacks in the destructor rather than a bigger wait.
const int kRunnerGraceMs = 20000;
const int kRunnerTerminateMs = 2000;

// Deliberately readable on both a light and a dark palette: these are read
// while something is going wrong, not admired.
QString colourFor(applog::Level level)
{
    switch (level) {
    case applog::Level::Trace: return QStringLiteral("#7a7a7a");
    case applog::Level::Debug: return QStringLiteral("#3a6ea5");
    case applog::Level::Info: return QStringLiteral("#1a7a2e");
    case applog::Level::Warn: return QStringLiteral("#a86400");
    case applog::Level::Error: return QStringLiteral("#c00030");
    case applog::Level::Off: break;
    }
    return QStringLiteral("#000000");
}

} // namespace

// ---------------------------------------------------------------------------
// DiagnosticsRunner
// ---------------------------------------------------------------------------

DiagnosticsRunner::DiagnosticsRunner(Job job, const QString &target, const QString &token,
                                     QObject *parent)
    : QThread(parent), m_job(job), m_target(target), m_token(token)
{
    setObjectName(QStringLiteral("diagnostics"));
}

void DiagnosticsRunner::run()
{
    QString report;
    // Redirects every out() inside SelfTest into `report` for the duration of
    // this one run; the command-line modes keep printing to stdout.
    selftest::captureReportInto(&report);
    int code = 1;
    switch (m_job) {
    case Job::Codec:
        code = selftest::runCodecTests();
        break;
    case Job::Probe:
        code = selftest::runProbe(m_target, m_token);
        break;
    case Job::Network:
        code = selftest::runNetworkTests(m_target, m_token);
        break;
    }
    selftest::captureReportInto(nullptr);
    emit completed(report, code);
}

// ---------------------------------------------------------------------------
// DiagnosticsWindow
// ---------------------------------------------------------------------------

DiagnosticsWindow::DiagnosticsWindow(SessionController *controller, AppConfig *config,
                                     QWidget *parent)
    : QWidget(parent, Qt::Window), m_controller(controller), m_config(config)
{
    setWindowTitle(QStringLiteral("Nhật ký & Chẩn đoán"));
    resize(1180, 720);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildLogTab(), QStringLiteral("Nhật ký"));
    tabs->addTab(buildDiagnosticsTab(), QStringLiteral("Chẩn đoán"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(tabs);

    // Start from what the logger already holds, so a window opened after the
    // fact still shows the run-up to whatever went wrong.
    cacheFilters();
    m_entries = applog::recent();
    recountAll();
    rerender();

    if (applog::Bus *bus = applog::bus())
        connect(bus, &applog::Bus::appended, this, &DiagnosticsWindow::onLogLine);
    if (m_controller) {
        connect(m_controller, &SessionController::connectionUpdated, this,
                &DiagnosticsWindow::onConnectionUpdated);
        onConnectionUpdated();
    }
    LOG_DEBUG(applog::cat::Ui) << "diagnostics window opened with" << m_entries.size()
                               << "buffered log lines";
}

DiagnosticsWindow::~DiagnosticsWindow()
{
    if (!m_runner)
        return;

    // This window is parented to the main window and only hidden when closed,
    // so getting here means the application is shutting down - with a
    // diagnostic still blocked on a socket somewhere.
    //
    // Two things must hold.  Nothing may reach the widget from here on, and
    // the QThread must not be deleted while it is still running: QThread's
    // destructor calls qFatal on that, which turns a stuck probe into a crash
    // on exit.
    m_runner->disconnect(this);

    if (!m_runner->wait(kRunnerGraceMs)) {
        // The same rule the RPC lanes and the session worker already follow: at
        // shutdown a stuck socket must not hold the whole application open.
        LOG_WARN(applog::cat::Ui)
            << "diagnostics thread still running after" << kRunnerGraceMs / 1000
            << "s - forcing terminate()";
        m_runner->terminate();
        m_runner->wait(kRunnerTerminateMs);
        // A terminated run never reached its own captureReportInto(nullptr),
        // so the process-wide capture is still aimed at a QString on that
        // thread's dead stack.  Clear it here or the QTextStream holding it
        // flushes into freed memory when the statics are torn down.
        selftest::captureReportInto(nullptr);
    }

    if (m_runner->isFinished()) {
        delete m_runner;
    } else {
        // Even terminate() did not take.  Detach it from the object tree so
        // ~QObject does not delete it either: leaking one thread as the process
        // exits is strictly better than aborting on the way out.
        LOG_ERROR(applog::cat::Ui) << "diagnostics thread would not stop - leaking it at exit";
        m_runner->setParent(nullptr);
    }
    m_runner = nullptr;
}

QWidget *DiagnosticsWindow::buildLogTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    // Row 1: what gets written, and where.
    auto *sinkRow = new QHBoxLayout();
    sinkRow->addWidget(new QLabel(QStringLiteral("Chế độ:"), page));
    m_mode = new QComboBox(page);
    logcontrols::fillModes(m_mode, applog::mode());
    m_mode->setToolTip(QStringLiteral(
        "Debug in ra console của cửa sổ lệnh đã mở ứng dụng; mở bằng nhấp đúp thì "
        "không thấy gì. Develop ghi ra tệp, luôn đọc lại được."));
    sinkRow->addWidget(m_mode);

    sinkRow->addSpacing(12);
    sinkRow->addWidget(new QLabel(QStringLiteral("Mức ghi:"), page));
    m_level = new QComboBox(page);
    logcontrols::fillLevels(m_level, applog::level());
    m_level->setToolTip(QStringLiteral(
        "Quyết định dòng nào được ghi ra. trace ghi từng gói audio - chỉ bật khi "
        "đang tái hiện lỗi, vì nó lấn át mọi thứ khác."));
    sinkRow->addWidget(m_level);
    sinkRow->addStretch();

    auto *clearButton = new QPushButton(QStringLiteral("Xoá màn hình"), page);
    auto *saveButton = new QPushButton(QStringLiteral("Lưu ra tệp..."), page);
    auto *folderButton = new QPushButton(QStringLiteral("Mở thư mục log"), page);
    sinkRow->addWidget(clearButton);
    sinkRow->addWidget(saveButton);
    sinkRow->addWidget(folderButton);
    layout->addLayout(sinkRow);

    // Row 2: what is shown of it.
    auto *filterRow = new QHBoxLayout();
    filterRow->addWidget(new QLabel(QStringLiteral("Hiện từ mức:"), page));
    m_minShown = new QComboBox(page);
    logcontrols::fillLevels(m_minShown, applog::Level::Trace);
    filterRow->addWidget(m_minShown);

    filterRow->addSpacing(12);
    filterRow->addWidget(new QLabel(QStringLiteral("Thành phần:"), page));
    m_category = new QComboBox(page);
    m_category->addItem(QStringLiteral("(tất cả)"), QString());
    for (const QString &name : applog::cat::all())
        m_category->addItem(name, name);
    filterRow->addWidget(m_category);

    filterRow->addSpacing(12);
    filterRow->addWidget(new QLabel(QStringLiteral("Tìm:"), page));
    m_search = new QLineEdit(page);
    m_search->setPlaceholderText(QStringLiteral("chuỗi con, VD: push_audio hoặc session"));
    filterRow->addWidget(m_search, 1);

    m_follow = new QCheckBox(QStringLiteral("Tự cuộn"), page);
    m_follow->setChecked(true);
    filterRow->addWidget(m_follow);
    m_hold = new QCheckBox(QStringLiteral("Tạm giữ"), page);
    m_hold->setToolTip(QStringLiteral(
        "Ngừng vẽ dòng mới để đọc yên; log vẫn được ghi và sẽ hiện lại khi bỏ chọn."));
    filterRow->addWidget(m_hold);
    layout->addLayout(filterRow);

    m_view = new QPlainTextEdit(page);
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(kMaxRenderedBlocks);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setStyleSheet(QStringLiteral("font-family:monospace; font-size:11px;"));
    layout->addWidget(m_view, 1);

    auto *footer = new QHBoxLayout();
    m_logPath = new QLabel(page);
    m_logPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_logPath->setWordWrap(true);
    footer->addWidget(m_logPath, 1);
    m_counters = new QLabel(page);
    m_counters->setStyleSheet(QStringLiteral("font-family:monospace;"));
    footer->addWidget(m_counters);
    layout->addLayout(footer);

    connect(m_mode, &QComboBox::currentIndexChanged, this, &DiagnosticsWindow::onModeChanged);
    connect(m_level, &QComboBox::currentIndexChanged, this, &DiagnosticsWindow::onLevelChanged);
    connect(m_minShown, &QComboBox::currentIndexChanged, this,
            &DiagnosticsWindow::onFilterChanged);
    connect(m_category, &QComboBox::currentIndexChanged, this,
            &DiagnosticsWindow::onFilterChanged);
    connect(m_search, &QLineEdit::textChanged, this, &DiagnosticsWindow::onFilterChanged);
    connect(m_hold, &QCheckBox::toggled, this, &DiagnosticsWindow::onFilterChanged);
    connect(clearButton, &QPushButton::clicked, this, &DiagnosticsWindow::clearLog);
    connect(saveButton, &QPushButton::clicked, this, &DiagnosticsWindow::saveLogAs);
    connect(folderButton, &QPushButton::clicked, this, &DiagnosticsWindow::openLogFolder);

    refreshLogPath();
    return page;
}

QWidget *DiagnosticsWindow::buildDiagnosticsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *server = new QGroupBox(QStringLiteral("Máy chủ AI"), page);
    auto *grid = new QGridLayout(server);
    grid->addWidget(new QLabel(QStringLiteral("Địa chỉ (host:port)"), server), 0, 0);
    m_target = new QLineEdit(m_config ? m_config->serverTarget : QString(), server);
    grid->addWidget(m_target, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Bearer token"), server), 1, 0);
    m_token = new QLineEdit(m_config ? m_config->apiToken : QString(), server);
    m_token->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    grid->addWidget(m_token, 1, 1);

    m_connectionLabel = new QLabel(server);
    m_connectionLabel->setWordWrap(true);
    grid->addWidget(m_connectionLabel, 2, 0, 1, 2);
    layout->addWidget(server);

    auto *buttons = new QHBoxLayout();
    m_recheckButton = new QPushButton(QStringLiteral("Kiểm tra lại kết nối"), page);
    m_recheckButton->setToolTip(QStringLiteral(
        "Gọi get_model_status một lần bằng cấu hình đang chạy - cùng thứ làm cho "
        "đèn báo trên thanh công cụ xanh hay đỏ."));
    m_probeButton = new QPushButton(QStringLiteral("Probe máy chủ"), page);
    m_probeButton->setToolTip(QStringLiteral(
        "Gọi thật các RPC chỉ-đọc với địa chỉ và token ở trên: model status, danh "
        "sách phiên, trạng thái DB giọng nói. Trả lời được câu \"server có sống và "
        "token có được chấp nhận không\" mà không tạo phiên nào."));
    m_codecButton = new QPushButton(QStringLiteral("Self-test giao thức"), page);
    m_codecButton->setToolTip(QStringLiteral(
        "Kiểm tra bộ mã proto3 và HPACK tự viết bằng các phép round-trip. Không "
        "chạm tới mạng, nên vẫn chạy được khi mất kết nối."));
    m_networkButton = new QPushButton(QStringLiteral("Test mạng đầy đủ"), page);
    m_networkButton->setToolTip(QStringLiteral(
        "Bộ khẳng định đầu-cuối, cần tools/mock_adapter.js đang chạy tại địa chỉ ở "
        "trên. Dành cho phát triển, không dùng với server thật."));
    buttons->addWidget(m_recheckButton);
    buttons->addWidget(m_probeButton);
    buttons->addWidget(m_codecButton);
    buttons->addWidget(m_networkButton);
    buttons->addStretch();
    m_copyButton = new QPushButton(QStringLiteral("Copy báo cáo"), page);
    buttons->addWidget(m_copyButton);
    layout->addLayout(buttons);

    m_jobStatus = new QLabel(QStringLiteral("Chưa chạy chẩn đoán nào."), page);
    m_jobStatus->setWordWrap(true);
    layout->addWidget(m_jobStatus);

    m_report = new QPlainTextEdit(page);
    m_report->setReadOnly(true);
    m_report->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_report->setStyleSheet(QStringLiteral("font-family:monospace; font-size:11px;"));
    layout->addWidget(m_report, 1);

    connect(m_recheckButton, &QPushButton::clicked, this, [this]() {
        if (!m_controller)
            return;
        LOG_INFO(applog::cat::Ui) << "action: manual connection re-check";
        m_controller->refreshConnection();
        m_jobStatus->setText(QStringLiteral("Đang kiểm tra kết nối..."));
    });
    connect(m_probeButton, &QPushButton::clicked, this, &DiagnosticsWindow::startProbe);
    connect(m_codecButton, &QPushButton::clicked, this, &DiagnosticsWindow::startCodecTest);
    connect(m_networkButton, &QPushButton::clicked, this, &DiagnosticsWindow::startNetworkTest);
    connect(m_copyButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_report->toPlainText());
        m_jobStatus->setText(QStringLiteral("Đã copy báo cáo vào clipboard."));
    });

    return page;
}

// ---- log tab --------------------------------------------------------------

void DiagnosticsWindow::cacheFilters()
{
    m_floor = logcontrols::selectedLevel(m_minShown);
    m_categoryFilter = m_category->currentData().toString();
    m_searchFilter = m_search->text().trimmed();
}

bool DiagnosticsWindow::passesFilter(const applog::Entry &entry) const
{
    if (entry.level < m_floor)
        return false;
    if (!m_categoryFilter.isEmpty() && entry.category != m_categoryFilter)
        return false;
    return m_searchFilter.isEmpty()
        || entry.line.contains(m_searchFilter, Qt::CaseInsensitive);
}

void DiagnosticsWindow::appendToView(const applog::Entry &entry)
{
    m_view->appendHtml(QStringLiteral("<span style=\"color:%1;white-space:pre\">%2</span>")
                           .arg(colourFor(entry.level), entry.line.toHtmlEscaped()));
}

void DiagnosticsWindow::scrollToTail()
{
    if (!m_follow->isChecked())
        return;
    m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
    // appendHtml() leaves the cursor at the end of the line it just wrote, and
    // with wrapping off the view slides sideways to keep it visible - far
    // enough to clip the timestamp column, which is the first thing anyone
    // reads.  Only forced back while following the tail; someone who scrolled
    // right to read the end of a line has turned that off.
    m_view->horizontalScrollBar()->setValue(m_view->horizontalScrollBar()->minimum());
}

void DiagnosticsWindow::rerender()
{
    m_view->clear();
    // The widget keeps only the last kMaxRenderedBlocks anyway, so walk back
    // from the end until that many lines pass the filter and render from
    // there - appending thousands of blocks just to have them dropped again
    // is the slow way to the same screen.
    int first = m_entries.size();
    int shown = 0;
    while (first > 0 && shown < kMaxRenderedBlocks) {
        --first;
        if (passesFilter(m_entries.at(first)))
            ++shown;
    }
    for (int i = first; i < m_entries.size(); ++i) {
        if (passesFilter(m_entries.at(i)))
            appendToView(m_entries.at(i));
    }
    scrollToTail();
    refreshCounters();
}

void DiagnosticsWindow::recountAll()
{
    m_warnings = 0;
    m_errors = 0;
    for (const applog::Entry &entry : m_entries)
        countEntry(entry, +1);
}

void DiagnosticsWindow::countEntry(const applog::Entry &entry, int delta)
{
    if (entry.level == applog::Level::Warn)
        m_warnings += delta;
    else if (entry.level == applog::Level::Error)
        m_errors += delta;
}

void DiagnosticsWindow::refreshCounters()
{
    // Kept incrementally rather than recounted: at trace level this label
    // would otherwise walk twenty thousand entries several times a second.
    m_counters->setText(QStringLiteral("%1 dòng · %2 cảnh báo · %3 lỗi")
                            .arg(m_entries.size())
                            .arg(m_warnings)
                            .arg(m_errors));
}

void DiagnosticsWindow::refreshLogPath()
{
    const QString path = applog::logFilePath();
    if (applog::mode() == applog::Mode::Develop && !path.isEmpty())
        m_logPath->setText(QStringLiteral("Tệp log: %1").arg(path));
    else if (applog::mode() == applog::Mode::Develop)
        m_logPath->setText(QStringLiteral("Thư mục log: %1").arg(applog::logDirectory()));
    else
        m_logPath->setText(QStringLiteral("Chế độ debug — log ra console, không ghi tệp."));
}

void DiagnosticsWindow::onLogLine(const applog::Entry &entry)
{
    m_entries.append(entry);
    countEntry(entry, +1);
    if (m_entries.size() > kMaxHeldEntries + kTrimBlock) {
        for (int i = 0; i < kTrimBlock; ++i)
            countEntry(m_entries.at(i), -1);
        m_entries.remove(0, kTrimBlock);
    }
    if (m_hold->isChecked())
        return;
    if (passesFilter(entry)) {
        appendToView(entry);
        scrollToTail();
    }
    refreshCounters();
}

void DiagnosticsWindow::onModeChanged()
{
    const applog::Mode chosen = logcontrols::selectedMode(m_mode);
    applog::setMode(chosen);
    if (m_config) {
        // Persisted the same way the settings dialog does it, so the choice
        // survives the restart that usually follows a repro attempt.
        m_config->logMode = chosen;
        m_config->save();
    }
    refreshLogPath();
}

void DiagnosticsWindow::onLevelChanged()
{
    const applog::Level chosen = logcontrols::selectedLevel(m_level);
    applog::setLevel(chosen);
    if (m_config) {
        m_config->logLevel = chosen;
        m_config->save();
    }
    LOG_INFO(applog::cat::Ui) << "log level ->" << applog::levelName(chosen);
}

void DiagnosticsWindow::onFilterChanged()
{
    cacheFilters();
    rerender();
}

void DiagnosticsWindow::clearLog()
{
    m_entries.clear();
    applog::clearRecent();
    m_view->clear();
    recountAll();
    refreshCounters();
}

void DiagnosticsWindow::saveLogAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Lưu nhật ký"), QStringLiteral("s2t_qt-log.txt"),
        QStringLiteral("Text (*.txt *.log)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Lưu nhật ký"),
                             QStringLiteral("Không ghi được %1: %2").arg(path, file.errorString()));
        return;
    }
    // What is saved is what is on screen: the filter the operator set is
    // usually the reason they are saving it in the first place.
    for (const applog::Entry &entry : m_entries) {
        if (!passesFilter(entry))
            continue;
        file.write(entry.line.toUtf8());
        file.write("\n");
    }
    file.close();
    LOG_INFO(applog::cat::Ui) << "log saved to" << path;
    m_logPath->setText(QStringLiteral("Đã lưu: %1").arg(path));
}

void DiagnosticsWindow::openLogFolder()
{
    const QString dir = applog::logDirectory();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        QMessageBox::information(this, QStringLiteral("Thư mục log"),
                                 QStringLiteral("Không mở được trình quản lý tệp.\n\n%1").arg(dir));
    }
}

// ---- diagnostics tab ------------------------------------------------------

void DiagnosticsWindow::startCodecTest()
{
    startJob(DiagnosticsRunner::Job::Codec, QStringLiteral("self-test giao thức"));
}

void DiagnosticsWindow::startProbe()
{
    startJob(DiagnosticsRunner::Job::Probe, QStringLiteral("probe máy chủ"));
}

void DiagnosticsWindow::startNetworkTest()
{
    startJob(DiagnosticsRunner::Job::Network, QStringLiteral("test mạng đầy đủ"));
}

void DiagnosticsWindow::startJob(DiagnosticsRunner::Job job, const QString &title)
{
    // One at a time: SelfTest's report capture is process-wide.
    if (m_runner && !m_runner->isFinished())
        return;
    // The previous runner is kept alive until here rather than deleting itself
    // when it finished - see onJobCompleted.  It has exited, so this is safe.
    delete m_runner;
    m_runner = nullptr;

    const QString target = m_target->text().trimmed();
    if (job != DiagnosticsRunner::Job::Codec && target.isEmpty()) {
        m_jobStatus->setText(QStringLiteral("Cần địa chỉ máy chủ dạng host:port."));
        return;
    }

    LOG_INFO(applog::cat::Ui) << "action: running diagnostics -" << title
                              << (job == DiagnosticsRunner::Job::Codec ? QString() : target);
    m_report->clear();
    m_jobStatus->setText(QStringLiteral("Đang chạy %1...").arg(title));
    setJobRunning(true);

    m_runner = new DiagnosticsRunner(job, target, m_token->text().trimmed(), this);
    connect(m_runner, &DiagnosticsRunner::completed, this, &DiagnosticsWindow::onJobCompleted);
    m_runner->start();
}

void DiagnosticsWindow::onJobCompleted(const QString &report, int code)
{
    m_report->setPlainText(report);
    m_jobStatus->setText(code == 0
                             ? QStringLiteral("✔ Xong — tất cả các phép kiểm tra đều đạt.")
                             : QStringLiteral("✖ Xong — có mục không đạt (mã thoát %1). "
                                              "Đọc các dòng FAIL bên dưới.")
                                   .arg(code));
    LOG_INFO(applog::cat::Ui) << "diagnostics finished with code" << code;
    setJobRunning(false);
    // The runner is deliberately not deleted here.  This slot runs on the
    // completed() signal, which run() emits as its last statement - the thread
    // has not actually exited yet, so a deleteLater() posted now could be
    // processed while it is still on its way out, and deleting a live QThread
    // aborts.  It is kept until the next run replaces it (startJob) or the
    // window goes away; a finished QThread object costs nothing to hold.
}

void DiagnosticsWindow::setJobRunning(bool running)
{
    m_codecButton->setEnabled(!running);
    m_probeButton->setEnabled(!running);
    m_networkButton->setEnabled(!running);
}

void DiagnosticsWindow::onConnectionUpdated()
{
    if (!m_controller || !m_connectionLabel)
        return;
    m_connectionLabel->setText(m_controller->connected()
                                   ? QStringLiteral("● Đang kết nối: %1")
                                         .arg(m_controller->connectionDetail())
                                   : QStringLiteral("● Không kết nối được: %1")
                                         .arg(m_controller->connectionDetail()));
}
