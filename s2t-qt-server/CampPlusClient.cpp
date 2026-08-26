#include "CampPlusClient.h"

#include "core/Logger.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrl>

namespace {

// enroll_service.py runs rebuild_db over the whole speaker database on every
// enrolment, so a minute is normal and two is not alarming.  The gRPC deadline
// the client sent is what really bounds it; this is only the floor.
const int kDefaultTimeoutMs = 120000;

// A reply bigger than this is not something this service produces - /status on
// a large registry is tens of kilobytes - so it is a runaway rather than data.
const int kMaxBodyBytes = 16 * 1024 * 1024;

bool readLine(QTcpSocket *socket, QByteArray *line, QElapsedTimer &clock, int timeoutMs)
{
    line->clear();
    for (;;) {
        if (socket->canReadLine()) {
            *line = socket->readLine();
            return true;
        }
        const qint64 spent = clock.elapsed();
        const int left = timeoutMs > spent ? int(timeoutMs - spent) : 0;
        if (left <= 0 || !socket->waitForReadyRead(left))
            return false;
    }
}

bool readExactly(QTcpSocket *socket, int count, QByteArray *out, QElapsedTimer &clock,
                 int timeoutMs)
{
    while (out->size() < count) {
        if (socket->bytesAvailable() > 0) {
            out->append(socket->read(count - out->size()));
            continue;
        }
        const qint64 spent = clock.elapsed();
        const int left = timeoutMs > spent ? int(timeoutMs - spent) : 0;
        if (left <= 0 || !socket->waitForReadyRead(left))
            return false;
    }
    return true;
}

// The service answers errors as {"error": "..."}; that sentence is what an
// operator can act on, so it is preferred over the status line.
QString errorFromBody(const QByteArray &body, int httpStatus)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        const QString message = document.object().value(QStringLiteral("error")).toString();
        if (!message.isEmpty())
            return message;
    }
    const QString text = QString::fromUtf8(body).trimmed();
    if (!text.isEmpty())
        return QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(text.left(400));
    return QStringLiteral("dịch vụ CAM++ trả về HTTP %1").arg(httpStatus);
}

} // namespace

CampPlusClient::CampPlusClient(const QString &baseUrl, int timeoutMs)
    : m_baseUrl(baseUrl.trimmed()), m_timeoutMs(timeoutMs > 0 ? timeoutMs : kDefaultTimeoutMs)
{
    if (m_baseUrl.isEmpty())
        return;
    const QUrl url(m_baseUrl);
    if (url.scheme() != QStringLiteral("http")) {
        // Refused rather than downgraded: the enrolment body is a person's
        // voice and the header carries their name, so quietly speaking plain
        // HTTP to something configured as https would be the wrong default.
        LOG_ERROR(applog::cat::Rpc)
            << "CAM++ url" << m_baseUrl << "is not http:// - đăng ký giọng sẽ không hoạt động";
        return;
    }
    m_host = url.host();
    m_port = quint16(url.port(8790));
    if (m_host.isEmpty())
        m_port = 0;
}

QByteArray CampPlusClient::urlEncode(const QString &text)
{
    // QUrl::toPercentEncoding leaves the unreserved set alone, which is the
    // same set urllib.parse.quote() keeps by default apart from '/', so '/' is
    // named explicitly as something to encode.
    return QUrl::toPercentEncoding(text, QByteArray(), QByteArrayLiteral("/"));
}

grpc::Status CampPlusClient::get(const QString &path, QByteArray *body, int timeoutMs)
{
    return request(QByteArrayLiteral("GET"), path, QByteArray(), {}, body, timeoutMs);
}

grpc::Status CampPlusClient::post(const QString &path, const QByteArray &payload,
                                  const QList<QPair<QByteArray, QByteArray>> &headers,
                                  QByteArray *body, int timeoutMs)
{
    return request(QByteArrayLiteral("POST"), path, payload, headers, body, timeoutMs);
}

grpc::Status CampPlusClient::request(const QByteArray &method, const QString &path,
                                     const QByteArray &payload,
                                     const QList<QPair<QByteArray, QByteArray>> &headers,
                                     QByteArray *body, int timeoutMs)
{
    grpc::Status status;
    if (!configured()) {
        status.code = grpc::FailedPrecondition;
        status.message = QStringLiteral(
            "chưa cấu hình dịch vụ đăng ký giọng (enroll/url) - xem s2t-qt-server.conf");
        return status;
    }

    const int budget = timeoutMs > 0 ? timeoutMs : m_timeoutMs;
    QElapsedTimer clock;
    clock.start();

    QTcpSocket socket;
    socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket.connectToHost(m_host, m_port);
    if (!socket.waitForConnected(qMin(budget, 10000))) {
        status.code = grpc::Unavailable;
        status.message = QStringLiteral("không nối được dịch vụ CAM++ %1 - %2")
                             .arg(m_baseUrl, socket.errorString());
        return status;
    }

    QByteArray head = method + " " + path.toUtf8() + " HTTP/1.1\r\n";
    head += "Host: " + m_host.toUtf8() + ":" + QByteArray::number(m_port) + "\r\n";
    // No keep-alive: one request per connection.  Enrolment happens a few
    // times a day, and a pooled connection would only add a state machine.
    head += "Connection: close\r\n";
    head += "User-Agent: s2t-qt-server/1.0\r\n";
    for (const auto &header : headers)
        head += header.first + ": " + header.second + "\r\n";
    if (!payload.isEmpty() || method == QByteArrayLiteral("POST"))
        head += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    head += "\r\n";

    socket.write(head);
    if (!payload.isEmpty())
        socket.write(payload);
    while (socket.bytesToWrite() > 0) {
        const qint64 spent = clock.elapsed();
        const int left = budget > spent ? int(budget - spent) : 0;
        if (left <= 0 || !socket.waitForBytesWritten(left)) {
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("gửi yêu cầu tới CAM++ quá hạn");
            return status;
        }
    }

    QByteArray line;
    if (!readLine(&socket, &line, clock, budget)) {
        status.code = grpc::DeadlineExceeded;
        status.message = QStringLiteral("dịch vụ CAM++ không trả lời trong %1 ms").arg(budget);
        return status;
    }
    const QList<QByteArray> statusParts = line.trimmed().split(' ');
    const int httpStatus = statusParts.size() >= 2 ? statusParts.at(1).toInt() : 0;

    qint64 contentLength = -1;
    bool chunked = false;
    for (;;) {
        if (!readLine(&socket, &line, clock, budget)) {
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("đọc header từ CAM++ quá hạn");
            return status;
        }
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty())
            break;
        const int colon = trimmed.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray name = trimmed.left(colon).trimmed().toLower();
        const QByteArray value = trimmed.mid(colon + 1).trimmed();
        if (name == "content-length")
            contentLength = value.toLongLong();
        else if (name == "transfer-encoding" && value.toLower().contains("chunked"))
            chunked = true;
    }

    body->clear();
    if (chunked) {
        // ThreadingHTTPServer does not chunk, but a proxy in between might.
        for (;;) {
            if (!readLine(&socket, &line, clock, budget))
                break;
            bool ok = false;
            const int size = line.trimmed().split(';').first().toInt(&ok, 16);
            if (!ok || size <= 0)
                break;
            if (body->size() + size > kMaxBodyBytes) {
                status.code = grpc::ResourceExhausted;
                status.message = QStringLiteral("phản hồi CAM++ vượt %1 byte").arg(kMaxBodyBytes);
                return status;
            }
            QByteArray chunk;
            if (!readExactly(&socket, size, &chunk, clock, budget))
                break;
            body->append(chunk);
            readLine(&socket, &line, clock, budget); // trailing CRLF
        }
    } else if (contentLength >= 0) {
        if (contentLength > kMaxBodyBytes) {
            status.code = grpc::ResourceExhausted;
            status.message = QStringLiteral("phản hồi CAM++ vượt %1 byte").arg(kMaxBodyBytes);
            return status;
        }
        if (!readExactly(&socket, int(contentLength), body, clock, budget)) {
            status.code = grpc::DeadlineExceeded;
            status.message = QStringLiteral("đọc thân phản hồi từ CAM++ quá hạn");
            return status;
        }
    } else {
        // No length and no chunking: read until the peer closes, which is what
        // "Connection: close" asked it to do.
        while (socket.state() == QAbstractSocket::ConnectedState) {
            const qint64 spent = clock.elapsed();
            const int left = budget > spent ? int(budget - spent) : 0;
            if (left <= 0 || !socket.waitForReadyRead(left))
                break;
            body->append(socket.readAll());
            if (body->size() > kMaxBodyBytes) {
                status.code = grpc::ResourceExhausted;
                status.message = QStringLiteral("phản hồi CAM++ vượt %1 byte").arg(kMaxBodyBytes);
                return status;
            }
        }
        body->append(socket.readAll());
    }

    if (httpStatus < 200 || httpStatus >= 300) {
        // The service's own sentence, not our paraphrase of a status code.
        status.code = httpStatus == 404 ? grpc::Unimplemented : grpc::FailedPrecondition;
        status.message = errorFromBody(*body, httpStatus);
        LOG_WARN(applog::cat::Rpc)
            << "CAM++" << method << path << "->" << httpStatus << status.message;
        return status;
    }

    LOG_DEBUG(applog::cat::Rpc) << "CAM++" << method << path << "-> 200," << body->size()
                                << "bytes in" << clock.elapsed() << "ms";
    return status;
}
