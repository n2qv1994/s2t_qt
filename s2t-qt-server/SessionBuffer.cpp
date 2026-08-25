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

// Seeds the live-state cache from whatever the pipeline last told us.  Doing it
// here means the first get_live_state after a start - or after a restart - is
// answered without a round trip, which is exactly when the client polls hardest.
void SessionBuffer::seedState()
{
    m_state.sessionId = m_sessionId;
    m_state.streamId = m_started.streamId;
    m_state.stateVersion = m_started.stateVersion;
    m_state.state = m_started.state;
    m_haveState = true;
    m_stateAge.start();
}

SessionBuffer::SessionBuffer(const QString &sessionId, const asr::StartSessionResponse &started,
                             const Settings &settings)
    : m_sessionId(sessionId), m_handle(jrn::store::handleFor(sessionId)), m_started(started),
      m_settings(settings)
{
    m_startedAt = nowSeconds();
    m_updatedAt = m_startedAt;
    m_stateVersion = started.stateVersion;
    m_title = started.state.title;
    m_clientReturned = true;
    seedState();

    if (!m_settings.journalDir.trimmed().isEmpty()) {
        jrn::Meta meta;
        meta.sessionId = sessionId;
        meta.client = settings.client;
        meta.startedAt = m_startedAt;
        meta.startResponse = started.serialize();
        QString error;
        if (!m_journal.create(m_settings.journalDir, m_handle, meta, m_settings.durability,
                              m_settings.journalKeep, m_settings.segmentBytes, &error)) {
            // Not survivable quietly.  push() promises the client that an ACK
            // means the packet is durable; without a journal that promise is
            // false, and a session that silently downgrades is worse than one
            // that refuses - see the check in push().
            m_journalFailed = true;
            m_lastError = error;
            m_lastErrorAt = nowSeconds();
            LOG_ERROR(applog::cat::Session)
                << "session" << sessionId << "has no journal:" << error
                << "- push_audio will be refused";
        }
    }

    m_stateLane = new RpcLane(QStringLiteral("state-%1").arg(sessionId.left(8)),
                              settings.upstreamTarget, settings.upstreamToken);

    setObjectName(QStringLiteral("forward-%1").arg(sessionId.left(8)));
    start();
    LOG_INFO(applog::cat::Session)
        << "buffered session" << sessionId << "opened for" << settings.client << "- capacity"
        << settings.capacityBytes << "bytes, journal"
        << (m_journal.isOpen() ? m_settings.journalDir : QStringLiteral("(off)"));
}

SessionBuffer::SessionBuffer(const jrn::Recovered &recovered, const Settings &settings)
    : m_sessionId(recovered.meta.sessionId), m_handle(recovered.handle),
      m_started(recovered.started), m_settings(settings)
{
    m_settings.client = recovered.meta.client;
    m_startedAt = recovered.meta.startedAt > 0.0 ? recovered.meta.startedAt : nowSeconds();
    m_updatedAt = nowSeconds();
    m_recovered = true;
    m_clientReturned = false;

    m_stateVersion = qMax(recovered.started.stateVersion, recovered.progress.stateVersion);
    m_title = recovered.started.state.title;
    seedState();

    // Everything the pipeline had already acknowledged before the restart.
    m_upstreamSourceSeenSec = recovered.progress.sourceSeenSec;
    m_upstreamSpeechSeenSec = recovered.progress.speechSeenSec;
    m_lastAcceptedSeq = recovered.lastAcceptedSeq;
    m_acceptedPackets = recovered.acceptedPackets;
    m_acceptedBytes = recovered.acceptedBytes;
    m_forwardedPackets = recovered.forwardedPackets;

    for (const jrn::Packet &packet : recovered.backlog) {
        m_queue.append(packet);
        m_pendingBytes += packet.pcm.size();
        m_sampleRate = packet.sampleRate ? packet.sampleRate : m_sampleRate;
        m_channels = packet.channels ? packet.channels : m_channels;
        if (!packet.audioFormat.isEmpty())
            m_lastAudioFormat = packet.audioFormat;
    }

    // The ACK a client would get for replaying its last seq has to be the same
    // one it got before the restart, or the retry it is about to make looks
    // like a different answer to the same question.
    m_lastAck.sessionId = m_sessionId;
    m_lastAck.streamId = m_started.streamId;
    m_lastAck.stateVersion = m_stateVersion;
    m_lastAck.sourceSeenSec = m_upstreamSourceSeenSec;
    m_lastAck.speechSeenSec = m_upstreamSpeechSeenSec;

    QString error;
    if (!m_journal.reopen(m_settings.journalDir, m_handle, recovered.meta, m_settings.durability,
                          m_settings.journalKeep, m_settings.segmentBytes, &error)) {
        m_journalFailed = true;
        m_lastError = error;
        m_lastErrorAt = nowSeconds();
        LOG_ERROR(applog::cat::Session)
            << "recovered session" << m_sessionId << "cannot reopen its journal:" << error
            << "- the backlog will still be delivered, but no new audio is accepted";
    }

    m_stateLane = new RpcLane(QStringLiteral("state-%1").arg(m_sessionId.left(8)),
                              m_settings.upstreamTarget, m_settings.upstreamToken);

    setObjectName(QStringLiteral("forward-%1").arg(m_sessionId.left(8)));
    start();
    LOG_INFO(applog::cat::Session)
        << "recovered session" << m_sessionId << "from" << recovered.handle << "-"
        << recovered.backlog.size() << "packets to re-send (seq" << (recovered.progress.seq + 1)
        << ".." << recovered.lastAcceptedSeq << "), originally from" << m_settings.client;
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
    m_journal.close();
}

bool SessionBuffer::awaitingClient() const
{
    QMutexLocker lock(&m_mutex);
    return m_recovered && !m_clientReturned;
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

    if (m_journalFailed) {
        // The durability promise cannot be kept for this session.  Refusing is
        // the honest answer: an ACK here would mean "safe across a restart" and
        // it would not be true.  The client stops loudly, which is what it does
        // with any rejection.
        grpc::Status status;
        status.code = grpc::Internal;
        status.message =
            QStringLiteral("máy chủ đệm không ghi được nhật ký cho phiên này (%1) - từ chối nhận "
                           "audio thay vì hứa suông là đã lưu bền")
                .arg(m_lastError);
        return status;
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

    // On disk before the ACK, never after.  An ACK means "durably in this
    // buffer", and after a restart that has to still be true - so the write
    // happens first and a failed write is a refusal, not a warning.
    if (m_journal.isOpen()) {
        QString error;
        if (!m_journal.appendPacket(packet, &error)) {
            m_journalFailed = true;
            m_lastError = error;
            m_lastErrorAt = nowSeconds();
            LOG_ERROR(applog::cat::Session)
                << "journal write failed for" << m_sessionId << "seq=" << request.seq << "-"
                << error << "- refusing the packet";
            grpc::Status status;
            status.code = grpc::Internal;
            status.message = QStringLiteral("máy chủ đệm không ghi được nhật ký: %1").arg(error);
            return status;
        }
    }

    m_queue.append(packet);
    m_pendingBytes += packet.pcm.size();
    m_acceptedPackets += 1;
    m_acceptedBytes += quint64(packet.pcm.size());
    m_lastAcceptedSeq = qMax(m_lastAcceptedSeq, request.seq);
    m_sampleRate = request.sampleRate ? request.sampleRate : m_sampleRate;
    m_channels = request.channels ? request.channels : m_channels;
    m_lastAudioFormat = request.audioFormat.isEmpty() ? m_lastAudioFormat : request.audioFormat;
    m_updatedAt = nowSeconds();
    if (!m_clientReturned) {
        m_clientReturned = true;
        LOG_INFO(applog::cat::Session)
            << "client came back for recovered session" << m_sessionId << "at seq=" << request.seq;
    }

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

void SessionBuffer::requestStop()
{
    QMutexLocker lock(&m_mutex);
    if (m_stopDone || m_stopRequested.loadRelaxed())
        return;
    LOG_INFO(applog::cat::Session) << "stop requested for" << m_sessionId << "-"
                                   << m_queue.size() << "packets still queued";
    m_stopRequested.storeRelease(1);
    m_notEmpty.wakeAll();
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
            // After the forward, never before.  A crash between the two
            // re-sends this seq on the next start and the pipeline's own seq
            // idempotency turns the duplicate into a no-op; writing this first
            // would instead lose the packet outright.
            if (m_journal.isOpen()) {
                jrn::Progress progress;
                progress.seq = packet.seq;
                progress.sourceSeenSec = m_upstreamSourceSeenSec;
                progress.speechSeenSec = m_upstreamSpeechSeenSec;
                progress.stateVersion = m_stateVersion;
                QString journalError;
                if (!m_journal.appendProgress(progress, &journalError)) {
                    // Losing the watermark costs a replay after a restart, not
                    // data - so it is a warning, unlike a lost packet record.
                    LOG_WARN(applog::cat::Session)
                        << "journal progress write failed for" << m_sessionId << "-"
                        << journalError << "- a restart would re-send from seq"
                        << (packet.seq + 1);
                }
            }
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
            // Marks the meeting as over, so the next start does not resume it.
            // This one is always fsynced: replaying a finished session is worse
            // than the cost of a single sync.
            if (m_journal.isOpen()) {
                QString journalError;
                if (!m_journal.appendStopped(status.ok(), &journalError)) {
                    LOG_WARN(applog::cat::Session) << "journal stop record failed for"
                                                   << m_sessionId << "-" << journalError;
                }
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
    // Deliberately not closed on the shutdown path before this point: the
    // journal is what the next start reads to pick this meeting back up.
    m_journal.close();
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
    // Reported as "bytes written to the journal", which is what the field now
    // means: the journal replaced the old write-only archive.
    out.spooledBytes = m_journal.bytesWritten();
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
