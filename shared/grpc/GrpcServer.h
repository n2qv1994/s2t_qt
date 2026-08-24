// gRPC unary server on top of the minimal HTTP/2 server.
//
// The mirror of grpc::Channel: that one turns a method path plus a serialized
// message into a request and reads a status back; this one turns a request
// back into a method path plus a message, calls a handler, and writes the
// status out - including the percent-encoded grpc-message the client already
// knows how to decode.
//
// Handlers run on the connection threads and may run concurrently.  Register
// them all before start(); the table is not locked, because a lock on every
// call to protect a map nobody writes to would be pure cost.
#ifndef GRPCSERVER_H
#define GRPCSERVER_H

#include "GrpcChannel.h"
#include "Http2Server.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <functional>

namespace grpc {

struct ServerCall
{
    // "/asr.ui.v1.ProductASRService/push_audio", exactly as the .proto spells it.
    QString method;
    // The request message, already unwrapped from its 5-byte length prefix.
    QByteArray message;
    QString peer;
    // From grpc-timeout, in milliseconds; 0 when the client sent none.  A
    // proxying handler passes this on rather than inventing its own deadline,
    // so a client that gave up is not still being waited for upstream.
    int deadlineMs = 0;
    // Every request header, so a handler can read call metadata the service
    // definition does not model - the client id this server tags sessions with,
    // for one.
    QList<hpack::Header> headers;

    QByteArray metadata(const char *name) const;
};

class Server : public QObject
{
    Q_OBJECT

public:
    using MethodHandler = std::function<Status(const ServerCall &, QByteArray *response)>;

    explicit Server(QObject *parent = nullptr);
    ~Server() override;

    // Bearer token every call must present.  Empty means the check is off,
    // which is what the adapter does when it is started without one - and it
    // is logged loudly at startup rather than left to be discovered.
    void setToken(const QString &token);
    void setMaxConnections(int limit);
    void setIdleTimeoutMs(int ms);

    void registerMethod(const QString &fullMethod, MethodHandler handler);
    QStringList methods() const { return m_methods.keys(); }

    bool start(const QHostAddress &address, quint16 port, QString *error);
    void stop();

    quint16 port() const { return m_http2.serverPort(); }
    bool isRunning() const { return m_http2.isListening(); }
    int activeConnections() const { return m_http2.activeConnections(); }
    quint64 totalConnections() const { return m_http2.totalConnections(); }
    quint64 totalRequests() const { return m_http2.totalRequests(); }
    quint64 rejectedCalls() const;

    // Exposed because the wire format is worth being able to test directly:
    // one byte of compression flag, four of big-endian length, then the proto.
    static QByteArray frameMessage(const QByteArray &message);
    static bool unframeMessage(const QByteArray &body, QByteArray *message, QString *error);
    // grpc-message is percent-encoded (gRPC over HTTP/2, "Status-Message"):
    // everything outside %x20-%x7E, plus '%' itself, goes out as %XX.  Without
    // it a Vietnamese error message reaches the client as mojibake.
    static QByteArray encodeGrpcMessage(const QString &message);

private:
    void dispatch(const http2::ServerRequest &request, http2::ServerResponse *response);
    static void writeStatus(http2::ServerResponse *response, const Status &status);

    http2::Http2Server m_http2;
    QHash<QString, MethodHandler> m_methods;
    QByteArray m_bearer;
    QAtomicInteger<quint64> m_rejected{0};
};

} // namespace grpc

#endif // GRPCSERVER_H
