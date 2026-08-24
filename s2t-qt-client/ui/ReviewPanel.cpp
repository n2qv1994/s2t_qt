#include "ReviewPanel.h"

#include "Dialogs.h"
#include "audio/WavIo.h"
#include "core/Logger.h"

#include <QApplication>
#include <QAudioOutput>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// Consecutive windows overlap by the server's own +-12 s margin, so rows are
// de-duplicated by key rather than by window boundary.
const double kWindowSec = 600.0;
const int kMaxWindows = 200; // 33 h at this window size; stops a runaway
// Enough to catch a word's attack and tail without spilling into the
// neighbouring row - rows are often only about a second apart.
const double kHearRadiusSec = 0.6;
const int kTailRefreshMs = 4000;

QString rowKey(const asr::DisplayRow &row)
{
    if (!row.rowId.isEmpty())
        return row.rowId;
    return QStringLiteral("%1:%2").arg(row.startSec).arg(row.endSec);
}

QString displayName(const asr::DisplayRow &row)
{
    if (!row.verifiedName.trimmed().isEmpty())
        return row.verifiedName.trimmed();
    return QStringLiteral("speaker %1").arg(row.speaker.isEmpty() ? QStringLiteral("?")
                                                                  : row.speaker);
}

QString rowText(const asr::DisplayRow &row)
{
    if (!row.displayTokens.isEmpty()) {
        QStringList parts;
        for (const asr::Word &token : row.displayTokens)
            parts << token.w;
        return parts.join(QLatin1Char(' '));
    }
    return row.mergedText;
}

} // namespace

ReviewPanel::ReviewPanel(SessionController *controller, QWidget *parent)
    : QWidget(parent), m_controller(controller)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);

    auto *top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("REVIEW"), this));
    m_picker = new QComboBox(this);
    m_picker->setMinimumWidth(320);
    m_picker->addItem(QStringLiteral("— chọn cuộc họp —"), QString());
    top->addWidget(m_picker, 1);
    m_sessionInput = new QLineEdit(this);
    m_sessionInput->setPlaceholderText(QStringLiteral("hoặc dán session_id"));
    top->addWidget(m_sessionInput, 1);
    auto *open = new QPushButton(QStringLiteral("Mở"), this);
    top->addWidget(open);
    auto *refresh = new QPushButton(QStringLiteral("Tải lại danh sách"), this);
    top->addWidget(refresh);
    m_info = new QLabel(this);
    top->addWidget(m_info, 2);
    layout->addLayout(top);

    auto *audioRow = new QHBoxLayout();
    m_playPause = new QPushButton(QStringLiteral("▶ Phát lại"), this);
    m_playPause->setEnabled(false);
    audioRow->addWidget(m_playPause);
    m_audioInfo = new QLabel(this);
    audioRow->addWidget(m_audioInfo, 1);
    layout->addLayout(audioRow);

    auto *hint = new QLabel(
        QStringLiteral("Bấm đúp vào cột văn bản để sửa cả câu · bấm đúp vào cột người nói để "
                       "đặt tên hoặc gộp · chữ xám = chưa chốt, chờ vài giây hoặc chờ họp xong"),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Thời gian"), QStringLiteral("Người nói"), QStringLiteral("Văn bản")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table, 1);

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    // Playback runs on whatever Qt Multimedia back end the machine has - the
    // FFmpeg one on a current Qt, GStreamer on RHEL's builds - and a missing
    // decoder plugin otherwise fails as silence with no explanation at all.
    // Say what the back end actually reported instead.
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                if (m_audioInfo)
                    m_audioInfo->setText(QStringLiteral("không phát được audio: %1").arg(message));
            });
    m_audioBuffer = new QBuffer(this);

    m_tailTimer = new QTimer(this);
    m_tailTimer->setInterval(kTailRefreshMs);

    connect(open, &QPushButton::clicked, this, &ReviewPanel::loadFromInputs);
    connect(refresh, &QPushButton::clicked, this, &ReviewPanel::refreshSessionList);
    connect(m_picker, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString id = m_picker->currentData().toString();
        if (!id.isEmpty()) {
            m_sessionInput->setText(id);
            loadFromInputs();
        }
    });
    connect(m_table, &QTableWidget::cellChanged, this, &ReviewPanel::onCellChanged);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &ReviewPanel::onCellDoubleClicked);
    connect(m_playPause, &QPushButton::clicked, this, [this]() {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
    });
    connect(m_tailTimer, &QTimer::timeout, this, &ReviewPanel::refreshTail);
}

void ReviewPanel::refreshSessionList()
{
    asr::ListSessionsRequest request;
    request.limit = 50;
    m_controller->rpc()->call<asr::ListSessionsResponse>(
        this,
        [request](AsrClient &client, asr::ListSessionsResponse &out) {
            return client.listSessions(request, &out, 30000);
        },
        [this](const grpc::Status &status, const asr::ListSessionsResponse &response) {
            if (!status.ok()) {
                // Fall back to what this process itself ran, so an older
                // adapter without list_sessions still offers something.
                m_picker->clear();
                m_picker->addItem(QStringLiteral("— phiên đã chạy trong lần mở này —"), QString());
                for (const FinishedSession &item : m_controller->finishedSessions()) {
                    m_picker->addItem(QStringLiteral("%1 · %2s")
                                          .arg(item.sourceName)
                                          .arg(item.durationSec, 0, 'f', 0),
                                      item.sessionId);
                }
                emit statusMessage(QStringLiteral("Không tải được danh sách phiên: %1")
                                       .arg(status.toString()));
                return;
            }
            const QString keep = m_picker->currentData().toString();
            m_picker->blockSignals(true);
            m_picker->clear();
            m_summaries.clear();
            m_picker->addItem(QStringLiteral("— chọn cuộc họp —"), QString());
            for (const asr::SessionSummary &summary : response.sessions) {
                m_summaries.insert(summary.sessionId, summary);
                const QString when =
                    summary.createdAt > 0
                        ? QDateTime::fromMSecsSinceEpoch(qint64(summary.createdAt * 1000.0))
                              .toString(QStringLiteral("dd/MM/yyyy HH:mm"))
                        : QString();
                const QString badge = summary.running ? QStringLiteral("đang họp")
                                                      : (summary.final ? QStringLiteral("đã xong")
                                                                       : QStringLiteral("dở dang"));
                const QString duration = summary.durationSec >= 60
                    ? QStringLiteral("%1p").arg(summary.durationSec / 60.0, 0, 'f', 1)
                    : QStringLiteral("%1s").arg(summary.durationSec, 0, 'f', 0);
                QString label = QStringLiteral("%1 · %2 · %3")
                                    .arg(summary.title.isEmpty() ? summary.sessionId : summary.title,
                                         duration, badge);
                if (summary.mode == QLatin1String("record_only"))
                    label += QStringLiteral(" · [chỉ ghi âm]");
                if (!summary.securityLevel.isEmpty())
                    label += QStringLiteral(" · [%1]").arg(summary.securityLevel);
                if (!when.isEmpty())
                    label += QStringLiteral(" · %1").arg(when);
                m_picker->addItem(label, summary.sessionId);
            }
            const int index = m_picker->findData(keep);
            if (index >= 0)
                m_picker->setCurrentIndex(index);
            m_picker->blockSignals(false);
        });
}

void ReviewPanel::openSession(const QString &sessionId)
{
    m_sessionInput->setText(sessionId);
    loadFromInputs();
}

void ReviewPanel::loadFromInputs()
{
    const QString sessionId = m_sessionInput->text().trimmed().isEmpty()
        ? m_picker->currentData().toString()
        : m_sessionInput->text().trimmed();
    if (sessionId.isEmpty()) {
        m_info->setText(QStringLiteral("cần session_id"));
        return;
    }
    m_rows.clear();
    m_ordered.clear();
    m_sessionId = sessionId;
    m_info->setText(QStringLiteral("đang tải…"));
    loadWindows(sessionId, 0.0);
}

void ReviewPanel::loadWindows(const QString &sessionId, double fromSec)
{
    if (m_loading)
        return;
    m_loading = true;
    requestWindow(sessionId, qMax(0.0, fromSec), 0.0, 0);
}

void ReviewPanel::requestWindow(const QString &sessionId, double cursor, double covered,
                                int windowIndex)
{
    asr::ReviewRequest request;
    request.sessionId = sessionId;
    request.hasViewStartSec = true;
    request.viewStartSec = cursor;
    request.hasViewEndSec = true;
    request.viewEndSec = cursor + kWindowSec;

    m_controller->rpc()->call<asr::StateResponse>(
        this,
        [request](AsrClient &client, asr::StateResponse &out) {
            return client.getReviewState(request, &out, 60000);
        },
        [this, sessionId, cursor, covered, windowIndex](const grpc::Status &status,
                                                        const asr::StateResponse &response) {
            if (m_sessionId != sessionId) {
                m_loading = false;
                return; // the operator moved to another meeting mid-walk
            }
            if (!status.ok()) {
                m_loading = false;
                m_info->setText(QStringLiteral("lỗi: %1").arg(status.toString()));
                return;
            }
            m_revision = response.transcriptRevision;
            m_final = response.transcriptFinal;
            m_commitBoundarySec = response.commitBoundarySec;
            for (const asr::DisplayRow &row : response.state.rows)
                m_rows.insert(rowKey(row), row);

            // source_total_sec is 0 for a live mic session whose length is not
            // known yet, so fall back to how far the pipeline has actually got.
            const double nowCovered = qMax(covered, qMax(response.state.sourceTotalSec,
                                                         response.state.sourceSeenSec));
            const double nextCursor = cursor + kWindowSec;
            if (nextCursor < nowCovered && windowIndex + 1 < kMaxWindows) {
                m_info->setText(QStringLiteral("đang tải… %1s/%2s")
                                    .arg(std::llround(nextCursor))
                                    .arg(std::llround(nowCovered)));
                requestWindow(sessionId, nextCursor, nowCovered, windowIndex + 1);
                return;
            }

            m_loading = false;
            renderRows();

            QString note;
            if (!m_final) {
                note = m_commitBoundarySec >= 0
                    ? QStringLiteral(" · chốt tới %1s").arg(m_commitBoundarySec, 0, 'f', 2)
                    : QStringLiteral(" · chưa có gì được chốt");
            }
            const asr::SessionSummary summary = m_summaries.value(sessionId);
            QStringList meta;
            if (summary.mode == QLatin1String("record_only"))
                meta << QStringLiteral("chỉ ghi âm");
            if (!summary.securityLevel.isEmpty())
                meta << QStringLiteral("mức bảo mật: %1").arg(summary.securityLevel);
            if (!summary.participants.isEmpty())
                meta << QStringLiteral("người tham gia: %1")
                            .arg(summary.participants.join(QStringLiteral(", ")));
            m_info->setText(QStringLiteral("rev=%1 %2%3 · %4 dòng%5")
                                .arg(m_revision)
                                .arg(m_final ? QStringLiteral("final") : QStringLiteral("đang họp"))
                                .arg(note)
                                .arg(m_ordered.size())
                                .arg(meta.isEmpty() ? QString()
                                                    : QStringLiteral(" · ")
                                                          + meta.join(QStringLiteral(" · "))));

            // Listening back only matters once the meeting is over: mid
            // session the operator was just in the room and is here to fix a
            // word, not to re-listen.
            m_playPause->setEnabled(m_final);
            if (m_final)
                m_tailTimer->stop();
            else
                m_tailTimer->start();
        });
}

void ReviewPanel::refreshTail()
{
    if (m_sessionId.isEmpty() || m_final || m_loading)
        return;
    // Never clobber a cell the operator is mid-edit in.  An open cell editor
    // is a child widget of the view's viewport, which is the public way to
    // ask this - QAbstractItemView::state() is protected.
    QWidget *focus = QApplication::focusWidget();
    if (focus && m_table->viewport()->isAncestorOf(focus))
        return;
    // Only rows past the commit boundary can still change, so refresh from one
    // window before it rather than walking the whole meeting again.
    loadWindows(m_sessionId,
                m_commitBoundarySec >= 0 ? qMax(0.0, m_commitBoundarySec - kWindowSec) : 0.0);
}

void ReviewPanel::renderRows()
{
    m_ordered = m_rows.values();
    std::sort(m_ordered.begin(), m_ordered.end(),
              [](const asr::DisplayRow &a, const asr::DisplayRow &b) {
                  return a.startSec < b.startSec;
              });

    m_populating = true;
    m_table->setRowCount(m_ordered.size());
    for (int i = 0; i < m_ordered.size(); ++i) {
        const asr::DisplayRow &row = m_ordered.at(i);
        // Mirrors session_store.py: while the transcript is not final, a row
        // ending past the commit boundary is refused with
        // edit_range_not_committed, so it is shown read-only here too.
        const bool locked = !m_final && row.endSec > m_commitBoundarySec + 1e-3;

        auto *time = new QTableWidgetItem(QStringLiteral("%1–%2s")
                                              .arg(row.startSec, 0, 'f', 2)
                                              .arg(row.endSec, 0, 'f', 2));
        time->setFlags(time->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(i, 0, time);

        auto *speaker = new QTableWidgetItem(displayName(row));
        speaker->setFlags(speaker->flags() & ~Qt::ItemIsEditable);
        speaker->setToolTip(QStringLiteral("Bấm đúp để đặt tên hoặc gộp người nói này"));
        m_table->setItem(i, 1, speaker);

        auto *text = new QTableWidgetItem(rowText(row));
        if (locked) {
            text->setFlags(text->flags() & ~Qt::ItemIsEditable);
            text->setForeground(QColor(0x8a, 0x8a, 0x8a));
            text->setToolTip(QStringLiteral(
                                 "Đoạn này vẫn sau điểm chốt (%1s) — correction có thể còn sửa "
                                 "lại. Chờ vài giây hoặc đợi cuộc họp kết thúc rồi sửa.")
                                 .arg(m_commitBoundarySec, 0, 'f', 2));
        }
        m_table->setItem(i, 2, text);
    }
    m_populating = false;
}

void ReviewPanel::onCellChanged(int row, int column)
{
    if (m_populating || column != 2 || row < 0 || row >= m_ordered.size())
        return;
    const asr::DisplayRow source = m_ordered.at(row);
    const QString text = m_table->item(row, column)->text().trimmed();
    if (text.isEmpty() || text == rowText(source)) {
        m_populating = true;
        m_table->item(row, column)->setText(rowText(source));
        m_populating = false;
        return;
    }
    if (m_editorId.isEmpty()) {
        emit statusMessage(QStringLiteral("cần nhập tên người thao tác (thanh trên) trước khi lưu"));
        m_populating = true;
        m_table->item(row, column)->setText(rowText(source));
        m_populating = false;
        return;
    }

    const double span = qMax(1e-3, source.endSec - source.startSec);
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    asr::TextEditRequest request;
    request.sessionId = m_sessionId;
    request.baseRevision = m_revision;
    request.startSec = source.startSec;
    request.endSec = source.endSec;
    request.editorId = m_editorId;
    request.note = QStringLiteral("s2t_qt review edit");
    for (int i = 0; i < words.size(); ++i) {
        asr::Word word;
        word.w = words.at(i);
        word.c = 1.0f;
        word.startSec = source.startSec + span * double(i) / double(words.size());
        word.endSec = qMin(source.endSec, source.startSec + span * double(i + 1) / double(words.size()));
        request.replacementWords.append(word);
    }

    LOG_INFO(applog::cat::Ui)
        << "sending apply_text_edit: session=" << request.sessionId
        << "baseRevision=" << request.baseRevision << "range=" << request.startSec << "-"
        << request.endSec << "s," << words.size() << "words, editor=" << request.editorId;
    m_controller->rpc()->call<asr::ReviewEditResponse>(
        this,
        [request](AsrClient &client, asr::ReviewEditResponse &out) {
            return client.applyTextEdit(request, &out, 60000);
        },
        [this](const grpc::Status &status, const asr::ReviewEditResponse &response) {
            if (!status.ok()) {
                LOG_WARN(applog::cat::Ui) << "apply_text_edit rejected:" << status.toString();
                emit statusMessage(
                    status.message.contains(QLatin1String("edit_range_not_committed"))
                        ? QStringLiteral("chưa chốt tới đoạn này, thử lại sau vài giây")
                        : QStringLiteral("từ chối: %1 — đang tải lại").arg(status.toString()));
                loadFromInputs();
                return;
            }
            m_revision = response.transcript.revision;
            LOG_INFO(applog::cat::Ui) << "apply_text_edit OK - new revision=" << m_revision;
            emit statusMessage(QStringLiteral("đã lưu, rev=%1").arg(m_revision));
            for (const asr::DisplayRow &row : response.state.rows)
                m_rows.insert(rowKey(row), row);
            renderRows();
        });
}

void ReviewPanel::onCellDoubleClicked(int row, int column)
{
    if (row < 0 || row >= m_ordered.size())
        return;
    const asr::DisplayRow source = m_ordered.at(row);
    if (column == 1) {
        renameSpeaker(source.speaker, source.verifiedName);
        return;
    }
    if (column == 0 && m_final)
        playAround((source.startSec + source.endSec) * 0.5);
}

void ReviewPanel::renameSpeaker(const QString &fromSpeaker, const QString &currentName)
{
    if (fromSpeaker.isEmpty()) {
        emit statusMessage(QStringLiteral("dòng này chưa có speaker_id để đổi tên"));
        return;
    }
    if (m_editorId.isEmpty()) {
        emit statusMessage(QStringLiteral("cần nhập tên người thao tác (thanh trên) trước khi lưu"));
        return;
    }
    QStringList others;
    for (const asr::DisplayRow &row : m_ordered) {
        if (!row.speaker.isEmpty() && row.speaker != fromSpeaker && !others.contains(row.speaker))
            others << row.speaker;
    }
    SpeakerRenameDialog dialog(fromSpeaker, currentName, others, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    asr::RenameSpeakerRequest request;
    request.sessionId = m_sessionId;
    request.fromSpeaker = fromSpeaker;
    request.toSpeaker = dialog.mergeTarget();
    request.verifiedName = dialog.displayName();
    request.editorId = m_editorId;
    request.note = QStringLiteral("s2t_qt speaker rename");

    LOG_INFO(applog::cat::Ui)
        << "sending rename_speaker: session=" << request.sessionId << request.fromSpeaker << "->"
        << (request.toSpeaker.isEmpty() ? QStringLiteral("(no merge)") : request.toSpeaker)
        << "verifiedName=" << request.verifiedName << "editor=" << request.editorId;
    m_controller->rpc()->call<asr::ReviewEditResponse>(
        this,
        [request](AsrClient &client, asr::ReviewEditResponse &out) {
            return client.renameSpeaker(request, &out, 60000);
        },
        [this](const grpc::Status &status, const asr::ReviewEditResponse &response) {
            if (!status.ok()) {
                LOG_ERROR(applog::cat::Ui) << "rename_speaker failed:" << status.toString();
                emit statusMessage(QStringLiteral("lỗi đổi người nói: %1").arg(status.toString()));
                return;
            }
            m_revision = response.transcript.revision;
            LOG_INFO(applog::cat::Ui) << "rename_speaker OK - new revision=" << m_revision;
            emit statusMessage(QStringLiteral("đã đổi người nói, rev=%1").arg(m_revision));
            for (const asr::DisplayRow &row : response.state.rows)
                m_rows.insert(rowKey(row), row);
            renderRows();
        });
}

void ReviewPanel::playAround(double centreSec)
{
    if (m_sessionId.isEmpty() || !std::isfinite(centreSec))
        return;
    const double from = qMax(0.0, centreSec - kHearRadiusSec);
    const double to = centreSec + kHearRadiusSec;
    const int seq = ++m_audioSeq; // a second click must not race the first

    asr::AudioRangeRequest request;
    request.sessionId = m_sessionId;
    request.startSec = from;
    request.endSec = to;
    m_audioInfo->setText(QStringLiteral("đang nạp…"));

    m_controller->rpc()->call<asr::AudioRangeResponse>(
        this,
        [request](AsrClient &client, asr::AudioRangeResponse &out) {
            return client.getAudioRange(request, &out, 120000);
        },
        [this, seq, centreSec, from, to](const grpc::Status &status,
                                         const asr::AudioRangeResponse &response) {
            if (seq != m_audioSeq)
                return;
            if (!status.ok()) {
                m_audioInfo->setText(QStringLiteral("lỗi: %1").arg(status.toString()));
                return;
            }
            // The RPC hands back raw little-endian PCM with no container;
            // QMediaPlayer needs a real WAV header in front of it.
            const QByteArray payload = wav::buildWav(response.pcm, int(response.sampleRate),
                                                     int(response.channels));
            m_player->stop();
            m_player->setSourceDevice(nullptr);
            m_audioBuffer->close();
            m_audioBuffer->setData(payload);
            m_audioBuffer->open(QIODevice::ReadOnly);
            m_player->setSourceDevice(m_audioBuffer);
            m_player->play();
            m_playPause->setEnabled(true);
            m_audioInfo->setText(QStringLiteral("quanh %1s (%2–%3s)")
                                     .arg(centreSec, 0, 'f', 2)
                                     .arg(from, 0, 'f', 2)
                                     .arg(to, 0, 'f', 2));
        });
}
