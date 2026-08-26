#include "BufferHub.h"

#include "backend/RivaBackend.h"
#include "backend/TritonBackend.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QRandomGenerator>
#include <QUuid>

#include <algorithm>

namespace {

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// Sample rate assumed when sizing a session's queue before the first packet
// has arrived to say otherwise.  48 kHz mono is what the client records at and
// what run_windows_ui.ps1 passed; a session that turns out to use something
// else is still bounded, just to a different number of seconds.
const int kAssumedSampleRate = 48000;
const int kAssumedChannels = 1;

} // namespace

// ---------------------------------------------------------------------------
// UpstreamProbe
// ---------------------------------------------------------------------------

UpstreamProbe::UpstreamProbe(BufferHub *hub, int periodMs) : m_hub(hub), m_periodMs(periodMs)
{
    setObjectName(QStringLiteral("upstream-probe"));
}

UpstreamProbe::~UpstreamProbe()
{
    requestStop();
    if (!wait(10000)) {
        LOG_WARN(applog::cat::Rpc) << "upstream probe did not stop - terminating it";
        terminate();
        wait(2000);
    }
}

void UpstreamProbe::requestStop()
{
    QMutexLocker lock(&m_mutex);
    m_stop = true;
    m_wake.wakeAll();
}

void UpstreamProbe::poke()
{
    QMutexLocker lock(&m_mutex);
    m_wake.wakeAll();
}

void UpstreamProbe::run()
{
    for (;;) {
        {
            QMutexLocker lock(&m_mutex);
            if (m_stop)
                break;
        }
        double latencyMs = 0.0;
        // The backend decides what a probe means - ServerLive for Triton,
        // GetRivaSpeechRecognitionConfig for Riva.  Both go through the
        // backend's own admin lane, which is what keeps this thread from
        // touching a socket that belongs to another one.
        const grpc::Status status = m_hub->backend().ping(3000, &latencyMs);
        m_hub->noteUpstream(status.ok(), latencyMs, status.ok() ? QString() : status.toString());
        QMutexLocker lock(&m_mutex);
        if (m_stop)
            break;
        m_wake.wait(&m_mutex, QDeadlineTimer(m_periodMs));
    }
}

// ---------------------------------------------------------------------------
// BufferHub
// ---------------------------------------------------------------------------

BufferHub::BufferHub(const ServerConfig &config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_uptime.start();
    m_upstream.target = config.upstreamTarget;

    // First, before any thread exists: a start that is going to be refused
    // should not have spun up a relay pool and a probe only to stop them again.
    //
    // Two instances over one journal directory would each recover the same
    // sessions and each re-send the same packets, duplicating words in a
    // transcript.  That is the worst failure mode this project has, and it is
    // cheap to make impossible.
    //
    // QLockFile and not an O_EXCL file: it records the pid and takes over a
    // lock whose owner is gone, so a SIGKILL cannot leave behind a stale lock
    // that blocks the very restart this journal exists to make possible.
    if (!m_config.journalDir.trimmed().isEmpty()) {
        QDir dir(m_config.journalDir);
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            m_ok = false;
            m_error = QStringLiteral("không tạo được thư mục nhật ký '%1'")
                          .arg(m_config.journalDir);
            return;
        }
        m_lock = std::make_unique<QLockFile>(dir.filePath(QStringLiteral(".s2t-qt-server.lock")));
        m_lock->setStaleLockTime(0); // rely on the pid check, not on a timeout
        if (!m_lock->tryLock(0)) {
            qint64 pid = 0;
            QString host;
            QString app;
            m_lock->getLockInfo(&pid, &host, &app);
            m_ok = false;
            m_error = QStringLiteral("thư mục nhật ký '%1' đang được tiến trình %2 trên '%3' giữ "
                                     "- hai máy chủ dùng chung một nhật ký sẽ gửi trùng audio")
                          .arg(m_config.journalDir)
                          .arg(pid)
                          .arg(host.isEmpty() ? QStringLiteral("?") : host);
            m_lock.reset();
            return;
        }
    }

    QString storeError;
    if (!m_store.open(m_config.databaseDir, &storeError)) {
        // Not fatal: a meeting still runs without an archive, and refusing to
        // start over it would take the live path down with the history.
        LOG_ERROR(applog::cat::Session)
            << "kho phiên không mở được -" << storeError
            << "- máy chủ vẫn chạy, nhưng cuộc họp sẽ không được lưu lại";
    }

    // Which tier this server talks to.  ServerConfig has already refused
    // anything but these two, so there is no "unknown backend" branch to write
    // here - a typo was rejected before any thread existed.
    if (config.backend == QStringLiteral("riva")) {
        m_backend = std::make_unique<RivaBackend>(config.upstreamTarget, config.upstreamToken,
                                                  config.model, config.language,
                                                  config.upstreamTimeoutMs);
    } else {
        m_backend = std::make_unique<TritonBackend>(config.upstreamTarget, config.upstreamToken,
                                                    config.model, config.upstreamTimeoutMs);
    }

    m_probe = new UpstreamProbe(this, config.upstreamProbeMs);
    m_probe->start();

    // Before the reaper, and before anything can listen: a meeting read back
    // from disk has to exist by the time a client can ask for it.
    recoverSessions();

    connect(&m_reaper, &QTimer::timeout, this, &BufferHub::reap);
    m_reaper.start(5000);
}

SessionBuffer::Settings BufferHub::settingsFor(const QString &client) const
{
    SessionBuffer::Settings settings;
    settings.backend = m_backend.get();
    settings.store = &m_store;
    settings.client = client;
    settings.capacityBytes =
        m_config.bufferBytesPerSession(kAssumedSampleRate, kAssumedChannels);
    settings.upstreamTimeoutMs = m_config.upstreamTimeoutMs;
    settings.statePollMs = m_config.statePollMs;
    settings.journalDir = m_config.journalDir;
    settings.durability = m_config.durability;
    settings.journalKeep = m_config.journalKeep;
    settings.segmentBytes = m_config.segmentBytes;
    return settings;
}

void BufferHub::recoverSessions()
{
    if (m_config.journalDir.trimmed().isEmpty())
        return;

    const QStringList handles = jrn::store::handles(m_config.journalDir);
    if (handles.isEmpty()) {
        LOG_INFO(applog::cat::Session)
            << "journal directory" << m_config.journalDir << "holds no sessions";
        return;
    }
    LOG_INFO(applog::cat::Session)
        << "reading" << handles.size() << "session journal(s) from" << m_config.journalDir;

    for (const QString &handle : handles) {
        jrn::Recovered recovered;
        QString error;
        if (!jrn::store::recover(m_config.journalDir, handle, &recovered, &error)) {
            // A journal we cannot read is left on disk rather than deleted: it
            // may be the only copy of a meeting's audio, and an operator can
            // look at it.  It is skipped on every start until they do.
            LOG_ERROR(applog::cat::Session)
                << "cannot recover" << handle << "-" << error << "- left on disk, skipped";
            continue;
        }
        if (recovered.truncated) {
            // Expected after a crash: the last record was half written.  Worth
            // one line, because it is also what a failing disk looks like.
            LOG_WARN(applog::cat::Session)
                << "journal" << handle << "ends in a torn record - everything before it was read";
        }
        if (recovered.stopped) {
            LOG_INFO(applog::cat::Session)
                << "session" << recovered.meta.sessionId << "had already finished - forgetting it";
            jrn::store::remove(m_config.journalDir, handle);
            continue;
        }
        if (recovered.meta.sessionId.isEmpty()) {
            LOG_WARN(applog::cat::Session) << "journal" << handle << "has no session id - skipped";
            continue;
        }

        auto buffer = std::make_shared<SessionBuffer>(
            recovered, settingsFor(recovered.meta.client));
        {
            QMutexLocker lock(&m_mutex);
            m_sessions.insert(recovered.meta.sessionId, buffer);
        }
        ++m_recoveredCount;
    }

    if (m_recoveredCount > 0) {
        LOG_INFO(applog::cat::Session)
            << "recovered" << m_recoveredCount
            << "session(s) - their backlogs go upstream before anything new";
    }
}

BufferHub::~BufferHub()
{
    shutdown();
    // The probe first: it calls into the backend on every tick, so the backend
    // must outlive it.
    delete m_probe;
    m_probe = nullptr;
    m_backend.reset();
}

grpc::Status BufferHub::startSession(const asr::StartSessionRequest &request,
                                     const QString &client, int timeoutMs,
                                     asr::StartSessionResponse *out)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_shuttingDown) {
            grpc::Status status;
            status.code = grpc::Unavailable;
            status.message = QStringLiteral("máy chủ đệm đang tắt");
            return status;
        }
    }

    Q_UNUSED(timeoutMs);

    QString warning;
    const BackendSessionConfig sessionConfig =
        BackendSessionConfig::fromJson(request.configJson, &warning);
    if (!warning.isEmpty())
        LOG_WARN(applog::cat::Session) << "start_session from" << client << "-" << warning;

    // The id is ours now.  There used to be an adapter with a session registry
    // to borrow one from; there is not any more, and the tier below - Riva or
    // Triton - has no notion of a meeting that outlives a stream.  A uuid keeps
    // it unguessable, which matters because the id is the only thing standing
    // between one workstation and another's transcript.
    const QString sessionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).left(16).remove(QLatin1Char('-'));
    // Triton keys its per-stream state on this, so it must be positive and must
    // not collide with the warm-up ids the model repository reserves.
    const qint64 streamId = qint64(QRandomGenerator::global()->bounded(2, 2000000000));

    auto buffer = std::make_shared<SessionBuffer>(sessionId, streamId, sessionConfig,
                                                  settingsFor(client));
    const grpc::Status status = buffer->openBackend();
    if (!status.ok()) {
        noteUpstream(false, 0.0, status.toString());
        pokeProbe();
        return status;
    }
    noteUpstream(true, 0.0, QString());

    *out = buffer->startResponse();
    {
        QMutexLocker lock(&m_mutex);
        m_sessions.insert(sessionId, buffer);
    }
    LOG_INFO(applog::cat::Session) << "session" << sessionId << "started by" << client << "-"
                                   << m_sessions.size() << "buffered now";
    return status;
}

SessionRef BufferHub::find(const QString &sessionId) const
{
    QMutexLocker lock(&m_mutex);
    return m_sessions.value(sessionId);
}

void BufferHub::forget(const QString &sessionId)
{
    SessionRef dropped;
    {
        QMutexLocker lock(&m_mutex);
        dropped = m_sessions.take(sessionId);
    }
    const QString handle = dropped ? dropped->journalHandle() : QString();
    // Released outside the lock: the last reference joins a thread, and doing
    // that while holding m_mutex would block every other call in the process.
    dropped.reset();
    // Only after the buffer is gone, so its journal is closed before the files
    // are unlinked.  Forgetting a session and leaving its journal behind would
    // resurrect it at the next start.
    if (!handle.isEmpty() && !m_config.journalDir.trimmed().isEmpty())
        jrn::store::remove(m_config.journalDir, handle);
}

void BufferHub::noteUpstream(bool reachable, double latencyMs, const QString &detail)
{
    QMutexLocker lock(&m_mutex);
    const bool was = m_upstream.reachable;
    m_upstream.reachable = reachable;
    m_upstream.latencyMs = latencyMs;
    m_upstream.detail = detail;
    m_upstream.checkedAt = nowSeconds();
    if (reachable)
        m_upstream.consecutiveFailures = 0;
    else
        ++m_upstream.consecutiveFailures;
    if (was != reachable) {
        // A transition is worth a line at info; the steady state is not.
        if (reachable)
            LOG_INFO(applog::cat::Rpc) << "inference tier" << m_upstream.target << "is reachable ("
                                       << latencyMs << "ms)";
        else
            LOG_WARN(applog::cat::Rpc) << "inference tier" << m_upstream.target
                                       << "is unreachable:" << detail
                                       << "- audio keeps being accepted and queued";
    }
}

buf::UpstreamStatus BufferHub::upstreamStatus() const
{
    QMutexLocker lock(&m_mutex);
    return m_upstream;
}

bool BufferHub::upstreamReady() const
{
    QMutexLocker lock(&m_mutex);
    return m_upstream.reachable;
}

void BufferHub::pokeProbe()
{
    if (m_probe)
        m_probe->poke();
}

QList<buf::BufferedSession> BufferHub::snapshots(bool includeFinished, int limit) const
{
    QList<SessionRef> refs;
    {
        QMutexLocker lock(&m_mutex);
        refs = m_sessions.values();
    }
    // Snapshotting takes each session's own locks, so it happens outside ours.
    QList<buf::BufferedSession> out;
    for (const SessionRef &ref : refs) {
        if (!ref)
            continue;
        if (!includeFinished && ref->isFinished())
            continue;
        out.append(ref->snapshot());
        if (limit > 0 && out.size() >= limit)
            break;
    }
    std::sort(out.begin(), out.end(),
              [](const buf::BufferedSession &a, const buf::BufferedSession &b) {
                  return a.startedAt > b.startedAt;
              });
    return out;
}

double BufferHub::uptimeSec() const
{
    return double(m_uptime.elapsed()) / 1000.0;
}

quint64 BufferHub::queueCapacityBytes() const
{
    QMutexLocker lock(&m_mutex);
    const quint64 perSession =
        quint64(m_config.bufferBytesPerSession(kAssumedSampleRate, kAssumedChannels));
    return perSession * quint64(m_sessions.size());
}

quint64 BufferHub::queueUsedBytes() const
{
    QList<SessionRef> refs;
    {
        QMutexLocker lock(&m_mutex);
        refs = m_sessions.values();
    }
    quint64 used = 0;
    for (const SessionRef &ref : refs) {
        if (ref)
            used += ref->snapshot().pendingBytes;
    }
    return used;
}

int BufferHub::sessionCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_sessions.size();
}

void BufferHub::reap()
{
    QList<SessionRef> live;
    {
        QMutexLocker lock(&m_mutex);
        live = m_sessions.values();
    }

    QStringList expired;
    QList<SessionRef> orphans;
    for (const SessionRef &ref : std::as_const(live)) {
        if (!ref)
            continue;
        if (ref->isFinished()) {
            if (ref->finishedSecondsAgo() > double(m_config.finishedRetentionSec))
                expired.append(ref->sessionId());
            continue;
        }
        // A session read back from a journal whose client never reconnected.
        // Its backlog has still been delivered - that happens the moment the
        // forwarder starts - but nobody is going to end it, so it is ended
        // here.  Only recovered sessions qualify: a live client that has gone
        // quiet is a different situation and is left alone.
        if (m_config.orphanTimeoutSec > 0 && ref->awaitingClient()
            && ref->idleSeconds() > double(m_config.orphanTimeoutSec)) {
            orphans.append(ref);
        }
    }

    for (const SessionRef &ref : std::as_const(orphans)) {
        LOG_WARN(applog::cat::Session)
            << "recovered session" << ref->sessionId() << "was never claimed after"
            << m_config.orphanTimeoutSec << "seconds - closing it upstream";
        // Asked for, not waited for.  This runs on the thread that accepts
        // connections; the forwarder does the drain and the stop_session on its
        // own thread, and the next reap tick sees the session finished.
        ref->requestStop();
    }

    for (const QString &id : std::as_const(expired)) {
        LOG_INFO(applog::cat::Session)
            << "forgetting finished session" << id << "after"
            << m_config.finishedRetentionSec << "seconds";
        forget(id);
    }
}

void BufferHub::shutdown()
{
    QList<SessionRef> refs;
    {
        QMutexLocker lock(&m_mutex);
        if (m_shuttingDown)
            return;
        m_shuttingDown = true;
        refs = m_sessions.values();
        m_sessions.clear();
    }
    m_reaper.stop();
    if (m_probe)
        m_probe->requestStop();
    // Ask every forwarder to stop first, then let the references go: two
    // passes, so twenty sessions stop in parallel instead of one after another.
    for (const SessionRef &ref : std::as_const(refs)) {
        if (ref)
            ref->shutdown();
    }
    LOG_INFO(applog::cat::Session) << "stopping" << refs.size() << "buffered sessions";
    refs.clear();
}

QList<asr::SessionSummary> BufferHub::summaries(int limit) const
{
    QList<SessionRef> sessions;
    {
        QMutexLocker lock(&m_mutex);
        sessions = m_sessions.values();
    }
    QList<asr::SessionSummary> out;
    out.reserve(sessions.size());
    for (const SessionRef &session : sessions)
        out.append(session->summary());
    // Newest first: an operator opening the list is looking for what just
    // happened, not for the oldest thing still in memory.
    std::sort(out.begin(), out.end(),
              [](const asr::SessionSummary &a, const asr::SessionSummary &b) {
                  return a.createdAt > b.createdAt;
              });
    if (limit > 0 && out.size() > limit)
        out = out.mid(0, limit);
    return out;
}
