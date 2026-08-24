#include "SessionBuffer.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>

#include <algorithm>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// How long a failed live-state refresh may be papered over with the cached
// answer.  The client polls five times a second; one lost poll flickering the
// whole transcript view would be worse than showing it 200 ms late.  Two
// seconds is well past "a blip" and well short of "nobody would notice".
const int kStaleGraceMs = 2000;

// Latency samples kept for the p50/p95 the admin RPC reports.  At six packets
// a second this is the last forty seconds or so - long enough to show a
// pipeline slowing down, short enough to recover when it speeds up again.
const int kLatencyWindow = 256;

// Between attempts on a transport failure.  Same value the client used, for
// the same reason: long enough not to spin on a dead socket, short enough that
// a recovered network is noticed inside one audio packet's worth of time.
const int kRetryPauseMs = 500;

double percentile(QList<double> values, double fraction)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    int index = int(fraction * double(values.size() - 1) + 0.5);
    index = qBound(0, index, values.size() - 1);
    return values.at(index);
}

} // namespace

SessionBuffer::SessionBuffer(const QString &sessionId, const asr::StartSessionResponse &started,
                             const Settings &settings)
    : m_sessionId(sessionId), m_started(started), m_settings(settings)
{
    m_startedAt = nowSeconds();
    m_updatedAt = m_startedAt;
    m_stateVersion = started.stateVersion;
    m_title = started.state.title;
    // The state that came back from start_session is a real state; seeding the
    // cache with it means the first get_live_state after a start is answered
    // without a round trip, which is when the client polls hardest.
    m_state.sessionId = sessionId;
    m_state.streamId = started.streamId;
    m_state.stateVersion = started.stateVersion;
    m_state.state = started.state;
    m_haveState = true;
    m_stateAge.start();

    m_stateLane = new RpcLane(QStringLiteral("state-%1").arg(sessionId.left(8)),
                              settings.upstreamTarget, settings.upstreamToken);
    openSpool();

    setObjectName(QStringLiteral("forward-%1").arg(sessionId.left(8)));
    start();
    LOG_INFO(applog::cat::Session)
        << "buffered session" << sessionId << "opened for" << settings.client << "- capacity"
        << settings.capacityBytes << "bytes";
}

SessionBuffer::~SessionBuffer()
{
    shutdown();
    if (!wait(15000)) {
        LOG_WARN(applog::cat::Session) << "forwarder for" << m_sessionId
                                       << "did not stop - terminating it";
        terminate();
        wait(2000);
    }
    delete m_stateLane;
    if (m_spool.isOpen())
        m_spool.close();
}

void SessionBuffer::openSpool()
{
    if (m_settings.spoolDir.trimmed().isEmpty())
        return;
    QDir dir(m_settings.spoolDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        LOG_WARN(applog::cat::Session)
            << "cannot create the spool directory" << m_settings.spoolDir << "- archiving is off";
        return;
    }
    m_spool.setFileName(dir.filePath(m_sessionId + QStringLiteral(".pcm")));
    if (!m_spool.open(QIODevice::WriteOnly | QIODevice::Append)) {
        LOG_WARN(applog::cat::Session) << "cannot open" << m_spool.fileName() << "-"
                                       << m_spool.errorString() << "- archiving is off";
        return;
    }
    LOG_INFO(applog::cat::Session) << "archiving accepted audio to" << m_spool.fileName();
}

// Called with m_mutex held.
void SessionBuffer::archive(const QByteArray &pcm)
{
    if (!m_spool.isOpen())
        return;
    const qint64 written = m_spool.write(pcm);
    if (written < 0) {
        // Losing the archive is not a reason to lose the meeting; say so once
        // and carry on serving the pipeline.
        LOG_WARN(applog::cat::Session)
            << "archive write failed for" << m_sessionId << "-" << m_spool.errorString();
        m_spool.close();
        return;
    }
    m_spooledBytes += quint64(written);
}

// Called with m_mutex held.
void SessionBuffer::recordForwardMs(double ms)
{
    m_forwardMs.append(ms);
    if (m_forwardMs.size() > kLatencyWindow)
        m_forwardMs.removeFirst();
}

void SessionBuffer::noteError(const grpc::Status &status)
{
    QMutexLocker lock(&m_mutex);
    m_lastError = status.toString();
    m_lastErrorAt = nowSeconds();
}

grpc::Status SessionBuffer::push(const asr::PushAudioRequest &request, asr::PushAudioResponse *out)
{
    QMutexLocker lock(&m_mutex);

    if (m_haveFatal) {
        // The pipeline rejected this meeting for good.  Hand the client the
        // upstream status unchanged, so its own rule - never retry anything
        // that is not a transport code - reaches the same conclusion here that
        // it used to reach against the adapter directly.
        return m_fatal;
    }
    if (m_finished || m_stopRequested.loadRelaxed()) {
        grpc::Status status;
        status.code = grpc::FailedPrecondition;
        status.message = QStringLiteral("phiên %1 đã dừng").arg(m_sessionId);
        return status;
    }

    // Idempotent replay, exactly as the adapter does it: a client that retries
    // after a timeout must not have its audio counted twice.  This is what
    // makes the client's transport-only retry safe against this buffer.
    if (request.seq != 0 && request.seq <= m_lastAcceptedSeq) {
        LOG_DEBUG(applog::cat::Session)
            << "replaying the stored ACK for" << m_sessionId << "seq=" << request.seq
            << "(last accepted" << m_lastAcceptedSeq << ")";
        *out = m_lastAck;
        return grpc::Status();
    }

    if (m_pendingBytes + request.pcm.size() > m_settings.capacityBytes) {
        // Refuse out loud.  Dropping the packet instead would leave a hole in
        // the audio that nothing downstream could detect, and the client is
        // built to stop on a rejection rather than to continue with one.
        grpc::Status status;
        status.code = grpc::ResourceExhausted;
        status.message =
            QStringLiteral("bộ đệm phiên đã đầy (%1/%2 byte) - tầng suy luận đang không theo kịp")
                .arg(m_pendingBytes)
                .arg(m_settings.capacityBytes);
        m_lastError = status.message;
        m_lastErrorAt = nowSeconds();
        LOG_ERROR(applog::cat::Session) << "push rejected for" << m_sessionId << "-"
                                        << status.message;
        return status;
    }

    Packet packet;
    packet.pcm = request.pcm;
    packet.sampleRate = request.sampleRate;
    packet.channels = request.channels;
    packet.audioFormat = request.audioFormat;
    packet.reset = request.reset;
    packet.vadChunkMs = request.vadChunkMs;
    // Forwarded under the client's own seq, not a fresh one: the adapter's
    // idempotency and ours then agree about which packet is which.
    packet.seq = request.seq;

    m_queue.append(packet);
    m_pendingBytes += packet.pcm.size();
    m_acceptedPackets += 1;
    m_acceptedBytes += quint64(packet.pcm.size());
    m_lastAcceptedSeq = qMax(m_lastAcceptedSeq, request.seq);
    m_sampleRate = request.sampleRate ? request.sampleRate : m_sampleRate;
    m_channels = request.channels ? request.channels : m_channels;
    m_lastAudioFormat = request.audioFormat.isEmpty() ? m_lastAudioFormat : request.audioFormat;
    m_updatedAt = nowSeconds();
    archive(packet.pcm);

    asr::PushAudioResponse ack;
    ack.sessionId = m_sessionId;
    ack.streamId = m_started.streamId;
    ack.stateVersion = m_stateVersion;
    // Still the pipeline's own figure, never this buffer's.  The client
    // subtracts it from what it has sent to get "hàng đợi server AI", and that
    // number now correctly includes whatever is sitting in this queue.
    ack.sourceSeenSec = m_upstreamSourceSeenSec;
    ack.speechSeenSec = m_upstreamSpeechSeenSec;
    // Carried through so the client's transport estimate keeps subtracting real
    // pipeline work rather than treating it as network time.
    ack.timing = m_upstreamTiming;
    m_lastAck = ack;
    *out = ack;

    m_notEmpty.wakeAll();
    return grpc::Status();
}

grpc::Status SessionBuffer::liveState(asr::StateResponse *out)
{
    // Lock order: state mutex first, then the queue mutex.  See the header.
    QMutexLocker stateLock(&m_stateMutex);
    {
        QMutexLocker lock(&m_mutex);
        ++m_stateReaders;
    }

    if (m_haveState && m_stateAge.isValid() && m_stateAge.elapsed() < m_settings.statePollMs) {
        // The fan-out that makes this worth having: ten clients watching one
        // meeting cost the pipeline one poll, not ten.
        *out = m_state;
        return grpc::Status();
    }

    asr::SessionRequest request;
    request.sessionId = m_sessionId;
    asr::StateResponse fresh;
    const int timeoutMs = m_settings.upstreamTimeoutMs;
    const grpc::Status status = m_stateLane->call([&](AsrClient &client) {
        return client.getLiveState(request, &fresh, timeoutMs);
    });

    if (status.ok()) {
        m_state = fresh;
        m_haveState = true;
        m_stateAge.restart();
        QMutexLocker lock(&m_mutex);
        ++m_statePolls;
        if (fresh.stateVersion != 0)
            m_stateVersion = fresh.stateVersion;
        if (!fresh.state.title.isEmpty())
            m_title = fresh.state.title;
        m_updatedAt = nowSeconds();
        *out = fresh;
        return status;
    }

    if (m_haveState && m_stateAge.isValid() && m_stateAge.elapsed() < kStaleGraceMs) {
        LOG_DEBUG(applog::cat::Poll) << "live state for" << m_sessionId << "failed ("
                                     << status.toString() << ") - serving a cache"
                                     << m_stateAge.elapsed() << "ms old";
        *out = m_state;
        return grpc::Status();
    }
    LOG_WARN(applog::cat::Poll) << "live state for" << m_sessionId << "failed:"
                                << status.toString();
    return status;
}

grpc::Status SessionBuffer::stop(int timeoutMs, asr::StopSessionResponse *out)
{
    QMutexLocker lock(&m_mutex);
    if (m_stopDone) {
        // Idempotent: a client that retried stop_session after a timeout gets
        // the same answer rather than a second, different one.
        *out = m_stopResponse;
        return m_stopStatus;
    }
    if (!m_stopRequested.loadRelaxed()) {
        LOG_INFO(applog::cat::Session)
            << "stop requested for" << m_sessionId << "-" << m_queue.size()
            << "packets still queued";
        m_stopRequested.storeRelease(1);
        m_notEmpty.wakeAll();
    }

    QElapsedTimer clock;
    clock.start();
    const int budget = timeoutMs > 0 ? timeoutMs : 120000;
    while (!m_stopDone) {
        const qint64 left = qint64(budget) - clock.elapsed();
        if (left <= 0) {
            // The drain is still running and will finish on its own; saying so
            // is better than either blocking forever or claiming the meeting
            // closed when it has not.
            grpc::Status status;
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("còn %1 gói đang chờ đẩy lên tầng suy luận")
                                 .arg(m_queue.size());
            return status;
        }
        m_stopped.wait(&m_mutex, QDeadlineTimer(int(left)));
    }
    *out = m_stopResponse;
    return m_stopStatus;
}

void SessionBuffer::shutdown()
{
    QMutexLocker lock(&m_mutex);
    m_shutdownRequested.storeRelease(1);
    m_notEmpty.wakeAll();
    m_stopped.wakeAll();
}

bool SessionBuffer::isFinished() const
{
    QMutexLocker lock(&m_mutex);
    return m_finished;
}

double SessionBuffer::idleSeconds() const
{
    QMutexLocker lock(&m_mutex);
    return nowSeconds() - m_updatedAt;
}

double SessionBuffer::finishedSecondsAgo() const
{
    QMutexLocker lock(&m_mutex);
    return m_finished ? nowSeconds() - m_finishedAt : 0.0;
}

bool SessionBuffer::forward(AsrClient &client, const Packet &packet, grpc::Status *fatal)
{
    asr::PushAudioRequest request;
    request.sessionId = m_sessionId;
    request.pcm = packet.pcm;
    request.sampleRate = packet.sampleRate;
    request.channels = packet.channels;
    request.audioFormat = packet.audioFormat;
    request.reset = packet.reset;
    request.vadChunkMs = packet.vadChunkMs;
    request.seq = packet.seq;

    QElapsedTimer clock;
    clock.start();
    bool announced = false;
    for (;;) {
        asr::PushAudioResponse response;
        const grpc::Status status =
            client.pushAudio(request, &response, m_settings.upstreamTimeoutMs);
        if (status.ok()) {
            const double ms = double(clock.nsecsElapsed()) / 1e6;
            QMutexLocker lock(&m_mutex);
            ++m_forwardedPackets;
            m_forwardedBytes += quint64(packet.pcm.size());
            m_upstreamSourceSeenSec = response.sourceSeenSec;
            m_upstreamSpeechSeenSec = response.speechSeenSec;
            m_upstreamTiming = response.timing;
            if (response.stateVersion != 0)
                m_stateVersion = response.stateVersion;
            recordForwardMs(ms);
            m_updatedAt = nowSeconds();
            if (announced) {
                LOG_INFO(applog::cat::Session)
                    << "reconnected to the inference tier - continuing" << m_sessionId
                    << "from seq=" << packet.seq;
                m_lastError.clear();
            }
            return true;
        }

        if (!status.isTransport()) {
            // INTERNAL and friends are NOT retried: the adapter returns them
            // precisely when the pipeline may already have consumed this
            // audio, and a blind retry there duplicates words.  The rule is
            // unchanged by the split - it has only moved one hop.
            LOG_ERROR(applog::cat::Session)
                << "push_audio seq=" << packet.seq << "for" << m_sessionId
                << "permanently rejected:" << status.toString()
                << "- not retried, a retry here would duplicate words";
            *fatal = status;
            noteError(status);
            return false;
        }

        if (!announced) {
            announced = true;
            LOG_WARN(applog::cat::Session)
                << "push_audio seq=" << packet.seq << "for" << m_sessionId
                << "transport failure:" << status.toString()
                << "- holding the packet and redialling; the client keeps recording";
        }
        noteError(status);
        {
            QMutexLocker lock(&m_mutex);
            ++m_retries;
        }
        // Do not wait out HTTP/2's own reconnect behaviour on a stale socket.
        client.reset();
        if (m_shutdownRequested.loadRelaxed()) {
            fatal->code = grpc::Cancelled;
            fatal->message = QStringLiteral("máy chủ đệm đang tắt");
            return false;
        }
        QThread::msleep(kRetryPauseMs);
        if (m_shutdownRequested.loadRelaxed()) {
            fatal->code = grpc::Cancelled;
            fatal->message = QStringLiteral("máy chủ đệm đang tắt");
            return false;
        }
    }
}

void SessionBuffer::run()
{
    // Created here, on this thread, and never handed to another: the socket
    // underneath has thread affinity.  See RpcLane.h for the same rule stated
    // once for the lanes that do have to cross threads.
    AsrClient client(m_settings.upstreamTarget, m_settings.upstreamToken);

    for (;;) {
        Packet packet;
        bool haveWork = false;
        bool drainDone = false;
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.isEmpty() && !m_stopRequested.loadRelaxed()
                   && !m_shutdownRequested.loadRelaxed()) {
                m_notEmpty.wait(&m_mutex);
            }
            if (m_shutdownRequested.loadRelaxed())
                break;
            if (!m_queue.isEmpty()) {
                packet = m_queue.takeFirst();
                m_pendingBytes -= packet.pcm.size();
                haveWork = true;
            } else {
                // Queue empty *and* a stop was asked for: this is the drain
                // barrier.  Everything the client sent is now upstream.
                drainDone = true;
            }
        }

        if (haveWork) {
            grpc::Status fatal;
            if (!forward(client, packet, &fatal)) {
                QMutexLocker lock(&m_mutex);
                m_fatal = fatal;
                m_haveFatal = true;
                m_droppedPackets += quint64(m_queue.size()) + 1;
                m_queue.clear();
                m_pendingBytes = 0;
                // The loop keeps running: the client still has a stop_session
                // to send, and closing the meeting upstream is worth doing
                // even after a failure.
            }
            continue;
        }

        if (drainDone) {
            asr::SessionRequest request;
            request.sessionId = m_sessionId;
            asr::StopSessionResponse response;
            LOG_INFO(applog::cat::Session) << "queue drained for" << m_sessionId
                                           << "- calling stop_session upstream";
            const grpc::Status status = client.stopSession(
                request, &response, qMax(m_settings.upstreamTimeoutMs, 60000));
            QMutexLocker lock(&m_mutex);
            m_stopResponse = response;
            m_stopStatus = status;
            m_stopDone = true;
            m_finished = true;
            m_finishedAt = nowSeconds();
            m_updatedAt = m_finishedAt;
            if (!status.ok()) {
                m_lastError = status.toString();
                m_lastErrorAt = m_finishedAt;
            }
            m_stopped.wakeAll();
            break;
        }
    }

    QMutexLocker lock(&m_mutex);
    if (!m_finished) {
        // Reached only on shutdown.  Mark it finished so the admin view does
        // not show a live session behind a stopped server, and wake anyone
        // still waiting in stop().
        m_finished = true;
        m_finishedAt = nowSeconds();
        if (!m_stopDone) {
            m_stopStatus.code = grpc::Unavailable;
            m_stopStatus.message = QStringLiteral("máy chủ đệm đã tắt trước khi phiên kết thúc");
            m_stopDone = true;
        }
        m_stopped.wakeAll();
    }
    if (m_spool.isOpen())
        m_spool.close();
    LOG_INFO(applog::cat::Session)
        << "forwarder for" << m_sessionId << "finished -" << m_forwardedPackets << "of"
        << m_acceptedPackets << "packets delivered," << m_droppedPackets << "dropped,"
        << m_retries << "retries";
}

buf::BufferedSession SessionBuffer::snapshot() const
{
    // Same lock order as everywhere else: state mutex, then queue mutex.
    QMutexLocker stateLock(&m_stateMutex);
    const double stateAge = m_haveState && m_stateAge.isValid()
        ? double(m_stateAge.elapsed()) / 1000.0
        : 0.0;

    QMutexLocker lock(&m_mutex);
    buf::BufferedSession out;
    out.sessionId = m_sessionId;
    out.client = m_settings.client;
    out.title = m_title;
    out.startedAt = m_startedAt;
    out.updatedAt = m_updatedAt;
    out.running = !m_finished;
    out.acceptedPackets = m_acceptedPackets;
    out.acceptedBytes = m_acceptedBytes;
    out.forwardedPackets = m_forwardedPackets;
    out.forwardedBytes = m_forwardedBytes;
    out.pendingPackets = quint64(m_queue.size());
    out.pendingBytes = quint64(m_pendingBytes);
    out.spooledBytes = m_spooledBytes;
    out.droppedPackets = m_droppedPackets;
    out.retries = m_retries;
    const double bytesPerSec =
        double(qMax(1u, m_sampleRate) * qMax(1u, m_channels) * 2u);
    out.lagSec = double(m_acceptedBytes - m_forwardedBytes) / bytesPerSec;
    out.forwardP50Ms = percentile(m_forwardMs, 0.50);
    out.forwardP95Ms = percentile(m_forwardMs, 0.95);
    out.lastError = m_lastError;
    out.lastErrorAt = m_lastErrorAt;
    out.statePolls = m_statePolls;
    out.stateAgeSec = stateAge;
    out.stateReaders = m_stateReaders;
    return out;
}
