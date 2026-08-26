// Everything the Server buffer owns that outlives one call: the live sessions,
// the inference backend, and what is currently known about that tier.
//
// Sessions are handed out as shared pointers on purpose.  A handler can be
// halfway through a two-minute EnrollSpeaker on one thread while the reaper
// decides on another that a finished session has aged out; a raw pointer there
// is a use-after-free waiting for a busy day.
#ifndef BUFFERHUB_H
#define BUFFERHUB_H

#include "ServerConfig.h"
#include "SessionBuffer.h"
#include "SessionStore.h"
#include "backend/InferenceBackend.h"
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
// It needs a thread of its own: a probe on the main thread would block the
// accept loop for the length of an upstream timeout, and a probe queued behind
// a busy meeting would report a stall that is ours, not the tier's.
class UpstreamProbe : public QThread
{
public:
    UpstreamProbe(BufferHub *hub, int periodMs);
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
    // The inference tier this server is pointed at.  Never null once ok().
    InferenceBackend &backend() { return *m_backend; }
    // The meeting archive.  Always present; disabled when database/dir is empty,
    // in which case every read answers empty and every write is a no-op.
    SessionStore &store() { return m_store; }

    // False when the journal directory is already held by another instance.
    // The caller must then refuse to start: two servers replaying one journal
    // would both re-send the same packets and duplicate words in a transcript,
    // which is the worst failure this project has.
    bool ok() const { return m_ok; }
    QString error() const { return m_error; }

    // Opens a meeting: mints its id, opens a backend session for it and starts
    // the forwarder.  Since 2026-08-25 the id is this server's own - the tier
    // below has no session registry to borrow one from, and this process is now
    // the thing that owns a meeting.
    grpc::Status startSession(const asr::StartSessionRequest &request, const QString &client,
                              int timeoutMs, asr::StartSessionResponse *out);

    // Null when the id is unknown here.  A meeting this server never had, or one
    // whose retention has expired, is reported as NOT_FOUND rather than quietly
    // recreated: the audio path has to go through a buffer or the counters lie.
    SessionRef find(const QString &sessionId) const;
    void forget(const QString &sessionId);

    // Called from the probe thread and from a failing backend call.
    void noteUpstream(bool reachable, double latencyMs, const QString &detail);
    buf::UpstreamStatus upstreamStatus() const;
    bool upstreamReady() const;
    void pokeProbe();

    QList<buf::BufferedSession> snapshots(bool includeFinished, int limit) const;
    // What list_sessions answers with: every meeting this process is still
    // holding, newest first.  There is no archive behind it - a meeting that
    // has aged past finished_retention_sec is simply gone.
    QList<asr::SessionSummary> summaries(int limit) const;
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
    std::unique_ptr<InferenceBackend> m_backend;
    // Mutable for the same reason m_mutex is: it is shared state that a const
    // accessor legitimately hands out for writing.  The archive is not part of
    // the hub's logical constness.
    mutable SessionStore m_store;
    UpstreamProbe *m_probe = nullptr;

    mutable QMutex m_mutex;
    QHash<QString, SessionRef> m_sessions;
    buf::UpstreamStatus m_upstream;
    QTimer m_reaper;
    QElapsedTimer m_uptime;
    bool m_shuttingDown = false;
};

#endif // BUFFERHUB_H
