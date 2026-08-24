#include "Http2Server.h"

#include "core/Logger.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QTcpSocket>
#include <QThread>

#include <utility>

namespace http2 {
namespace {

enum FrameType : quint8 {
    FrameData = 0x0,
    FrameHeaders = 0x1,
    FramePriority = 0x2,
    FrameRstStream = 0x3,
    FrameSettings = 0x4,
    FramePushPromise = 0x5,
    FramePing = 0x6,
    FrameGoaway = 0x7,
    FrameWindowUpdate = 0x8,
    FrameContinuation = 0x9,
};

enum FrameFlag : quint8 {
    FlagEndStream = 0x1,
    FlagAck = 0x1,
    FlagEndHeaders = 0x4,
    FlagPadded = 0x8,
    FlagPriority = 0x20,
};

enum SettingId : quint16 {
    SettingHeaderTableSize = 0x1,
    SettingEnablePush = 0x2,
    SettingMaxConcurrentStreams = 0x3,
    SettingInitialWindowSize = 0x4,
    SettingMaxFrameSize = 0x5,
    SettingMaxHeaderListSize = 0x6,
};

enum ErrorCode : quint32 {
    ErrNoError = 0x0,
    ErrProtocol = 0x1,
    ErrInternal = 0x2,
    ErrFlowControl = 0x3,
    ErrFrameSize = 0x6,
    ErrRefusedStream = 0x7,
    ErrEnhanceYourCalm = 0xb,
};

const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const int kPrefaceLength = int(sizeof(kPreface)) - 1;

// Matches what Http2Client advertises, for the same reason: a multi-megabyte
// get_review_state must not stall on flow control before the first
// WINDOW_UPDATE round trip.
const quint32 kOurInitialWindow = 8u * 1024u * 1024u;
const quint32 kOurConnectionWindowBump = 8u * 1024u * 1024u;

// Once a frame header has arrived, the rest of that frame is already on the
// wire.  A peer that stops halfway through one is broken, not idle, so this is
// deliberately much shorter than the connection idle timeout.
const int kFramePayloadTimeoutMs = 30000;
const int kWriteTimeoutMs = 30000;
// Slice length for every blocking wait, so a shutdown request is noticed
// within a quarter of a second no matter how long the outer deadline is.
const int kWaitSliceMs = 200;

// How many half-open streams one connection may hold at once.  Each carries a
// buffered request body, so without a cap a peer that opens streams and never
// ends them is an out-of-memory attack rather than a slow client.  Our own
// client uses one stream at a time; this is generous enough that only a
// deliberate abuser reaches it.
const int kMaxOpenStreams = 64;

// Advertised MAX_CONCURRENT_STREAMS.  It bounds streams per connection, which
// is a different budget from the server's connection limit.
const quint32 kMaxConcurrentStreams = quint32(kMaxOpenStreams);

void appendUint24(QByteArray &out, quint32 value)
{
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

void appendUint32(QByteArray &out, quint32 value)
{
    out.append(char((value >> 24) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

void appendUint16(QByteArray &out, quint16 value)
{
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

quint32 readUint32(const QByteArray &buf, int offset)
{
    return (quint32(quint8(buf.at(offset))) << 24) | (quint32(quint8(buf.at(offset + 1))) << 16)
        | (quint32(quint8(buf.at(offset + 2))) << 8) | quint32(quint8(buf.at(offset + 3)));
}

quint16 readUint16(const QByteArray &buf, int offset)
{
    return quint16((quint16(quint8(buf.at(offset))) << 8) | quint16(quint8(buf.at(offset + 1))));
}

// The server preface: our SETTINGS, plus an explicit connection-level
// WINDOW_UPDATE.  SETTINGS_INITIAL_WINDOW_SIZE only raises the *stream*
// window, so without the second frame a large enrolment upload would stall at
// the spec's 64 KiB connection default.
QByteArray serverPreface(quint32 maxConcurrentStreams)
{
    QByteArray settings;
    appendUint16(settings, SettingEnablePush);
    appendUint32(settings, 0);
    appendUint16(settings, SettingMaxConcurrentStreams);
    appendUint32(settings, maxConcurrentStreams);
    appendUint16(settings, SettingInitialWindowSize);
    appendUint32(settings, kOurInitialWindow);
    appendUint16(settings, SettingMaxFrameSize);
    appendUint32(settings, 1u << 14);
    appendUint16(settings, SettingHeaderTableSize);
    appendUint32(settings, 4096);

    QByteArray out;
    appendUint24(out, quint32(settings.size()));
    out.append(char(FrameSettings));
    out.append(char(0));
    appendUint32(out, 0);
    out.append(settings);

    appendUint24(out, 4);
    out.append(char(FrameWindowUpdate));
    out.append(char(0));
    appendUint32(out, 0);
    appendUint32(out, kOurConnectionWindowBump);
    return out;
}

QByteArray goawayFrame(quint32 lastStreamId, quint32 code, const QByteArray &debug)
{
    QByteArray payload;
    appendUint32(payload, lastStreamId & 0x7fffffffu);
    appendUint32(payload, code);
    payload.append(debug);

    QByteArray out;
    appendUint24(out, quint32(payload.size()));
    out.append(char(FrameGoaway));
    out.append(char(0));
    appendUint32(out, 0);
    out.append(payload);
    return out;
}

} // namespace

QByteArray ServerRequest::header(const char *name) const
{
    for (const hpack::Header &h : headers) {
        if (h.name == name)
            return h.value;
    }
    return QByteArray();
}

// ---------------------------------------------------------------------------
// One connection, one thread, one blocking loop.
// ---------------------------------------------------------------------------

class ServerConnection
{
public:
    ServerConnection(qintptr descriptor, Http2Server *owner, Http2Server::Handler handler,
                     int idleTimeoutMs, int id)
        : m_descriptor(descriptor), m_owner(owner), m_handler(std::move(handler)),
          m_idleTimeoutMs(idleTimeoutMs), m_id(id)
    {
    }

    ~ServerConnection() { delete m_socket; }

    ServerConnection(const ServerConnection &) = delete;
    ServerConnection &operator=(const ServerConnection &) = delete;

    void run();

private:
    struct Frame
    {
        quint32 length = 0;
        quint8 type = 0;
        quint8 flags = 0;
        quint32 streamId = 0;
        QByteArray payload;
    };

    struct Stream
    {
        QList<hpack::Header> headers;
        QByteArray body;
        bool headersDone = false;
    };

    bool stopping() const { return m_owner->m_stopping.loadRelaxed() != 0; }

    bool readExactly(int count, QByteArray *out, int timeoutMs, QString *error);
    bool readFrame(Frame *frame, int firstByteTimeoutMs, QString *error);
    bool writeAll(const QByteArray &data, QString *error);
    bool sendFrame(quint8 type, quint8 flags, quint32 streamId, const QByteArray &payload,
                   QString *error);
    bool sendSettingsAck(QString *error);
    bool sendWindowUpdate(quint32 streamId, quint32 increment, QString *error);
    bool sendRstStream(quint32 streamId, quint32 code, QString *error);
    bool sendHeaders(quint32 streamId, const QList<hpack::Header> &headers, quint8 flags,
                     QString *error);
    bool sendData(quint32 streamId, const QByteArray &body, QString *error);
    bool awaitSendWindow(quint32 streamId, int needed, QString *error);
    bool handleConnectionFrame(const Frame &frame, QString *error);
    bool handleHeaderBlock(const Frame &frame, QString *error);
    bool handleData(const Frame &frame, QString *error);
    bool serve(quint32 streamId, QString *error);

    qintptr m_descriptor;
    Http2Server *m_owner = nullptr;
    Http2Server::Handler m_handler;
    int m_idleTimeoutMs = 300000;
    int m_id = 0;

    QTcpSocket *m_socket = nullptr;
    QString m_peer;
    hpack::Decoder m_decoder;

    QHash<quint32, Stream> m_streams;
    quint32 m_lastStreamId = 0;
    QByteArray m_pendingBlock;
    quint32 m_pendingStream = 0;
    bool m_pendingEndStream = false;
    bool m_awaitingContinuation = false;

    quint32 m_peerMaxFrameSize = 16384;
    quint32 m_peerInitialWindow = 65535;
    qint64 m_connSendWindow = 65535;
    QHash<quint32, qint64> m_streamSendWindow;
    bool m_goawaySeen = false;
    quint64 m_served = 0;
};

void ServerConnection::run()
{
    m_socket = new QTcpSocket();
    if (!m_socket->setSocketDescriptor(m_descriptor)) {
        LOG_WARN(applog::cat::Http2) << "connection" << m_id << "could not adopt the socket:"
                                     << m_socket->errorString();
        return;
    }
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_peer = QStringLiteral("%1:%2")
                 .arg(m_socket->peerAddress().toString())
                 .arg(m_socket->peerPort());
    LOG_DEBUG(applog::cat::Http2) << "connection" << m_id << "accepted from" << m_peer;

    QString error;
    // Our SETTINGS go out first, before the client preface is even read: the
    // spec allows it, and it means a client that waits for the server preface
    // before sending anything cannot deadlock against a server that is waiting
    // for the client preface.
    if (!writeAll(serverPreface(kMaxConcurrentStreams), &error)) {
        LOG_WARN(applog::cat::Http2) << "connection" << m_id << "- sending the server preface"
                                     << "failed:" << error;
        return;
    }

    QByteArray preface;
    if (!readExactly(kPrefaceLength, &preface, m_idleTimeoutMs, &error)) {
        LOG_DEBUG(applog::cat::Http2) << "connection" << m_id << "closed before the preface:"
                                      << error;
        return;
    }
    if (preface != QByteArray(kPreface, kPrefaceLength)) {
        // Almost always a plain HTTP/1.1 client or a port scan.  Say which,
        // because "connection dropped" on its own sends people hunting for a
        // network fault that is not there.
        LOG_WARN(applog::cat::Http2) << "connection" << m_id << "from" << m_peer
                                     << "is not HTTP/2 (bad preface) - refusing";
        QString ignored;
        writeAll(goawayFrame(0, ErrProtocol, QByteArrayLiteral("expected the HTTP/2 preface")),
                 &ignored);
        return;
    }

    while (!stopping()) {
        Frame frame;
        if (!readFrame(&frame, m_idleTimeoutMs, &error))
            break;

        // A header block is atomic on the wire: nothing else may be
        // interleaved inside it, because HPACK state is per connection and a
        // gap would desynchronise the decoder for every later request.
        if (m_awaitingContinuation && frame.type != FrameContinuation) {
            error = QStringLiteral("frame interleaved inside a header block");
            QString ignored;
            writeAll(goawayFrame(m_lastStreamId, ErrProtocol, error.toUtf8()), &ignored);
            break;
        }

        bool ok = true;
        switch (frame.type) {
        case FrameHeaders:
        case FrameContinuation:
            ok = handleHeaderBlock(frame, &error);
            break;
        case FrameData:
            ok = handleData(frame, &error);
            break;
        case FrameRstStream:
            m_streams.remove(frame.streamId);
            m_streamSendWindow.remove(frame.streamId);
            LOG_DEBUG(applog::cat::Http2)
                << "connection" << m_id << "- client reset stream" << frame.streamId;
            break;
        case FramePriority:
            break;
        default:
            ok = handleConnectionFrame(frame, &error);
            break;
        }
        if (!ok) {
            LOG_WARN(applog::cat::Http2) << "connection" << m_id << "from" << m_peer
                                         << "failed:" << error;
            QString ignored;
            writeAll(goawayFrame(m_lastStreamId, ErrProtocol, error.toUtf8()), &ignored);
            break;
        }
        if (m_goawaySeen)
            break;
    }

    if (stopping()) {
        QString ignored;
        writeAll(goawayFrame(m_lastStreamId, ErrNoError, QByteArrayLiteral("server shutting down")),
                 &ignored);
    }
    LOG_DEBUG(applog::cat::Http2) << "connection" << m_id << "from" << m_peer << "closed after"
                                  << m_served << "requests";
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->waitForDisconnected(1000);
}

bool ServerConnection::readExactly(int count, QByteArray *out, int timeoutMs, QString *error)
{
    out->clear();
    QElapsedTimer clock;
    clock.start();
    while (out->size() < count) {
        if (stopping()) {
            *error = QStringLiteral("server is shutting down");
            return false;
        }
        if (m_socket->bytesAvailable() > 0) {
            out->append(m_socket->read(count - out->size()));
            continue;
        }
        if (timeoutMs > 0 && clock.elapsed() >= timeoutMs) {
            *error = QStringLiteral("idle for %1 ms").arg(clock.elapsed());
            return false;
        }
        if (!m_socket->waitForReadyRead(kWaitSliceMs)
            && m_socket->state() != QAbstractSocket::ConnectedState) {
            *error = QStringLiteral("client closed the connection");
            return false;
        }
    }
    return true;
}

bool ServerConnection::readFrame(Frame *frame, int firstByteTimeoutMs, QString *error)
{
    QByteArray header;
    if (!readExactly(9, &header, firstByteTimeoutMs, error))
        return false;
    frame->length = (quint32(quint8(header.at(0))) << 16) | (quint32(quint8(header.at(1))) << 8)
        | quint32(quint8(header.at(2)));
    frame->type = quint8(header.at(3));
    frame->flags = quint8(header.at(4));
    frame->streamId = readUint32(header, 5) & 0x7fffffffu;
    if (frame->length > (1u << 24)) {
        *error = QStringLiteral("client sent an oversized frame");
        return false;
    }
    frame->payload.clear();
    if (frame->length > 0
        && !readExactly(int(frame->length), &frame->payload, kFramePayloadTimeoutMs, error))
        return false;
    return true;
}

bool ServerConnection::writeAll(const QByteArray &data, QString *error)
{
    qint64 written = 0;
    QElapsedTimer clock;
    clock.start();
    while (written < data.size()) {
        const qint64 n = m_socket->write(data.constData() + written, data.size() - written);
        if (n < 0) {
            *error = QStringLiteral("write failed: %1").arg(m_socket->errorString());
            return false;
        }
        written += n;
        // waitForBytesWritten returning false with no error just means the
        // buffer already drained, so only a real socket error is fatal.
        if (!m_socket->waitForBytesWritten(kWaitSliceMs)) {
            if (m_socket->state() != QAbstractSocket::ConnectedState) {
                *error = QStringLiteral("connection lost while sending");
                return false;
            }
            if (clock.elapsed() >= kWriteTimeoutMs) {
                *error = QStringLiteral("timed out sending the response");
                return false;
            }
        }
    }
    return true;
}

bool ServerConnection::sendFrame(quint8 type, quint8 flags, quint32 streamId,
                                 const QByteArray &payload, QString *error)
{
    QByteArray out;
    out.reserve(payload.size() + 9);
    appendUint24(out, quint32(payload.size()));
    out.append(char(type));
    out.append(char(flags));
    appendUint32(out, streamId & 0x7fffffffu);
    out.append(payload);
    return writeAll(out, error);
}

bool ServerConnection::sendSettingsAck(QString *error)
{
    return sendFrame(FrameSettings, FlagAck, 0, QByteArray(), error);
}

bool ServerConnection::sendWindowUpdate(quint32 streamId, quint32 increment, QString *error)
{
    if (increment == 0)
        return true;
    QByteArray payload;
    appendUint32(payload, increment & 0x7fffffffu);
    return sendFrame(FrameWindowUpdate, 0, streamId, payload, error);
}

bool ServerConnection::sendRstStream(quint32 streamId, quint32 code, QString *error)
{
    QByteArray payload;
    appendUint32(payload, code);
    return sendFrame(FrameRstStream, 0, streamId, payload, error);
}

bool ServerConnection::handleConnectionFrame(const Frame &frame, QString *error)
{
    switch (frame.type) {
    case FrameSettings:
        if (frame.flags & FlagAck)
            return true;
        for (int offset = 0; offset + 6 <= frame.payload.size(); offset += 6) {
            const quint16 id = readUint16(frame.payload, offset);
            const quint32 value = readUint32(frame.payload, offset + 2);
            switch (id) {
            case SettingMaxFrameSize:
                if (value >= (1u << 14) && value <= ((1u << 24) - 1))
                    m_peerMaxFrameSize = value;
                break;
            case SettingInitialWindowSize: {
                // A change retroactively adjusts every open stream's send
                // window by the delta (RFC 7540 6.9.2).
                const qint64 delta = qint64(value) - qint64(m_peerInitialWindow);
                m_peerInitialWindow = value;
                for (auto it = m_streamSendWindow.begin(); it != m_streamSendWindow.end(); ++it)
                    it.value() += delta;
                break;
            }
            case SettingHeaderTableSize:
                // Our encoder never indexes, so this changes nothing we send;
                // it is accepted so the ACK below is honest.
                break;
            default:
                break;
            }
        }
        return sendSettingsAck(error);

    case FramePing:
        if (frame.flags & FlagAck)
            return true;
        return sendFrame(FramePing, FlagAck, 0, frame.payload, error);

    case FrameWindowUpdate: {
        if (frame.payload.size() < 4)
            return true;
        const qint64 increment = qint64(readUint32(frame.payload, 0) & 0x7fffffffu);
        if (frame.streamId == 0)
            m_connSendWindow += increment;
        else
            m_streamSendWindow[frame.streamId] += increment;
        return true;
    }

    case FrameGoaway:
        m_goawaySeen = true;
        LOG_DEBUG(applog::cat::Http2) << "connection" << m_id << "- client sent GOAWAY";
        return true;

    case FramePushPromise:
        *error = QStringLiteral("client sent PUSH_PROMISE, which a server never accepts");
        return false;

    default:
        return true;
    }
}

bool ServerConnection::handleHeaderBlock(const Frame &frame, QString *error)
{
    QByteArray fragment = frame.payload;
    if (frame.type == FrameHeaders) {
        if (frame.streamId == 0) {
            *error = QStringLiteral("HEADERS on stream 0");
            return false;
        }
        int cut = 0;
        int padLength = 0;
        if (frame.flags & FlagPadded) {
            if (fragment.isEmpty()) {
                *error = QStringLiteral("malformed padded HEADERS frame");
                return false;
            }
            padLength = quint8(fragment.at(0));
            cut = 1;
        }
        if (frame.flags & FlagPriority)
            cut += 5;
        if (fragment.size() < cut + padLength) {
            *error = QStringLiteral("malformed HEADERS frame");
            return false;
        }
        fragment = fragment.mid(cut, fragment.size() - cut - padLength);
        m_pendingBlock.clear();
        m_pendingStream = frame.streamId;
        m_pendingEndStream = (frame.flags & FlagEndStream) != 0;
        if (frame.streamId > m_lastStreamId)
            m_lastStreamId = frame.streamId;
    } else if (frame.streamId != m_pendingStream) {
        *error = QStringLiteral("CONTINUATION for the wrong stream");
        return false;
    }

    m_pendingBlock.append(fragment);
    m_awaitingContinuation = !(frame.flags & FlagEndHeaders);
    if (m_awaitingContinuation)
        return true;

    QList<hpack::Header> decoded;
    if (!m_decoder.decode(m_pendingBlock, &decoded, error)) {
        // HPACK is stateful across the whole connection and cannot be
        // resynchronised, so this is fatal to the connection, not the stream.
        return false;
    }
    m_pendingBlock.clear();

    if (!m_streams.contains(m_pendingStream) && m_streams.size() >= kMaxOpenStreams) {
        // The block above still had to be decoded, or HPACK would be out of
        // sync for every later request on this connection - so refuse the
        // stream, not the frame.
        LOG_WARN(applog::cat::Http2) << "connection" << m_id << "- refusing stream"
                                     << m_pendingStream << "-" << kMaxOpenStreams
                                     << "are already open";
        return sendRstStream(m_pendingStream, ErrRefusedStream, error);
    }

    Stream &stream = m_streams[m_pendingStream];
    if (stream.headersDone) {
        // Request trailers.  gRPC unary clients do not send them; accept and
        // ignore rather than failing a peer that is merely more general.
        LOG_TRACE(applog::cat::Http2) << "connection" << m_id << "- ignoring request trailers on"
                                      << "stream" << m_pendingStream;
    } else {
        stream.headers = decoded;
        stream.headersDone = true;
        m_streamSendWindow[m_pendingStream] = qint64(m_peerInitialWindow);
    }

    if (m_pendingEndStream)
        return serve(m_pendingStream, error);
    return true;
}

bool ServerConnection::handleData(const Frame &frame, QString *error)
{
    QByteArray payload = frame.payload;
    if (frame.flags & FlagPadded) {
        if (payload.isEmpty()) {
            *error = QStringLiteral("malformed padded DATA frame");
            return false;
        }
        const int padLength = quint8(payload.at(0));
        if (payload.size() < 1 + padLength) {
            *error = QStringLiteral("malformed DATA frame");
            return false;
        }
        payload = payload.mid(1, payload.size() - 1 - padLength);
    }

    auto it = m_streams.find(frame.streamId);
    if (it == m_streams.end()) {
        // DATA for a stream we already answered or reset.  The bytes still
        // count against the connection flow-control window, so credit them
        // back before dropping them, or the peer slowly starves.
        if (frame.length > 0 && !sendWindowUpdate(0, frame.length, error))
            return false;
        return true;
    }

    if (it->body.size() + payload.size() > kMaxRequestBytes) {
        *error = QStringLiteral("request exceeded %1 bytes").arg(kMaxRequestBytes);
        return false;
    }
    it->body.append(payload);

    // Replenish immediately: the frame has already been copied out, so the
    // credit is genuinely free again, and a stalled uploader is worse than an
    // extra 13-byte frame per DATA.
    if (frame.length > 0) {
        if (!sendWindowUpdate(0, frame.length, error))
            return false;
        if (!(frame.flags & FlagEndStream) && !sendWindowUpdate(frame.streamId, frame.length, error))
            return false;
    }

    if (frame.flags & FlagEndStream)
        return serve(frame.streamId, error);
    return true;
}

bool ServerConnection::awaitSendWindow(quint32 streamId, int needed, QString *error)
{
    QElapsedTimer clock;
    clock.start();
    while (m_connSendWindow < needed || m_streamSendWindow.value(streamId, 0) < needed) {
        if (stopping()) {
            *error = QStringLiteral("server is shutting down");
            return false;
        }
        if (clock.elapsed() >= kWriteTimeoutMs) {
            *error = QStringLiteral("timed out waiting for client flow-control credit");
            return false;
        }
        Frame frame;
        if (!readFrame(&frame, kWriteTimeoutMs, error))
            return false;
        if (frame.type == FrameRstStream && frame.streamId == streamId) {
            *error = QStringLiteral("client reset the stream while the response was being sent");
            return false;
        }
        if (frame.type == FrameHeaders || frame.type == FrameContinuation
            || frame.type == FramePushPromise) {
            // Our client sends one request at a time per connection, so this
            // means a peer that pipelines.  Decoding the block here would be
            // correct but serving it re-entrantly would not, and skipping it
            // would desynchronise HPACK for everything after - so refuse.
            *error = QStringLiteral("client pipelined a request while a response was in flight");
            return false;
        }
        if (frame.type == FrameData) {
            if (frame.length > 0 && !sendWindowUpdate(0, frame.length, error))
                return false;
            continue;
        }
        if (!handleConnectionFrame(frame, error))
            return false;
        if (m_goawaySeen) {
            *error = QStringLiteral("client went away while the response was being sent");
            return false;
        }
    }
    return true;
}

bool ServerConnection::sendHeaders(quint32 streamId, const QList<hpack::Header> &headers,
                                   quint8 flags, QString *error)
{
    const QByteArray block = hpack::Encoder::encode(headers);
    if (block.size() > int(m_peerMaxFrameSize)) {
        // Only reachable if a peer advertised a tiny MAX_FRAME_SIZE; ours are
        // a handful of short ASCII headers.  CONTINUATION would be the fix,
        // and it is deliberately not implemented until something needs it.
        *error = QStringLiteral("response headers do not fit one frame");
        return false;
    }
    return sendFrame(FrameHeaders, flags, streamId, block, error);
}

bool ServerConnection::sendData(quint32 streamId, const QByteArray &body, QString *error)
{
    int offset = 0;
    do {
        const int chunk = qMin<int>(body.size() - offset, int(m_peerMaxFrameSize));
        if (chunk > 0 && !awaitSendWindow(streamId, chunk, error))
            return false;
        // Never END_STREAM here: the trailing HEADERS block carries
        // grpc-status and it is what closes the stream.
        if (!sendFrame(FrameData, 0, streamId, body.mid(offset, chunk), error))
            return false;
        m_connSendWindow -= chunk;
        m_streamSendWindow[streamId] -= chunk;
        offset += chunk;
    } while (offset < body.size());
    return true;
}

bool ServerConnection::serve(quint32 streamId, QString *error)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return true;

    ServerRequest request;
    request.headers = it->headers;
    request.body = it->body;
    request.peer = m_peer;
    request.streamId = streamId;
    m_streams.erase(it);

    ServerResponse response;
    if (m_handler) {
        m_handler(request, &response);
    } else {
        // A server with no handler is a programming error, not a client one;
        // still answer, so the caller sees a status instead of a hang.
        response.trailersOnly = true;
        response.trailers.append({QByteArrayLiteral("grpc-status"), QByteArrayLiteral("12")});
    }
    ++m_served;
    m_owner->noteRequest();

    QList<hpack::Header> headers;
    // Always 200: gRPC carries its own status in the trailers, and a non-200
    // would make a conforming client report a transport failure instead of
    // the status the handler actually chose.
    headers.append({QByteArrayLiteral(":status"), QByteArrayLiteral("200")});
    headers.append(
        {QByteArrayLiteral("content-type"), QByteArrayLiteral("application/grpc+proto")});
    headers.append({QByteArrayLiteral("grpc-encoding"), QByteArrayLiteral("identity")});
    headers.append({QByteArrayLiteral("grpc-accept-encoding"), QByteArrayLiteral("identity")});
    for (const hpack::Header &h : std::as_const(response.headers))
        headers.append(h);

    if (response.trailersOnly) {
        for (const hpack::Header &h : std::as_const(response.trailers))
            headers.append(h);
        const bool ok = sendHeaders(streamId, headers, FlagEndHeaders | FlagEndStream, error);
        m_streamSendWindow.remove(streamId);
        return ok;
    }

    if (!sendHeaders(streamId, headers, FlagEndHeaders, error))
        return false;
    if (!sendData(streamId, response.body, error))
        return false;
    const bool ok =
        sendHeaders(streamId, response.trailers, FlagEndHeaders | FlagEndStream, error);
    m_streamSendWindow.remove(streamId);
    return ok;
}

// ---------------------------------------------------------------------------
// Http2Server
// ---------------------------------------------------------------------------

Http2Server::Http2Server(QObject *parent) : QTcpServer(parent) {}

Http2Server::~Http2Server()
{
    stop();
}

void Http2Server::setHandler(Handler handler)
{
    m_handler = std::move(handler);
}

void Http2Server::setMaxConnections(int limit)
{
    m_maxConnections = qMax(1, limit);
}

bool Http2Server::start(const QHostAddress &address, quint16 port, QString *error)
{
    m_stopping.storeRelaxed(0);
    if (!listen(address, port)) {
        *error = errorString();
        return false;
    }
    LOG_INFO(applog::cat::Http2) << "listening on"
                                 << QStringLiteral("%1:%2").arg(address.toString()).arg(serverPort())
                                 << "- at most" << m_maxConnections << "connections";
    return true;
}

void Http2Server::stop()
{
    if (m_stopping.loadRelaxed() != 0 && m_threads.isEmpty())
        return;
    m_stopping.storeRelaxed(1);
    if (isListening())
        close();

    QList<QThread *> threads;
    {
        QMutexLocker lock(&m_mutex);
        threads = m_threads.values();
        m_threads.clear();
    }
    for (QThread *thread : std::as_const(threads)) {
        // Every blocking wait inside a connection is sliced, so the stop flag
        // is noticed within a quarter of a second; this bound is for a socket
        // wedged in the kernel, not for the loop itself.
        if (!thread->wait(10000)) {
            LOG_WARN(applog::cat::Http2) << "a connection thread did not stop - terminating it";
            thread->terminate();
            thread->wait(2000);
        }
        delete thread;
    }
    if (!threads.isEmpty())
        LOG_INFO(applog::cat::Http2) << "closed" << threads.size() << "connections";
}

void Http2Server::incomingConnection(qintptr descriptor)
{
    // Reap here rather than on a timer: this is the only place that runs on
    // the server thread often enough to matter, and a server with no new
    // connections has nothing to reap.
    int active = 0;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_threads.begin(); it != m_threads.end();) {
            QThread *thread = *it;
            if (thread->isFinished()) {
                it = m_threads.erase(it);
                thread->wait();
                delete thread;
            } else {
                ++active;
                ++it;
            }
        }
    }

    if (m_stopping.loadRelaxed() != 0 || active >= m_maxConnections) {
        // Refuse out loud.  Accepting and then starving the connection would
        // look to the operator like the network had gone, which is exactly
        // the wrong thing to go looking for.
        QTcpSocket socket;
        if (socket.setSocketDescriptor(descriptor)) {
            socket.write(serverPreface(kMaxConcurrentStreams));
            socket.write(goawayFrame(0, ErrRefusedStream,
                                     QByteArrayLiteral("server is at its connection limit")));
            socket.waitForBytesWritten(1000);
            socket.disconnectFromHost();
        }
        LOG_WARN(applog::cat::Http2) << "refused a connection -" << active << "of"
                                     << m_maxConnections << "already open";
        return;
    }

    const int id = ++m_nextConnectionId;
    Handler handler = m_handler;
    const int idleTimeoutMs = m_idleTimeoutMs;
    QThread *thread = QThread::create([this, descriptor, handler, idleTimeoutMs, id]() {
        ServerConnection connection(descriptor, this, handler, idleTimeoutMs, id);
        connection.run();
    });
    // Named so `thread apply all bt` under gdb reads as the connection table
    // rather than as a list of numbers.
    thread->setObjectName(QStringLiteral("http2-conn-%1").arg(id));
    {
        QMutexLocker lock(&m_mutex);
        m_threads.insert(thread);
        ++m_totalConnections;
    }
    thread->start();
}

void Http2Server::noteRequest()
{
    QMutexLocker lock(&m_mutex);
    ++m_totalRequests;
}

int Http2Server::activeConnections() const
{
    QMutexLocker lock(&m_mutex);
    int active = 0;
    for (QThread *thread : m_threads) {
        if (!thread->isFinished())
            ++active;
    }
    return active;
}

quint64 Http2Server::totalConnections() const
{
    QMutexLocker lock(&m_mutex);
    return m_totalConnections;
}

quint64 Http2Server::totalRequests() const
{
    QMutexLocker lock(&m_mutex);
    return m_totalRequests;
}

} // namespace http2
