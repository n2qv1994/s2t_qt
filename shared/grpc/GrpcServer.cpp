#include "GrpcServer.h"

#include "core/Logger.h"

#include <QElapsedTimer>

namespace grpc {
namespace {

// grpc-timeout is a decimal count followed by a one-character unit
// (RFC: H hours, M minutes, S seconds, m milliseconds, u microseconds,
// n nanoseconds).  Anything finer than a millisecond rounds up to one, since
// zero would read as "no deadline" and mean the opposite.
int parseGrpcTimeout(const QByteArray &raw)
{
    if (raw.size() < 2)
        return 0;
    bool ok = false;
    const qint64 value = raw.left(raw.size() - 1).toLongLong(&ok);
    if (!ok || value < 0)
        return 0;
    const char unit = raw.at(raw.size() - 1);
    qint64 ms = 0;
    switch (unit) {
    case 'H': ms = value * 3600000; break;
    case 'M': ms = value * 60000; break;
    case 'S': ms = value * 1000; break;
    case 'm': ms = value; break;
    case 'u': ms = (value + 999) / 1000; break;
    case 'n': ms = (value + 999999) / 1000000; break;
    default: return 0;
    }
    if (ms <= 0)
        return value > 0 ? 1 : 0;
    return ms > 24 * 3600 * 1000 ? 24 * 3600 * 1000 : int(ms);
}

// One line per call, from the destructor, so it is emitted on every one of
// dispatch()'s early returns without a return statement having to remember.
class CallLog
{
public:
    CallLog(const QString &method, const QString &peer, int requestBytes, const Status &status,
            const QByteArray *response)
        : m_method(method), m_peer(peer), m_requestBytes(requestBytes), m_status(status),
          m_response(response)
    {
        m_clock.start();
    }

    CallLog(const CallLog &) = delete;
    CallLog &operator=(const CallLog &) = delete;

    ~CallLog()
    {
        // push_audio arrives six times a second per client; at debug it would
        // drown out everything else, so only a failure is loud.
        const bool hot = m_method.endsWith(QLatin1String("/push_audio"));
        const applog::Level level = m_status.ok()
            ? (hot ? applog::Level::Trace : applog::Level::Debug)
            : applog::Level::Warn;
        if (!applog::isEnabled(level))
            return;
        applog::Record record(level, applog::cat::Grpc, __FILE__, __LINE__);
        record.stream() << m_peer << m_method << "->" << m_status.toString() << "|"
                        << double(m_clock.nsecsElapsed()) / 1e6 << "ms | received"
                        << m_requestBytes << "bytes, sent"
                        << (m_response ? m_response->size() : 0) << "bytes";
    }

private:
    QString m_method;
    QString m_peer;
    int m_requestBytes;
    const Status &m_status;
    const QByteArray *m_response;
    QElapsedTimer m_clock;
};

} // namespace

QByteArray ServerCall::metadata(const char *name) const
{
    for (const hpack::Header &h : headers) {
        if (h.name == name)
            return h.value;
    }
    return QByteArray();
}

Server::Server(QObject *parent) : QObject(parent)
{
    m_http2.setHandler([this](const http2::ServerRequest &request, http2::ServerResponse *response) {
        dispatch(request, response);
    });
}

Server::~Server()
{
    stop();
}

void Server::setToken(const QString &token)
{
    const QString trimmed = token.trimmed();
    m_bearer = trimmed.isEmpty() ? QByteArray() : QByteArray("Bearer ") + trimmed.toUtf8();
}

void Server::setMaxConnections(int limit)
{
    m_http2.setMaxConnections(limit);
}

void Server::setIdleTimeoutMs(int ms)
{
    m_http2.setIdleTimeoutMs(ms);
}

void Server::registerMethod(const QString &fullMethod, MethodHandler handler)
{
    m_methods.insert(fullMethod, std::move(handler));
}

bool Server::start(const QHostAddress &address, quint16 port, QString *error)
{
    if (m_methods.isEmpty()) {
        *error = QStringLiteral("no RPC methods are registered");
        return false;
    }
    if (m_bearer.isEmpty()) {
        LOG_WARN(applog::cat::Grpc)
            << "no API token configured - every caller that can reach this port is accepted";
    }
    return m_http2.start(address, port, error);
}

void Server::stop()
{
    m_http2.stop();
}

quint64 Server::rejectedCalls() const
{
    return m_rejected.loadRelaxed();
}

QByteArray Server::frameMessage(const QByteArray &message)
{
    QByteArray out;
    out.reserve(message.size() + 5);
    out.append(char(0));
    out.append(char((message.size() >> 24) & 0xff));
    out.append(char((message.size() >> 16) & 0xff));
    out.append(char((message.size() >> 8) & 0xff));
    out.append(char(message.size() & 0xff));
    out.append(message);
    return out;
}

bool Server::unframeMessage(const QByteArray &body, QByteArray *message, QString *error)
{
    if (body.isEmpty()) {
        // Legal for a client-streaming call with no messages, never for the
        // unary ones this server carries.
        *error = QStringLiteral("request had no message body");
        return false;
    }
    if (body.size() < 5) {
        *error = QStringLiteral("truncated gRPC frame");
        return false;
    }
    if (quint8(body.at(0)) != 0) {
        *error = QStringLiteral("compressed requests are not accepted");
        return false;
    }
    const quint32 length = (quint32(quint8(body.at(1))) << 24)
        | (quint32(quint8(body.at(2))) << 16) | (quint32(quint8(body.at(3))) << 8)
        | quint32(quint8(body.at(4)));
    if (quint32(body.size() - 5) < length) {
        *error = QStringLiteral("gRPC frame shorter than its declared length");
        return false;
    }
    // Exactly one message per unary request; more would silently drop data.
    if (quint32(body.size() - 5) > length) {
        *error = QStringLiteral("more than one message in a unary request");
        return false;
    }
    *message = body.mid(5, int(length));
    return true;
}

QByteArray Server::encodeGrpcMessage(const QString &message)
{
    const QByteArray utf8 = message.toUtf8();
    QByteArray out;
    out.reserve(utf8.size());
    for (int i = 0; i < utf8.size(); ++i) {
        const quint8 ch = quint8(utf8.at(i));
        if (ch < 0x20 || ch > 0x7e || ch == '%') {
            static const char digits[] = "0123456789ABCDEF";
            out.append('%');
            out.append(digits[(ch >> 4) & 0xf]);
            out.append(digits[ch & 0xf]);
        } else {
            out.append(char(ch));
        }
    }
    return out;
}

void Server::writeStatus(http2::ServerResponse *response, const Status &status)
{
    response->trailers.append(
        {QByteArrayLiteral("grpc-status"), QByteArray::number(status.code)});
    if (!status.message.isEmpty()) {
        response->trailers.append(
            {QByteArrayLiteral("grpc-message"), encodeGrpcMessage(status.message)});
    }
}

void Server::dispatch(const http2::ServerRequest &request, http2::ServerResponse *response)
{
    Status status;
    const QString method = request.path();
    const CallLog callLog(method, request.peer, request.body.size(), status, &response->body);

    // Everything below that fails answers trailers-only, which is the shape a
    // real gRPC server uses for an error and the shape Http2Client already
    // reads the status out of.
    response->trailersOnly = true;

    if (request.method() != QLatin1String("POST")) {
        status.code = Internal;
        status.message = QStringLiteral("gRPC requires POST");
        writeStatus(response, status);
        return;
    }

    const QByteArray contentType = request.header("content-type");
    if (!contentType.startsWith("application/grpc")) {
        status.code = Internal;
        status.message = QStringLiteral("content-type '%1' is not gRPC")
                             .arg(QString::fromUtf8(contentType));
        writeStatus(response, status);
        return;
    }

    if (!m_bearer.isEmpty() && request.header("authorization") != m_bearer) {
        m_rejected.fetchAndAddRelaxed(1);
        status.code = Unauthenticated;
        // Deliberately says nothing about whether a token was sent at all: an
        // operator reads this in the client as "sai token", and a scanner
        // learns nothing it did not already know.
        status.message = QStringLiteral("token không hợp lệ");
        writeStatus(response, status);
        return;
    }

    const auto handler = m_methods.constFind(method);
    if (handler == m_methods.constEnd()) {
        status.code = Unimplemented;
        status.message = QStringLiteral("phương thức %1 không tồn tại").arg(method);
        writeStatus(response, status);
        return;
    }

    ServerCall call;
    call.method = method;
    call.peer = request.peer;
    call.deadlineMs = parseGrpcTimeout(request.header("grpc-timeout"));
    call.headers = request.headers;
    QString error;
    if (!unframeMessage(request.body, &call.message, &error)) {
        status.code = Internal;
        status.message = error;
        writeStatus(response, status);
        return;
    }

    QByteArray message;
    status = handler.value()(call, &message);
    if (!status.ok()) {
        writeStatus(response, status);
        return;
    }

    response->trailersOnly = false;
    response->body = frameMessage(message);
    writeStatus(response, status);
}

} // namespace grpc
