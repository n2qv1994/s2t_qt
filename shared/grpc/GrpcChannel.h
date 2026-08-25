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

class ClientStream;

class Channel
{
    friend class ClientStream;

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
    // The header block every call on this channel sends.  Shared by invoke()
    // and ClientStream so a token or a content-type can never be right on one
    // path and wrong on the other.
    QList<hpack::Header> callHeaders(const QString &fullMethod, int timeoutMs) const;

    QString m_target;
    QString m_host;
    quint16 m_port = 0;
    QByteArray m_authority;
    QByteArray m_bearer;
    http2::Http2Connection m_connection;
};

// One streaming RPC on a Channel.
//
// Bidirectional by construction, because that is the shape both backends need:
// Riva's StreamingRecognize takes audio for the length of a meeting and answers
// as it goes, and Triton's ModelStreamInfer does the same.  A server-streaming
// call is this with closeSend() called right after start().
//
// The channel carries one stream at a time and the stream borrows it, so a
// ClientStream must not outlive its Channel and no unary call may be made on
// that channel while a stream is open.  Both hold for the intended use: a
// SessionBuffer owns one lane, one channel and one meeting.
class ClientStream
{
public:
    explicit ClientStream(Channel *channel) : m_channel(channel) {}
    ~ClientStream();

    ClientStream(const ClientStream &) = delete;
    ClientStream &operator=(const ClientStream &) = delete;

    // Opens the stream. `timeoutMs` is the deadline for the *whole* call, sent
    // as grpc-timeout; pass 0 for a meeting-length stream with no deadline.
    Status start(const QString &fullMethod, int timeoutMs);
    // Sends one message. Blocks only for flow control.
    Status send(const QByteArray &message, int timeoutMs);
    // Half-closes: no more messages will be sent. The far side answers what is
    // still outstanding and then ends the stream.
    Status closeSend(int timeoutMs);

    // Pulls the next complete message. `*have` false with an OK status means
    // "nothing yet" - ended() tells that apart from "nothing ever again".
    // waitMs == 0 polls, which is what lets one thread push audio and collect
    // results without either starving the other.
    Status receive(QByteArray *message, bool *have, int waitMs);

    bool active() const { return m_active; }
    bool ended() const;

    // The trailing grpc-status. Only meaningful once ended() is true; calling
    // it earlier reports FAILED_PRECONDITION rather than guessing OK.
    Status finish();
    // Tears the stream down without waiting for the far side.
    void cancel();

private:
    Status headerStatus() const;

    Channel *m_channel = nullptr;
    bool m_active = false;
    bool m_checkedHeaders = false;
};

} // namespace grpc

#endif // GRPCCHANNEL_H
