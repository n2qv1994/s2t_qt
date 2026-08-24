// gRPC unary calls over the minimal HTTP/2 client.
//
// One Channel == one TCP connection == one owning thread, mirroring how the
// Python bridge kept the audio push, the state poll and the request-scoped
// RPCs on separate channels so a large state serialization could never queue
// behind (or in front of) a 160 ms audio packet.
#ifndef GRPCCHANNEL_H
#define GRPCCHANNEL_H

#include "Http2Client.h"

#include <QByteArray>
#include <QString>

namespace grpc {

enum StatusCode {
    Ok = 0,
    Cancelled = 1,
    Unknown = 2,
    InvalidArgument = 3,
    DeadlineExceeded = 4,
    NotFound = 5,
    AlreadyExists = 6,
    PermissionDenied = 7,
    ResourceExhausted = 8,
    FailedPrecondition = 9,
    Aborted = 10,
    OutOfRange = 11,
    Unimplemented = 12,
    Internal = 13,
    Unavailable = 14,
    DataLoss = 15,
    Unauthenticated = 16,
};

struct Status
{
    int code = Ok;
    QString message;

    bool ok() const { return code == Ok; }
    QString codeName() const;

    // The only three codes it is safe to retry a stateful push_audio on.
    // INTERNAL is deliberately excluded: the adapter returns it precisely when
    // the server may already have consumed the audio, and a blind retry there
    // duplicates words - see grpc_session_adapter.ProductASRService._abort.
    bool isTransport() const
    {
        return code == Unavailable || code == DeadlineExceeded || code == Cancelled;
    }

    QString toString() const;
};

class Channel
{
public:
    // `target` is "host:port"; `token` goes out as `authorization: Bearer ...`
    // on every call, and may be empty when the adapter runs without a token.
    Channel(const QString &target, const QString &token);
    ~Channel();

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    void setTarget(const QString &target, const QString &token);
    QString target() const { return m_target; }

    // fullMethod is "/package.Service/method" exactly as it appears in the
    // .proto, e.g. "/asr.ui.v1.ProductASRService/push_audio".
    Status invoke(const QString &fullMethod, const QByteArray &request, int timeoutMs,
                  QByteArray *response);

    // Drops the TCP connection so the next invoke() dials again.  The audio
    // sender calls this after a transport failure instead of waiting out
    // HTTP/2's own reconnect behaviour, exactly as the Python bridge replaced
    // its grpc.Channel rather than retrying on the stale one.
    void reset();

    // Transport-level reachability probe backing the "AI connected" badge.
    // Says nothing about whether the token is valid - only that a channel can
    // be established, same meaning /api/server_status had.
    Status ping(int timeoutMs, double *latencyMs);

private:
    bool ensureOpen(int timeoutMs, Status *status);

    QString m_target;
    QString m_host;
    quint16 m_port = 0;
    QByteArray m_authority;
    QByteArray m_bearer;
    http2::Http2Connection m_connection;
};

} // namespace grpc

#endif // GRPCCHANNEL_H
