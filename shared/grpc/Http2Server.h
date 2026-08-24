// Minimal blocking HTTP/2 *server*, just enough to carry gRPC unary calls.
//
// The mirror image of Http2Client, and deliberately built the same way: one
// TCP connection == one thread == one blocking loop.  The reason is the same
// too - every call this server answers is unary, so a thread that owns the
// socket for the length of one request/response is simpler than a callback
// state machine and costs nothing while the connection is idle.
//
// The client opens a handful of long-lived connections per workstation (audio
// push, state poll, three RPC lanes), so the thread count tracks operators,
// not requests.  It is bounded anyway: past maxConnections() a new connection
// is refused with GOAWAY(REFUSED_STREAM) rather than accepted and starved.
//
// Handlers run on the connection thread and may run concurrently with each
// other.  Anything they touch has to be thread-safe.
#ifndef HTTP2SERVER_H
#define HTTP2SERVER_H

#include "Hpack.h"

#include <QAtomicInt>
#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QTcpServer>

#include <functional>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace http2 {

// Refuse to accumulate an unbounded request.  The biggest thing a client ever
// sends is an enrolment WAV; gRPC's own default cap is 4 MiB and this leaves
// an order of magnitude of headroom while still bounding a hostile peer.
const int kMaxRequestBytes = 64 * 1024 * 1024;

struct ServerRequest
{
    QList<hpack::Header> headers;
    QByteArray body;
    // Filled in by the connection so a handler can log or rate-limit without
    // reaching back into the socket.
    QString peer;
    quint32 streamId = 0;

    QByteArray header(const char *name) const;
    QString path() const { return QString::fromUtf8(header(":path")); }
    QString method() const { return QString::fromUtf8(header(":method")); }
};

struct ServerResponse
{
    // Response headers *excluding* :status, which the connection always sets
    // to 200 - gRPC carries its own status in the trailers, and a non-200
    // would only make a conforming client report a transport failure instead.
    QList<hpack::Header> headers;
    QByteArray body;
    QList<hpack::Header> trailers;
    // True for the trailers-only shape: one HEADERS frame carrying both the
    // response headers and the status, with no DATA at all.  That is what a
    // gRPC server sends for an error, and what Http2Client already knows how
    // to read out of the initial block.
    bool trailersOnly = false;
};

class Http2Server : public QTcpServer
{
    Q_OBJECT

public:
    using Handler = std::function<void(const ServerRequest &, ServerResponse *)>;

    explicit Http2Server(QObject *parent = nullptr);
    ~Http2Server() override;

    // Called from every connection thread, so it must be set before start()
    // and not changed afterwards.
    void setHandler(Handler handler);

    // Past this many live connections a new one is accepted only far enough
    // to say GOAWAY, so an exhausted server refuses out loud instead of
    // letting connections pile up behind a thread that will never come.
    void setMaxConnections(int limit);
    int maxConnections() const { return m_maxConnections; }

    // How long a connection may sit with no bytes at all before it is closed.
    // The state poller speaks every 200 ms, but an RPC lane can genuinely be
    // idle for the length of a meeting, so this is generous by default.
    void setIdleTimeoutMs(int ms) { m_idleTimeoutMs = ms; }

    bool start(const QHostAddress &address, quint16 port, QString *error);
    // Stops listening, asks every live connection to finish, and waits for
    // their threads.  Safe to call twice.
    void stop();

    int activeConnections() const;
    quint64 totalConnections() const;
    quint64 totalRequests() const;

protected:
    void incomingConnection(qintptr descriptor) override;

private:
    friend class ServerConnection;

    void noteRequest();

    Handler m_handler;
    mutable QMutex m_mutex;
    QSet<QThread *> m_threads;
    QAtomicInt m_stopping{0};
    int m_maxConnections = 128;
    int m_idleTimeoutMs = 300000;
    quint64 m_totalConnections = 0;
    quint64 m_totalRequests = 0;
    int m_nextConnectionId = 0;
};

} // namespace http2

#endif // HTTP2SERVER_H
