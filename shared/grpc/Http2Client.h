// Minimal blocking HTTP/2 client, just enough to carry gRPC unary calls.
//
// Blocking on purpose.  Every caller here already owns a dedicated worker
// thread (audio push, state poll, request-scoped RPCs), which is the same
// shape the Python bridge used - a separate channel per concern so a big
// get_live_state serialization can never sit in front of a 160 ms audio
// packet.  A synchronous socket makes that shape ~400 lines instead of a
// callback state machine.
//
// One Http2Connection belongs to exactly one thread.  It is not re-entrant.
#ifndef HTTP2CLIENT_H
#define HTTP2CLIENT_H

#include "Hpack.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QString>

class QTcpSocket;

namespace http2 {

// Refuse to accumulate an unbounded response.  get_review_state on a long
// meeting is the biggest thing this client ever reads and the adapter itself
// caps at gRPC's 4 MiB; this leaves an order of magnitude of headroom while
// still bounding a runaway or hostile peer.
const int kMaxResponseBytes = 64 * 1024 * 1024;

struct Response
{
    QList<hpack::Header> headers;  // initial HEADERS block
    QList<hpack::Header> trailers; // trailing HEADERS block, if any
    QByteArray body;               // concatenated DATA payload
};

class Http2Connection
{
public:
    Http2Connection();
    ~Http2Connection();

    Http2Connection(const Http2Connection &) = delete;
    Http2Connection &operator=(const Http2Connection &) = delete;

    bool open(const QString &host, quint16 port, int timeoutMs, QString *error);
    void close();
    bool isOpen() const;

    const QString &host() const { return m_host; }

    // Sends one complete request and reads the response to end of stream.
    // Returns false on transport failure only; an HTTP or gRPC level error is
    // reported through `out` (its :status / grpc-status headers).
    bool request(const QList<hpack::Header> &headers,
                 const QByteArray &body,
                 int timeoutMs,
                 Response *out,
                 QString *error);

    // ---- long-lived streams ------------------------------------------------
    //
    // request() is unary by construction: it puts END_STREAM on the last DATA
    // frame and then reads to end of stream.  Riva's StreamingRecognize and
    // Triton's ModelStreamInfer are both bidirectional - the request body keeps
    // growing for the length of a meeting while responses come back on the same
    // stream - so they get their own small state machine here.
    //
    // One stream at a time per connection, for exactly the reason request()
    // opens one at a time: the HPACK decoder is per connection, so two
    // interleaved header blocks would desynchronise it for every later call.
    bool openStream(const QList<hpack::Header> &headers, int timeoutMs, QString *error);
    // Appends one message to the request body.  `endStream` is the half-close
    // that tells the far side no more audio is coming.
    bool sendStreamData(const QByteArray &body, bool endStream, int timeoutMs, QString *error);
    // Absorbs whatever has arrived into streamBuffer(), waiting at most
    // `waitMs` for the first byte.  waitMs == 0 polls - it returns at once with
    // whatever was already buffered, which is what lets one thread push audio
    // and collect results without either starving the other.
    bool pumpStream(int waitMs, QString *error);

    bool streamOpen() const { return m_stream.open; }
    bool streamSendClosed() const { return m_stream.sendClosed; }
    // True once the far side has sent END_STREAM: streamBuffer() then holds
    // everything there will ever be, and streamTrailers() is final.
    bool streamEnded() const { return m_stream.recvClosed; }
    QByteArray &streamBuffer() { return m_stream.buffer; }
    // Bytes received but not yet taken out of the buffer.  A caller uses this
    // to tell "the stream ended" from "the stream ended and I have read
    // everything it said".
    int streamPending() const { return m_stream.buffer.size(); }
    const QList<hpack::Header> &streamHeaders() const { return m_stream.headers; }
    const QList<hpack::Header> &streamTrailers() const { return m_stream.trailers; }
    // Sends RST_STREAM if the stream is still live, then forgets it.  The
    // connection stays usable.
    void closeStream();

private:
    struct StreamState
    {
        quint32 id = 0;
        bool open = false;
        bool sendClosed = false;
        bool recvClosed = false;
        bool seenHeaders = false;
        QList<hpack::Header> headers;
        QList<hpack::Header> trailers;
        QByteArray buffer;
        QByteArray pendingHeaderBlock;
        bool pendingIsTrailer = false;
        bool awaitingContinuation = false;
    };

    struct Frame
    {
        quint32 length = 0;
        quint8 type = 0;
        quint8 flags = 0;
        quint32 streamId = 0;
        QByteArray payload;
    };

    bool sendPreface(QString *error);
    bool writeAll(const QByteArray &data, QElapsedTimer &clock, int timeoutMs, QString *error);
    bool readExactly(int count, QByteArray *out, QElapsedTimer &clock, int timeoutMs, QString *error);
    bool readFrame(Frame *frame, QElapsedTimer &clock, int timeoutMs, QString *error);
    bool sendFrame(quint8 type, quint8 flags, quint32 streamId, const QByteArray &payload,
                   QElapsedTimer &clock, int timeoutMs, QString *error);
    bool sendSettingsAck(QElapsedTimer &clock, int timeoutMs, QString *error);
    bool sendWindowUpdate(quint32 streamId, quint32 increment, QElapsedTimer &clock, int timeoutMs,
                          QString *error);
    // Handles every frame that is not part of the caller's stream response:
    // SETTINGS, PING, WINDOW_UPDATE, GOAWAY, and stray frames for other ids.
    bool handleConnectionFrame(const Frame &frame, QElapsedTimer &clock, int timeoutMs, QString *error);
    bool sendBody(quint32 streamId, const QByteArray &body, QElapsedTimer &clock, int timeoutMs,
                  QString *error);
    bool awaitSendWindow(quint32 streamId, int needed, QElapsedTimer &clock, int timeoutMs,
                         QString *error);

    // True when a whole frame - header and payload - is already buffered, so
    // readFrame() cannot block.  This is what makes pumpStream(0) a poll.
    bool frameReady() const;
    // Folds one frame belonging to the open stream into m_stream.
    bool absorbStreamFrame(const Frame &frame, QElapsedTimer &clock, int timeoutMs, QString *error);

    QTcpSocket *m_socket = nullptr;
    hpack::Decoder m_decoder;
    QString m_host;
    quint16 m_port = 0;
    quint32 m_nextStreamId = 1;
    // Peer-controlled limits, updated from its SETTINGS.
    quint32 m_peerMaxFrameSize = 16384;
    qint64 m_connSendWindow = 65535;
    qint64 m_streamSendWindow = 65535;
    quint32 m_peerInitialWindow = 65535;
    bool m_goaway = false;
    QString m_goawayReason;
    StreamState m_stream;
};

} // namespace http2

#endif // HTTP2CLIENT_H
