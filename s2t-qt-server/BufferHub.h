// Everything the Server buffer owns that outlives one call: the live sessions,
// the relay pool, and what is currently known about the inference tier.
//
// Sessions are handed out as shared pointers on purpose.  A handler can be
// halfway through a two-minute EnrollSpeaker on one thread while the reaper
// decides on another that a finished session has aged out; a raw pointer there
// is a use-after-free waiting for a busy day.
#ifndef BUFFERHUB_H
#define BUFFERHUB_H

#include "ServerConfig.h"
#include "SessionBuffer.h"
#include "UpstreamPool.h"
#include "proto/BufferAdmin.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLockFile>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWaitCondition>

#include <memory>

using SessionRef = std::shared_ptr<SessionBuffer>;

class BufferHub;

// Reachability of the inference tier, checked on a thread of its own.
//
// It cannot share the relay pool: a probe on the main thread would block the
// accept loop for the length of an upstream timeout, and a probe queued behind
// four busy relay lanes would report a stall that is ours, not the pipeline's.
class UpstreamProbe : public QThread
{
public:
    UpstreamProbe(BufferHub *hub, const QString &target, const QString &token, int periodMs);
    ~UpstreamProbe() override;

    void requestStop();
    // Asks for a probe now instead of at the next tick - used after a relay
    // call fails, so the badge in the client moves at once rather than up to a
    // period later.
    void poke();

protected:
    void run() override;

private:
    BufferHub *m_hub = nullptr;
    QString m_target;
    QString m_token;
    int m_periodMs = 5000;
    QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_stop = false;
};

class BufferHub : public QObject
{
    Q_OBJECT

public:
    explicit BufferHub(const ServerConfig &config, QObject *parent = nullptr);
    ~BufferHub() override;

    const ServerConfig &config() const { return m_config; }
    UpstreamPool &pool() { return *m_pool; }

    // False when the journal directory is already held by another instance.
    // The caller must then refuse to start: two servers replaying one journal
    // would both re-send the same packets and duplicate words in a transcript,
    // which is the worst failure this project has.
    bool ok() const { return m_ok; }
    QString error() const { return m_error; }

    // Relays start_session upstream and, if it succeeds, opens a buffer for the
    // session it created.  The id in the reply is the pipeline's own id: this
    // server never invents one, so a session can be looked up on the adapter by
    // the same string an operator reads off the client.
    grpc::Status startSession(const asr::StartSessionRequest &request, const QString &client,
                              int timeoutMs, asr::StartSessionResponse *out);

    // Null when the id is unknown here.  A session started before this server
    // was restarted is unknown even though the adapter still has it, and that
    // is reported as NOT_FOUND rather than silently relayed - the audio path
    // has to go through a buffer or the counters would lie.
    SessionRef find(const QString &sessionId) const;
    void forget(const QString &sessionId);

    // Called from the probe thread and from failing relays.
    void noteUpstream(bool reachable, double latencyMs, const QString &detail);
    buf::UpstreamStatus upstreamStatus() const;
    bool upstreamReady() const;
    void pokeProbe();

    QList<buf::BufferedSession> snapshots(bool includeFinished, int limit) const;
    double uptimeSec() const;
    quint64 queueCapacityBytes() const;
    quint64 queueUsedBytes() const;
    int sessionCount() const;

    // Stops every forwarder and waits for them.  Called before the gRPC server
    // stops accepting, so a client draining a meeting is not cut off first.
    // Journals are left on disk: that is what the next start reads.
    void shutdown();

    // Sessions read back from disk at startup, for the log line and the tests.
    int recoveredCount() const { return m_recoveredCount; }

private slots:
    void reap();

private:
    // Reads every journal in the configured directory back into a live session.
    // Runs in the constructor, before the gRPC server starts listening, so a
    // client that reconnects the instant the port opens finds its meeting
    // already there.
    void recoverSessions();
    SessionBuffer::Settings settingsFor(const QString &client) const;

    ServerConfig m_config;
    int m_recoveredCount = 0;
    bool m_ok = true;
    QString m_error;
    // Held for the life of the process.  QLockFile stores the pid and clears a
    // lock whose owner is gone, so a SIGKILL does not leave a stale lock that
    // blocks the very restart this journal exists to make possible.
    std::unique_ptr<QLockFile> m_lock;
    UpstreamPool *m_pool = nullptr;
    UpstreamProbe *m_probe = nullptr;

    mutable QMutex m_mutex;
    QHash<QString, SessionRef> m_sessions;
    buf::UpstreamStatus m_upstream;
    QTimer m_reaper;
    QElapsedTimer m_uptime;
    bool m_shuttingDown = false;
};

#endif // BUFFERHUB_H
