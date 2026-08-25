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

SessionBuffer::SessionBuffer(const QString &sessionId, qint64 streamId,
                             const BackendSessionConfig &config, const Settings &settings)
    : m_sessionId(sessionId), m_handle(jrn::store::handleFor(sessionId)), m_settings(settings),
      m_config(config), m_backendStreamId(streamId)
{
    m_startedAt = nowSeconds();
    m_updatedAt = m_startedAt;
    m_clientReturned = true;
    m_title = config.title;
    m_sampleRate = config.sampleRate;
    m_channels = config.channels;
    m_live.configure(config.title, config.sampleRate, config.channels);

    // The reply to start_session is ours to compose now: there is no adapter
    // answer to pass on.  It carries the empty state the client will start
    // polling against, so its shape has to be the same one liveState() returns.
    m_started.sessionId = sessionId;
    m_started.streamId = streamId;
    m_started.stateVersion = 0;
    m_started.state = m_live.snapshot(sessionId, streamId).state;

    if (!m_settings.journalDir.trimmed().isEmpty()) {
        jrn::Meta meta;
        meta.sessionId = sessionId;
        meta.client = settings.client;
        meta.startedAt = m_startedAt;
        meta.configJson = config.rawJson;
        meta.startResponse = m_started.serialize();
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
    m_backendStreamId = recovered.started.streamId;

    // The meeting's configuration is in the journal precisely so a recovered
    // session decodes the same way it did before the restart - a different
    // sample rate or model on the second half would corrupt the transcript far
    // more quietly than a failure would.
    QString warning;
    m_config = BackendSessionConfig::fromJson(recovered.meta.configJson, &warning);
    if (!warning.isEmpty()) {
        LOG_WARN(applog::cat::Session)
            << "recovered session" << m_sessionId << "-" << warning;
    }
    m_live.configure(m_title.isEmpty() ? m_config.title : m_title, m_config.sampleRate,
                     m_config.channels);
    // The transcript itself is NOT recovered: the words were never on disk, only
    // the audio was.  The backlog replays through the tier and rebuilds them,
    // which is why order across the restart had to be preserved.

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
    // m_backend is released on the forwarder thread, inside run(), for the same
    // thread-affinity reason it was created there.  By here that thread is
    // gone and the pointer is already null.
    m_journal.close();
}

grpc::Status SessionBuffer::openBackend()
{
    QMutexLocker lock(&m_mutex);
    // The forwarder opens it as its first act; this just waits for the answer.
    // A bound rather than an indefinite wait: a tier that never answers would
    // otherwise hold the connection thread that is answering start_session.
    while (!m_openDone) {
        if (!m_opened.wait(&m_mutex, QDeadlineTimer(qMax(5000, m_settings.upstreamTimeoutMs)))) {
            grpc::Status status;
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("tầng suy luận không mở được phiên trong %1 ms")
                                 .arg(qMax(5000, m_settings.upstreamTimeoutMs));
            return status;
        }
    }
    return m_openStatus;
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
    //
    // No upstream call and no cache any more.  The pipeline that used to own
    // this state is gone; LiveTranscript here is the authority, so the whole
    // fan-out problem the cache existed for - ten clients on one meeting - is
    // now just ten copies of a local structure.  statePollMs survives as the
    // client's poll interval, not as a cache lifetime.
    QMutexLocker stateLock(&m_stateMutex);
    {
        QMutexLocker lock(&m_mutex);
        ++m_stateReaders;
        ++m_statePolls;
    }
    *out = m_live.snapshot(m_sessionId, m_backendStreamId);
    return grpc::Status();
}

grpc::Status SessionBuffer::reviewState(double viewStartSec, double viewEndSec,
                                        asr::StateResponse *out)
{
    QMutexLocker stateLock(&m_stateMutex);
    *out = m_live.snapshot(m_sessionId, m_backendStreamId, viewStartSec, viewEndSec);
    return grpc::Status();
}

grpc::Status SessionBuffer::applyTextEdit(const asr::TextEditRequest &request,
                                          asr::ReviewEditResponse *out)
{
    QMutexLocker stateLock(&m_stateMutex);
    if (!m_live.applyEdit(request.baseRevision, request.startSec, request.endSec,
                          request.replacementWords)) {
        // ABORTED and not INVALID_ARGUMENT: the client's cure is to re-read and
        // retry, which is exactly what ABORTED tells it to do.  Someone else
        // edited the same transcript in between.
        grpc::Status status;
        status.code = grpc::Aborted;
        status.message = QStringLiteral("bản chép đã đổi (phiên bản %1, bạn gửi %2) - hãy đọc lại "
                                        "rồi sửa tiếp")
                             .arg(m_live.revision())
                             .arg(request.baseRevision);
        return status;
    }
    out->sessionId = m_sessionId;
    out->transcript = m_live.transcript();
    out->state = m_live.snapshot(m_sessionId, m_backendStreamId).state;
    {
        QMutexLocker lock(&m_mutex);
        m_updatedAt = nowSeconds();
    }
    LOG_INFO(applog::cat::Session)
        << "text edit on" << m_sessionId << "by"
        << (request.editorId.isEmpty() ? QStringLiteral("?") : request.editorId) << "-"
        << request.startSec << ".." << request.endSec << "->" << request.replacementWords.size()
        << "words, revision now" << m_live.revision();
    return grpc::Status();
}

grpc::Status SessionBuffer::renameSpeaker(const asr::RenameSpeakerRequest &request,
                                          asr::ReviewEditResponse *out)
{
    QMutexLocker stateLock(&m_stateMutex);
    m_live.renameSpeaker(request.fromSpeaker, request.toSpeaker, request.verifiedName);
    out->sessionId = m_sessionId;
    out->transcript = m_live.transcript();
    out->state = m_live.snapshot(m_sessionId, m_backendStreamId).state;
    {
        QMutexLocker lock(&m_mutex);
        m_updatedAt = nowSeconds();
    }
    LOG_INFO(applog::cat::Session) << "speaker rename on" << m_sessionId << "-"
                                   << request.fromSpeaker << "->" << request.toSpeaker
                                   << "verified as"
                                   << (request.verifiedName.isEmpty() ? QStringLiteral("(xoá tên)")
                                                                      : request.verifiedName);
    return grpc::Status();
}

asr::SessionSummary SessionBuffer::summary() const
{
    QMutexLocker stateLock(&m_stateMutex);
    QMutexLocker lock(&m_mutex);
    asr::SessionSummary out;
    out.sessionId = m_sessionId;
    out.title = m_title.isEmpty() ? m_config.title : m_title;
    out.createdAt = m_startedAt;
    out.updatedAt = m_updatedAt;
    out.durationSec = m_live.sourceSeenSec();
    out.final = m_live.done();
    out.running = !m_finished;
    out.participants = m_live.speakerIds();
    return out;
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

bool SessionBuffer::forward(BackendSession &session, const Packet &packet, grpc::Status *fatal)
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
        const grpc::Status status = session.push(request, &response);
        if (status.ok()) {
            const double ms = double(clock.nsecsElapsed()) / 1e6;
            // Fold the tier's answer into the transcript before the counters,
            // and under the transcript's own lock.  Lock order is state then
            // queue, so this block has to come first - see the header.
            {
                QMutexLocker stateLock(&m_stateMutex);
                m_stateVersion = m_live.apply(response);
            }
            QMutexLocker lock(&m_mutex);
            ++m_forwardedPackets;
            m_forwardedBytes += quint64(packet.pcm.size());
            m_upstreamSourceSeenSec = response.sourceSeenSec;
            m_upstreamSpeechSeenSec = response.speechSeenSec;
            m_upstreamTiming = response.timing;
            // m_stateVersion is the transcript's now, set above.  The tier does
            // not have one to report and never did after the adapter went.
            recordForwardMs(ms);
            m_updatedAt = nowSeconds();
            // After the forward, never before.  A crash between the two
            // re-sends this seq on the next start; writing it first would lose
            // the packet outright, which is the worse of the two.
            //
            // NOTE: the duplicate is no longer absorbed.  The adapter used to
            // replay its stored answer for a seq it had already processed, and
            // neither Riva nor Triton has any such memory - so a crash here
            // costs one packet's worth of duplicated words (160 ms) on the
            // next start.  Making that exact again needs the transcript itself
            // to be persisted; see SessionStore in the handover notes.
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
            // INTERNAL and friends are NOT retried: they come back precisely
            // when the tier may already have consumed this audio, and a blind
            // retry there duplicates words.  The rule has survived two moves -
            // client to buffer, adapter to tier - unchanged.
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
        session.reset();
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
    // Opened here, on this thread, and never handed to another: the socket
    // underneath has thread affinity.  See RpcLane.h for the same rule stated
    // once for the lanes that genuinely do have to cross threads.
    //
    // openBackend() is blocked on the answer, so this must publish one whether
    // it worked or not - a failure here that never woke the waiter would hang
    // the connection thread answering start_session for its whole deadline.
    {
        grpc::Status opened;
        std::unique_ptr<BackendSession> session;
        if (!m_settings.backend) {
            opened.code = grpc::Internal;
            opened.message = QStringLiteral("phiên không được gắn tầng suy luận nào");
        } else {
            session = m_settings.backend->open(m_sessionId, m_backendStreamId, m_config, &opened);
        }
        QMutexLocker lock(&m_mutex);
        m_backend = std::move(session);
        m_openStatus = opened;
        m_openDone = true;
        m_opened.wakeAll();
        if (!opened.ok() || !m_backend) {
            // Nothing to drain and nothing to close: the meeting never began.
            m_finished = true;
            m_finishedAt = nowSeconds();
            m_stopStatus = opened;
            m_stopDone = true;
            m_stopped.wakeAll();
            return;
        }
    }

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
            if (!forward(*m_backend, packet, &fatal)) {
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
            LOG_INFO(applog::cat::Session) << "queue drained for" << m_sessionId
                                           << "- flushing the inference tier";
            // The far end of the drain barrier.  For Triton this is one final
            // infer with is_final=1, which flushes the endpointer and the last
            // correction pass; for Riva it half-closes the stream and reads
            // what is still outstanding.  Either way, when it returns the tier
            // has seen every byte the client sent.
            asr::PushAudioResponse tail;
            const grpc::Status status = m_backend->finish(&tail);

            asr::StopSessionResponse response;
            {
                QMutexLocker stateLock(&m_stateMutex);
                if (status.ok())
                    m_live.apply(tail);
                m_live.markDone();
                const asr::StateResponse snapshot =
                    m_live.snapshot(m_sessionId, m_backendStreamId);
                response.sessionId = m_sessionId;
                response.streamId = m_backendStreamId;
                response.stateVersion = snapshot.stateVersion;
                response.events.final = true;
                response.result = tail;
                response.state = snapshot.state;
            }

            QMutexLocker lock(&m_mutex);
            m_stateVersion = response.stateVersion;
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
        // Dropped, not flushed: on the shutdown path the meeting is not over,
        // it is interrupted, and telling the tier it ended cleanly would be a
        // lie the journal then contradicts on the next start.
        if (m_backend)
            m_backend->abandon();
    }
    // Released here because it was created here.  Letting the destructor do it
    // would free a socket from whichever thread happened to drop the last
    // reference to this session.
    m_backend.reset();
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
    // The transcript is built here now, so it is never stale.  The field stays
    // in the admin proto because an operator reading it wants a number, and 0
    // is the honest one: this server is not waiting on anybody for it.
    const double stateAge = 0.0;

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
