#include "Dialogs.h"

#include "LogControls.h"
#include "audio/AudioCapture.h"
#include "core/Logger.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace {

// The xvf3800 host-control tool ships as xvf_host.exe on Windows and as a
// plain ELF binary with no extension on Linux.  Filtering on "*.exe" there
// would hide the only file the operator could pick.
#ifdef Q_OS_WIN
const QString kControlAppName = QStringLiteral("xvf_host.exe");
const QString kControlAppFilter = QStringLiteral("*.exe");
#else
const QString kControlAppName = QStringLiteral("xvf_host");
const QString kControlAppFilter = QString();
#endif

QString formatTimestamp(double epochSeconds)
{
    if (epochSeconds <= 0)
        return QStringLiteral("-");
    return QDateTime::fromMSecsSinceEpoch(qint64(epochSeconds * 1000.0))
        .toString(QStringLiteral("dd/MM/yyyy HH:mm:ss"));
}

QString auditEventLabel(const QString &event)
{
    if (event == QLatin1String("session_start"))
        return QStringLiteral("Bắt đầu phiên");
    if (event == QLatin1String("text_edit"))
        return QStringLiteral("Sửa văn bản");
    if (event == QLatin1String("rename_speaker"))
        return QStringLiteral("Đổi tên người nói");
    if (event == QLatin1String("speaker_publish"))
        return QStringLiteral("Publish giọng nói");
    return event.isEmpty() ? QStringLiteral("?") : event;
}

} // namespace

// ---------------------------------------------------------------------------
// StartSessionDialog
// ---------------------------------------------------------------------------

StartSessionDialog::StartSessionDialog(SessionController *controller, Purpose purpose,
                                       QWidget *parent)
    : QDialog(parent), m_controller(controller), m_purpose(purpose)
{
    setWindowTitle(purpose == Purpose::Microphone ? QStringLiteral("Bắt đầu ghi âm")
                                                  : QStringLiteral("Chạy tệp audio"));
    resize(620, 560);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        purpose == Purpose::Microphone
            ? QStringLiteral("Chọn những người được phép nhận diện trong phiên này.")
            : QStringLiteral("Chọn những người được phép nhận diện trong tệp này."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    m_title = new QLineEdit(this);
    m_title->setMaxLength(200);
    m_title->setPlaceholderText(QStringLiteral("VD: Họp điều hành tác chiến"));
    form->addRow(QStringLiteral("Tên phiên (tùy chọn)"), m_title);

    m_participants = new QLineEdit(this);
    m_participants->setPlaceholderText(QStringLiteral("VD: Nguyễn Văn A, Trần Thị B"));
    form->addRow(QStringLiteral("Người tham gia (cách nhau bằng dấu phẩy)"), m_participants);

    m_security = new QComboBox(this);
    m_security->addItem(QStringLiteral("- Chưa chọn -"), QString());
    m_security->addItem(QStringLiteral("Thường"), QStringLiteral("thuong"));
    m_security->addItem(QStringLiteral("Mật"), QStringLiteral("mat"));
    m_security->addItem(QStringLiteral("Tối mật"), QStringLiteral("toi_mat"));
    m_security->addItem(QStringLiteral("Tuyệt mật"), QStringLiteral("tuyet_mat"));
    // Stored and displayed as metadata only; it does not replace server-side
    // authorisation and the label says so.
    form->addRow(QStringLiteral("Mức bảo mật (chỉ lưu nhãn)"), m_security);

    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Ghi âm + chuyển văn bản (S2T)"),
                    QStringLiteral("record_and_s2t"));
    m_mode->addItem(QStringLiteral("Chỉ ghi âm (không chạy AI)"), QStringLiteral("record_only"));
    form->addRow(QStringLiteral("Chế độ"), m_mode);
    layout->addLayout(form);

    auto *group = new QGroupBox(QStringLiteral("Nhận diện người nói"), this);
    auto *groupLayout = new QVBoxLayout(group);
    m_unrestricted = new QRadioButton(
        QStringLiteral("Không giới hạn - so khớp với toàn bộ database chung"), group);
    m_restricted = new QRadioButton(QStringLiteral("Chỉ những người được chọn bên dưới"), group);
    m_restricted->setChecked(true);
    groupLayout->addWidget(m_unrestricted);
    groupLayout->addWidget(m_restricted);

    auto *rosterRow = new QHBoxLayout();
    auto *reload = new QPushButton(QStringLiteral("Tải lại DB"), group);
    rosterRow->addWidget(reload);
    rosterRow->addStretch();
    groupLayout->addLayout(rosterRow);

    m_roster = new QListWidget(group);
    m_roster->setSelectionMode(QAbstractItemView::NoSelection);
    groupLayout->addWidget(m_roster, 1);
    layout->addWidget(group, 1);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    layout->addWidget(m_info);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_confirm = buttons->addButton(purpose == Purpose::Microphone
                                       ? QStringLiteral("BẮT ĐẦU GHI ÂM")
                                       : QStringLiteral("CHỌN TỆP AUDIO"),
                                   QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(reload, &QPushButton::clicked, this, &StartSessionDialog::reloadRoster);
    connect(m_unrestricted, &QRadioButton::toggled, this, &StartSessionDialog::updateSummary);
    connect(m_roster, &QListWidget::itemChanged, this, &StartSessionDialog::updateSummary);

    reloadRoster();
    updateSummary();
}

void StartSessionDialog::reloadRoster()
{
    m_roster->clear();
    m_roster->addItem(QStringLiteral("Đang tải danh sách người đã đăng ký..."));
    reg::GetSpeakerRegistryStatusRequest request;
    m_controller->rpc()->call<reg::GetSpeakerRegistryStatusResponse>(
        this,
        [request](AsrClient &client, reg::GetSpeakerRegistryStatusResponse &out) {
            return client.getSpeakerRegistryStatus(request, &out, 20000);
        },
        [this](const grpc::Status &status, const reg::GetSpeakerRegistryStatusResponse &response) {
            m_roster->clear();
            if (!status.ok()) {
                m_roster->addItem(QStringLiteral("Không tải được database: %1")
                                      .arg(status.toString()));
                return;
            }
            if (response.globalSpeakerNames.isEmpty()) {
                m_roster->addItem(QStringLiteral("Database chưa có người nào; vẫn có thể ghi âm."));
                updateSummary();
                return;
            }
            // Speakers whose every usable sample was accepted below policy get
            // a marker: the fix is always "have them enrol again", and hiding
            // that would let a weak voice look as trustworthy as any other.
            QSet<QString> weak;
            for (const reg::SpeakerBelowPolicy &item : response.speakersBelowPolicy)
                weak.insert(QString(item.spkName).replace(QLatin1Char('_'), QLatin1Char('-')).trimmed());
            for (const QString &name : response.globalSpeakerNames) {
                auto *item = new QListWidgetItem(m_roster);
                const QString normalized =
                    QString(name).replace(QLatin1Char('_'), QLatin1Char('-')).trimmed();
                item->setText(weak.contains(normalized)
                                  ? name + QStringLiteral("  ⚠ (đăng ký cấp bách - nên đăng ký lại)")
                                  : name);
                item->setData(Qt::UserRole, name);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
            }
            updateSummary();
        });
}

void StartSessionDialog::updateSummary()
{
    m_roster->setEnabled(m_restricted->isChecked());
    const QStringList selected = selectedSpeakers();
    if (!m_restricted->isChecked()) {
        m_info->setText(QStringLiteral(
            "Không giới hạn: mọi tên trong database chung đều có thể được gán."));
        return;
    }
    if (selected.isEmpty()) {
        m_info->setText(QStringLiteral(
            "Không chọn ai: vẫn ghi âm và tách người nói, nhưng KHÔNG nạp hay gán "
            "bất kỳ tên nào từ database."));
        return;
    }
    m_info->setText(QStringLiteral("Giới hạn nhận diện trong %1 người đã chọn.")
                        .arg(selected.size()));
}

bool StartSessionDialog::restrictSpeakers() const
{
    return m_restricted->isChecked();
}

QStringList StartSessionDialog::selectedSpeakers() const
{
    QStringList out;
    for (int i = 0; i < m_roster->count(); ++i) {
        QListWidgetItem *item = m_roster->item(i);
        const QString name = item->data(Qt::UserRole).toString();
        if (!name.isEmpty() && item->checkState() == Qt::Checked)
            out << name;
    }
    return out;
}

SessionMeta StartSessionDialog::meta() const
{
    SessionMeta meta;
    meta.title = m_title->text().trimmed();
    const QStringList people =
        m_participants->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &person : people) {
        const QString name = person.trimmed();
        if (!name.isEmpty())
            meta.participants << name;
    }
    meta.securityLevel = m_security->currentData().toString();
    meta.mode = m_mode->currentData().toString();
    return meta;
}

// ---------------------------------------------------------------------------
// SpeakerRenameDialog
// ---------------------------------------------------------------------------

SpeakerRenameDialog::SpeakerRenameDialog(const QString &fromSpeaker, const QString &currentName,
                                         const QStringList &otherSpeakers, QWidget *parent)
    : QDialog(parent), m_fromSpeaker(fromSpeaker)
{
    setWindowTitle(QStringLiteral("Sửa người nói"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_name = new QLineEdit(currentName, this);
    m_name->setPlaceholderText(QStringLiteral("vd. Anh Minh"));
    form->addRow(QStringLiteral("Tên hiển thị"), m_name);

    m_merge = new QComboBox(this);
    m_merge->addItem(QStringLiteral("— giữ nguyên ID —"), QString());
    for (const QString &speaker : otherSpeakers) {
        if (speaker == fromSpeaker)
            continue;
        m_merge->addItem(QStringLiteral("speaker_%1").arg(speaker), speaker);
    }
    form->addRow(QStringLiteral("Gộp vào ID khác (tuỳ chọn)"), m_merge);
    layout->addLayout(form);

    auto *note = new QLabel(
        QStringLiteral("Thao tác này đổi nhãn trên toàn bộ bản ghi của phiên, không chỉ dòng "
                       "đang chọn. Để trống tên = xoá tên đã gán trước đó."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString SpeakerRenameDialog::displayName() const
{
    return m_name->text().trimmed();
}

QString SpeakerRenameDialog::mergeTarget() const
{
    const QString target = m_merge->currentData().toString();
    return target.isEmpty() ? m_fromSpeaker : target;
}

// ---------------------------------------------------------------------------
// SentenceEditDialog
// ---------------------------------------------------------------------------

SentenceEditDialog::SentenceEditDialog(SessionController *controller, const asr::DisplayRow &row,
                                       const QString &sessionId, quint64 baseRevision,
                                       bool transcriptFinal, double commitBoundarySec,
                                       const QString &editorId, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_row(row), m_sessionId(sessionId),
      m_editorId(editorId), m_revision(baseRevision), m_final(transcriptFinal),
      m_commitBoundarySec(commitBoundarySec)
{
    setWindowTitle(QStringLiteral("Sửa câu"));
    resize(560, 320);

    auto *layout = new QVBoxLayout(this);
    auto *header = new QLabel(
        QStringLiteral("%1–%2s · %3")
            .arg(row.startSec, 0, 'f', 2)
            .arg(row.endSec, 0, 'f', 2)
            .arg(row.verifiedName.isEmpty() ? QStringLiteral("speaker %1").arg(row.speaker)
                                            : row.verifiedName),
        this);
    layout->addWidget(header);

    m_tokens = new QListWidget(this);
    layout->addWidget(m_tokens, 1);

    m_info = new QLabel(QStringLiteral("Bấm đúp vào một từ để sửa; Enter để lưu."), this);
    m_info->setWordWrap(true);
    layout->addWidget(m_info);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(m_tokens, &QListWidget::itemChanged, this, &SentenceEditDialog::commitItem);

    rebuild();
}

void SentenceEditDialog::rebuild()
{
    m_rebuilding = true;
    m_tokens->clear();

    QList<asr::Word> tokens = m_row.displayTokens;
    if (tokens.isEmpty()) {
        asr::Word whole;
        whole.w = m_row.mergedText;
        whole.startSec = m_row.startSec;
        whole.endSec = m_row.endSec;
        tokens.append(whole);
    }
    for (const asr::Word &token : tokens) {
        auto *item = new QListWidgetItem(token.w, m_tokens);
        item->setData(Qt::UserRole, token.startSec);
        item->setData(Qt::UserRole + 1, token.endSec);
        item->setData(Qt::UserRole + 2, token.w);
        // Mirrors the server's own rule in session_store.py: while the
        // transcript is not final, anything past the commit boundary can
        // still be rewritten by correction, so the server refuses to edit it.
        const bool locked = !m_final
            && (m_row.isProvisional || token.endSec > m_commitBoundarySec + 1e-3);
        if (locked) {
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            item->setForeground(QColor(0x8a, 0x8a, 0x8a));
            item->setToolTip(QStringLiteral("chưa chốt (điểm chốt %1s) - chờ vài giây hoặc "
                                            "đợi cuộc họp kết thúc")
                                 .arg(m_commitBoundarySec, 0, 'f', 2));
        } else {
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            item->setToolTip(QStringLiteral("%1–%2s — bấm đúp để sửa")
                                 .arg(token.startSec, 0, 'f', 2)
                                 .arg(token.endSec, 0, 'f', 2));
        }
    }
    m_rebuilding = false;
}

void SentenceEditDialog::commitItem(QListWidgetItem *item)
{
    if (m_rebuilding || !item)
        return;
    const QString original = item->data(Qt::UserRole + 2).toString();
    const QString text = item->text().trimmed();
    if (text.isEmpty() || text == original) {
        m_rebuilding = true;
        item->setText(original);
        m_rebuilding = false;
        return;
    }
    if (m_editorId.isEmpty()) {
        m_info->setText(QStringLiteral("Cần nhập tên người thao tác (thanh trên) trước khi lưu."));
        m_rebuilding = true;
        item->setText(original);
        m_rebuilding = false;
        return;
    }

    const double startSec = item->data(Qt::UserRole).toDouble();
    const double endSec = item->data(Qt::UserRole + 1).toDouble();
    const double span = qMax(1e-3, endSec - startSec);
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    asr::TextEditRequest request;
    request.sessionId = m_sessionId;
    request.baseRevision = m_revision;
    request.startSec = startSec;
    request.endSec = endSec;
    request.editorId = m_editorId;
    request.note = QStringLiteral("s2t_qt sentence edit");
    // The server keys words by time and rejects anything reaching outside the
    // edited range, so typed words are spread evenly across it with the last
    // end clamped.
    for (int i = 0; i < words.size(); ++i) {
        asr::Word word;
        word.w = words.at(i);
        word.c = 1.0f;
        word.startSec = startSec + span * double(i) / double(words.size());
        word.endSec = qMin(endSec, startSec + span * double(i + 1) / double(words.size()));
        request.replacementWords.append(word);
    }

    m_info->setText(QStringLiteral("đang lưu…"));
    m_controller->rpc()->call<asr::ReviewEditResponse>(
        this,
        [request](AsrClient &client, asr::ReviewEditResponse &out) {
            return client.applyTextEdit(request, &out, 60000);
        },
        [this, item, original](const grpc::Status &status, const asr::ReviewEditResponse &response) {
            if (!status.ok()) {
                m_info->setText(status.message.contains(QLatin1String("edit_range_not_committed"))
                                    ? QStringLiteral("chưa chốt, thử lại sau vài giây")
                                    : QStringLiteral("từ chối: %1").arg(status.toString()));
                m_rebuilding = true;
                item->setText(original);
                m_rebuilding = false;
                return;
            }
            m_revision = response.transcript.revision;
            m_info->setText(QStringLiteral("đã lưu, rev=%1").arg(m_revision));
            // Adopt the row the server sent back so a second edit in this
            // dialog carries the new timings, not the ones it opened with.
            for (const asr::DisplayRow &row : response.state.rows) {
                if (std::abs(row.startSec - m_row.startSec) < 0.5) {
                    m_row = row;
                    break;
                }
            }
            rebuild();
            emit transcriptChanged(response.state, m_revision);
        });
}

// ---------------------------------------------------------------------------
// AuditHistoryDialog
// ---------------------------------------------------------------------------

AuditHistoryDialog::AuditHistoryDialog(SessionController *controller, const QString &sessionId,
                                       QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
    setWindowTitle(QStringLiteral("Lịch sử hiệu chỉnh"));
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout();
    m_session = new QLineEdit(sessionId, this);
    m_session->setPlaceholderText(QStringLiteral("session_id"));
    auto *load = new QPushButton(QStringLiteral("Tải"), this);
    top->addWidget(m_session, 1);
    top->addWidget(load);
    layout->addLayout(top);

    m_info = new QLabel(this);
    layout->addWidget(m_info);

    m_rows = new QListWidget(this);
    m_rows->setWordWrap(true);
    layout->addWidget(m_rows, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(load, &QPushButton::clicked, this, &AuditHistoryDialog::reload);

    if (!sessionId.isEmpty())
        reload();
}

void AuditHistoryDialog::reload()
{
    const QString sessionId = m_session->text().trimmed();
    if (sessionId.isEmpty()) {
        m_info->setText(QStringLiteral("Chưa có session_id."));
        m_rows->clear();
        return;
    }
    m_info->setText(QStringLiteral("Đang tải..."));
    m_rows->clear();

    asr::AuditHistoryRequest request;
    request.sessionId = sessionId;
    request.limit = 200;
    m_controller->rpc()->call<asr::AuditHistoryResponse>(
        this,
        [request](AsrClient &client, asr::AuditHistoryResponse &out) {
            return client.getAuditHistory(request, &out, 30000);
        },
        [this](const grpc::Status &status, const asr::AuditHistoryResponse &response) {
            if (!status.ok()) {
                m_info->setText(QStringLiteral("Lỗi: %1").arg(status.toString()));
                return;
            }
            m_info->setText(response.events.isEmpty()
                                ? QStringLiteral("Chưa có hiệu chỉnh nào.")
                                : QStringLiteral("%1 sự kiện").arg(response.events.size()));
            for (const asr::AuditEvent &event : response.events) {
                const QJsonObject payload =
                    QJsonDocument::fromJson(event.payloadJson.toUtf8()).object();
                QString detail;
                if (event.event == QLatin1String("text_edit")) {
                    detail = QStringLiteral("[%1–%2s] \"%3\" → \"%4\"")
                                 .arg(payload.value(QStringLiteral("start_sec")).toDouble(), 0, 'f', 2)
                                 .arg(payload.value(QStringLiteral("end_sec")).toDouble(), 0, 'f', 2)
                                 .arg(payload.value(QStringLiteral("previous_text")).toString(
                                     QStringLiteral("(rỗng)")))
                                 .arg(payload.value(QStringLiteral("new_text")).toString());
                } else if (event.event == QLatin1String("rename_speaker")) {
                    const QString previous =
                        payload.value(QStringLiteral("previous_verified_name")).toString();
                    const QString current =
                        payload.value(QStringLiteral("verified_name")).toString();
                    const QString from = payload.value(QStringLiteral("from_speaker")).toString();
                    const QString to = payload.value(QStringLiteral("to_speaker")).toString();
                    detail = (to.isEmpty() || to == from)
                        ? QStringLiteral("giọng %1").arg(from)
                        : QStringLiteral("giọng %1 → %2").arg(from, to);
                    // Show the real before/after, not just the new name: the
                    // server captures previous_verified_name before the
                    // rename precisely so this can be honest.
                    if (previous != current) {
                        detail += QStringLiteral(" — tên: \"%1\" → \"%2\"")
                                      .arg(previous.isEmpty() ? QStringLiteral("(chưa có)") : previous,
                                           current.isEmpty() ? QStringLiteral("(xóa tên)") : current);
                    }
                } else if (event.event == QLatin1String("speaker_publish")) {
                    detail = QStringLiteral("giọng %1 → %2 (%3)")
                                 .arg(payload.value(QStringLiteral("session_speaker_id")).toString(),
                                      payload.value(QStringLiteral("destination")).toString(),
                                      payload.value(QStringLiteral("status")).toString());
                }
                const QString editor =
                    payload.value(QStringLiteral("editor_id")).toString(QStringLiteral("-"));
                const QString note = payload.value(QStringLiteral("note")).toString();
                QString text = QStringLiteral("%1  ·  %2  ·  người thực hiện: %3")
                                   .arg(formatTimestamp(event.ts), auditEventLabel(event.event),
                                        editor);
                if (!detail.isEmpty())
                    text += QStringLiteral("\n    %1").arg(detail);
                if (!note.isEmpty())
                    text += QStringLiteral("\n    %1").arg(note);
                m_rows->addItem(text);
            }
        });
}

// ---------------------------------------------------------------------------
// SettingsDialog
// ---------------------------------------------------------------------------

SettingsDialog::SettingsDialog(AppConfig *config, QWidget *parent)
    : QDialog(parent), m_config(config)
{
    setWindowTitle(QStringLiteral("Cấu hình"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    m_server = new QLineEdit(config->serverTarget, this);
    m_server->setPlaceholderText(QStringLiteral("192.168.1.47:8800"));
    form->addRow(QStringLiteral("Server buffer (host:port)"), m_server);

    auto *tokenRow = new QHBoxLayout();
    m_token = new QLineEdit(config->apiToken, this);
    m_token->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    auto *tokenBrowse = new QPushButton(QStringLiteral("Từ tệp..."), this);
    tokenRow->addWidget(m_token, 1);
    tokenRow->addWidget(tokenBrowse);
    form->addRow(QStringLiteral("Bearer token"), tokenRow);

    m_device = new QComboBox(this);
    m_device->addItem(QStringLiteral("(mặc định hệ thống)"), QByteArray());
    for (const QAudioDevice &device : QMediaDevices::audioInputs())
        m_device->addItem(device.description(), device.id());
    const int index = m_device->findData(config->inputDeviceId);
    m_device->setCurrentIndex(index >= 0 ? index : 0);
    form->addRow(QStringLiteral("Microphone"), m_device);

    m_expectedName = new QLineEdit(config->expectedDeviceName, this);
    m_expectedName->setPlaceholderText(QStringLiteral("Speaker"));
    m_expectedName->setToolTip(QStringLiteral(
        "Tên thiết bị phải chứa chuỗi này. Chống trường hợp hệ điều hành tái sử "
        "dụng endpoint sau khi rút USB mic và thu nhầm thiết bị khác."));
    form->addRow(QStringLiteral("Tên thiết bị bắt buộc chứa"), m_expectedName);

    m_sampleRate = new QSpinBox(this);
    m_sampleRate->setRange(8000, 192000);
    m_sampleRate->setSingleStep(1000);
    m_sampleRate->setValue(config->sampleRate);
    form->addRow(QStringLiteral("Sample rate"), m_sampleRate);

    m_channels = new QSpinBox(this);
    m_channels->setRange(1, 2);
    m_channels->setValue(config->channels);
    form->addRow(QStringLiteral("Số kênh"), m_channels);

    m_bufferSec = new QDoubleSpinBox(this);
    m_bufferSec->setRange(10.0, 600.0);
    m_bufferSec->setValue(config->bufferSec);
    m_bufferSec->setSuffix(QStringLiteral(" s"));
    m_bufferSec->setToolTip(QStringLiteral(
        "Lượng audio đã thu nhưng chưa ACK được phép tồn đọng trên máy này "
        "trước khi phiên dừng lại thay vì âm thầm xoá audio."));
    form->addRow(QStringLiteral("Hàng đợi tối đa"), m_bufferSec);

    auto *appRow = new QHBoxLayout();
    m_controlApp = new QLineEdit(config->micControlApp, this);
    m_controlApp->setPlaceholderText(kControlAppName);
    auto *appBrowse = new QPushButton(QStringLiteral("Chọn..."), this);
    appRow->addWidget(m_controlApp, 1);
    appRow->addWidget(appBrowse);
    form->addRow(QStringLiteral("xvF3800 host-control"), appRow);

    m_pipelineTrace = new QCheckBox(QStringLiteral("Bật pipeline trace cho phiên mới"), this);
    m_pipelineTrace->setChecked(config->pipelineTrace);
    form->addRow(QString(), m_pipelineTrace);

    m_paceReplay = new QCheckBox(
        QStringLiteral("Phát lại tệp theo đúng tốc độ thật (thay vì nhanh nhất có thể)"), this);
    m_paceReplay->setChecked(config->paceFileReplay);
    m_paceReplay->setToolTip(QStringLiteral(
        "Bật: mô phỏng đúng nhịp cuộc họp, dùng để đo độ trễ. "
        "Tắt: xử lý lại một bản ghi càng nhanh càng tốt."));
    form->addRow(QString(), m_paceReplay);

    // The debug-log flag.  Debug writes to the console, which is only visible
    // when the app was started from one; develop writes to a file, which is
    // what a deployed workstation needs when something has to be sent back.
    m_logMode = new QComboBox(this);
    logcontrols::fillModes(m_logMode, config->logMode);
    form->addRow(QStringLiteral("Chế độ log"), m_logMode);

    m_logLevel = new QComboBox(this);
    logcontrols::fillLevels(m_logLevel, config->logLevel);
    form->addRow(QStringLiteral("Mức log"), m_logLevel);

    m_logPath = new QLabel(this);
    m_logPath->setWordWrap(true);
    m_logPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString(), m_logPath);

    layout->addLayout(form);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(tokenBrowse, &QPushButton::clicked, this, &SettingsDialog::browseTokenFile);
    connect(appBrowse, &QPushButton::clicked, this, &SettingsDialog::browseControlApp);
    connect(m_logMode, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateLogHint);
    updateLogHint();

    // Size last, and from the layout rather than from a constant: the log hint
    // is a word-wrapped label that only gets its text in updateLogHint() just
    // above, and the same Vietnamese text wraps to one more line under the
    // RHEL font stack than under MinGW.  620x400 fits Windows and cut the hint
    // in half there.  Keep the old numbers as a floor so the kit that already
    // fitted does not change, and never open larger than the screen.
    QSize target = sizeHint().expandedTo(QSize(620, 400));
    if (const QScreen *display = screen())
        target = target.boundedTo(display->availableGeometry().size());
    resize(target);
}

void SettingsDialog::updateLogHint()
{
    const applog::Mode chosen = logcontrols::selectedMode(m_logMode);
    if (chosen == applog::Mode::Develop) {
        const QString path = applog::logFilePath();
        m_logPath->setText(
            path.isEmpty()
                ? QStringLiteral("Log sẽ được ghi vào thư mục dữ liệu ứng dụng (tệp "
                                 "s2t_qt.log, tự xoay vòng khi đầy 8 MB).")
                : QStringLiteral("Tệp log: %1").arg(path));
        return;
    }
    m_logPath->setText(QStringLiteral(
        "Log in ra console. Chỉ thấy được nếu mở ứng dụng từ cửa sổ lệnh; "
        "nếu mở bằng cách nhấp đúp thì hãy chọn chế độ Develop."));
}

void SettingsDialog::browseTokenFile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Chọn tệp token"));
    if (path.isEmpty())
        return;
    QString error;
    const QString token = AppConfig::tokenFromFile(path, &error);
    if (token.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Token"),
                             error.isEmpty() ? QStringLiteral("tệp token rỗng") : error);
        return;
    }
    m_token->setText(token);
}

void SettingsDialog::browseControlApp()
{
    // The host-control tool is xvf_host.exe on Windows and a plain ELF binary
    // called xvf_host on Linux, where an extension filter would hide the very
    // file the operator is looking for.
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Chọn %1").arg(kControlAppName),
                                     QString(), kControlAppFilter);
    if (!path.isEmpty())
        m_controlApp->setText(path);
}

void SettingsDialog::applyToConfig() const
{
    m_config->serverTarget = m_server->text().trimmed();
    m_config->apiToken = m_token->text().trimmed();
    m_config->inputDeviceId = m_device->currentData().toByteArray();
    m_config->expectedDeviceName = m_expectedName->text().trimmed();
    m_config->sampleRate = m_sampleRate->value();
    m_config->channels = m_channels->value();
    m_config->bufferSec = m_bufferSec->value();
    m_config->micControlApp = m_controlApp->text().trimmed();
    m_config->pipelineTrace = m_pipelineTrace->isChecked();
    m_config->paceFileReplay = m_paceReplay->isChecked();
    m_config->logMode = logcontrols::selectedMode(m_logMode);
    m_config->logLevel = logcontrols::selectedLevel(m_logLevel);
    // Applied straight away rather than at the next start: an operator who
    // switches to develop mode is usually about to reproduce something.  An
    // explicit choice here outranks --log-mode for the rest of the run.
    applog::setLevel(m_config->logLevel);
    applog::setMode(m_config->logMode);
    m_config->save();
}
