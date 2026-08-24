#include "GrpcChannel.h"

#include "core/Logger.h"

#include <QElapsedTimer>

namespace grpc {
namespace {

QByteArray headerValue(const QList<hpack::Header> &headers, const char *name)
{
    for (const hpack::Header &header : headers) {
        if (header.name == name)
            return header.value;
    }
    return QByteArray();
}

bool hasHeader(const QList<hpack::Header> &headers, const char *name)
{
    for (const hpack::Header &header : headers) {
        if (header.name == name)
            return true;
    }
    return false;
}

// grpc-message is percent-encoded (gRPC over HTTP/2 spec, "Status-Message"):
// anything outside %x20-%x7E, plus '%' itself, arrives as %XX.  Decoding it is
// what turns "edit_range_not_committed" style adapter errors and Vietnamese
// validation messages back into something a person can read.
QString decodeGrpcMessage(const QByteArray &raw)
{
    QByteArray out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const char ch = raw.at(i);
        if (ch == '%' && i + 2 < raw.size()) {
            bool ok = false;
            const int value = raw.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) {
                out.append(char(quint8(value)));
                i += 2;
                continue;
            }
        }
        out.append(ch);
    }
    return QString::fromUtf8(out);
}

// Every RPC in the client goes through Channel::invoke, so one line emitted
// here covers all twenty-odd call sites without touching any of them.  It is
// bound to the caller's `status` by reference and logs from its destructor,
// which is what makes it correct on every one of invoke()'s early returns.
class CallLog
{
public:
    CallLog(const QString &method, int requestBytes, int timeoutMs, const Status &status,
            const QByteArray *response)
        : m_method(method), m_requestBytes(requestBytes), m_timeoutMs(timeoutMs),
          m_status(status), m_response(response)
    {
        m_clock.start();
    }

    CallLog(const CallLog &) = delete;
    CallLog &operator=(const CallLog &) = delete;

    ~CallLog()
    {
        // push_audio runs six times a second all session long; at debug it
        // would drown out everything else, so only a failure is loud.
        const bool hot = m_method.endsWith(QLatin1String("/push_audio"));
        const applog::Level level = m_status.ok()
            ? (hot ? applog::Level::Trace : applog::Level::Debug)
            : applog::Level::Warn;
        if (!applog::isEnabled(level))
            return;
        applog::Record record(level, applog::cat::Grpc, __FILE__, __LINE__);
        record.stream() << m_method << "->" << m_status.toString() << "|"
                        << double(m_clock.nsecsElapsed()) / 1e6 << "ms | sent" << m_requestBytes
                        << "bytes, received" << (m_response ? m_response->size() : 0)
                        << "bytes, deadline" << m_timeoutMs << "ms";
    }

private:
    QString m_method;
    int m_requestBytes;
    int m_timeoutMs;
    const Status &m_status;
    const QByteArray *m_response;
    QElapsedTimer m_clock;
};

int statusFromTransport(const QString &error)
{
    // A deadline that expired locally must surface as DEADLINE_EXCEEDED, not
    // UNAVAILABLE: both are retryable here, but the distinction is what the
    // latency panel and the reconnect message show the operator.
    if (error.contains(QStringLiteral("timed out")))
        return DeadlineExceeded;
    return Unavailable;
}

} // namespace

QString Status::codeName() const
{
    switch (code) {
    case Ok: return QStringLiteral("OK");
    case Cancelled: return QStringLiteral("CANCELLED");
    case Unknown: return QStringLiteral("UNKNOWN");
    case InvalidArgument: return QStringLiteral("INVALID_ARGUMENT");
    case DeadlineExceeded: return QStringLiteral("DEADLINE_EXCEEDED");
    case NotFound: return QStringLiteral("NOT_FOUND");
    case AlreadyExists: return QStringLiteral("ALREADY_EXISTS");
    case PermissionDenied: return QStringLiteral("PERMISSION_DENIED");
    case ResourceExhausted: return QStringLiteral("RESOURCE_EXHAUSTED");
    case FailedPrecondition: return QStringLiteral("FAILED_PRECONDITION");
    case Aborted: return QStringLiteral("ABORTED");
    case OutOfRange: return QStringLiteral("OUT_OF_RANGE");
    case Unimplemented: return QStringLiteral("UNIMPLEMENTED");
    case Internal: return QStringLiteral("INTERNAL");
    case Unavailable: return QStringLiteral("UNAVAILABLE");
    case DataLoss: return QStringLiteral("DATA_LOSS");
    case Unauthenticated: return QStringLiteral("UNAUTHENTICATED");
    default: return QStringLiteral("CODE_%1").arg(code);
    }
}

QString Status::toString() const
{
    if (ok())
        return QStringLiteral("OK");
    return message.isEmpty() ? codeName() : QStringLiteral("%1: %2").arg(codeName(), message);
}

Channel::Channel(const QString &target, const QString &token)
{
    setTarget(target, token);
}

Channel::~Channel() = default;

void Channel::setTarget(const QString &target, const QString &token)
{
    m_connection.close();
    m_target = target.trimmed();
    m_authority = m_target.toUtf8();
    const int colon = m_target.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        m_host = m_target.left(colon);
        m_port = quint16(m_target.mid(colon + 1).toUShort());
    } else {
        m_host = m_target;
        m_port = 8700;
    }
    m_bearer = token.trimmed().isEmpty() ? QByteArray()
                                         : QByteArray("Bearer ") + token.trimmed().toUtf8();
    LOG_DEBUG(applog::cat::Grpc) << "channel pointed at"
                                 << QStringLiteral("%1:%2").arg(m_host).arg(m_port)
                                 << (m_bearer.isEmpty() ? "(no token)" : "(bearer token set)");
}

void Channel::reset()
{
    if (m_connection.isOpen())
        LOG_DEBUG(applog::cat::Grpc) << "dropping the connection to" << m_target
                                     << "- the next call will dial again";
    m_connection.close();
}

bool Channel::ensureOpen(int timeoutMs, Status *status)
{
    if (m_connection.isOpen())
        return true;
    QString error;
    QElapsedTimer clock;
    clock.start();
    if (!m_connection.open(m_host, m_port, timeoutMs, &error)) {
        LOG_WARN(applog::cat::Grpc) << "opening a connection to" << m_target << "failed after"
                                    << clock.elapsed() << "ms:" << error;
        status->code = Unavailable;
        status->message = error;
        return false;
    }
    LOG_INFO(applog::cat::Grpc) << "HTTP/2 connection to" << m_target << "opened in"
                                << clock.elapsed() << "ms";
    return true;
}

Status Channel::ping(int timeoutMs, double *latencyMs)
{
    QElapsedTimer clock;
    clock.start();
    Status status;
    const bool opened = ensureOpen(timeoutMs, &status);
    if (latencyMs)
        *latencyMs = double(clock.nsecsElapsed()) / 1e6;
    if (!opened)
        return status;
    return Status();
}

Status Channel::invoke(const QString &fullMethod, const QByteArray &request, int timeoutMs,
                       QByteArray *response)
{
    Status status;
    const CallLog callLog(fullMethod, request.size(), timeoutMs, status, response);
    if (!ensureOpen(timeoutMs, &status))
        return status;

    QList<hpack::Header> headers;
    headers.append({QByteArrayLiteral(":method"), QByteArrayLiteral("POST")});
    headers.append({QByteArrayLiteral(":scheme"), QByteArrayLiteral("http")});
    headers.append({QByteArrayLiteral(":path"), fullMethod.toUtf8()});
    headers.append({QByteArrayLiteral(":authority"), m_authority});
    headers.append({QByteArrayLiteral("content-type"), QByteArrayLiteral("application/grpc+proto")});
    headers.append({QByteArrayLiteral("user-agent"), QByteArrayLiteral("s2t-qt/1.0 grpc-qt/1.0")});
    // `te: trailers` is mandatory: it is how the server knows this client can
    // read the trailing grpc-status instead of needing it faked into headers.
    headers.append({QByteArrayLiteral("te"), QByteArrayLiteral("trailers")});
    headers.append({QByteArrayLiteral("grpc-encoding"), QByteArrayLiteral("identity")});
    headers.append({QByteArrayLiteral("grpc-accept-encoding"), QByteArrayLiteral("identity")});
    if (timeoutMs > 0) {
        headers.append({QByteArrayLiteral("grpc-timeout"),
                        QByteArray::number(timeoutMs) + QByteArrayLiteral("m")});
    }
    if (!m_bearer.isEmpty())
        headers.append({QByteArrayLiteral("authorization"), m_bearer});

    // Length-prefixed message: one byte of compression flag, four bytes of
    // big-endian length, then the serialized proto.
    QByteArray body;
    body.reserve(request.size() + 5);
    body.append(char(0));
    body.append(char((request.size() >> 24) & 0xff));
    body.append(char((request.size() >> 16) & 0xff));
    body.append(char((request.size() >> 8) & 0xff));
    body.append(char(request.size() & 0xff));
    body.append(request);

    http2::Response result;
    QString error;
    // Give the transport a little more headroom than the gRPC deadline we
    // advertised, so a server that honours grpc-timeout gets to answer with a
    // real DEADLINE_EXCEEDED status rather than us tearing the socket down
    // first and reporting it as a connection failure.
    const int transportTimeout = timeoutMs > 0 ? timeoutMs + 2000 : 0;
    if (!m_connection.request(headers, body, transportTimeout, &result, &error)) {
        m_connection.close();
        status.code = statusFromTransport(error);
        status.message = error;
        return status;
    }

    const QByteArray httpStatus = headerValue(result.headers, ":status");
    if (httpStatus != "200") {
        m_connection.close();
        status.code = Unavailable;
        status.message = QStringLiteral("HTTP %1 from AI server")
                             .arg(QString::fromUtf8(httpStatus.isEmpty() ? QByteArrayLiteral("?")
                                                                         : httpStatus));
        return status;
    }

    // Trailers-only responses (the common shape for an error) carry the status
    // in the initial HEADERS block; a normal response carries it in trailers.
    QByteArray statusRaw = headerValue(result.trailers, "grpc-status");
    QByteArray messageRaw = headerValue(result.trailers, "grpc-message");
    if (statusRaw.isEmpty() && hasHeader(result.headers, "grpc-status")) {
        statusRaw = headerValue(result.headers, "grpc-status");
        messageRaw = headerValue(result.headers, "grpc-message");
    }
    if (!statusRaw.isEmpty()) {
        bool ok = false;
        const int code = statusRaw.toInt(&ok);
        if (ok && code != Ok) {
            status.code = code;
            status.message = decodeGrpcMessage(messageRaw);
            return status;
        }
        if (!ok) {
            status.code = Internal;
            status.message = QStringLiteral("unparseable grpc-status '%1'")
                                 .arg(QString::fromUtf8(statusRaw));
            return status;
        }
    }

    if (result.body.isEmpty()) {
        // OK with no message is legal for a server-streaming call but never
        // for these unary ones; treat it as a protocol failure rather than
        // handing the caller a default-constructed reply as if it were real.
        status.code = Internal;
        status.message = QStringLiteral("server returned no message body");
        return status;
    }
    if (result.body.size() < 5) {
        status.code = Internal;
        status.message = QStringLiteral("truncated gRPC frame");
        return status;
    }
    const quint8 compressed = quint8(result.body.at(0));
    const quint32 length = (quint32(quint8(result.body.at(1))) << 24)
        | (quint32(quint8(result.body.at(2))) << 16)
        | (quint32(quint8(result.body.at(3))) << 8) | quint32(quint8(result.body.at(4)));
    if (compressed) {
        // We advertise grpc-accept-encoding: identity, so this cannot happen
        // against a conforming server - and silently handing compressed bytes
        // to the proto parser would look like corruption instead.
        status.code = Internal;
        status.message = QStringLiteral("server compressed the response despite identity encoding");
        return status;
    }
    if (quint32(result.body.size() - 5) < length) {
        status.code = Internal;
        status.message = QStringLiteral("gRPC frame shorter than its declared length");
        return status;
    }
    *response = result.body.mid(5, int(length));
    return status;
}

} // namespace grpc
