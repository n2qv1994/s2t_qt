#include "BufferHub.h"

#include "core/Logger.h"

#include <QDateTime>

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

UpstreamProbe::UpstreamProbe(BufferHub *hub, const QString &target, const QString &token,
                             int periodMs)
    : m_hub(hub), m_target(target), m_token(token), m_periodMs(periodMs)
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
    // Its own client, created on this thread - the socket underneath has
    // thread affinity.
    AsrClient client(m_target, m_token);
    for (;;) {
        {
            QMutexLocker lock(&m_mutex);
            if (m_stop)
                break;
        }
        double latencyMs = 0.0;
        // A transport-level probe only.  It says the pipeline is reachable,
        // not that a token is accepted - the same meaning /api/server_status
        // had, and the same meaning the client's own badge carries.
        const grpc::Status status = client.ping(3000, &latencyMs);
        m_hub->noteUpstream(status.ok(), latencyMs, status.ok() ? QString() : status.toString());
        if (!status.ok())
            client.reset();
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
    m_pool = new UpstreamPool(config.upstreamLanes, config.upstreamTarget, config.upstreamToken);
    m_probe = new UpstreamProbe(this, config.upstreamTarget, config.upstreamToken,
                                config.upstreamProbeMs);
    m_probe->start();

    connect(&m_reaper, &QTimer::timeout, this, &BufferHub::reap);
    m_reaper.start(5000);
}

BufferHub::~BufferHub()
{
    shutdown();
    delete m_probe;
    m_probe = nullptr;
    delete m_pool;
    m_pool = nullptr;
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

    // Relayed through the pool rather than through a fresh channel: a start is
    // request-scoped, and the session's own channels do not exist yet.
    const grpc::Status status = m_pool->call([&](AsrClient &upstream) {
        return upstream.startSession(request, out, timeoutMs);
    });
    if (!status.ok()) {
        noteUpstream(false, 0.0, status.toString());
        pokeProbe();
        return status;
    }
    if (out->sessionId.isEmpty()) {
        grpc::Status bad;
        bad.code = grpc::Internal;
        bad.message = QStringLiteral("tầng suy luận trả về phiên không có mã");
        return bad;
    }
    noteUpstream(true, 0.0, QString());

    SessionBuffer::Settings settings;
    settings.upstreamTarget = m_config.upstreamTarget;
    settings.upstreamToken = m_config.upstreamToken;
    settings.client = client;
    settings.capacityBytes =
        m_config.bufferBytesPerSession(kAssumedSampleRate, kAssumedChannels);
    settings.upstreamTimeoutMs = m_config.upstreamTimeoutMs;
    settings.statePollMs = m_config.statePollMs;
    settings.spoolDir = m_config.spoolDir;

    auto buffer = std::make_shared<SessionBuffer>(out->sessionId, *out, settings);
    {
        QMutexLocker lock(&m_mutex);
        m_sessions.insert(out->sessionId, buffer);
    }
    LOG_INFO(applog::cat::Session) << "session" << out->sessionId << "started by" << client << "-"
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
    // Released outside the lock: the last reference joins a thread, and doing
    // that while holding m_mutex would block every other call in the process.
    dropped.reset();
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
    QStringList expired;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it) {
            const SessionRef &ref = it.value();
            if (!ref)
                continue;
            if (ref->isFinished()
                && ref->finishedSecondsAgo() > double(m_config.finishedRetentionSec)) {
                expired.append(it.key());
            }
        }
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
