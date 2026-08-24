#include "EnrollDialog.h"

#include "../audio/AudioCapture.h"
#include "../audio/WavIo.h"
#include "../core/Logger.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString statusStyle(const QString &kind)
{
    if (kind == QLatin1String("ok"))
        return QStringLiteral("background:#e8f5e9; color:#1b5e20; padding:8px;");
    if (kind == QLatin1String("err"))
        return QStringLiteral("background:#ffebee; color:#b71c1c; padding:8px;");
    if (kind == QLatin1String("warn"))
        return QStringLiteral("background:#fff8e1; color:#8d6e00; padding:8px;");
    if (kind == QLatin1String("busy"))
        return QStringLiteral("background:#e3f2fd; color:#0d47a1; padding:8px;");
    return QString();
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
    resize(880, 720);

    auto *layout = new QVBoxLayout(this);
    auto *header = new QLabel(
        m_editorId.isEmpty()
            ? QStringLiteral("<b style='color:#b71c1c;'>Chưa nhập tên người thao tác</b> — "
                             "nhập ở thanh trên của cửa sổ chính trước khi ghi âm hoặc lưu.")
            : QStringLiteral("Người thao tác: <b>%1</b>").arg(m_editorId.toHtmlEscaped()),
        this);
    header->setWordWrap(true);
    layout->addWidget(header);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildEnrollTab(), QStringLiteral("Đăng ký giọng"));
    tabs->addTab(buildSessionTab(), QStringLiteral("Speaker của phiên"));
    layout->addWidget(tabs, 1);

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
    m_belowPolicy->setMaximumHeight(120);
    m_belowPolicy->setWordWrap(true);
    layout->addWidget(m_belowPolicy);

    m_script = new QTextEdit(page);
    m_script->setReadOnly(true);
    m_script->setPlainText(QStringLiteral("đang tải đoạn văn bản..."));
    m_script->setStyleSheet(QStringLiteral("font-size:16px; background:#f5f5f5;"));
    layout->addWidget(m_script, 1);

    auto *form = new QFormLayout();
    m_speakerName = new QLineEdit(page);
    m_speakerName->setMaxLength(64);
    m_speakerName->setPlaceholderText(QStringLiteral("Ví dụ: Nguyễn Văn A"));
    form->addRow(QStringLiteral("Tên người nói"), m_speakerName);
    layout->addLayout(form);

    auto *row = new QHBoxLayout();
    m_recordButton = new QPushButton(QStringLiteral("Bắt đầu ghi âm"), page);
    m_timerLabel = new QLabel(QStringLiteral("0.0s"), page);
    m_retryButton = new QPushButton(QStringLiteral("Gửi lại bản ghi"), page);
    m_retryButton->setVisible(false);
    row->addWidget(m_recordButton);
    row->addWidget(m_timerLabel);
    row->addWidget(m_retryButton);
    row->addStretch();
    layout->addLayout(row);

    m_status = new QLabel(page);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *rosterGroup = new QGroupBox(QStringLiteral("Danh sách giọng toàn cục"), page);
    auto *rosterLayout = new QVBoxLayout(rosterGroup);
    auto *reload = new QPushButton(QStringLiteral("Tải lại"), rosterGroup);
    rosterLayout->addWidget(reload);
    m_roster = new QListWidget(rosterGroup);
    rosterLayout->addWidget(m_roster);
    layout->addWidget(rosterGroup);

    connect(m_recordButton, &QPushButton::clicked, this, &EnrollDialog::toggleRecording);
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
                  QStringLiteral("Vui lòng nhập tên người thao tác (thanh trên) trước khi ghi âm."));
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
            QStringLiteral("Vui lòng nhập tên người thao tác (thanh trên) trước khi lưu."));
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
