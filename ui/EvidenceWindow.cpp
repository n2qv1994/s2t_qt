#include "EvidenceWindow.h"

#include "../audio/WavIo.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

// Static description of which model family backs which stage.  It is a label,
// not evidence - the live READY/version table below it is the evidence.
const char kAsrLabel[] = "FastConformer CTC large tiếng Việt (asr_vi / asr_vi_long, ONNX+TensorRT)";
const char kVadLabel[] = "TEN-VAD (sherpa-onnx), khung 16ms";
const char kItnLabel[] = "BiLSTM chấm câu + PhoBERT streaming ITN";
const char kSpeakerLabel[] = "CAM++ (véc tơ đặc trưng 192 chiều)";
const char kDenoiseLabel[] =
    "Lọc nhiễu ở phần cứng microphone XVF3800 (bật/tắt qua PP_MIN_NS/PP_MIN_NN); "
    "không có xử lý khử nhiễu ở phần mềm - chỉ chuẩn hoá trước khi gửi server.";

const int kLatencyRows = 40;
const int kStagePageGuard = 50;

} // namespace

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void WaveformWidget::setWav(const QByteArray &wavBytes)
{
    m_envelope.clear();
    QString error;
    const wav::Pcm pcm = wav::parseWav(wavBytes, &error);
    if (pcm.isValid() && !pcm.frames.isEmpty()) {
        const auto *samples = reinterpret_cast<const qint16 *>(pcm.frames.constData());
        const int frameCount = pcm.frames.size() / (pcm.channels * 2);
        const int columns = qMax(1, width() > 0 ? width() : 420);
        const int perColumn = qMax(1, frameCount / columns);
        for (int column = 0; column < columns; ++column) {
            const int start = column * perColumn;
            if (start >= frameCount)
                break;
            const int end = qMin(frameCount, start + perColumn);
            float minimum = 1.0f;
            float maximum = -1.0f;
            for (int i = start; i < end; ++i) {
                const float value = float(samples[i * pcm.channels]) / 32768.0f;
                minimum = qMin(minimum, value);
                maximum = qMax(maximum, value);
            }
            m_envelope.append({minimum, maximum});
        }
    }
    update();
}

void WaveformWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x0c, 0x10, 0x17));
    if (m_envelope.isEmpty()) {
        painter.setPen(QColor(0x7d, 0x88, 0x94));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("chưa có mẫu"));
        return;
    }
    const double mid = height() / 2.0;
    painter.setPen(QColor(0x7e, 0xe7, 0x87));
    for (int i = 0; i < m_envelope.size(); ++i) {
        const double x = double(i) * width() / double(m_envelope.size());
        painter.drawLine(QPointF(x, mid - m_envelope.at(i).first * mid),
                         QPointF(x, mid - m_envelope.at(i).second * mid));
    }
}

EvidenceWindow::EvidenceWindow(SessionController *controller, QWidget *parent)
    : QWidget(parent, Qt::Window), m_controller(controller)
{
    setWindowTitle(QStringLiteral("Pipeline Evidence / Nghiệm thu"));
    resize(1200, 860);

    auto *outer = new QVBoxLayout(this);
    auto *top = new QHBoxLayout();
    m_session = new QLineEdit(this);
    m_session->setPlaceholderText(QStringLiteral("session_id (để trống = chỉ mô hình/thiết bị)"));
    auto *load = new QPushButton(QStringLiteral("Tải"), this);
    m_status = new QLabel(this);
    top->addWidget(m_session, 1);
    top->addWidget(load);
    top->addWidget(m_status, 1);
    outer->addLayout(top);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    auto *grid = new QGridLayout(body);

    auto *modelBox = new QGroupBox(QStringLiteral("Mô hình đang dùng (kiến trúc)"), body);
    auto *modelLayout = new QVBoxLayout(modelBox);
    m_architecture = new QLabel(
        QStringLiteral("<b>ASR:</b> %1<br><b>VAD:</b> %2<br><b>ITN / chấm câu:</b> %3<br>"
                       "<b>Nhận diện người nói:</b> %4<br><b>Lọc nhiễu:</b> %5")
            .arg(QString::fromUtf8(kAsrLabel), QString::fromUtf8(kVadLabel),
                 QString::fromUtf8(kItnLabel), QString::fromUtf8(kSpeakerLabel),
                 QString::fromUtf8(kDenoiseLabel)),
        modelBox);
    m_architecture->setWordWrap(true);
    modelLayout->addWidget(m_architecture);
    grid->addWidget(modelBox, 0, 0);

    auto *statusBox = new QGroupBox(
        QStringLiteral("Trạng thái model trong Triton (đọc trực tiếp, không hardcode)"), body);
    auto *statusLayout = new QVBoxLayout(statusBox);
    m_modelSummary = new QLabel(QStringLiteral("Đang tải..."), statusBox);
    statusLayout->addWidget(m_modelSummary);
    m_models = new QTableWidget(0, 3, statusBox);
    m_models->setHorizontalHeaderLabels(
        {QStringLiteral("Model"), QStringLiteral("Version"), QStringLiteral("State")});
    m_models->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_models->verticalHeader()->setVisible(false);
    statusLayout->addWidget(m_models);
    grid->addWidget(statusBox, 0, 1);

    // "&&" because a QGroupBox title reads a single "&" as a mnemonic marker
    // and swallows it - on X11 the title rendered as "Thiết bị _hàng đợi".
    auto *deviceBox = new QGroupBox(QStringLiteral("Thiết bị && hàng đợi (phiên hiện tại)"), body);
    auto *deviceLayout = new QVBoxLayout(deviceBox);
    m_device = new QLabel(deviceBox);
    m_device->setWordWrap(true);
    m_device->setTextFormat(Qt::RichText);
    deviceLayout->addWidget(m_device);
    grid->addWidget(deviceBox, 1, 0, 1, 2);

    auto *latencyBox = new QGroupBox(
        QStringLiteral("Log thời gian xử lý theo nhịp (mỗi gói audio gửi đi, ~160ms/gói)"), body);
    auto *latencyLayout = new QVBoxLayout(latencyBox);
    auto *latencyNote = new QLabel(
        QStringLiteral("Ghi theo thời gian thực, không phải đo một lần: mỗi dòng là một gói audio "
                       "— RTT gọi server, thời gian AI xử lý (server tự báo), phần mạng+gRPC ước "
                       "lượng, và độ dài hàng đợi hai phía tại thời điểm đó. Hàng đợi tăng dần "
                       "qua nhiều dòng liên tiếp = đang không theo kịp real-time."),
        latencyBox);
    latencyNote->setWordWrap(true);
    latencyLayout->addWidget(latencyNote);
    m_latency = new QTableWidget(0, 6, latencyBox);
    m_latency->setHorizontalHeaderLabels({QStringLiteral("Giờ"), QStringLiteral("RTT (ms)"),
                                          QStringLiteral("AI wait (ms)"),
                                          QStringLiteral("Mạng+gRPC (ms)"),
                                          QStringLiteral("Hàng đợi máy này (s)"),
                                          QStringLiteral("Hàng đợi server (s)")});
    m_latency->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_latency->verticalHeader()->setVisible(false);
    m_latency->setMinimumHeight(260);
    latencyLayout->addWidget(m_latency);
    grid->addWidget(latencyBox, 2, 0, 1, 2);

    auto *denoiseBox = new QGroupBox(QStringLiteral("Bằng chứng lọc nhiễu (ghi đối chứng tắt/bật)"),
                                     body);
    auto *denoiseLayout = new QVBoxLayout(denoiseBox);
    auto *denoiseNote = new QLabel(
        QStringLiteral("Không có AEC/AGC phần mềm — chỉ có công tắc lọc nhiễu phần cứng XVF3800. "
                       "Bấm để ghi hai đoạn ngắn liên tiếp (tắt rồi bật) và nghe trực tiếp thay "
                       "vì chỉ tin vào nhãn trạng thái. Yêu cầu không có phiên nào đang chạy."),
        denoiseBox);
    denoiseNote->setWordWrap(true);
    denoiseLayout->addWidget(denoiseNote);
    auto *denoiseRow = new QHBoxLayout();
    m_denoiseButton = new QPushButton(QStringLiteral("Ghi mẫu đối chứng (~7s)"), denoiseBox);
    m_denoiseStatus = new QLabel(denoiseBox);
    denoiseRow->addWidget(m_denoiseButton);
    denoiseRow->addWidget(m_denoiseStatus, 1);
    denoiseLayout->addLayout(denoiseRow);
    m_denoiseRestore = new QLabel(denoiseBox);
    m_denoiseRestore->setWordWrap(true);
    m_denoiseRestore->setVisible(false);
    denoiseLayout->addWidget(m_denoiseRestore);

    auto *clips = new QHBoxLayout();
    auto *offColumn = new QVBoxLayout();
    m_playOff = new QPushButton(QStringLiteral("▶ Tắt lọc nhiễu"), denoiseBox);
    m_playOff->setEnabled(false);
    m_waveOff = new WaveformWidget(denoiseBox);
    offColumn->addWidget(m_playOff);
    offColumn->addWidget(m_waveOff);
    auto *onColumn = new QVBoxLayout();
    m_playOn = new QPushButton(QStringLiteral("▶ Bật lọc nhiễu"), denoiseBox);
    m_playOn->setEnabled(false);
    m_waveOn = new WaveformWidget(denoiseBox);
    onColumn->addWidget(m_playOn);
    onColumn->addWidget(m_waveOn);
    clips->addLayout(offColumn, 1);
    clips->addLayout(onColumn, 1);
    denoiseLayout->addLayout(clips);
    grid->addWidget(denoiseBox, 3, 0, 1, 2);

    auto *vadBox = new QGroupBox(QStringLiteral("VAD / Segment (theo session_id)"), body);
    auto *vadLayout = new QVBoxLayout(vadBox);
    m_vad = new QLabel(QStringLiteral("Nhập session_id rồi bấm Tải."), vadBox);
    m_vad->setWordWrap(true);
    m_vad->setTextFormat(Qt::RichText);
    vadLayout->addWidget(m_vad);
    grid->addWidget(vadBox, 4, 0);

    auto *camppBox = new QGroupBox(QStringLiteral("CAM++ verify (theo session_id)"), body);
    auto *camppLayout = new QVBoxLayout(camppBox);
    m_campp = new QLabel(QStringLiteral("Nhập session_id rồi bấm Tải."), camppBox);
    m_campp->setWordWrap(true);
    m_campp->setTextFormat(Qt::RichText);
    camppLayout->addWidget(m_campp);
    grid->addWidget(camppBox, 4, 1);

    auto *speakerBox = new QGroupBox(QStringLiteral("Speaker của phiên"), body);
    auto *speakerLayout = new QVBoxLayout(speakerBox);
    m_speakers = new QTableWidget(0, 6, speakerBox);
    m_speakers->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("Tên"),
                                           QStringLiteral("Trạng thái"), QStringLiteral("Cửa sổ"),
                                           QStringLiteral("Thời lượng nói"),
                                           QStringLiteral("Diar slot")});
    m_speakers->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_speakers->verticalHeader()->setVisible(false);
    speakerLayout->addWidget(m_speakers);
    grid->addWidget(speakerBox, 5, 0, 1, 2);

    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    // Playback runs on whatever Qt Multimedia back end the machine has - the
    // FFmpeg one on a current Qt, GStreamer on RHEL's builds - and a missing
    // decoder plugin otherwise fails as silence with no explanation at all.
    // Say what the back end actually reported instead.
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                if (m_denoiseStatus)
                    m_denoiseStatus->setText(QStringLiteral("không phát được audio: %1").arg(message));
            });
    m_buffer = new QBuffer(this);

    m_deviceTimer = new QTimer(this);
    m_deviceTimer->setInterval(2000);
    connect(m_deviceTimer, &QTimer::timeout, this, [this]() {
        refreshDevice();
        refreshLatency();
    });
    m_deviceTimer->start();

    m_modelTimer = new QTimer(this);
    m_modelTimer->setInterval(15000);
    connect(m_modelTimer, &QTimer::timeout, this, &EvidenceWindow::refreshModels);
    m_modelTimer->start();

    connect(load, &QPushButton::clicked, this, &EvidenceWindow::loadSessionEvidence);
    connect(m_denoiseButton, &QPushButton::clicked, this, &EvidenceWindow::startDenoiseAb);
    connect(m_controller, &SessionController::denoiseAbReady, this,
            &EvidenceWindow::onDenoiseAbReady);
    connect(m_playOff, &QPushButton::clicked, this, [this]() {
        m_player->stop();
        m_player->setSourceDevice(nullptr);
        m_buffer->close();
        m_buffer->setData(m_offWav);
        m_buffer->open(QIODevice::ReadOnly);
        m_player->setSourceDevice(m_buffer);
        m_player->play();
    });
    connect(m_playOn, &QPushButton::clicked, this, [this]() {
        m_player->stop();
        m_player->setSourceDevice(nullptr);
        m_buffer->close();
        m_buffer->setData(m_onWav);
        m_buffer->open(QIODevice::ReadOnly);
        m_player->setSourceDevice(m_buffer);
        m_player->play();
    });

    refreshDevice();
    refreshModels();
    refreshLatency();
}

void EvidenceWindow::setSession(const QString &sessionId)
{
    if (!sessionId.isEmpty())
        m_session->setText(sessionId);
}

void EvidenceWindow::refreshDevice()
{
    const SessionTelemetry &telemetry = m_controller->telemetry();
    const MicStatus status = m_controller->micStatus();
    QString history;
    const QList<MicStatusTransition> &transitions = m_controller->micStatusHistory();
    for (int i = transitions.size() - 1; i >= 0 && i >= transitions.size() - 10; --i) {
        const MicStatusTransition &item = transitions.at(i);
        history += QStringLiteral("%1  %2 → %3<br>")
                       .arg(QDateTime::fromMSecsSinceEpoch(qint64(item.ts * 1000.0))
                                .toString(QStringLiteral("HH:mm:ss")),
                            item.from.isEmpty() ? QStringLiteral("(khởi tạo)") : item.from,
                            item.to);
    }
    const qint64 elapsed = m_controller->micStatusSince() > 0
        ? qint64(QDateTime::currentMSecsSinceEpoch() / 1000 - m_controller->micStatusSince())
        : -1;

    m_device->setText(
        QStringLiteral(
            "<b>Thiết bị đang dùng:</b> %1<br>"
            "<b>Trạng thái:</b> %2%3<br>"
            "<b>Cảnh báo thiết bị:</b> %4<br>"
            "<b>Lọc nhiễu (phần cứng):</b> %5<br>"
            "<b>Hàng đợi máy này:</b> %6s &nbsp; <b>Hàng đợi server AI:</b> %7s<br>"
            "<b>RTT gọi server (gần nhất / max):</b> %8ms / %9ms<br>"
            "<b>Gói bị rớt (client):</b> %10 &nbsp; <b>Giới hạn speaker phiên:</b> %11<br>"
            "<b>State poll (gần nhất / max):</b> %12ms / %13ms%14"
            "%15")
            .arg(m_controller->deviceName().isEmpty() ? QStringLiteral("(chưa xác định)")
                                                      : m_controller->deviceName(),
                 micStatusKey(status).isEmpty() ? QStringLiteral("(chưa chạy)")
                                                : micStatusKey(status),
                 elapsed >= 0 ? QStringLiteral(" (%1m%2s kể từ lần chuyển gần nhất)")
                                    .arg(elapsed / 60)
                                    .arg(elapsed % 60, 2, 10, QLatin1Char('0'))
                              : QString(),
                 m_controller->deviceWarning().isEmpty() ? QStringLiteral("-")
                                                         : m_controller->deviceWarning(),
                 m_controller->denoiseStateKey())
            .arg(telemetry.localQueueSec, 0, 'f', 2)
            .arg(telemetry.serverQueueSec, 0, 'f', 2)
            .arg(telemetry.rpcLastMs, 0, 'f', 0)
            .arg(telemetry.rpcMaxMs, 0, 'f', 0)
            .arg(telemetry.droppedChunks)
            .arg(m_controller->speakerFilterLabel())
            .arg(telemetry.statePollLastMs, 0, 'f', 0)
            .arg(telemetry.statePollMaxMs, 0, 'f', 0)
            .arg(telemetry.pollError.isEmpty()
                     ? QString()
                     : QStringLiteral("<br><span style='color:#b71c1c;'>poll: %1</span>")
                           .arg(telemetry.pollError))
            .arg(history.isEmpty()
                     ? QString()
                     : QStringLiteral("<br><br><b>Lịch sử chuyển trạng thái:</b><br>%1")
                           .arg(history)));
}

void EvidenceWindow::refreshModels()
{
    m_controller->rpc()->call<asr::ModelStatusResponse>(
        this,
        [](AsrClient &client, asr::ModelStatusResponse &out) {
            return client.getModelStatus(&out, 15000);
        },
        [this](const grpc::Status &status, const asr::ModelStatusResponse &response) {
            if (!status.ok()) {
                m_modelSummary->setText(
                    QStringLiteral("<span style='color:#b71c1c;'>Không đọc được từ Triton: %1</span>")
                        .arg(status.toString()));
                m_models->setRowCount(0);
                return;
            }
            int ready = 0;
            m_models->setRowCount(response.models.size());
            for (int i = 0; i < response.models.size(); ++i) {
                const asr::ModelStatusEntry &entry = response.models.at(i);
                const bool isReady = entry.state.toUpper() == QLatin1String("READY");
                if (isReady)
                    ++ready;
                m_models->setItem(i, 0, new QTableWidgetItem(entry.name));
                m_models->setItem(i, 1, new QTableWidgetItem(
                                            entry.version.isEmpty() ? QStringLiteral("-")
                                                                    : entry.version));
                auto *state = new QTableWidgetItem(entry.state);
                state->setForeground(isReady ? QColor(0x1b, 0x5e, 0x20)
                                             : QColor(0xb7, 0x1c, 0x1c));
                m_models->setItem(i, 2, state);
            }
            m_modelSummary->setText(QStringLiteral("%1/%2 READY")
                                        .arg(ready)
                                        .arg(response.models.size()));
        });
}

void EvidenceWindow::refreshLatency()
{
    const QList<LatencySample> &history = m_controller->latencyHistory();
    if (history.isEmpty()) {
        m_latency->setRowCount(0);
        return;
    }
    // Newest first, most recent rows only: a full 300-entry log at once is
    // more than anyone needs to eyeball a trend.
    const int count = qMin(kLatencyRows, history.size());
    m_latency->setRowCount(count);
    for (int i = 0; i < count; ++i) {
        const LatencySample &sample = history.at(history.size() - 1 - i);
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(qint64(sample.ts * 1000.0));
        m_latency->setItem(i, 0, new QTableWidgetItem(when.toString(QStringLiteral("HH:mm:ss.zzz"))));
        m_latency->setItem(i, 1,
                           new QTableWidgetItem(QString::number(sample.rpcMs, 'f', 0)));
        m_latency->setItem(i, 2,
                           new QTableWidgetItem(QString::number(sample.aiWaitMs, 'f', 0)));
        m_latency->setItem(i, 3,
                           new QTableWidgetItem(QString::number(sample.transportMs, 'f', 0)));
        auto *localQueue =
            new QTableWidgetItem(QString::number(sample.localQueueSec, 'f', 2));
        auto *serverQueue = new QTableWidgetItem(QString::number(sample.serverQueueSec, 'f', 2));
        // Any non-trivial queue depth is the direct signal of "not keeping
        // up"; flag it rather than making the reader do the arithmetic.
        const bool bad = sample.localQueueSec > 1.0 || sample.serverQueueSec > 1.0;
        const bool warn = sample.localQueueSec > 0.3 || sample.serverQueueSec > 0.3;
        if (bad || warn) {
            const QColor color = bad ? QColor(0xb7, 0x1c, 0x1c) : QColor(0x8d, 0x6e, 0x00);
            localQueue->setForeground(color);
            serverQueue->setForeground(color);
        }
        m_latency->setItem(i, 4, localQueue);
        m_latency->setItem(i, 5, serverQueue);
    }
}

void EvidenceWindow::countStage(const QString &sessionId, const QString &stage, int generation,
                                std::function<void(const QList<asr::PipelineTraceEvent> &)> done)
{
    // Pages one stage to completion (bounded), summing seq pages via
    // after_seq/next_seq exactly like the trace tester does.
    auto collected = QSharedPointer<QList<asr::PipelineTraceEvent>>::create();
    auto step = QSharedPointer<std::function<void(quint64, int)>>::create();
    *step = [this, sessionId, stage, generation, done, collected, step](quint64 after, int guard) {
        asr::PipelineTraceRequest request;
        request.sessionId = sessionId;
        request.afterSeq = after;
        request.limit = 1000;
        request.stages << stage;
        m_controller->rpc()->call<asr::PipelineTraceResponse>(
            this,
            [request](AsrClient &client, asr::PipelineTraceResponse &out) {
                return client.getPipelineTrace(request, &out, 30000);
            },
            [this, generation, done, collected, step, guard](
                const grpc::Status &status, const asr::PipelineTraceResponse &response) {
                if (generation != m_generation)
                    return;
                if (!status.ok()) {
                    done(*collected);
                    return;
                }
                collected->append(response.events);
                if (response.hasMore && guard + 1 < kStagePageGuard) {
                    (*step)(response.nextSeq, guard + 1);
                    return;
                }
                done(*collected);
            });
    };
    (*step)(0, 0);
}

void EvidenceWindow::loadSessionEvidence()
{
    const QString sessionId = m_session->text().trimmed();
    if (sessionId.isEmpty()) {
        m_status->setText(QStringLiteral("chưa nhập session_id - chỉ tải mô hình/thiết bị"));
        return;
    }
    ++m_generation;
    const int generation = m_generation;
    m_status->setText(QStringLiteral("đang tải..."));
    m_vad->setText(QStringLiteral("Đang tải..."));
    m_campp->setText(QStringLiteral("Đang tải..."));

    auto diarCount = QSharedPointer<int>::create(-1);
    auto boundaryCount = QSharedPointer<int>::create(0);
    auto streamCount = QSharedPointer<int>::create(-1);
    auto correctionCount = QSharedPointer<int>::create(-1);
    auto renderVad = [this, diarCount, boundaryCount, streamCount, correctionCount]() {
        if (*diarCount < 0 || *streamCount < 0 || *correctionCount < 0)
            return;
        m_vad->setText(QStringLiteral("<b>Đoạn diar đã xử lý:</b> %1<br>"
                                      "<b>Trong đó có chuyển biên (đổi người nói):</b> %2<br>"
                                      "<b>Lượt streaming ASR:</b> %3<br>"
                                      "<b>Lượt correction/chỉnh sửa lại:</b> %4")
                           .arg(*diarCount)
                           .arg(*boundaryCount)
                           .arg(*streamCount)
                           .arg(*correctionCount));
        m_status->setText(QStringLiteral("xong"));
    };

    countStage(sessionId, QStringLiteral("diar"), generation,
               [diarCount, boundaryCount, renderVad](const QList<asr::PipelineTraceEvent> &events) {
                   *diarCount = events.size();
                   int boundaries = 0;
                   for (const asr::PipelineTraceEvent &event : events) {
                       const QJsonObject payload =
                           QJsonDocument::fromJson(event.payloadJson.toUtf8()).object();
                       if (payload.value(QStringLiteral("next_temporary_diar_slot")).toInt(-1) >= 0)
                           ++boundaries;
                   }
                   *boundaryCount = boundaries;
                   renderVad();
               });
    countStage(sessionId, QStringLiteral("streaming_asr"), generation,
               [streamCount, renderVad](const QList<asr::PipelineTraceEvent> &events) {
                   *streamCount = events.size();
                   renderVad();
               });
    countStage(sessionId, QStringLiteral("correction_asr"), generation,
               [correctionCount, renderVad](const QList<asr::PipelineTraceEvent> &events) {
                   *correctionCount = events.size();
                   renderVad();
               });

    countStage(sessionId, QStringLiteral("campp_verify"), generation,
               [this](const QList<asr::PipelineTraceEvent> &events) {
                   QSet<QString> modes;
                   double scoreSum = 0.0;
                   int scoreCount = 0;
                   int unknown = 0;
                   for (const asr::PipelineTraceEvent &event : events) {
                       const QJsonObject payload =
                           QJsonDocument::fromJson(event.payloadJson.toUtf8()).object();
                       const QString mode =
                           payload.value(QStringLiteral("database_mode")).toString();
                       if (!mode.isEmpty())
                           modes.insert(mode);
                       if (payload.contains(QStringLiteral("best_score"))) {
                           scoreSum += payload.value(QStringLiteral("best_score")).toDouble();
                           ++scoreCount;
                       }
                       const QString best = payload.value(QStringLiteral("best_name")).toString();
                       if (best.isEmpty() || best.toLower() == QLatin1String("unknown"))
                           ++unknown;
                   }
                   QStringList modeList(modes.constBegin(), modes.constEnd());
                   m_campp->setText(
                       QStringLiteral("<b>Số lần verify:</b> %1<br>"
                                      "<b>Chế độ database dùng:</b> %2<br>"
                                      "<b>Điểm trung bình:</b> %3<br>"
                                      "<b>Không nhận diện được (unknown):</b> %4 / %5")
                           .arg(events.size())
                           .arg(modeList.isEmpty() ? QStringLiteral("-")
                                                   : modeList.join(QStringLiteral(", ")))
                           .arg(scoreCount ? QString::number(scoreSum / scoreCount, 'f', 3)
                                           : QStringLiteral("-"))
                           .arg(unknown)
                           .arg(events.size()));
               });

    reg::ListSessionSpeakersRequest speakerRequest;
    speakerRequest.sessionId = sessionId;
    m_controller->rpc()->call<reg::ListSessionSpeakersResponse>(
        this,
        [speakerRequest](AsrClient &client, reg::ListSessionSpeakersResponse &out) {
            return client.listSessionSpeakers(speakerRequest, &out, 30000);
        },
        [this, generation](const grpc::Status &status,
                           const reg::ListSessionSpeakersResponse &response) {
            if (generation != m_generation)
                return;
            m_speakers->setRowCount(0);
            if (!status.ok())
                return;
            m_speakers->setRowCount(response.speakers.size());
            for (int i = 0; i < response.speakers.size(); ++i) {
                const reg::SessionSpeakerEntry &entry = response.speakers.at(i);
                m_speakers->setItem(i, 0, new QTableWidgetItem(entry.sessionSpeakerId));
                m_speakers->setItem(i, 1, new QTableWidgetItem(
                                              entry.verifiedName.isEmpty() ? QStringLiteral("-")
                                                                           : entry.verifiedName));
                m_speakers->setItem(i, 2, new QTableWidgetItem(entry.status));
                m_speakers->setItem(i, 3,
                                    new QTableWidgetItem(QString::number(entry.windows)));
                m_speakers->setItem(
                    i, 4,
                    new QTableWidgetItem(entry.hasEvidence
                                             ? QStringLiteral("%1s").arg(
                                                   entry.evidence.totalSpeechSec, 0, 'f', 2)
                                             : QStringLiteral("-")));
                m_speakers->setItem(
                    i, 5, new QTableWidgetItem(entry.diarSlots.join(QStringLiteral(", "))));
            }
        });
}

void EvidenceWindow::startDenoiseAb()
{
    m_denoiseButton->setEnabled(false);
    m_denoiseRestore->setVisible(false);
    m_denoiseStatus->setText(QStringLiteral("đang ghi (tắt lọc nhiễu trước, rồi bật)..."));
    m_controller->captureDenoiseAb(3.0);
}

void EvidenceWindow::onDenoiseAbReady(const mic::DenoiseAbResult &result)
{
    m_denoiseButton->setEnabled(true);
    if (!result.error.isEmpty()) {
        m_denoiseStatus->setText(QStringLiteral("Lỗi: %1").arg(result.error));
        return;
    }
    m_offWav = result.offWav;
    m_onWav = result.onWav;
    m_waveOff->setWav(m_offWav);
    m_waveOn->setWav(m_onWav);
    m_playOff->setEnabled(!m_offWav.isEmpty());
    m_playOn->setEnabled(!m_onWav.isEmpty());
    m_denoiseStatus->setText(QStringLiteral("xong — nghe hoặc so waveform để đối chứng"));

    if (!result.restored) {
        // Two distinct honest cases, never collapsed into one: the prior state
        // was never known (nothing was even attempted), or it WAS known and
        // the restore command itself failed.
        m_denoiseRestore->setVisible(true);
        const QString message = result.restoreAttempted
            ? QStringLiteral("Đã BIẾT trạng thái lọc nhiễu trước khi ghi mẫu (<b>%1</b>) nhưng "
                             "lệnh khôi phục THẤT BẠI: %2.")
                  .arg(result.priorState, result.restoreError.isEmpty()
                                              ? QStringLiteral("không rõ lỗi")
                                              : result.restoreError)
            : QStringLiteral("Chưa xác định được trạng thái lọc nhiễu TRƯỚC khi ghi mẫu "
                             "(ứng dụng mới khởi động, chưa có thao tác nào) - KHÔNG khôi phục.");
        m_denoiseRestore->setText(
            QStringLiteral("<span style='color:#b71c1c;'>%1 Sau khi ghi mẫu, lọc nhiễu hiện "
                           "đang: <b>%2</b>. Tự kiểm tra lại nếu cần đúng trạng thái ban đầu.</span>")
                .arg(message, result.finalState));
    }
}
