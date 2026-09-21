#include "EnrollDialog.h"

#include "Theme.h"

#include "audio/AudioCapture.h"
#include "audio/MediaDecode.h"
#include "audio/WavIo.h"
#include "core/Logger.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// The four status tones, taken from the shared palette instead of from four
// hand-picked Material colours: those were chosen for a light desktop and
// painted dark text on a light plate no matter what scheme the operator's
// machine was running.
QString statusStyle(const QString &kind)
{
    theme::Tone tone = theme::Tone::Neutral;
    if (kind == QLatin1String("ok"))
        tone = theme::Tone::Ok;
    else if (kind == QLatin1String("err"))
        tone = theme::Tone::Danger;
    else if (kind == QLatin1String("warn"))
        tone = theme::Tone::Warn;
    else if (kind == QLatin1String("busy"))
        tone = theme::Tone::Info;
    else
        return QString();

    theme::Role ink = theme::Role::TextMuted;
    theme::Role plate = theme::Role::SurfaceSunken;
    switch (tone) {
    case theme::Tone::Ok:      ink = theme::Role::Ok;     plate = theme::Role::OkSoft;     break;
    case theme::Tone::Warn:    ink = theme::Role::Warn;   plate = theme::Role::WarnSoft;   break;
    case theme::Tone::Danger:  ink = theme::Role::Danger; plate = theme::Role::DangerSoft; break;
    case theme::Tone::Info:    ink = theme::Role::Accent; plate = theme::Role::AccentSoft; break;
    case theme::Tone::Neutral: break;
    }
    return QStringLiteral("background:%1; color:%2; border-radius:6px; padding:8px 10px;")
        .arg(theme::color(plate).name(), theme::color(ink).name());
}

// The global registry sends ISO-8601 in UTC ("2026-08-19T07:39:01Z"), unlike
// every other timestamp in this contract, which is a double.  Rendered as
// local date + minutes: the column answers "when was this last touched", and
// the full string only gets elided at the T, which hides the time and keeps
// none of the width it cost.  An unparseable value is shown as it arrived
// rather than blanked - a date nobody can read still beats an empty cell.
QString shortTimestamp(const QString &iso)
{
    if (iso.trimmed().isEmpty())
        return QString();
    const QDateTime parsed = QDateTime::fromString(iso, Qt::ISODate);
    if (!parsed.isValid())
        return iso;
    return parsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString kindLabel(const QString &kind)
{
    if (kind == QLatin1String("urgent"))
        return QStringLiteral("cấp bách");
    if (kind == QLatin1String("legacy"))
        return QStringLiteral("dữ liệu cũ");
    return QStringLiteral("từ phiên họp");
}

QString statusLabel(const QString &status)
{
    if (status == QLatin1String("pending"))
        return QStringLiteral("chưa quyết định");
    if (status == QLatin1String("session_only"))
        return QStringLiteral("chỉ dùng trong phiên");
    if (status == QLatin1String("global_shared"))
        return QStringLiteral("đã publish global");
    if (status == QLatin1String("publish_failed"))
        return QStringLiteral("publish lỗi");
    return status;
}

} // namespace

EnrollDialog::EnrollDialog(SessionController *controller, const QString &editorId, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_editorId(editorId)
{
    setWindowTitle(QStringLiteral("Đăng ký giọng nói (CAM++)"));
    // Size last - see theme::sizeToContent() at the end of this constructor.

    auto *layout = new QVBoxLayout(this);
    auto *header = new QLabel(
        m_editorId.isEmpty()
            ? QStringLiteral("<b style='color:%1;'>Chưa nhập tên người thao tác</b> — "
                             "nhập ở thanh dưới của cửa sổ chính trước khi ghi âm hoặc lưu.")
                  .arg(theme::color(theme::Role::Danger).name())
            : QStringLiteral("Người thao tác: <b>%1</b>").arg(m_editorId.toHtmlEscaped()),
        this);
    header->setWordWrap(true);
    layout->addWidget(header);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildEnrollTab(), QStringLiteral("Đăng ký giọng"));
    tabs->addTab(buildSessionTab(), QStringLiteral("Người nói trong phiên"));
    tabs->addTab(buildGlobalTab(), QStringLiteral("DB giọng chung"));
    layout->addWidget(tabs, 1);

    // For the enrolment preview.  Built here rather than in the tab so the
    // buffer outlives any one playback: setSourceDevice does not take
    // ownership, and a device that goes away under the player is a crash.
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    m_audioBuffer = new QBuffer(this);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_timerLabel->setText(QStringLiteral("%1s").arg(m_clock.elapsed() / 1000.0, 0, 'f', 1));
    });

    loadScript();
    loadRoster();

    theme::sizeToContent(this, QSize(880, 720));
}

EnrollDialog::~EnrollDialog()
{
    if (m_capture)
        m_capture->stop();
}

QWidget *EnrollDialog::buildEnrollTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_guidance = new QLabel(
        QStringLiteral("Đọc to, rõ ràng đoạn văn bản bên dưới, giữ khoảng cách đều với micro."),
        page);
    m_guidance->setWordWrap(true);
    layout->addWidget(m_guidance);

    m_allowBelow = new QCheckBox(
        QStringLiteral("Chế độ cấp bách: chấp nhận bản ghi ngắn hơn yêu cầu "
                       "(giọng vẫn dùng được, nhưng bị đánh dấu là cần đăng ký lại)"),
        page);
    layout->addWidget(m_allowBelow);

    m_belowPolicy = new QListWidget(page);
    // Sized from the font rather than in pixels, and hidden until it has
    // something to say: an empty framed box in the middle of the dialog reads
    // as a control that failed to load.
    m_belowPolicy->setMaximumHeight(m_belowPolicy->fontMetrics().height() * 6);
    m_belowPolicy->setWordWrap(true);
    m_belowPolicy->setVisible(false);
    layout->addWidget(m_belowPolicy);

    m_script = new QTextEdit(page);
    m_script->setReadOnly(true);
    m_script->setPlainText(QStringLiteral("Đang tải đoạn văn bản..."));
    {
        // This is the text somebody reads aloud into a microphone, so it is
        // set large - but as a multiple of the desktop font rather than at
        // 16 px, which is a different physical size on each of the two kits.
        QFont reading = m_script->font();
        reading.setPointSizeF(qMax(11.0, reading.pointSizeF() * 1.55));
        m_script->setFont(reading);
    }
    m_script->setStyleSheet(QStringLiteral("background:%1;")
                                .arg(theme::color(theme::Role::SurfaceAlt).name()));
    layout->addWidget(m_script, 1);

    auto *form = new QFormLayout();
    m_speakerName = new QLineEdit(page);
    m_speakerName->setMaxLength(64);
    m_speakerName->setPlaceholderText(QStringLiteral("Ví dụ: Nguyễn Văn A"));
    form->addRow(QStringLiteral("Tên người nói"), m_speakerName);
    layout->addLayout(form);

    auto *row = new QHBoxLayout();
    m_recordButton = new QPushButton(QStringLiteral("Bắt đầu ghi âm"), page);
    theme::markPrimary(m_recordButton);
    m_timerLabel = new QLabel(QStringLiteral("0.0s"), page);
    m_timerLabel->setFont(theme::mono());
    m_fileButton = new QPushButton(QStringLiteral("Nạp từ tệp…"), page);
    m_retryButton = new QPushButton(QStringLiteral("Gửi lại bản ghi"), page);
    m_retryButton->setVisible(false);
    // The answer to the one-way problem.  An enrolment cannot be undone
    // through any API, and the failure that matters - two people in one
    // recording - produces a blended voice that mis-names people in later
    // meetings without ever reporting an error.  This asks the same question
    // without committing: same VAD trim, nothing written, and the audio comes
    // back so it can be listened to.
    m_previewButton = new QPushButton(QStringLiteral("Nghe thử (không ghi vào DB)"), page);
    row->addWidget(m_recordButton);
    row->addWidget(m_fileButton);
    row->addWidget(m_previewButton);
    row->addWidget(m_timerLabel);
    row->addWidget(m_retryButton);
    row->addStretch();
    layout->addLayout(row);

    m_previewInfo = new QLabel(page);
    m_previewInfo->setWordWrap(true);
    m_previewInfo->setVisible(false);
    layout->addWidget(m_previewInfo);

    m_status = new QLabel(page);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *rosterGroup = new QGroupBox(QStringLiteral("Danh sách giọng toàn cục"), page);
    auto *rosterLayout = new QVBoxLayout(rosterGroup);
    auto *reload = new QPushButton(QStringLiteral("Tải lại"), rosterGroup);
    // In a QVBoxLayout a button fills the whole width, which reads as the
    // group's banner rather than as something to press.  Put it on its own row
    // with the slack after it.
    auto *reloadRow = new QHBoxLayout();
    reloadRow->addWidget(reload);
    reloadRow->addStretch(1);
    rosterLayout->addLayout(reloadRow);
    m_roster = new QListWidget(rosterGroup);
    rosterLayout->addWidget(m_roster);
    layout->addWidget(rosterGroup);

    connect(m_recordButton, &QPushButton::clicked, this, &EnrollDialog::toggleRecording);
    connect(m_fileButton, &QPushButton::clicked, this, &EnrollDialog::loadFromFile);
    connect(m_previewButton, &QPushButton::clicked, this, &EnrollDialog::previewRecording);
    connect(m_retryButton, &QPushButton::clicked, this, [this]() {
        m_recorded = m_pendingWav;
        m_speakerName->setText(m_pendingName);
        submitRecording();
    });
    connect(reload, &QPushButton::clicked, this, &EnrollDialog::loadRoster);
    return page;
}

QWidget *EnrollDialog::buildSessionTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *intro = new QLabel(
        QStringLiteral("Xem các giọng CAM++ đã tự phân cụm trong một phiên, rồi chọn giữ riêng "
                       "cho phiên đó (SESSION_ONLY) hoặc publish vào DB chung (GLOBAL_SHARED). "
                       "GLOBAL_SHARED chỉ chạy được sau khi phiên đã kết thúc."),
        page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *row = new QHBoxLayout();
    m_sessionInput = new QLineEdit(page);
    m_sessionInput->setPlaceholderText(QStringLiteral("session_id"));
    m_sessionInput->setText(m_controller->sessionId());
    auto *load = new QPushButton(QStringLiteral("Tải danh sách"), page);
    row->addWidget(m_sessionInput, 1);
    row->addWidget(load);
    layout->addLayout(row);

    m_registryStatus = new QLabel(page);
    m_registryStatus->setWordWrap(true);
    layout->addWidget(m_registryStatus);

    m_speakers = new QTableWidget(0, 5, page);
    m_speakers->setHorizontalHeaderLabels({QStringLiteral("Giọng"), QStringLiteral("Trạng thái"),
                                           QStringLiteral("Bằng chứng"), QStringLiteral("Đích"),
                                           QStringLiteral("Tên global")});
    m_speakers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_speakers->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_speakers->verticalHeader()->setVisible(false);
    layout->addWidget(m_speakers, 1);

    auto *save = new QPushButton(QStringLiteral("Lưu lựa chọn"), page);
    layout->addWidget(save);
    m_saveResults = new QLabel(page);
    m_saveResults->setWordWrap(true);
    layout->addWidget(m_saveResults);

    connect(load, &QPushButton::clicked, this, &EnrollDialog::loadSessionSpeakers);
    connect(save, &QPushButton::clicked, this, &EnrollDialog::saveSelections);
    return page;
}

QWidget *EnrollDialog::buildGlobalTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *intro = new QLabel(
        QStringLiteral(
            "Mọi giọng trong DB chung, kể cả các giọng đã ngưng dùng hoặc đã xoá. Đây là "
            "danh sách mà bộ xác thực đem ra so khớp với từng cuộc họp: một giọng rác trong "
            "này là một cái tên sai chờ sẵn. Mỗi thay đổi đều ghi nhật ký kèm tên người thao "
            "tác và lý do."),
        page);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *row = new QHBoxLayout();
    auto *reload = new QPushButton(QStringLiteral("Tải lại"), page);
    row->addWidget(reload);
    row->addStretch(1);
    layout->addLayout(row);

    m_globalStatus = new QLabel(page);
    m_globalStatus->setWordWrap(true);
    layout->addWidget(m_globalStatus);

    m_global = new QTableWidget(0, 7, page);
    m_global->setHorizontalHeaderLabels(
        {QStringLiteral("spk_id"), QStringLiteral("Tên"), QStringLiteral("Trạng thái"),
         QStringLiteral("Mẫu"), QStringLiteral("Dùng được"), QStringLiteral("Cập nhật"),
         QStringLiteral("Người duyệt / lý do")});
    m_global->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_global->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_global->verticalHeader()->setVisible(false);
    // Whole rows, one at a time: every action below works on exactly one
    // speaker, and a multi-select here would invite a bulk delete.
    m_global->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_global->setSelectionMode(QAbstractItemView::SingleSelection);
    m_global->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_global, 1);

    auto *form = new QFormLayout();
    m_globalReason = new QLineEdit(page);
    m_globalReason->setMaxLength(200);
    m_globalReason->setPlaceholderText(
        QStringLiteral("Ví dụ: mẫu thử nghiệm, thu nhầm hai người"));
    form->addRow(QStringLiteral("Lý do"), m_globalReason);
    layout->addLayout(form);

    auto *actions = new QHBoxLayout();
    m_deactivateButton = new QPushButton(QStringLiteral("Ngưng dùng"), page);
    m_activateButton = new QPushButton(QStringLiteral("Dùng lại"), page);
    m_deleteButton = new QPushButton(QStringLiteral("Xoá vĩnh viễn…"), page);
    // No dedicated "danger" button style exists in the theme, and inventing
    // one here would drift from it.  Colouring the label is enough to stop
    // this being the button somebody presses by muscle memory.
    m_deleteButton->setStyleSheet(
        QStringLiteral("color:%1;").arg(theme::color(theme::Role::Danger).name()));
    actions->addWidget(m_deactivateButton);
    actions->addWidget(m_activateButton);
    actions->addStretch(1);
    actions->addWidget(m_deleteButton);
    layout->addLayout(actions);

    auto *note = new QLabel(
        QStringLiteral(
            "<b>Ngưng dùng</b> là thao tác nên dùng: giọng thôi được đem ra so khớp nhưng vẫn "
            "còn đó, và <b>Dùng lại</b> đưa nó trở lại. <b>Xoá vĩnh viễn</b> thì không có "
            "đường về từ đây."),
        page);
    note->setWordWrap(true);
    layout->addWidget(note);

    connect(reload, &QPushButton::clicked, this, &EnrollDialog::loadGlobalSpeakers);
    connect(m_deactivateButton, &QPushButton::clicked, this,
            [this]() { runGlobalAction(QStringLiteral("deactivate")); });
    connect(m_activateButton, &QPushButton::clicked, this,
            [this]() { runGlobalAction(QStringLiteral("activate")); });
    connect(m_deleteButton, &QPushButton::clicked, this,
            [this]() { runGlobalAction(QStringLiteral("delete")); });
    return page;
}

QString EnrollDialog::selectedGlobalSpeaker(QString *name) const
{
    const int row = m_global->currentRow();
    if (row < 0 || !m_global->item(row, 0))
        return QString();
    if (name && m_global->item(row, 1))
        *name = m_global->item(row, 1)->text();
    return m_global->item(row, 0)->text();
}

void EnrollDialog::loadGlobalSpeakers()
{
    m_globalStatus->setText(QStringLiteral("Đang tải..."));
    m_controller->rpc()->call<reg::ListGlobalSpeakersResponse>(
        this,
        [](AsrClient &client, reg::ListGlobalSpeakersResponse &out) {
            return client.listGlobalSpeakers(&out, 30000);
        },
        [this](const grpc::Status &status, const reg::ListGlobalSpeakersResponse &response) {
            if (!status.ok()) {
                m_globalStatus->setText(
                    QStringLiteral("Không tải được: %1").arg(status.toString()));
                return;
            }
            m_global->setRowCount(0);
            int active = 0;
            for (const reg::GlobalSpeakerEntry &entry : response.speakers) {
                const int row = m_global->rowCount();
                m_global->insertRow(row);
                const auto cell = [this, row](int column, const QString &text) {
                    m_global->setItem(row, column, new QTableWidgetItem(text));
                };
                cell(0, entry.spkId);
                cell(1, entry.spkName);
                cell(2, entry.status);
                cell(3, QString::number(entry.sampleCount));
                cell(4, QString::number(entry.usableSampleCount));
                // "2026-08-19T07:39:01Z" is too wide for the column and gets
                // elided at the T, which hides the time and keeps none of the
                // width it cost.  Shown as local date + minutes instead - the
                // question this column answers is "when was this last touched",
                // and seconds never mattered to it.
                cell(5, shortTimestamp(entry.lastUpdated));
                cell(6, entry.reviewReason.isEmpty()
                            ? entry.reviewedBy
                            : QStringLiteral("%1 — %2").arg(entry.reviewedBy, entry.reviewReason));
                if (entry.status == QLatin1String("approved")) {
                    ++active;
                } else if (m_global->item(row, 2)) {
                    // A tombstone is listed but must not read like a live row.
                    m_global->item(row, 2)->setForeground(theme::color(theme::Role::TextMuted));
                }
            }
            m_globalStatus->setText(
                QStringLiteral("%1 giọng, %2 đang dùng được cho nhận dạng.")
                    .arg(response.speakers.size())
                    .arg(active));
            LOG_INFO(applog::cat::Ui) << "ListGlobalSpeakers:" << response.speakers.size()
                                      << "entries," << active << "approved";
        });
}

void EnrollDialog::runGlobalAction(const QString &verb)
{
    QString name;
    const QString spkId = selectedGlobalSpeaker(&name);
    if (spkId.isEmpty()) {
        m_globalStatus->setText(QStringLiteral("Chưa chọn giọng nào trong bảng."));
        return;
    }
    if (m_editorId.isEmpty()) {
        m_globalStatus->setText(QStringLiteral(
            "Tên người thao tác bị trống - nhập ở thanh dưới cửa sổ chính trước đã. "
            "Mọi thay đổi trên DB chung đều phải có người chịu trách nhiệm."));
        return;
    }
    const QString reason = m_globalReason->text().trimmed();
    const bool destructive = verb != QLatin1String("activate");
    if (destructive && reason.isEmpty()) {
        m_globalStatus->setText(
            QStringLiteral("Thao tác này cần một lý do - nhập vào ô Lý do rồi bấm lại."));
        return;
    }

    // The confirmation is proportional to what the action costs.  Deactivate
    // is reversible from the button next to it, so it asks once; delete is not
    // reversible from here at all, so it makes the operator type the id.
    if (verb == QLatin1String("delete")) {
        bool typed = false;
        const QString answer = QInputDialog::getText(
            this, QStringLiteral("Xoá vĩnh viễn một giọng"),
            QStringLiteral("Sắp xoá <b>%1</b> (%2) khỏi DB chung.<br><br>"
                           "Thao tác này <b>không hoàn tác được</b> từ ứng dụng — muốn lấy lại "
                           "thì phải khôi phục thủ công trên máy chủ.<br>"
                           "Nếu chỉ muốn nó thôi được đem ra so khớp, hãy bấm "
                           "<b>Ngưng dùng</b> thay vì xoá.<br><br>"
                           "Gõ lại <b>%2</b> để xác nhận:")
                .arg(name.toHtmlEscaped(), spkId.toHtmlEscaped()),
            QLineEdit::Normal, QString(), &typed);
        if (!typed || answer.trimmed() != spkId)
            return;
    } else if (verb == QLatin1String("deactivate")) {
        const auto choice = QMessageBox::question(
            this, QStringLiteral("Ngưng dùng một giọng"),
            QStringLiteral("Ngưng dùng %1 (%2)?\n\nGiọng vẫn còn trong DB và có thể bật lại "
                           "bằng \"Dùng lại\"; nó chỉ thôi được đem ra so khớp.")
                .arg(name, spkId));
        if (choice != QMessageBox::Yes)
            return;
    }

    reg::GlobalSpeakerActionRequest request;
    request.spkId = spkId;
    request.editorId = m_editorId;
    request.reason = reason;

    m_deactivateButton->setEnabled(false);
    m_activateButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    m_globalStatus->setText(QStringLiteral("Đang gửi..."));
    LOG_INFO(applog::cat::Ui) << "global speaker" << verb << spkId << "by" << m_editorId
                              << "reason=" << reason;

    m_controller->rpc()->call<reg::GlobalSpeakerActionResponse>(
        this,
        [request, verb](AsrClient &client, reg::GlobalSpeakerActionResponse &out) {
            // Deactivate and delete rebuild the CAM++ database afterwards, the
            // same pass EnrollSpeaker pays for, so the deadline matches it.
            if (verb == QLatin1String("deactivate"))
                return client.deactivateGlobalSpeaker(request, &out, 120000);
            if (verb == QLatin1String("delete"))
                return client.deleteGlobalSpeaker(request, &out, 120000);
            return client.activateGlobalSpeaker(request, &out, 120000);
        },
        [this, spkId](const grpc::Status &status,
                      const reg::GlobalSpeakerActionResponse &response) {
            m_deactivateButton->setEnabled(true);
            m_activateButton->setEnabled(true);
            m_deleteButton->setEnabled(true);
            if (!status.ok()) {
                m_globalStatus->setText(
                    QStringLiteral("Không gửi được: %1").arg(status.toString()));
                return;
            }
            if (!response.ok) {
                m_globalStatus->setText(QStringLiteral("Máy chủ từ chối: %1").arg(response.error));
                return;
            }
            // "changed" told apart from "ok" on purpose: deactivating a
            // speaker who is already inactive succeeds and does nothing, and
            // reporting that as done would be a small lie.
            QString text = response.changed
                ? QStringLiteral("Xong: %1 giờ ở trạng thái <b>%2</b>.")
                      .arg(spkId, response.status)
                : QStringLiteral("Không có gì thay đổi: %1 vốn đã ở trạng thái <b>%2</b>.")
                      .arg(spkId, response.status);
            if (response.samplesRetired > 0)
                text += QStringLiteral(" %1 mẫu đã gỡ khỏi DB nhận dạng.")
                            .arg(response.samplesRetired);
            if (!response.message.isEmpty())
                text += QStringLiteral("<br>%1").arg(response.message.toHtmlEscaped());
            m_globalStatus->setText(text);
            m_globalReason->clear();
            // Re-read rather than patching the row in place: the far side
            // decides the resulting status, and guessing it here is how a
            // table starts disagreeing with the database.
            loadGlobalSpeakers();
            loadRoster();
        });
}

void EnrollDialog::setStatus(const QString &kind, const QString &text)
{
    m_status->setStyleSheet(statusStyle(kind));
    m_status->setText(text);
}

void EnrollDialog::loadScript()
{
    m_controller->rpc()->call<reg::GetEnrollmentScriptResponse>(
        this,
        [](AsrClient &client, reg::GetEnrollmentScriptResponse &out) {
            return client.getEnrollmentScript(&out, 20000);
        },
        [this](const grpc::Status &status, const reg::GetEnrollmentScriptResponse &response) {
            if (!status.ok()) {
                m_script->setPlainText(
                    QStringLiteral("(không tải được đoạn văn bản - %1)").arg(status.toString()));
                return;
            }
            m_script->setPlainText(response.scriptText);
            if (response.recommendedDurationSec > 0) {
                // Say both numbers: how long to read, and that the gate is on
                // speech after silence is trimmed - a reading that looks long
                // enough can still fail the VAD-trimmed threshold.
                m_guidance->setText(
                    QStringLiteral("Đọc to, rõ ràng đoạn văn bản bên dưới trong khoảng %1 giây, "
                                   "đọc liên tục và tránh ngắt quãng dài — sau khi trừ khoảng "
                                   "lặng phần tiếng nói mới là phần được tính. Mục tiêu: %2 đoạn "
                                   "nhúng. Giữ khoảng cách đều với micro.")
                        .arg(qRound(response.recommendedDurationSec))
                        .arg(response.targetSegments));
            }
        });
}

void EnrollDialog::toggleRecording()
{
    if (m_recording) {
        m_recording = false;
        m_timer->stop();
        if (m_capture)
            m_capture->stop();
        m_recordButton->setText(QStringLiteral("Bắt đầu ghi âm"));
        submitRecording();
        return;
    }

    if (m_controller->isRunning()) {
        setStatus(QStringLiteral("err"),
                  QStringLiteral("Phiên ghi âm đang chạy - dừng phiên trước khi đăng ký giọng "
                                 "(cả hai dùng chung một microphone)."));
        return;
    }
    if (m_speakerName->text().trimmed().isEmpty()) {
        setStatus(QStringLiteral("err"), QStringLiteral("Vui lòng nhập tên trước khi ghi âm."));
        return;
    }
    if (m_editorId.isEmpty()) {
        setStatus(QStringLiteral("err"),
                  QStringLiteral("Vui lòng nhập tên người thao tác (thanh dưới cửa sổ chính) trước khi ghi âm."));
        return;
    }

    if (!m_capture) {
        m_capture = new AudioCapture(this);
        connect(m_capture, &AudioCapture::chunk, this,
                [this](const QByteArray &pcm) { m_recorded.append(pcm); });
        connect(m_capture, &AudioCapture::failed, this, [this](const QString &message) {
            m_recording = false;
            m_timer->stop();
            m_recordButton->setText(QStringLiteral("Bắt đầu ghi âm"));
            setStatus(QStringLiteral("err"), QStringLiteral("Không mở được micro: %1").arg(message));
        });
        connect(m_capture, &AudioCapture::deviceLost, this, [this](const QString &reason) {
            m_recording = false;
            m_timer->stop();
            m_recordButton->setText(QStringLiteral("Bắt đầu ghi âm"));
            setStatus(QStringLiteral("err"), QStringLiteral("Mất microphone: %1").arg(reason));
        });
    }

    // Record on the configured microphone at its configured format, then
    // resample on submit.  Opening at the server's preferred enrolment rate
    // instead would fail on a device that only offers 48 kHz, and falling back
    // to the system default would quietly enrol the wrong physical mic.
    const AudioDeviceChoice choice = m_controller->inputDevice();
    m_sampleRate = choice.sampleRate;
    m_channels = choice.channels;
    m_recorded.clear();
    m_recording = true;
    m_clock.restart();
    m_timer->start();
    m_recordButton->setText(QStringLiteral("Dừng ghi âm"));
    setStatus(QStringLiteral("busy"), QStringLiteral("Đang ghi âm..."));
    m_capture->start(choice);
}

void EnrollDialog::loadFromFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Chọn tệp giọng mẫu"), QString(),
        QStringLiteral("Âm thanh / video (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.mp4 *.mkv "
                       "*.mov);;Tất cả (*)"));
    if (path.isEmpty())
        return;

    // Decoded straight to what CAM++ wants.  The service takes a complete WAV
    // at 16 kHz mono and infers nothing from the file name, so converting here
    // means the operator can hand it whatever their recorder produced.
    QString error;
    const wav::Pcm pcm = audio::decodeMedia(path, &error, 16000, 1);
    if (!pcm.isValid()) {
        setStatus(QStringLiteral("err"), error);
        return;
    }

    m_recorded = pcm.frames;
    m_sampleRate = pcm.sampleRate;
    m_channels = pcm.channels;

    // Said out loud, because a sample that is too short or mostly silence is
    // the single commonest reason an enrolment comes back rejected, and the
    // operator can fix it before spending two minutes on rebuild_db.
    const QString name = QFileInfo(path).fileName();
    setStatus(QStringLiteral("ok"),
              QStringLiteral("Đã nạp %1 — %2 giây. Nhấn \"Gửi lại bản ghi\" hoặc đặt tên rồi gửi.\n"
                             "Lưu ý: mẫu tốt là MỘT người nói, liên tục, không nhạc nền.")
                  .arg(name)
                  .arg(pcm.durationSec(), 0, 'f', 1));
    LOG_INFO(applog::cat::Ui) << "enrolment sample loaded from" << name << pcm.durationSec()
                              << "s at 16 kHz mono";
    m_pendingWav = m_recorded;
    m_pendingName = m_speakerName->text().trimmed();
    m_retryButton->setVisible(true);
}

void EnrollDialog::playWav(const QByteArray &wavBytes)
{
    if (wavBytes.isEmpty() || !m_player)
        return;
    // Same shape as ReviewPanel: the player is pointed at a QBuffer that
    // outlives the call, because setSourceDevice does not take ownership and
    // a local buffer would be gone by the time playback starts.
    m_player->stop();
    m_player->setSourceDevice(nullptr);
    m_audioBuffer->close();
    m_audioBuffer->setData(wavBytes);
    m_audioBuffer->open(QIODevice::ReadOnly);
    m_player->setSourceDevice(m_audioBuffer);
    m_player->play();
}

void EnrollDialog::previewRecording()
{
    if (m_recorded.isEmpty()) {
        setStatus(QStringLiteral("err"), QStringLiteral("Chưa có bản ghi nào để nghe thử."));
        return;
    }
    // No editor id required and none sent: a preview writes nothing, so there
    // is nothing to attribute.  That is also why this is safe to press on a
    // workstation where nobody has filled the operator name in yet.
    const QByteArray mono = wav::toMono16k(m_recorded, m_sampleRate, m_channels);

    reg::PreviewEnrollmentRequest request;
    request.displayName = m_speakerName->text().trimmed();
    request.wav = wav::buildWav(mono, 16000, 1);
    request.allowBelowPolicy = m_allowBelow->isChecked();

    m_previewButton->setEnabled(false);
    setStatus(QStringLiteral("busy"), QStringLiteral("Đang cắt thử bằng VAD..."));
    LOG_INFO(applog::cat::Ui) << "sending PreviewEnrollment: wav=" << request.wav.size()
                              << "bytes - read-only, nothing is written";

    m_controller->rpc()->call<reg::PreviewEnrollmentResponse>(
        this,
        [request](AsrClient &client, reg::PreviewEnrollmentResponse &out) {
            // VAD only, no rebuild_db pass, so this is seconds rather than
            // minutes - but it still carries the audio back.
            return client.previewEnrollment(request, &out, 60000);
        },
        [this](const grpc::Status &status, const reg::PreviewEnrollmentResponse &response) {
            m_previewButton->setEnabled(true);
            if (!status.ok()) {
                setStatus(QStringLiteral("err"),
                          QStringLiteral("Không nghe thử được: %1").arg(status.toString()));
                return;
            }
            if (!response.ok) {
                setStatus(QStringLiteral("err"),
                          QStringLiteral("Bản ghi bị từ chối: %1").arg(response.error));
                return;
            }
            setStatus(QString(), QString());
            const double kept = response.rawSeconds > 0.0
                ? response.speechSecondsAfterVad / response.rawSeconds
                : 0.0;
            QString text =
                QStringLiteral("Thử xong, <b>chưa ghi gì vào DB</b>. Thô %1 s → còn <b>%2 s</b> "
                               "tiếng nói (giữ %3).")
                    .arg(response.rawSeconds, 0, 'f', 1)
                    .arg(response.speechSecondsAfterVad, 0, 'f', 1)
                    .arg(kept, 0, 'f', 2);
            if (!response.policyCompliant) {
                text += QStringLiteral(
                            "<br><span style='color:%1;'><b>Dưới chuẩn:</b> %2</span> — đăng ký "
                            "thật sẽ rơi vào chế độ cấp bách. Nên thu lại 30–40 giây.")
                            .arg(theme::color(theme::Role::Danger).name(),
                                 response.warning.isEmpty()
                                     ? QStringLiteral("không đủ tiếng nói sau khi cắt")
                                     : response.warning.toHtmlEscaped());
            }
            text += QStringLiteral(
                "<br>Đang phát lại đúng đoạn sẽ được đăng ký — <b>nghe kỹ xem có đúng một "
                "người nói không</b>. Hai giọng trong một mẫu tạo ra một giọng pha trộn, và "
                "nó gọi sai tên người ở các cuộc họp sau mà không báo lỗi gì.");
            m_previewInfo->setText(text);
            m_previewInfo->setVisible(true);
            playWav(response.trimmedWav);
            LOG_INFO(applog::cat::Ui)
                << "PreviewEnrollment ok: raw=" << response.rawSeconds
                << "s kept=" << response.speechSecondsAfterVad
                << "s policyCompliant=" << response.policyCompliant;
        });
}

void EnrollDialog::submitRecording()
{
    if (m_recorded.isEmpty()) {
        setStatus(QStringLiteral("err"), QStringLiteral("Không thu được audio nào."));
        return;
    }
    if (m_editorId.isEmpty()) {
        setStatus(QStringLiteral("err"),
                  QStringLiteral("Tên người thao tác bị trống - nhập lại rồi ghi âm."));
        return;
    }
    const QString name = m_speakerName->text().trimmed();
    // CAM++ wants the enrolment sample at its own rate; the meeting path lets
    // the server resample, but a one-shot WAV is converted here.
    const QByteArray mono = wav::toMono16k(m_recorded, m_sampleRate, m_channels);
    const QByteArray wavBytes = wav::buildWav(mono, 16000, 1);

    reg::EnrollSpeakerRequest request;
    request.displayName = name;
    request.wav = wavBytes;
    request.editorId = m_editorId;
    request.note = QStringLiteral("s2t_qt enrollment");
    request.allowBelowPolicy = m_allowBelow->isChecked();

    setStatus(QStringLiteral("busy"),
              QStringLiteral("Đang xử lý (VAD + trích embedding + cập nhật DB)..."));
    m_recordButton->setEnabled(false);
    LOG_INFO(applog::cat::Ui)
        << "sending EnrollSpeaker: name=" << name << "editor=" << m_editorId << "wav="
        << wavBytes.size() << "bytes (16 kHz mono) allowBelowPolicy="
        << m_allowBelow->isChecked();

    m_controller->rpc()->call<reg::EnrollSpeakerResponse>(
        this,
        [request](AsrClient &client, reg::EnrollSpeakerResponse &out) {
            // Covers a full rebuild_db pass over every existing speaker, not
            // just this one's audio, so it needs real headroom.
            return client.enrollSpeaker(request, &out, 120000);
        },
        [this, wavBytes, name](const grpc::Status &status,
                               const reg::EnrollSpeakerResponse &response) {
            m_recordButton->setEnabled(true);
            if (!status.ok()) {
                LOG_ERROR(applog::cat::Ui)
                    << "EnrollSpeaker failed for" << name << ":" << status.toString()
                    << "- keeping" << wavBytes.size() << "bytes of audio for a retry";
                // Keep the audio: a connection that drops between "stop" and
                // the reply must not throw away what the person just read.
                m_pendingWav = wavBytes;
                m_pendingName = name;
                m_retryButton->setVisible(true);
                setStatus(QStringLiteral("err"),
                          QStringLiteral("Không gửi được bản ghi (%1).\n\nBản ghi %2 MB vẫn còn "
                                         "trong cửa sổ này - đừng đóng. Có mạng lại thì bấm "
                                         "\"Gửi lại bản ghi\".")
                              .arg(status.toString())
                              .arg(wavBytes.size() / 1024.0 / 1024.0, 0, 'f', 1));
                return;
            }
            if (!response.ok) {
                LOG_WARN(applog::cat::Ui)
                    << "EnrollSpeaker rejected by the server for" << name << ":" << response.error;
                setStatus(QStringLiteral("err"),
                          QStringLiteral("Lỗi: %1")
                              .arg(response.error.isEmpty() ? QStringLiteral("enrollment failed")
                                                            : response.error));
                return;
            }
            LOG_INFO(applog::cat::Ui)
                << "EnrollSpeaker OK: speaker_id=" << response.speakerId << "raw="
                << response.rawSeconds << "s afterVad=" << response.speechSecondsAfterVad
                << "s segments=" << response.segmentsEnrolled << "/" << response.targetSegments
                << (response.warning.isEmpty() ? QString()
                                               : QStringLiteral("| warning: ") + response.warning);
            m_pendingWav.clear();
            m_retryButton->setVisible(false);
            QString message =
                QStringLiteral("Đã đăng ký \"%1\"\nBản ghi gốc: %2s, sau lọc VAD: %3s\n"
                               "Số đoạn nhúng đã ghi vào DB: %4/%5")
                    .arg(response.speakerId)
                    .arg(response.rawSeconds, 0, 'f', 1)
                    .arg(response.speechSecondsAfterVad, 0, 'f', 1)
                    .arg(response.segmentsEnrolled)
                    .arg(response.targetSegments);
            if (!response.warning.isEmpty())
                message += QStringLiteral("\n\n⚠ %1").arg(response.warning);
            setStatus(response.warning.isEmpty() ? QStringLiteral("ok") : QStringLiteral("warn"),
                      message);
            loadRoster();
        });
}

void EnrollDialog::loadRoster()
{
    reg::GetSpeakerRegistryStatusRequest request;
    m_controller->rpc()->call<reg::GetSpeakerRegistryStatusResponse>(
        this,
        [request](AsrClient &client, reg::GetSpeakerRegistryStatusResponse &out) {
            return client.getSpeakerRegistryStatus(request, &out, 20000);
        },
        [this](const grpc::Status &status, const reg::GetSpeakerRegistryStatusResponse &response) {
            m_roster->clear();
            m_belowPolicy->clear();
            if (!status.ok()) {
                m_roster->addItem(QStringLiteral("Lỗi: %1").arg(status.toString()));
                return;
            }
            if (response.globalSpeakerNames.isEmpty())
                m_roster->addItem(QStringLiteral("DB chung chưa có speaker nào."));
            for (const QString &name : response.globalSpeakerNames)
                m_roster->addItem(name);

            if (!response.speakersBelowPolicy.isEmpty()) {
                m_belowPolicy->addItem(
                    QStringLiteral("⚠ %1 người chưa có bản ghi đạt chuẩn — giọng vẫn được nhận "
                                   "diện nhưng độ chính xác có thể kém:")
                        .arg(response.speakersBelowPolicy.size()));
                for (const reg::SpeakerBelowPolicy &item : response.speakersBelowPolicy) {
                    QString text = QStringLiteral("%1 [%2]: %3 mẫu, dài nhất %4s tiếng nói")
                                       .arg(item.spkName.isEmpty() ? item.spkId : item.spkName,
                                            kindLabel(item.kind))
                                       .arg(item.sampleCount)
                                       .arg(item.longestSampleSec, 0, 'f', 1);
                    if (!item.reason.isEmpty())
                        text += QStringLiteral(" — %1").arg(item.reason);
                    m_belowPolicy->addItem(text);
                }
            }
            m_belowPolicy->setVisible(m_belowPolicy->count() > 0);
            if (!m_sessionInput->text().trimmed().isEmpty())
                m_registryStatus->setText(
                    QStringLiteral("DB chung: %1 speaker, revision %2, sidecar %3")
                        .arg(response.globalSpeakerCount)
                        .arg(response.globalDbRevision,
                             response.sidecarReachable ? QStringLiteral("reachable")
                                                       : QStringLiteral("KHÔNG reachable")));
        });
}

void EnrollDialog::loadSessionSpeakers()
{
    const QString sessionId = m_sessionInput->text().trimmed();
    if (sessionId.isEmpty()) {
        m_registryStatus->setText(QStringLiteral("Chưa nhập session_id."));
        return;
    }
    reg::ListSessionSpeakersRequest request;
    request.sessionId = sessionId;
    m_controller->rpc()->call<reg::ListSessionSpeakersResponse>(
        this,
        [request](AsrClient &client, reg::ListSessionSpeakersResponse &out) {
            return client.listSessionSpeakers(request, &out, 30000);
        },
        [this](const grpc::Status &status, const reg::ListSessionSpeakersResponse &response) {
            m_speakers->setRowCount(0);
            if (!status.ok()) {
                m_registryStatus->setText(QStringLiteral("Lỗi: %1").arg(status.toString()));
                return;
            }
            m_speakers->setRowCount(response.speakers.size());
            for (int i = 0; i < response.speakers.size(); ++i) {
                const reg::SessionSpeakerEntry &entry = response.speakers.at(i);
                const QString label = entry.verifiedName.isEmpty()
                    ? QStringLiteral("(chưa đặt tên, %1)").arg(entry.sessionSpeakerId)
                    : entry.verifiedName;
                auto *name = new QTableWidgetItem(
                    QStringLiteral("%1\nscore %2 / %3 cửa sổ · diar [%4]")
                        .arg(label)
                        .arg(entry.score, 0, 'f', 3)
                        .arg(entry.windows)
                        .arg(entry.diarSlots.join(QStringLiteral(", "))));
                name->setData(Qt::UserRole, entry.sessionSpeakerId);
                m_speakers->setItem(i, 0, name);

                QString statusText = statusLabel(entry.status);
                if (entry.status == QLatin1String("global_shared"))
                    statusText += QStringLiteral("\nđã publish: %1").arg(entry.publishedName);
                if (entry.status == QLatin1String("publish_failed"))
                    statusText += QStringLiteral("\nlỗi: %1").arg(entry.publishError);
                m_speakers->setItem(i, 1, new QTableWidgetItem(statusText));

                // Evidence is audio a reviewer's rename pointed at, staged but
                // not published anywhere until a selection here says so.
                m_speakers->setItem(
                    i, 2,
                    new QTableWidgetItem(entry.hasEvidence
                                             ? QStringLiteral("%1s / %2 đoạn")
                                                   .arg(entry.evidence.totalSpeechSec, 0, 'f', 1)
                                                   .arg(entry.evidence.spanCount)
                                             : QStringLiteral("chưa có evidence")));

                auto *destination = new QComboBox(m_speakers);
                destination->addItem(QStringLiteral("Không đổi"), 0);
                destination->addItem(QStringLiteral("Chỉ giữ trong phiên"),
                                     int(reg::SessionOnly));
                destination->addItem(QStringLiteral("Publish vào DB chung"),
                                     int(reg::GlobalShared));
                m_speakers->setCellWidget(i, 3, destination);

                auto *globalName = new QLineEdit(m_speakers);
                globalName->setPlaceholderText(
                    QStringLiteral("để trống = dùng %1").arg(label));
                m_speakers->setCellWidget(i, 4, globalName);
            }
            m_speakers->resizeRowsToContents();
        });

    reg::GetSpeakerRegistryStatusRequest statusRequest;
    statusRequest.sessionId = sessionId;
    m_controller->rpc()->call<reg::GetSpeakerRegistryStatusResponse>(
        this,
        [statusRequest](AsrClient &client, reg::GetSpeakerRegistryStatusResponse &out) {
            return client.getSpeakerRegistryStatus(statusRequest, &out, 20000);
        },
        [this](const grpc::Status &status, const reg::GetSpeakerRegistryStatusResponse &response) {
            if (!status.ok()) {
                m_registryStatus->setText(
                    QStringLiteral("Không lấy được registry status: %1").arg(status.toString()));
                return;
            }
            m_registryStatus->setText(
                QStringLiteral("DB chung: %1 speaker, revision %2, sidecar %3 | Phiên: %4 pending, "
                               "%5 published, %6 failed")
                    .arg(response.globalSpeakerCount)
                    .arg(response.globalDbRevision,
                         response.sidecarReachable ? QStringLiteral("reachable")
                                                   : QStringLiteral("KHÔNG reachable"))
                    .arg(response.sessionPendingCount)
                    .arg(response.sessionPublishedCount)
                    .arg(response.sessionFailedCount));
        });
}

void EnrollDialog::saveSelections()
{
    const QString sessionId = m_sessionInput->text().trimmed();
    if (sessionId.isEmpty()) {
        m_saveResults->setText(QStringLiteral("Chưa nhập session_id."));
        return;
    }
    if (m_editorId.isEmpty()) {
        m_saveResults->setText(
            QStringLiteral("Vui lòng nhập tên người thao tác (thanh dưới cửa sổ chính) trước khi lưu."));
        return;
    }

    reg::SaveSessionSpeakersRequest request;
    request.sessionId = sessionId;
    request.editorId = m_editorId;
    for (int i = 0; i < m_speakers->rowCount(); ++i) {
        auto *destination = qobject_cast<QComboBox *>(m_speakers->cellWidget(i, 3));
        auto *globalName = qobject_cast<QLineEdit *>(m_speakers->cellWidget(i, 4));
        if (!destination || destination->currentData().toInt() == 0)
            continue;
        reg::SpeakerSelection selection;
        selection.sessionSpeakerId = m_speakers->item(i, 0)->data(Qt::UserRole).toString();
        selection.destination = destination->currentData().toInt();
        selection.globalName = globalName ? globalName->text().trimmed() : QString();
        request.selections.append(selection);
    }
    if (request.selections.isEmpty()) {
        m_saveResults->setText(QStringLiteral("Chưa chọn lựa chọn nào khác \"Không đổi\"."));
        return;
    }

    m_saveResults->setText(QStringLiteral("Đang lưu..."));
    LOG_INFO(applog::cat::Ui) << "sending SaveSessionSpeakers for session" << request.sessionId
                              << "-" << request.selections.size() << "selections, editor="
                              << m_editorId;
    m_controller->rpc()->call<reg::SaveSessionSpeakersResponse>(
        this,
        [request](AsrClient &client, reg::SaveSessionSpeakersResponse &out) {
            return client.saveSessionSpeakers(request, &out, 120000);
        },
        [this](const grpc::Status &status, const reg::SaveSessionSpeakersResponse &response) {
            if (!status.ok()) {
                LOG_ERROR(applog::cat::Ui) << "SaveSessionSpeakers failed:" << status.toString();
                m_saveResults->setText(QStringLiteral("Lỗi: %1").arg(status.toString()));
                return;
            }
            for (const reg::SaveSpeakerResult &result : response.results) {
                // Per speaker, because a partial failure is normal here and
                // the audit trail cares which one it was.
                LOG_INFO(applog::cat::Ui)
                    << "SaveSessionSpeakers:" << result.sessionSpeakerId
                    << (result.ok ? "OK" : "FAILED") << result.status << result.error;
            }
            // Partial failure is reported per speaker: one failing never drops
            // or blocks the others.
            QStringList lines;
            for (const reg::SaveSpeakerResult &result : response.results) {
                QString line = QStringLiteral("%1: %2 → %3")
                                   .arg(result.sessionSpeakerId,
                                        result.ok ? QStringLiteral("OK") : QStringLiteral("LỖI"),
                                        statusLabel(result.status));
                if (!result.error.isEmpty())
                    line += QStringLiteral(" (%1)").arg(result.error);
                if (result.segmentsEnrolled > 0)
                    line += QStringLiteral(" [%1 segments]").arg(result.segmentsEnrolled);
                lines << line;
            }
            m_saveResults->setText(lines.join(QLatin1Char('\n')));
            loadSessionSpeakers();
        });
}
