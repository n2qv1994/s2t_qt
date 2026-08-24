#include "TraceWindow.h"

#include "audio/WavIo.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

// A long session would otherwise append one card per event forever.
const int kMaxLiveCards = 400;
// Both caps exist so a malformed or oversized payload cannot turn into
// hundreds of RPCs or minutes of stitched audio.  Exceeding either is a hard
// refusal, never a silent truncation: a button that quietly stitched the
// first 200 of 350 real spans would still say "350" while playing back audio
// missing a third of what the model actually received.
const int kStitchMaxSpans = 200;
const double kStitchMaxTotalSec = 300.0;

QString clock(double seconds)
{
    const double value = qMax(0.0, seconds);
    const int minutes = int(value) / 60;
    const double rest = value - minutes * 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(rest, 5, 'f', 2, QLatin1Char('0'));
}

QString updateKindLabel(const QString &kind)
{
    if (kind == QLatin1String("insert"))
        return QStringLiteral("Thêm phần mới");
    if (kind == QLatin1String("replace"))
        return QStringLiteral("Thay phần cũ bằng phần mới");
    if (kind == QLatin1String("delete"))
        return QStringLiteral("Xóa phần cũ");
    if (kind == QLatin1String("no_text_change"))
        return QStringLiteral("Không đổi chữ; có thể chỉ đổi timestamp/confidence");
    return kind;
}

QString wordsText(const QJsonValue &value)
{
    if (!value.isArray())
        return QString();
    QStringList parts;
    for (const QJsonValue &entry : value.toArray()) {
        const QString word = entry.toObject().value(QStringLiteral("w")).toString().trimmed();
        if (!word.isEmpty())
            parts << word;
    }
    return parts.join(QLatin1Char(' '));
}

QString firstNonEmpty(const QJsonObject &payload, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString value = payload.value(key).toString();
        if (!value.isEmpty())
            return value;
    }
    return QString();
}

void appendLine(QString *out, const QString &label, const QString &value)
{
    if (value.isEmpty())
        return;
    *out += QStringLiteral("\n    %1: %2").arg(label, value);
}

// The header range and the continuous-audio link are for listenability, and
// are not necessarily byte-for-byte what a stage received - VAD can compress a
// window into disjoint speech spans.  These are the fields that carry the
// real spans, per stage.
QString spanFieldForStage(const QString &stage)
{
    if (stage == QLatin1String("campp_verify"))
        return QStringLiteral("audio_source_spans");
    if (stage == QLatin1String("streaming_asr"))
        return QStringLiteral("window_source_spans");
    if (stage == QLatin1String("correction_asr"))
        return QStringLiteral("decode_audio_source_spans");
    return QString();
}

QString describe(const asr::PipelineTraceEvent &event, const QJsonObject &payload)
{
    QString out = QStringLiteral("#%1  %2 · %3   %4 → %5")
                      .arg(event.seq)
                      .arg(event.stage, event.event, clock(event.audioStartSec),
                           clock(event.audioEndSec));

    if (event.stage == QLatin1String("streaming_asr")) {
        appendLine(&out, QStringLiteral("MODEL 8S"),
                   firstNonEmpty(payload, {QStringLiteral("model_text"),
                                           QStringLiteral("raw_asr_text")}));
        if (payload.contains(QStringLiteral("word_start_sec"))) {
            appendLine(&out, QStringLiteral("KHOẢNG TỪ DECODE ĐƯỢC"),
                       QStringLiteral("%1 → %2")
                           .arg(clock(payload.value(QStringLiteral("word_start_sec")).toDouble()),
                                clock(payload.value(QStringLiteral("word_end_sec")).toDouble())));
        }
        appendLine(&out, QStringLiteral("BỎ TRÊN UI"),
                   payload.value(QStringLiteral("removed_text")).toString());
        appendLine(&out, QStringLiteral("GHI LÊN UI"),
                   firstNonEmpty(payload, {QStringLiteral("ui_text"),
                                           QStringLiteral("streaming_itn_text")}));
        appendLine(&out, QStringLiteral("THAO TÁC UI"),
                   updateKindLabel(payload.value(QStringLiteral("update_kind")).toString()));
    } else if (event.stage == QLatin1String("correction_asr")) {
        appendLine(&out, QStringLiteral("MODEL CORRECTION"),
                   firstNonEmpty(payload, {QStringLiteral("model_text"),
                                           QStringLiteral("decoded_text")}));
        if (payload.value(QStringLiteral("core_start_sec")).toDouble(-1) >= 0) {
            appendLine(&out, QStringLiteral("CORE ĐƯỢC PUBLISH"),
                       QStringLiteral("%1 → %2")
                           .arg(clock(payload.value(QStringLiteral("core_start_sec")).toDouble()),
                                clock(payload.value(QStringLiteral("core_end_sec")).toDouble())));
        }
        if (payload.contains(QStringLiteral("left_context_samples"))) {
            appendLine(&out, QStringLiteral("CONTEXT TRÁI/PHẢI (mẫu)"),
                       QStringLiteral("%1 trái / %2 phải · cửa sổ decode: %3 mẫu")
                           .arg(payload.value(QStringLiteral("left_context_samples")).toInt())
                           .arg(payload.value(QStringLiteral("right_context_samples")).toInt())
                           .arg(payload.value(QStringLiteral("decode_audio_samples")).toInt()));
        }
    } else if (event.stage == QLatin1String("correction_a2_merge")) {
        appendLine(&out, QStringLiteral("CÂU A2 ĐANG CÓ"),
                   payload.value(QStringLiteral("left_text")).toString());
        appendLine(&out, QStringLiteral("CÂU CORRECTION MỚI"),
                   firstNonEmpty(payload, {QStringLiteral("right_text"),
                                           QStringLiteral("input_text")}));
        appendLine(&out, QStringLiteral("KẾT QUẢ MERGE A2"),
                   firstNonEmpty(payload, {QStringLiteral("result_text"), QStringLiteral("ui_text")}));
    } else if (event.stage == QLatin1String("correction_itn_merge")
               || event.stage == QLatin1String("correction_merge")) {
        QString left = firstNonEmpty(payload, {QStringLiteral("left_text"),
                                               QStringLiteral("previous_text")});
        if (left.isEmpty())
            left = wordsText(payload.value(QStringLiteral("previous_snapshot")));
        QString right = firstNonEmpty(payload, {QStringLiteral("right_text"),
                                                QStringLiteral("input_text")});
        if (right.isEmpty())
            right = wordsText(payload.value(QStringLiteral("incoming_itn_records")));
        if (right.isEmpty())
            right = wordsText(payload.value(QStringLiteral("incoming_chunk_words")));
        QString result = firstNonEmpty(payload, {QStringLiteral("result_text"),
                                                 QStringLiteral("ui_text")});
        if (result.isEmpty())
            result = wordsText(payload.value(QStringLiteral("merged_snapshot")));
        appendLine(&out, QStringLiteral("TRANSCRIPT ĐANG CÓ"), left);
        appendLine(&out, QStringLiteral("CORRECTION MỚI"), right);
        appendLine(&out, QStringLiteral("KẾT QUẢ MERGE"), result);
    } else if (event.stage == QLatin1String("itn")) {
        QString input = payload.value(QStringLiteral("input_text")).toString();
        if (input.isEmpty())
            input = wordsText(payload.value(QStringLiteral("input_words")));
        appendLine(&out, QStringLiteral("TEXT VÀO ITN"), input);
        appendLine(&out, QStringLiteral("MODEL ITN TRẢ"),
                   firstNonEmpty(payload, {QStringLiteral("model_text"),
                                           QStringLiteral("output_text")}));
    } else if (event.stage == QLatin1String("system_update")) {
        appendLine(&out, QStringLiteral("BỎ TRÊN UI"),
                   payload.value(QStringLiteral("removed_text")).toString());
        appendLine(&out, QStringLiteral("GHI LÊN UI"),
                   firstNonEmpty(payload, {QStringLiteral("ui_text"),
                                           QStringLiteral("replacement_text")}));
        appendLine(&out, QStringLiteral("THAO TÁC UI"),
                   updateKindLabel(payload.value(QStringLiteral("update_kind")).toString()));
    } else if (event.stage == QLatin1String("diar")) {
        const int next = payload.value(QStringLiteral("next_temporary_diar_slot")).toInt(-1);
        appendLine(&out, QStringLiteral("SEGMENT DIAR"),
                   QStringLiteral("slot %1 · xác suất TB %2")
                       .arg(payload.value(QStringLiteral("temporary_diar_slot")).toInt())
                       .arg(payload.value(QStringLiteral("speaker_probability")).toDouble(), 0, 'f', 3));
        appendLine(&out, QStringLiteral("BIÊN TIẾP THEO"),
                   next >= 0 ? QStringLiteral("chuyển sang slot %1").arg(next)
                             : QStringLiteral("kết thúc audio"));
    } else if (event.stage == QLatin1String("campp_verify")) {
        appendLine(&out, QStringLiteral("SEGMENT VERIFY"),
                   QStringLiteral("segment %1 · %2 lần feed · tổng %3s speech chunk")
                       .arg(payload.value(QStringLiteral("verify_segment_id")).toInt())
                       .arg(payload.value(QStringLiteral("feed_calls")).toInt())
                       .arg(payload.value(QStringLiteral("fed_duration_sec")).toDouble(), 0, 'f', 3));
        QStringList candidates;
        for (const QJsonValue &value :
             payload.value(QStringLiteral("database_candidates")).toArray())
            candidates << value.toString();
        appendLine(&out, QStringLiteral("DATABASE"),
                   QStringLiteral("%1 · %2")
                       .arg(payload.value(QStringLiteral("database_mode")).toString(),
                            candidates.isEmpty() ? QStringLiteral("toàn bộ DB")
                                                 : candidates.join(QStringLiteral(", "))));
        appendLine(&out, QStringLiteral("KẾT QUẢ CUỐI SEGMENT"),
                   QStringLiteral("%1 · %2")
                       .arg(payload.value(QStringLiteral("best_name")).toString(
                                QStringLiteral("unknown")))
                       .arg(payload.value(QStringLiteral("best_score")).toDouble(), 0, 'f', 3));
    }

    const QString spanField = spanFieldForStage(event.stage);
    if (!spanField.isEmpty()) {
        const QJsonArray spans = payload.value(spanField).toArray();
        const bool truncated = payload.value(spanField + QStringLiteral("_truncated")).toBool();
        if (!spans.isEmpty()) {
            const int total = payload.value(spanField.chopped(1) + QStringLiteral("_count")).toInt(
                spans.size());
            // model.py caps how many disjoint spans one trace event may carry.
            // If it did, the stitched playback below covers LESS than the real
            // window, and saying so is the whole point.
            appendLine(&out, QStringLiteral("SPAN NGUỒN THẬT"),
                       truncated
                           ? QStringLiteral("%1/%2 span · ⚠ model đã cắt bớt danh sách - audio "
                                            "ghép KHÔNG đủ toàn bộ input thật")
                                 .arg(spans.size())
                                 .arg(total)
                           : QStringLiteral("%1 span").arg(spans.size()));
        }
    }
    return out;
}

} // namespace

TraceWindow::TraceWindow(SessionController *controller, QWidget *parent)
    : QWidget(parent, Qt::Window), m_controller(controller)
{
    setWindowTitle(QStringLiteral("Pipeline Trace Tester"));
    resize(1100, 760);

    auto *layout = new QVBoxLayout(this);
    auto *top = new QHBoxLayout();
    m_session = new QLineEdit(this);
    m_session->setPlaceholderText(QStringLiteral("session_id (để trống = phiên hiện tại)"));
    top->addWidget(m_session, 1);

    m_stage = new QComboBox(this);
    m_stage->addItem(QStringLiteral("tất cả stage"), QString());
    for (const char *stage : {"streaming_asr", "correction_asr", "correction_a2_merge",
                              "correction_itn_merge", "correction_merge", "itn", "system_update",
                              "diar", "campp_verify"}) {
        m_stage->addItem(QString::fromLatin1(stage), QString::fromLatin1(stage));
    }
    top->addWidget(m_stage);

    auto *load = new QPushButton(QStringLiteral("Load"), this);
    auto *clear = new QPushButton(QStringLiteral("Clear"), this);
    top->addWidget(load);
    top->addWidget(clear);

    m_follow = new QCheckBox(QStringLiteral("realtime"), this);
    m_follow->setChecked(true);
    top->addWidget(m_follow);

    m_afterSeq = new QSpinBox(this);
    m_afterSeq->setRange(0, 1000000000);
    m_afterSeq->setPrefix(QStringLiteral("after_seq "));
    m_afterSeq->setVisible(false);
    top->addWidget(m_afterSeq);
    m_loadPage = new QPushButton(QStringLiteral("Tải trang lịch sử"), this);
    m_loadPage->setVisible(false);
    top->addWidget(m_loadPage);
    layout->addLayout(top);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    m_cards = new QListWidget(this);
    m_cards->setWordWrap(true);
    m_cards->setStyleSheet(QStringLiteral("font-family:monospace; font-size:11px;"));
    layout->addWidget(m_cards, 1);

    auto *bottom = new QHBoxLayout();
    auto *play = new QPushButton(QStringLiteral("Phát audio raw của sự kiện"), this);
    // "&&" - a lone "&" in a button label is taken as the mnemonic marker.
    auto *stitch = new QPushButton(QStringLiteral("Ghép && nghe span thật"), this);
    stitch->setToolTip(QStringLiteral(
        "Ghép đúng các đoạn audio model thực sự nhận. Khác với nút bên trái: khoảng "
        "liên tục ở trên chỉ để nghe cho dễ, có thể gồm cả phần VAD đã cắt bỏ."));
    bottom->addWidget(play);
    bottom->addWidget(stitch);
    bottom->addStretch();
    layout->addLayout(bottom);

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    // Playback runs on whatever Qt Multimedia back end the machine has - the
    // FFmpeg one on a current Qt, GStreamer on RHEL's builds - and a missing
    // decoder plugin otherwise fails as silence with no explanation at all.
    // Say what the back end actually reported instead.
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                if (m_status)
                    m_status->setText(QStringLiteral("không phát được audio: %1").arg(message));
            });
    m_buffer = new QBuffer(this);

    m_timer = new QTimer(this);
    m_timer->setInterval(500);

    connect(load, &QPushButton::clicked, this, &TraceWindow::reload);
    connect(clear, &QPushButton::clicked, this, [this]() {
        ++m_generation;
        m_afterLive = 0;
        m_cards->clear();
        m_status->clear();
    });
    connect(m_session, &QLineEdit::editingFinished, this, &TraceWindow::reload);
    connect(m_stage, &QComboBox::currentIndexChanged, this, [this](int) { reload(); });
    connect(m_follow, &QCheckBox::toggled, this, &TraceWindow::syncMode);
    connect(m_loadPage, &QPushButton::clicked, this, &TraceWindow::loadHistoryPage);
    connect(m_timer, &QTimer::timeout, this, &TraceWindow::poll);
    connect(play, &QPushButton::clicked, this, &TraceWindow::playSelectedRange);
    connect(stitch, &QPushButton::clicked, this, &TraceWindow::stitchSelectedSpans);

    syncMode();
}

void TraceWindow::setSession(const QString &sessionId)
{
    if (!sessionId.isEmpty() && m_session->text().trimmed() != sessionId) {
        m_session->setText(sessionId);
        reload();
    }
}

void TraceWindow::syncMode()
{
    ++m_generation;
    const bool live = m_follow->isChecked();
    m_afterSeq->setVisible(!live);
    m_loadPage->setVisible(!live);
    // Clear on every mode switch: leaving realtime cards in place and then
    // appending a history page after them mixes two different views of the
    // same session in one list.
    m_cards->clear();
    if (live) {
        m_afterLive = 0;
        m_timer->start();
        poll();
    } else {
        m_timer->stop();
        m_afterSeq->setValue(int(m_afterLive));
        m_status->clear();
    }
}

void TraceWindow::reload()
{
    ++m_generation;
    m_cards->clear();
    if (m_follow->isChecked()) {
        m_afterLive = 0;
        poll();
    } else {
        m_afterSeq->setValue(0);
        m_status->clear();
    }
}

void TraceWindow::appendEvents(const QList<asr::PipelineTraceEvent> &events)
{
    for (const asr::PipelineTraceEvent &event : events) {
        const QJsonObject payload =
            QJsonDocument::fromJson(event.payloadJson.toUtf8()).object();
        auto *item = new QListWidgetItem(describe(event, payload), m_cards);
        item->setData(Qt::UserRole, event.audioStartSec);
        item->setData(Qt::UserRole + 1, event.audioEndSec);
        item->setData(Qt::UserRole + 2, event.stage);
        item->setData(Qt::UserRole + 3, event.payloadJson);
    }
    if (!events.isEmpty())
        m_cards->scrollToBottom();
}

void TraceWindow::poll()
{
    if (!m_follow->isChecked() || m_busyLive)
        return;
    const QString sessionId = m_session->text().trimmed().isEmpty()
        ? m_controller->sessionId()
        : m_session->text().trimmed();
    if (sessionId.isEmpty()) {
        m_status->setText(QStringLiteral("chưa có session"));
        return;
    }
    m_busyLive = true;
    const int generation = m_generation;

    asr::PipelineTraceRequest request;
    request.sessionId = sessionId;
    request.afterSeq = m_afterLive;
    request.limit = 200;
    if (!m_stage->currentData().toString().isEmpty())
        request.stages << m_stage->currentData().toString();

    m_controller->rpc()->call<asr::PipelineTraceResponse>(
        this,
        [request](AsrClient &client, asr::PipelineTraceResponse &out) {
            return client.getPipelineTrace(request, &out, 30000);
        },
        [this, generation](const grpc::Status &status, const asr::PipelineTraceResponse &response) {
            m_busyLive = false;
            if (generation != m_generation)
                return; // the operator switched away while this was in flight
            if (!status.ok()) {
                m_status->setText(status.toString());
                return;
            }
            appendEvents(response.events);
            while (m_cards->count() > kMaxLiveCards)
                delete m_cards->takeItem(0);
            m_afterLive = response.nextSeq ? response.nextSeq : m_afterLive;
            QString capped;
            if (response.truncated) {
                capped = QStringLiteral(" · ĐÃ DỪNG Ở %1MB")
                             .arg(double(response.maxBytes) / 1048576.0, 0, 'f', 0);
            }
            m_status->setText(response.enabled
                                  ? QStringLiteral("%1 events%2%3 · hiện %4/%5 card")
                                        .arg(m_afterLive)
                                        .arg(response.hasMore ? QStringLiteral(" · còn dữ liệu")
                                                              : QString(), capped)
                                        .arg(m_cards->count())
                                        .arg(kMaxLiveCards)
                                  : QStringLiteral("trace chưa bật cho phiên này"));
        });
}

void TraceWindow::loadHistoryPage()
{
    if (m_follow->isChecked() || m_busyHistory)
        return;
    const QString sessionId = m_session->text().trimmed().isEmpty()
        ? m_controller->sessionId()
        : m_session->text().trimmed();
    if (sessionId.isEmpty()) {
        m_status->setText(QStringLiteral("chưa có session"));
        return;
    }
    m_busyHistory = true;
    const int generation = m_generation;
    const quint64 startSeq = quint64(qMax(0, m_afterSeq->value()));

    asr::PipelineTraceRequest request;
    request.sessionId = sessionId;
    request.afterSeq = startSeq;
    request.limit = 200;
    if (!m_stage->currentData().toString().isEmpty())
        request.stages << m_stage->currentData().toString();

    m_controller->rpc()->call<asr::PipelineTraceResponse>(
        this,
        [request](AsrClient &client, asr::PipelineTraceResponse &out) {
            return client.getPipelineTrace(request, &out, 30000);
        },
        [this, generation, startSeq](const grpc::Status &status,
                                     const asr::PipelineTraceResponse &response) {
            m_busyHistory = false;
            if (generation != m_generation)
                return;
            if (!status.ok()) {
                m_status->setText(status.toString());
                return;
            }
            appendEvents(response.events);
            m_afterSeq->setValue(int(response.nextSeq ? response.nextSeq : startSeq));
            m_status->setText(QStringLiteral("lịch sử: +%1 sự kiện (seq %2→%3)%4")
                                  .arg(response.events.size())
                                  .arg(startSeq)
                                  .arg(response.nextSeq)
                                  .arg(response.hasMore
                                           ? QStringLiteral(" · còn dữ liệu, bấm Tải trang tiếp")
                                           : QString()));
        });
}

void TraceWindow::playWav(const QByteArray &wavBytes, const QString &caption)
{
    m_player->stop();
    m_player->setSourceDevice(nullptr);
    m_buffer->close();
    m_buffer->setData(wavBytes);
    m_buffer->open(QIODevice::ReadOnly);
    m_player->setSourceDevice(m_buffer);
    m_player->play();
    m_status->setText(caption);
}

void TraceWindow::playSelectedRange()
{
    QListWidgetItem *item = m_cards->currentItem();
    if (!item) {
        m_status->setText(QStringLiteral("chọn một sự kiện trước"));
        return;
    }
    const QString sessionId = m_session->text().trimmed().isEmpty()
        ? m_controller->sessionId()
        : m_session->text().trimmed();
    asr::AudioRangeRequest request;
    request.sessionId = sessionId;
    request.startSec = item->data(Qt::UserRole).toDouble();
    request.endSec = item->data(Qt::UserRole + 1).toDouble();
    if (request.endSec <= request.startSec) {
        m_status->setText(QStringLiteral("sự kiện này không có khoảng audio"));
        return;
    }

    m_controller->rpc()->call<asr::AudioRangeResponse>(
        this,
        [request](AsrClient &client, asr::AudioRangeResponse &out) {
            return client.getAudioRange(request, &out, 120000);
        },
        [this, request](const grpc::Status &status, const asr::AudioRangeResponse &response) {
            if (!status.ok()) {
                m_status->setText(status.toString());
                return;
            }
            playWav(wav::buildWav(response.pcm, int(response.sampleRate), int(response.channels)),
                    QStringLiteral("audio RAW liên tục %1–%2s (có thể gồm cả phần VAD đã cắt)")
                        .arg(request.startSec, 0, 'f', 2)
                        .arg(request.endSec, 0, 'f', 2));
        });
}

void TraceWindow::stitchSelectedSpans()
{
    QListWidgetItem *item = m_cards->currentItem();
    if (!item) {
        m_status->setText(QStringLiteral("chọn một sự kiện trước"));
        return;
    }
    const QString stage = item->data(Qt::UserRole + 2).toString();
    const QString field = spanFieldForStage(stage);
    if (field.isEmpty()) {
        m_status->setText(QStringLiteral("stage này không mang span nguồn thật"));
        return;
    }
    const QJsonObject payload =
        QJsonDocument::fromJson(item->data(Qt::UserRole + 3).toString().toUtf8()).object();
    const QJsonArray raw = payload.value(field).toArray();

    QList<QPair<double, double>> spans;
    double totalSec = 0.0;
    for (const QJsonValue &value : raw) {
        const QJsonObject span = value.toObject();
        const double start = span.value(QStringLiteral("start_sec")).toDouble();
        const double end = span.value(QStringLiteral("end_sec")).toDouble();
        if (end > start) {
            spans.append({start, end});
            totalSec += end - start;
        }
    }
    if (spans.isEmpty()) {
        m_status->setText(QStringLiteral("sự kiện này không có span hợp lệ"));
        return;
    }
    if (spans.size() > kStitchMaxSpans) {
        QMessageBox::warning(this, QStringLiteral("Ghép audio"),
                             QStringLiteral("Segment có %1 span, vượt giới hạn %2 - từ chối ghép "
                                            "để không phát audio thiếu mà không cảnh báo.")
                                 .arg(spans.size())
                                 .arg(kStitchMaxSpans));
        return;
    }
    if (totalSec > kStitchMaxTotalSec) {
        QMessageBox::warning(this, QStringLiteral("Ghép audio"),
                             QStringLiteral("Tổng thời lượng span yêu cầu %1s vượt giới hạn %2s - "
                                            "từ chối ghép (không tự động cắt bớt).")
                                 .arg(totalSec, 0, 'f', 1)
                                 .arg(kStitchMaxTotalSec, 0, 'f', 0));
        return;
    }

    const QString sessionId = m_session->text().trimmed().isEmpty()
        ? m_controller->sessionId()
        : m_session->text().trimmed();
    const bool truncated = payload.value(field + QStringLiteral("_truncated")).toBool();
    m_status->setText(QStringLiteral("đang ghép %1 span...").arg(spans.size()));

    // One get_audio_range per span, concatenated on the RPC pool thread so the
    // UI keeps repainting while a long segment is fetched.
    m_controller->rpc()->call<QByteArray>(
        this,
        [sessionId, spans](AsrClient &client, QByteArray &out) {
            QByteArray pcm;
            int sampleRate = 16000;
            int channels = 1;
            for (const auto &span : spans) {
                asr::AudioRangeRequest request;
                request.sessionId = sessionId;
                request.startSec = span.first;
                request.endSec = span.second;
                asr::AudioRangeResponse response;
                const grpc::Status status = client.getAudioRange(request, &response, 120000);
                if (!status.ok())
                    return status;
                pcm.append(response.pcm);
                if (response.sampleRate > 0)
                    sampleRate = int(response.sampleRate);
                channels = qMax(1, int(response.channels));
            }
            out = wav::buildWav(pcm, sampleRate, channels);
            return grpc::Status();
        },
        [this, spans, truncated](const grpc::Status &status, const QByteArray &wavBytes) {
            if (!status.ok()) {
                m_status->setText(QStringLiteral("lỗi ghép audio: %1").arg(status.toString()));
                return;
            }
            playWav(wavBytes,
                    truncated
                        ? QStringLiteral("đã ghép %1 span — ⚠ model đã cắt bớt danh sách span, "
                                         "audio này KHÔNG đủ toàn bộ input thật")
                              .arg(spans.size())
                        : QStringLiteral("đã ghép %1 span thật model đã nhận").arg(spans.size()));
        });
}
