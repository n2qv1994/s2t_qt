// The CAM++ enrolment service, which is plain HTTP/1.1 and not gRPC.
//
// `campp_native/enroll_service.py` in s2t-dgpu is a ThreadingHTTPServer on
// :8790.  The Python adapter reached it with urllib while presenting a gRPC
// face to the UI; that translation is what this class replaces.
//
// Why a hand-written client rather than QNetworkAccessManager: QNAM is
// asynchronous and has thread affinity, and every caller here is an HTTP/2
// connection thread that owes a reply on the call it is already handling.  A
// blocking request on a socket that thread owns is the same shape the rest of
// this server uses (see RpcLane), and it is about 150 lines.
//
// Endpoints used, all on the same base URL:
//
//   GET  /enroll_script   the reading an enrolment sample should follow
//   GET  /status          the global CAM++ database: size, mtime, weak samples
//   POST /enroll          a complete WAV, 16 kHz mono, X-Speaker-Name header
//
// `/enroll_from_pcm`, `/session/start` and `/session/stop` are deliberately
// absent: they serve the per-session speaker registry, which needs a session
// store this server does not have yet.
#ifndef CAMPPLUSCLIENT_H
#define CAMPPLUSCLIENT_H

#include "grpc/GrpcChannel.h"

#include <QByteArray>
#include <QString>

class CampPlusClient
{
public:
    // `baseUrl` is "http://host:port"; https is not supported and is refused
    // rather than silently downgraded.
    CampPlusClient(const QString &baseUrl, int timeoutMs);

    bool configured() const { return m_port != 0; }
    QString baseUrl() const { return m_baseUrl; }

    // Each returns the raw response body.  A non-2xx answer is reported
    // through `status` with the service's own message where it sent one -
    // enroll_service.py answers errors as {"error": "..."} and that text is
    // what an operator needs to see, not "HTTP 400".
    grpc::Status get(const QString &path, QByteArray *body, int timeoutMs = 0);
    // `headers` are extra request headers, already percent-encoded where the
    // value can carry Vietnamese - enroll_service.py calls unquote() on
    // X-Speaker-Name and X-Editor-Id, and an HTTP header is Latin-1 only.
    grpc::Status post(const QString &path, const QByteArray &payload,
                      const QList<QPair<QByteArray, QByteArray>> &headers, QByteArray *body,
                      int timeoutMs = 0);

    // Percent-encodes everything outside the unreserved set, which is what
    // urllib.parse.quote() does on the far side.
    static QByteArray urlEncode(const QString &text);

private:
    grpc::Status request(const QByteArray &method, const QString &path, const QByteArray &payload,
                         const QList<QPair<QByteArray, QByteArray>> &headers, QByteArray *body,
                         int timeoutMs);

    QString m_baseUrl;
    QString m_host;
    quint16 m_port = 0;
    int m_timeoutMs = 120000;
};

#endif // CAMPPLUSCLIENT_H
