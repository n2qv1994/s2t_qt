#include "SessionWorker.h"

#include "Logger.h"
#include "../grpc/AsrClient.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>

namespace {

// Live transport packet.  20 ms of capture is assembled into 160 ms before a
// request goes out, so no network or protobuf work happens on the audio path.
const int kLivePacketMs = 160;
// File replay uses the pipeline's native 320 ms diar hop: it halves request
// and state-poll overhead versus 160 ms without exceeding the model's fixed
// ingestion limit.
const int kReplayPacketMs = 320;

const int kStartTimeoutMs = 20000;
// Short deadline so a dropped network link is detected promptly.
// Retrying the same seq is safe: the adapter replays the stored response.
const int kPushTimeoutMs = 5000;
// Stop drains the durable server-side queue.  After a fast file upload or a
// network recovery that queue can legitimately hold minutes of audio, so this
// has to be generous or a partial session gets finalised.
const int kStopTimeoutMs = 3600 * 1000;
const int kReviewTimeoutMs = 20000;

// A live session pushes ~6 packets a second and a fast file replay far more
// than that.  Every packet is logged at trace; at debug only every Nth one
// carries a running summary, which is enough to see the pipeline moving
// without burying everything else.
const quint64 kPacketLogEvery = 50;

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

} // namespace

SessionWorker::SessionWorker(const Settings &settings, AudioQueue *queue, QObject *parent)
    : QThread(parent), m_settings(settings), m_queue(queue)
{
    // Named so `info threads` in gdb and every valgrind report identify this
    // thread by its job.  Qt pushes objectName down to the OS thread name on
    // Linux, which is where that pays off.
    setObjectName(QStringLiteral("session-worker"));
    // A replayed file declares its own rate and channel count; the microphone
    // settings do not apply to it.  push_audio tells the server exactly what
    // it is being sent, so a mismatch here would corrupt every timestamp the
    // pipeline derives from the sample count.
    if (m_settings.source == Source::File && m_settings.file.isValid()) {
        m_settings.sampleRate = m_settings.file.sampleRate;
        m_settings.channels = m_settings.file.channels;
    }
    LOG_DEBUG(applog::cat::Worker)
        << "worker created - source="
        << (m_settings.source == Source::Microphone ? "microphone" : "file")
        << m_settings.sampleRate << "Hz /" << m_settings.channels << "ch, target="
        << m_settings.target << "pipelineTrace=" << m_settings.pipelineTrace;
}

SessionWorker::~SessionWorker()
{
    requestStop();
    if (!wait(10000)) {
        LOG_ERROR(applog::cat::Worker)
            << "worker did not finish within 10 s - forcing terminate()";
        terminate();
    }
}

QString SessionWorker::sessionId() const
{
    QMutexLocker lock(&m_mutex);
    return m_sessionId;
}

void SessionWorker::requestStop()
{
    // m_seq belongs to the worker thread; this runs on the caller's, so the
    // count is left to the drain log rather than raced for here.
    if (!m_stopRequested.loadAcquire())
        LOG_INFO(applog::cat::Worker) << "stop requested for session" << sessionId();
    m_stopRequested.storeRelease(1);
    if (m_queue)
        m_queue->wake();
}

void SessionWorker::setPaused(bool paused)
{
    LOG_INFO(applog::cat::Worker) << (paused ? "pausing" : "resuming") << "the audio upload";
    m_paused.storeRelease(paused ? 1 : 0);
    if (m_queue)
        m_queue->setPaused(paused);
}

bool SessionWorker::isPaused() const
{
    return m_paused.loadAcquire() != 0;
}

void SessionWorker::noteDroppedChunk()
{
    const int total = m_dropped.fetchAndAddOrdered(1) + 1;
    LOG_ERROR(applog::cat::Worker) << "capture reported a dropped chunk (total" << total
                                   << ") - the session will stop";
}

QString SessionWorker::buildConfigJson() const
{
    QJsonObject config;
    if (m_settings.pipelineTrace)
        config.insert(QStringLiteral("pipeline_trace"), true);
    if (m_settings.source == Source::File && m_settings.file.isValid())
        config.insert(QStringLiteral("source_total_sec"), m_settings.file.durationSec());
    // Tri-state: the key is written only when the operator actually restricted
    // the roster.  An explicit empty array is NOT the same as omitting it -
    // "recognise nobody" must not fall back to "match the whole registry".
    if (m_settings.restrictSpeakers) {
        QJsonArray names;
        for (const QString &name : m_settings.expectedSpeakers)
            names.append(name);
        config.insert(QStringLiteral("expected_speakers"), names);
    }
    const SessionMeta &meta = m_settings.meta;
    if (!meta.mode.isEmpty())
        config.insert(QStringLiteral("mode"), meta.mode);
    if (!meta.title.isEmpty())
        config.insert(QStringLiteral("session_title"), meta.title);
    if (!meta.participants.isEmpty()) {
        QJsonArray people;
        for (const QString &name : meta.participants)
            people.append(name);
        config.insert(QStringLiteral("participants"), people);
    }
    if (!meta.securityLevel.isEmpty())
        config.insert(QStringLiteral("security_level"), meta.securityLevel);
    const QString json = QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact));
    // The tri-state expected_speakers is the single most misread thing in a
    // start_session, so log exactly what went on the wire.
    LOG_DEBUG(applog::cat::Worker) << "config_json =" << json;
    return json;
}

void SessionWorker::publishTelemetry()
{
    const double bytesPerSec = double(qMax(1, m_settings.sampleRate * m_settings.channels * 2));
    m_telemetry.capturedSec = double(m_capturedBytes) / bytesPerSec;
    m_telemetry.sentSec = double(m_sentBytes) / bytesPerSec;
    m_telemetry.droppedChunks = m_dropped.loadAcquire();
    emit telemetryChanged(m_telemetry);
}

bool SessionWorker::pushPacket(AsrClient &client, const QByteArray &pcm, bool reset, int vadChunkMs,
                               QString *error)
{
    if (pcm.isEmpty())
        return true;

    ++m_seq;
    asr::PushAudioRequest request;
    {
        QMutexLocker lock(&m_mutex);
        request.sessionId = m_sessionId;
    }
    request.pcm = pcm;
    request.sampleRate = quint32(m_settings.sampleRate);
    request.channels = quint32(m_settings.channels);
    request.audioFormat = QStringLiteral("s16le");
    request.reset = reset;
    request.vadChunkMs = quint32(vadChunkMs);
    request.seq = m_seq;

    QElapsedTimer clock;
    clock.start();
    asr::PushAudioResponse response;
    while (true) {
        const grpc::Status status = client.pushAudio(request, &response, kPushTimeoutMs);
        if (status.ok())
            break;
        if (!status.isTransport()) {
            // INTERNAL and friends are NOT retried: the adapter returns them
            // precisely when the server may already have consumed this audio,
            // and a blind retry would duplicate words in the transcript.
            LOG_ERROR(applog::cat::Worker)
                << "push_audio seq=" << m_seq << "permanently rejected:" << status.toString()
                << "- not retried, a retry here would duplicate words";
            *error = status.toString();
            return false;
        }
        LOG_WARN(applog::cat::Worker)
            << "push_audio seq=" << m_seq << "transport failure:" << status.toString()
            << "- redialling and resending this same seq";
        if (!m_networkReconnecting) {
            m_networkReconnecting = true;
            emit statusChanged(MicStatus::NetworkReconnecting);
            emit errorChanged(QStringLiteral(
                "Mất kết nối tới AI server. Đang giữ nguyên phiên và tự kết nối lại; "
                "audio mới đang được giữ tạm trên máy này."));
        }
        // Do not wait out HTTP/2's own reconnect behaviour on a stale socket:
        // a fresh connection observes restored networking on the next retry.
        client.reset();
        if (m_stopRequested.loadAcquire()) {
            LOG_WARN(applog::cat::Worker) << "stopped while waiting to reconnect";
            *error = QStringLiteral("đã dừng trong lúc chờ AI server");
            return false;
        }
        QThread::msleep(500);
        if (m_stopRequested.loadAcquire()) {
            LOG_WARN(applog::cat::Worker) << "stopped while waiting to reconnect";
            *error = QStringLiteral("đã dừng trong lúc chờ AI server");
            return false;
        }
    }

    if (m_networkReconnecting) {
        m_networkReconnecting = false;
        LOG_INFO(applog::cat::Worker)
            << "reconnected to the AI server - continuing from seq=" << m_seq;
        emit errorChanged(QString());
        emit statusChanged(isPaused() ? MicStatus::Paused : MicStatus::Recording);
    }

    m_sentBytes += pcm.size();
    const double rpcMs = double(clock.nsecsElapsed()) / 1e6;
    // Both ends already report the time spent preparing and waiting for
    // Triton.  Removing that server-side work from the locally measured ACK
    // round trip leaves network + gRPC + adapter serialization.  Half of it
    // is only an estimate (it assumes a roughly symmetric LAN path) but it
    // does not depend on the two machines' clocks agreeing.
    const double aiPrepareMs = qMax(0.0, response.timing.clientPrepareMs);
    const double aiWaitMs = qMax(0.0, response.timing.clientWaitMs);
    const double transportMs = qMax(0.0, rpcMs - aiPrepareMs - aiWaitMs);

    const double bytesPerSec = double(qMax(1, m_settings.sampleRate * m_settings.channels * 2));
    m_lastSourceSeenSec = response.sourceSeenSec;
    m_telemetry.rpcLastMs = rpcMs;
    m_telemetry.rpcMaxMs = qMax(m_telemetry.rpcMaxMs, rpcMs);
    m_telemetry.aiWaitLastMs = aiPrepareMs + aiWaitMs;
    m_telemetry.transportRoundTripMs = transportMs;
    m_telemetry.transportOneWayEstMs = transportMs / 2.0;
    m_telemetry.serverQueueSec =
        qMax(0.0, double(m_sentBytes) / bytesPerSec - response.sourceSeenSec);
    m_telemetry.localQueueSec =
        m_queue ? double(m_queue->pendingBytes()) / bytesPerSec : 0.0;

    LatencySample sample;
    sample.ts = nowSeconds();
    sample.rpcMs = rpcMs;
    sample.aiWaitMs = m_telemetry.aiWaitLastMs;
    sample.transportMs = transportMs;
    sample.localQueueSec = m_telemetry.localQueueSec;
    sample.serverQueueSec = m_telemetry.serverQueueSec;
    emit latencySample(sample);
    publishTelemetry();

    LOG_TRACE(applog::cat::Worker)
        << "push_audio seq=" << m_seq << pcm.size() << "bytes reset=" << reset
        << "vad=" << vadChunkMs << "ms | rpc=" << rpcMs << "ms ai=" << m_telemetry.aiWaitLastMs
        << "ms transport=" << transportMs << "ms queue(local/server)=" << m_telemetry.localQueueSec
        << "/" << m_telemetry.serverQueueSec << "s";
    if (m_seq % kPacketLogEvery == 0) {
        LOG_DEBUG(applog::cat::Worker)
            << "sent" << m_seq << "packets /" << m_telemetry.sentSec
            << "s of audio | last rpc=" << rpcMs << "ms (max" << m_telemetry.rpcMaxMs
            << "ms) | queue local=" << m_telemetry.localQueueSec
            << "s server=" << m_telemetry.serverQueueSec << "s";
    }
    return true;
}

void SessionWorker::run()
{
    AsrClient client(m_settings.target, m_settings.token);

    asr::StartSessionRequest startRequest;
    startRequest.configJson = buildConfigJson();
    asr::StartSessionResponse started;
    LOG_INFO(applog::cat::Worker) << "calling start_session on" << m_settings.target;
    const grpc::Status status = client.startSession(startRequest, &started, kStartTimeoutMs);
    if (!status.ok()) {
        LOG_ERROR(applog::cat::Worker) << "start_session failed:" << status.toString();
        emit statusChanged(MicStatus::Error);
        emit failed(QStringLiteral("Không tạo được phiên trên AI server - %1").arg(status.toString()));
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_sessionId = started.sessionId;
    }
    LOG_INFO(applog::cat::Worker) << "start_session OK - session_id=" << started.sessionId;
    emit sessionStarted(started.sessionId);
    emit statusChanged(MicStatus::Recording);

    if (m_settings.source == Source::Microphone)
        runMicrophone(client);
    else
        runFile(client);
    LOG_INFO(applog::cat::Worker) << "worker thread for session" << started.sessionId << "finished";
}

void SessionWorker::runMicrophone(AsrClient &client)
{
    const int frameBytes = qMax(1, m_settings.channels * 2);
    const int packetBytes =
        qMax(frameBytes, m_settings.sampleRate * frameBytes * kLivePacketMs / 1000);
    QByteArray buffer;
    LOG_INFO(applog::cat::Worker)
        << "microphone loop starting - packet" << kLivePacketMs << "ms =" << packetBytes << "bytes";

    while (!m_stopRequested.loadAcquire()) {
        if (m_dropped.loadAcquire() > 0) {
            LOG_ERROR(applog::cat::Worker) << "stopping the session: the microphone queue overflowed";
            emit statusChanged(MicStatus::Error);
            emit failed(QStringLiteral(
                "Hàng đợi microphone bị tràn; phiên dừng lại để không mất audio một cách âm thầm."));
            return;
        }
        if (isPaused()) {
            // The queue already discards while paused; clear the partial
            // packet too so speech that raced the click is not delivered.
            buffer.clear();
            msleep(30);
            continue;
        }
        const QByteArray incoming = m_queue->take(100);
        if (incoming.isEmpty())
            continue;
        m_capturedBytes += incoming.size();
        buffer.append(incoming);
        while (buffer.size() >= packetBytes) {
            QString error;
            if (!pushPacket(client, buffer.left(packetBytes), m_seq == 0, kLivePacketMs, &error)) {
                emit statusChanged(MicStatus::Error);
                emit failed(error);
                return;
            }
            buffer.remove(0, packetBytes);
        }
    }

    // Everything the device already handed over before STOP was pressed is
    // real speech and must still reach the server; this drain is finite
    // because capture has been told to stop by the controller first.
    buffer.append(m_queue->takeAllNow());
    LOG_INFO(applog::cat::Worker)
        << "microphone loop ended - draining the last" << buffer.size() << "bytes after" << m_seq
        << "packets";
    while (buffer.size() >= packetBytes) {
        QString error;
        if (!pushPacket(client, buffer.left(packetBytes), m_seq == 0, kLivePacketMs, &error)) {
            emit statusChanged(MicStatus::Error);
            emit failed(error);
            return;
        }
        buffer.remove(0, packetBytes);
    }
    if (!drainAndStop(client, buffer.left(buffer.size() - (buffer.size() % frameBytes))))
        return;
}

void SessionWorker::runFile(AsrClient &client)
{
    const wav::Pcm &pcm = m_settings.file;
    const int frameBytes = qMax(1, pcm.channels * 2);
    const int packetBytes =
        qMax(frameBytes, pcm.sampleRate * frameBytes * kReplayPacketMs / 1000);
    const double bytesPerSec = double(qMax(1, pcm.sampleRate * pcm.channels * 2));

    QElapsedTimer wall;
    wall.start();
    LOG_INFO(applog::cat::Worker)
        << "file replay starting -" << m_settings.fileName << pcm.durationSec() << "s, packet"
        << kReplayPacketMs << "ms =" << packetBytes << "bytes, pacedToSourceClock="
        << m_settings.pacedToSourceClock;
    for (int offset = 0; offset < pcm.frames.size(); offset += packetBytes) {
        if (m_stopRequested.loadAcquire()) {
            LOG_WARN(applog::cat::Worker)
                << "file replay stopped early at byte" << offset << "/" << pcm.frames.size();
            break;
        }
        const QByteArray packet = pcm.frames.mid(offset, packetBytes);
        m_capturedBytes += packet.size();
        QString error;
        if (!pushPacket(client, packet, m_seq == 0, kReplayPacketMs, &error)) {
            emit statusChanged(MicStatus::Error);
            emit failed(error);
            return;
        }
        if (m_settings.pacedToSourceClock) {
            const double target = double(qMin<qint64>(pcm.frames.size(), offset + packetBytes))
                / bytesPerSec;
            const qint64 sleepMs = qint64(target * 1000.0) - wall.elapsed();
            if (sleepMs > 0)
                msleep(ulong(sleepMs));
        }
    }
    LOG_INFO(applog::cat::Worker)
        << "file replay done after" << wall.elapsed() << "ms wall /" << m_seq << "packets";
    drainAndStop(client, QByteArray());
}

bool SessionWorker::drainAndStop(AsrClient &client, const QByteArray &tail)
{
    if (!tail.isEmpty()) {
        QString error;
        const int vadChunkMs =
            m_settings.source == Source::Microphone ? kLivePacketMs : kReplayPacketMs;
        if (!pushPacket(client, tail, m_seq == 0, vadChunkMs, &error)) {
            emit statusChanged(MicStatus::Error);
            emit failed(error);
            return false;
        }
    }

    emit statusChanged(MicStatus::Finalizing);
    asr::SessionRequest request;
    request.sessionId = sessionId();
    asr::StopSessionResponse stopped;
    QElapsedTimer stopClock;
    stopClock.start();
    LOG_INFO(applog::cat::Worker)
        << "calling stop_session for" << request.sessionId
        << "- draining the server-side queue (deadline" << kStopTimeoutMs / 1000 << "s)";
    const grpc::Status status = client.stopSession(request, &stopped, kStopTimeoutMs);
    if (!status.ok()) {
        LOG_ERROR(applog::cat::Worker) << "stop_session failed after" << stopClock.elapsed()
                                       << "ms:" << status.toString();
        emit statusChanged(MicStatus::Error);
        emit failed(QStringLiteral("Kết thúc phiên thất bại - %1").arg(status.toString()));
        return false;
    }
    LOG_INFO(applog::cat::Worker)
        << "stop_session OK after" << stopClock.elapsed() << "ms - the server has consumed"
        << stopped.state.sourceSeenSec << "s of audio";

    // StopSessionResponse carries the final state, not a transcript envelope:
    // the durable review RPC is what owns the revision number.
    quint64 revision = 0;
    asr::ReviewRequest reviewRequest;
    reviewRequest.sessionId = request.sessionId;
    asr::StateResponse review;
    const grpc::Status reviewStatus = client.getReviewState(reviewRequest, &review, kReviewTimeoutMs);
    if (reviewStatus.ok()) {
        revision = review.transcriptRevision;
    } else {
        // Not fatal: the meeting is already stopped and stored, only the
        // revision number for the summary is missing.
        LOG_WARN(applog::cat::Worker)
            << "get_review_state after stop failed:" << reviewStatus.toString()
            << "- the summary will carry no revision number";
    }

    FinishedSession summary;
    summary.sessionId = request.sessionId;
    summary.sourceName = m_settings.source == Source::Microphone
        ? QStringLiteral("Microphone trực tiếp")
        : m_settings.fileName;
    summary.durationSec = double(m_sentBytes)
        / double(qMax(1, m_settings.sampleRate * m_settings.channels * 2));
    summary.revision = revision;

    LOG_INFO(applog::cat::Worker)
        << "session complete:" << summary.sessionId << summary.durationSec << "s sent, rev="
        << summary.revision << ", total" << m_seq << "packets /" << m_sentBytes << "bytes";
    emit statusChanged(MicStatus::Stopped);
    emit finished(summary);
    return true;
}
