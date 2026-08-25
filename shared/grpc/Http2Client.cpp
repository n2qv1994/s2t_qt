#include "Http2Client.h"

#include "core/Logger.h"

#include <QTcpSocket>

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

const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// Big enough that a multi-megabyte get_review_state never stalls on flow
// control before the first WINDOW_UPDATE replenishment round-trip.
const quint32 kOurInitialWindow = 8u * 1024u * 1024u;
const quint32 kOurConnectionWindowBump = 8u * 1024u * 1024u;

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

int remainingMs(const QElapsedTimer &clock, int timeoutMs)
{
    if (timeoutMs <= 0)
        return 0;
    const qint64 left = qint64(timeoutMs) - clock.elapsed();
    return left <= 0 ? 0 : int(left);
}

} // namespace

// Padding and the optional priority block sit around the header fragment and
// the DATA payload rather than inside them.  Both shapes are stripped in two
// places now - the unary path and the streaming one - so they live here once:
// getting either off by a byte corrupts an HPACK block or a proto message in a
// way that reads as a codec bug somewhere far from the cause.
static bool stripHeaderPadding(quint8 flags, QByteArray *fragment, QString *error)
{
    int cut = 0;
    int padLength = 0;
    if (flags & FlagPadded) {
        if (fragment->isEmpty()) {
            *error = QStringLiteral("malformed padded HEADERS frame");
            return false;
        }
        padLength = quint8(fragment->at(0));
        cut = 1;
    }
    if (flags & FlagPriority)
        cut += 5;
    if (fragment->size() < cut + padLength) {
        *error = QStringLiteral("malformed HEADERS frame");
        return false;
    }
    *fragment = fragment->mid(cut, fragment->size() - cut - padLength);
    return true;
}

static bool stripDataPadding(quint8 flags, QByteArray *payload, QString *error)
{
    if (!(flags & FlagPadded))
        return true;
    if (payload->isEmpty()) {
        *error = QStringLiteral("malformed padded DATA frame");
        return false;
    }
    const int padLength = quint8(payload->at(0));
    if (payload->size() < 1 + padLength) {
        *error = QStringLiteral("malformed DATA frame");
        return false;
    }
    *payload = payload->mid(1, payload->size() - 1 - padLength);
    return true;
}

Http2Connection::Http2Connection() = default;

Http2Connection::~Http2Connection()
{
    close();
}

bool Http2Connection::isOpen() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState && !m_goaway;
}

void Http2Connection::close()
{
    if (m_socket) {
        m_socket->abort();
        delete m_socket;
        m_socket = nullptr;
    }
    m_decoder = hpack::Decoder();
    m_nextStreamId = 1;
    m_peerMaxFrameSize = 16384;
    m_connSendWindow = 65535;
    m_streamSendWindow = 65535;
    m_peerInitialWindow = 65535;
    m_goaway = false;
    m_goawayReason.clear();
    m_stream = StreamState();
}

bool Http2Connection::open(const QString &host, quint16 port, int timeoutMs, QString *error)
{
    close();
    m_host = host;
    m_port = port;
    m_socket = new QTcpSocket();
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->connectToHost(host, port);
    if (!m_socket->waitForConnected(timeoutMs > 0 ? timeoutMs : 10000)) {
        *error = QStringLiteral("cannot reach %1:%2 - %3")
                     .arg(host).arg(port).arg(m_socket->errorString());
        LOG_WARN(applog::cat::Http2) << "TCP connect failed:" << *error;
        close();
        return false;
    }
    if (!sendPreface(error)) {
        LOG_WARN(applog::cat::Http2) << "sending the HTTP/2 preface failed:" << *error;
        close();
        return false;
    }
    LOG_DEBUG(applog::cat::Http2) << "TCP + HTTP/2 preface done with"
                                  << QStringLiteral("%1:%2").arg(host).arg(port);
    return true;
}

bool Http2Connection::sendPreface(QString *error)
{
    QElapsedTimer clock;
    clock.start();
    const int timeoutMs = 10000;

    QByteArray out(kPreface, int(sizeof(kPreface)) - 1);

    QByteArray settings;
    appendUint16(settings, SettingEnablePush);
    appendUint32(settings, 0);
    appendUint16(settings, SettingInitialWindowSize);
    appendUint32(settings, kOurInitialWindow);
    appendUint16(settings, SettingMaxFrameSize);
    appendUint32(settings, 1u << 14); // keep the spec default; nothing here needs bigger frames
    // Our encoder never indexes, so the peer's decoder table stays empty and
    // the advertised table size is irrelevant to us - but it must be a value
    // we can honour if the peer ever sends a size update.
    appendUint16(settings, SettingHeaderTableSize);
    appendUint32(settings, 4096);

    appendUint24(out, quint32(settings.size()));
    out.append(char(FrameSettings));
    out.append(char(0));
    appendUint32(out, 0);
    out.append(settings);

    // SETTINGS cannot raise the connection-level receive window (only the
    // per-stream one), so bump it explicitly or a large response stalls at
    // 64 KiB no matter what INITIAL_WINDOW_SIZE says.
    appendUint24(out, 4);
    out.append(char(FrameWindowUpdate));
    out.append(char(0));
    appendUint32(out, 0);
    appendUint32(out, kOurConnectionWindowBump);

    return writeAll(out, clock, timeoutMs, error);
}

bool Http2Connection::writeAll(const QByteArray &data, QElapsedTimer &clock, int timeoutMs,
                               QString *error)
{
    if (!m_socket) {
        *error = QStringLiteral("socket is closed");
        return false;
    }
    qint64 written = 0;
    while (written < data.size()) {
        const qint64 n = m_socket->write(data.constData() + written, data.size() - written);
        if (n < 0) {
            *error = QStringLiteral("write failed: %1").arg(m_socket->errorString());
            return false;
        }
        written += n;
        // waitForBytesWritten returning false with no error can just mean the
        // buffer already drained, so only treat a real socket error as fatal.
        if (!m_socket->waitForBytesWritten(remainingMs(clock, timeoutMs))) {
            if (m_socket->state() != QAbstractSocket::ConnectedState) {
                *error = QStringLiteral("connection lost while sending");
                return false;
            }
            if (remainingMs(clock, timeoutMs) <= 0) {
                *error = QStringLiteral("timed out sending request");
                return false;
            }
        }
    }
    return true;
}

bool Http2Connection::readExactly(int count, QByteArray *out, QElapsedTimer &clock, int timeoutMs,
                                  QString *error)
{
    if (!m_socket) {
        *error = QStringLiteral("socket is closed");
        return false;
    }
    out->clear();
    while (out->size() < count) {
        if (m_socket->bytesAvailable() > 0) {
            out->append(m_socket->read(count - out->size()));
            continue;
        }
        const int left = remainingMs(clock, timeoutMs);
        if (left <= 0) {
            *error = QStringLiteral("timed out waiting for server");
            return false;
        }
        if (!m_socket->waitForReadyRead(left)) {
            if (m_socket->state() != QAbstractSocket::ConnectedState)
                *error = QStringLiteral("connection closed by server");
            else
                *error = QStringLiteral("timed out waiting for server");
            return false;
        }
    }
    return true;
}

bool Http2Connection::readFrame(Frame *frame, QElapsedTimer &clock, int timeoutMs, QString *error)
{
    QByteArray header;
    if (!readExactly(9, &header, clock, timeoutMs, error))
        return false;
    frame->length = (quint32(quint8(header.at(0))) << 16) | (quint32(quint8(header.at(1))) << 8)
        | quint32(quint8(header.at(2)));
    frame->type = quint8(header.at(3));
    frame->flags = quint8(header.at(4));
    frame->streamId = readUint32(header, 5) & 0x7fffffffu;
    if (frame->length > (1u << 24)) {
        *error = QStringLiteral("server sent an oversized frame");
        return false;
    }
    frame->payload.clear();
    if (frame->length > 0 && !readExactly(int(frame->length), &frame->payload, clock, timeoutMs, error))
        return false;
    return true;
}

bool Http2Connection::sendFrame(quint8 type, quint8 flags, quint32 streamId,
                                const QByteArray &payload, QElapsedTimer &clock, int timeoutMs,
                                QString *error)
{
    QByteArray out;
    out.reserve(payload.size() + 9);
    appendUint24(out, quint32(payload.size()));
    out.append(char(type));
    out.append(char(flags));
    appendUint32(out, streamId & 0x7fffffffu);
    out.append(payload);
    return writeAll(out, clock, timeoutMs, error);
}

bool Http2Connection::sendSettingsAck(QElapsedTimer &clock, int timeoutMs, QString *error)
{
    return sendFrame(FrameSettings, FlagAck, 0, QByteArray(), clock, timeoutMs, error);
}

bool Http2Connection::sendWindowUpdate(quint32 streamId, quint32 increment, QElapsedTimer &clock,
                                       int timeoutMs, QString *error)
{
    if (increment == 0)
        return true;
    QByteArray payload;
    appendUint32(payload, increment & 0x7fffffffu);
    return sendFrame(FrameWindowUpdate, 0, streamId, payload, clock, timeoutMs, error);
}

bool Http2Connection::handleConnectionFrame(const Frame &frame, QElapsedTimer &clock, int timeoutMs,
                                            QString *error)
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
                if (value >= (1u << 14) && value <= ((1u << 24) - 1)) {
                    if (value != m_peerMaxFrameSize)
                        LOG_DEBUG(applog::cat::Http2) << "peer MAX_FRAME_SIZE ->" << value;
                    m_peerMaxFrameSize = value;
                }
                break;
            case SettingInitialWindowSize: {
                // A change retroactively adjusts every open stream's send
                // window by the delta (RFC 7540 6.9.2).
                const qint64 delta = qint64(value) - qint64(m_peerInitialWindow);
                m_peerInitialWindow = value;
                m_streamSendWindow += delta;
                break;
            }
            default:
                break;
            }
        }
        return sendSettingsAck(clock, timeoutMs, error);

    case FramePing:
        if (frame.flags & FlagAck)
            return true;
        return sendFrame(FramePing, FlagAck, 0, frame.payload, clock, timeoutMs, error);

    case FrameWindowUpdate: {
        if (frame.payload.size() < 4)
            return true;
        const qint64 increment = qint64(readUint32(frame.payload, 0) & 0x7fffffffu);
        if (frame.streamId == 0)
            m_connSendWindow += increment;
        else
            m_streamSendWindow += increment;
        return true;
    }

    case FrameGoaway: {
        m_goaway = true;
        quint32 code = 0;
        if (frame.payload.size() >= 8)
            code = readUint32(frame.payload, 4);
        QString detail = QString::fromUtf8(frame.payload.mid(8));
        m_goawayReason = detail.isEmpty()
            ? QStringLiteral("server closed the connection (GOAWAY %1)").arg(code)
            : QStringLiteral("server closed the connection (GOAWAY %1: %2)").arg(code).arg(detail);
        LOG_WARN(applog::cat::Http2) << "GOAWAY received from" << m_host << "-" << m_goawayReason;
        return true;
    }

    case FramePushPromise:
        // We advertise ENABLE_PUSH=0; a peer sending one is broken enough that
        // continuing on this connection is not safe.
        *error = QStringLiteral("server sent PUSH_PROMISE despite ENABLE_PUSH=0");
        LOG_ERROR(applog::cat::Http2) << *error;
        return false;

    default:
        return true;
    }
}

bool Http2Connection::awaitSendWindow(quint32 streamId, int needed, QElapsedTimer &clock,
                                      int timeoutMs, QString *error)
{
    bool stalled = false;
    while (m_connSendWindow < needed || m_streamSendWindow < needed) {
        if (!stalled) {
            stalled = true;
            // Worth seeing: it means the server is not draining what we send,
            // which shows up to the operator as growing upload latency.
            LOG_DEBUG(applog::cat::Http2)
                << "waiting for flow-control credit on stream" << streamId << "- need" << needed
                << "bytes, have conn=" << m_connSendWindow << "stream=" << m_streamSendWindow;
        }
        if (remainingMs(clock, timeoutMs) <= 0) {
            *error = QStringLiteral("timed out waiting for server flow-control credit");
            LOG_WARN(applog::cat::Http2) << *error << "- stream" << streamId;
            return false;
        }
        Frame frame;
        if (!readFrame(&frame, clock, timeoutMs, error))
            return false;
        if (frame.type == FrameRstStream && frame.streamId == streamId) {
            *error = QStringLiteral("server reset the stream while it was being sent");
            return false;
        }
        // On a long-lived stream the peer is answering *while* we push, so the
        // frames read here to find flow-control credit are usually its
        // responses.  Dropping them - which is what handleConnectionFrame does
        // with a DATA frame - would lose transcript the far side already sent.
        if (m_stream.open && frame.streamId == m_stream.id) {
            if (!absorbStreamFrame(frame, clock, timeoutMs, error))
                return false;
            continue;
        }
        if (!handleConnectionFrame(frame, clock, timeoutMs, error))
            return false;
        if (m_goaway) {
            *error = m_goawayReason;
            return false;
        }
    }
    return true;
}

bool Http2Connection::sendBody(quint32 streamId, const QByteArray &body, QElapsedTimer &clock,
                               int timeoutMs, QString *error)
{
    int offset = 0;
    // A zero-length body still needs one END_STREAM DATA frame; several RPCs
    // here (ModelStatusRequest, GetEnrollmentScriptRequest) serialise empty.
    do {
        const int chunk = qMin<int>(body.size() - offset, int(m_peerMaxFrameSize));
        if (chunk > 0 && !awaitSendWindow(streamId, chunk, clock, timeoutMs, error))
            return false;
        const bool last = (offset + chunk) >= body.size();
        if (!sendFrame(FrameData, last ? FlagEndStream : 0, streamId,
                       body.mid(offset, chunk), clock, timeoutMs, error))
            return false;
        m_connSendWindow -= chunk;
        m_streamSendWindow -= chunk;
        offset += chunk;
    } while (offset < body.size());
    return true;
}

bool Http2Connection::request(const QList<hpack::Header> &headers, const QByteArray &body,
                              int timeoutMs, Response *out, QString *error)
{
    if (!isOpen()) {
        *error = m_goaway ? m_goawayReason : QStringLiteral("connection is not open");
        return false;
    }
    QElapsedTimer clock;
    clock.start();

    const quint32 streamId = m_nextStreamId;
    m_nextStreamId += 2;
    m_streamSendWindow = qint64(m_peerInitialWindow);
    LOG_TRACE(applog::cat::Http2) << "stream" << streamId << "->"
                                  << QStringLiteral("%1:%2").arg(m_host).arg(m_port)
                                  << body.size() << "body bytes, deadline" << timeoutMs << "ms";

    const QByteArray block = hpack::Encoder::encode(headers);
    // CONTINUATION on the send side would only be needed for a header block
    // above the peer's max frame size; ours is a handful of short ASCII
    // headers, so a single HEADERS frame always suffices.
    if (block.size() > int(m_peerMaxFrameSize)) {
        *error = QStringLiteral("request headers do not fit one frame");
        return false;
    }
    if (!sendFrame(FrameHeaders, FlagEndHeaders, streamId, block, clock, timeoutMs, error))
        return false;
    if (!sendBody(streamId, body, clock, timeoutMs, error))
        return false;

    bool seenHeaders = false;
    bool done = false;
    QByteArray pendingHeaderBlock;
    bool pendingIsTrailer = false;
    bool awaitingContinuation = false;

    while (!done) {
        Frame frame;
        if (!readFrame(&frame, clock, timeoutMs, error))
            return false;

        if (awaitingContinuation && frame.type != FrameContinuation) {
            *error = QStringLiteral("server interleaved a frame inside a header block");
            return false;
        }

        if (frame.streamId != streamId) {
            // HPACK state is per connection, not per stream: skipping a header
            // block belonging to some other stream would desynchronise the
            // decoder for every later response on this connection.  We open
            // one stream at a time and disable push, so this cannot happen
            // against a conforming peer - fail loudly rather than continue
            // with a decoder that is quietly wrong from here on.
            if (frame.type == FrameHeaders || frame.type == FrameContinuation
                || frame.type == FramePushPromise) {
                *error = QStringLiteral("server sent headers for an unexpected stream");
                return false;
            }
            if (!handleConnectionFrame(frame, clock, timeoutMs, error))
                return false;
            if (m_goaway && !seenHeaders) {
                *error = m_goawayReason;
                return false;
            }
            continue;
        }

        switch (frame.type) {
        case FrameHeaders:
        case FrameContinuation: {
            QByteArray fragment = frame.payload;
            if (frame.type == FrameHeaders) {
                if (!stripHeaderPadding(frame.flags, &fragment, error))
                    return false;
                pendingIsTrailer = seenHeaders;
                pendingHeaderBlock.clear();
            }
            pendingHeaderBlock.append(fragment);
            awaitingContinuation = !(frame.flags & FlagEndHeaders);
            if (!awaitingContinuation) {
                QList<hpack::Header> decoded;
                if (!m_decoder.decode(pendingHeaderBlock, &decoded, error))
                    return false;
                if (pendingIsTrailer)
                    out->trailers = decoded;
                else
                    out->headers = decoded;
                seenHeaders = true;
                pendingHeaderBlock.clear();
            }
            if (!awaitingContinuation && (frame.flags & FlagEndStream))
                done = true;
            break;
        }

        case FrameData: {
            QByteArray payload = frame.payload;
            if (!stripDataPadding(frame.flags, &payload, error))
                return false;
            if (out->body.size() + payload.size() > kMaxResponseBytes) {
                *error = QStringLiteral("response exceeded %1 bytes").arg(kMaxResponseBytes);
                LOG_ERROR(applog::cat::Http2) << *error << "- aborting stream" << streamId;
                return false;
            }
            out->body.append(payload);
            // Replenish immediately rather than tracking a half-window
            // threshold: the whole frame has already been copied out, so the
            // credit is genuinely free again and a stalled reader is worse
            // than an extra 13-byte frame per DATA.
            if (frame.length > 0) {
                if (!sendWindowUpdate(0, frame.length, clock, timeoutMs, error))
                    return false;
                if (!(frame.flags & FlagEndStream)
                    && !sendWindowUpdate(streamId, frame.length, clock, timeoutMs, error))
                    return false;
            }
            if (frame.flags & FlagEndStream)
                done = true;
            break;
        }

        case FrameRstStream: {
            quint32 code = 0;
            if (frame.payload.size() >= 4)
                code = readUint32(frame.payload, 0);
            *error = QStringLiteral("server reset the stream (code %1)").arg(code);
            LOG_WARN(applog::cat::Http2)
                << "RST_STREAM on stream" << streamId << "code" << code << "-"
                << out->body.size() << "body bytes received so far";
            return false;
        }

        default:
            if (!handleConnectionFrame(frame, clock, timeoutMs, error))
                return false;
            break;
        }
    }
    return true;
}

// ---- long-lived streams ----------------------------------------------------

bool Http2Connection::frameReady() const
{
    if (!m_socket)
        return false;
    const qint64 available = m_socket->bytesAvailable();
    if (available < 9)
        return false;
    const QByteArray head = m_socket->peek(9);
    if (head.size() < 9)
        return false;
    const quint32 length = (quint32(quint8(head.at(0))) << 16) | (quint32(quint8(head.at(1))) << 8)
        | quint32(quint8(head.at(2)));
    return available >= qint64(9) + qint64(length);
}

bool Http2Connection::openStream(const QList<hpack::Header> &headers, int timeoutMs, QString *error)
{
    if (!isOpen()) {
        *error = m_goaway ? m_goawayReason : QStringLiteral("connection is not open");
        return false;
    }
    if (m_stream.open) {
        *error = QStringLiteral("a stream is already open on this connection");
        return false;
    }

    QElapsedTimer clock;
    clock.start();
    const quint32 streamId = m_nextStreamId;
    m_nextStreamId += 2;
    m_streamSendWindow = qint64(m_peerInitialWindow);

    const QByteArray block = hpack::Encoder::encode(headers);
    if (block.size() > int(m_peerMaxFrameSize)) {
        *error = QStringLiteral("request headers do not fit one frame");
        return false;
    }
    // No END_STREAM: that is the whole difference from request().  The far side
    // now knows more DATA is coming and may start answering at any time.
    if (!sendFrame(FrameHeaders, FlagEndHeaders, streamId, block, clock, timeoutMs, error))
        return false;

    m_stream = StreamState();
    m_stream.id = streamId;
    m_stream.open = true;
    LOG_DEBUG(applog::cat::Http2) << "stream" << streamId << "opened to"
                                  << QStringLiteral("%1:%2").arg(m_host).arg(m_port);
    return true;
}

bool Http2Connection::sendStreamData(const QByteArray &body, bool endStream, int timeoutMs,
                                     QString *error)
{
    if (!m_stream.open) {
        *error = QStringLiteral("no stream is open");
        return false;
    }
    if (m_stream.sendClosed) {
        *error = QStringLiteral("the stream has already been half-closed");
        return false;
    }
    QElapsedTimer clock;
    clock.start();

    int offset = 0;
    do {
        const int chunk = qMin<int>(body.size() - offset, int(m_peerMaxFrameSize));
        if (chunk > 0 && !awaitSendWindow(m_stream.id, chunk, clock, timeoutMs, error))
            return false;
        const bool last = (offset + chunk) >= body.size();
        if (!sendFrame(FrameData, (last && endStream) ? FlagEndStream : 0, m_stream.id,
                       body.mid(offset, chunk), clock, timeoutMs, error))
            return false;
        m_connSendWindow -= chunk;
        m_streamSendWindow -= chunk;
        offset += chunk;
    } while (offset < body.size());

    if (endStream) {
        m_stream.sendClosed = true;
        LOG_DEBUG(applog::cat::Http2) << "stream" << m_stream.id << "half-closed (no more audio)";
    }
    return true;
}

bool Http2Connection::absorbStreamFrame(const Frame &frame, QElapsedTimer &clock, int timeoutMs,
                                        QString *error)
{
    if (frame.streamId != m_stream.id) {
        // Same rule as the unary path: the HPACK decoder is per connection, so
        // a header block for a stream we are not tracking cannot be skipped -
        // it would leave the decoder quietly wrong from here on.
        if (frame.type == FrameHeaders || frame.type == FrameContinuation
            || frame.type == FramePushPromise) {
            *error = QStringLiteral("server sent headers for an unexpected stream");
            return false;
        }
        return handleConnectionFrame(frame, clock, timeoutMs, error);
    }

    if (m_stream.awaitingContinuation && frame.type != FrameContinuation) {
        *error = QStringLiteral("server interleaved a frame inside a header block");
        return false;
    }

    switch (frame.type) {
    case FrameHeaders:
    case FrameContinuation: {
        QByteArray fragment = frame.payload;
        if (frame.type == FrameHeaders) {
            if (!stripHeaderPadding(frame.flags, &fragment, error))
                return false;
            m_stream.pendingIsTrailer = m_stream.seenHeaders;
            m_stream.pendingHeaderBlock.clear();
        }
        m_stream.pendingHeaderBlock.append(fragment);
        m_stream.awaitingContinuation = !(frame.flags & FlagEndHeaders);
        if (!m_stream.awaitingContinuation) {
            QList<hpack::Header> decoded;
            if (!m_decoder.decode(m_stream.pendingHeaderBlock, &decoded, error))
                return false;
            if (m_stream.pendingIsTrailer)
                m_stream.trailers = decoded;
            else
                m_stream.headers = decoded;
            m_stream.seenHeaders = true;
            m_stream.pendingHeaderBlock.clear();
            if (frame.flags & FlagEndStream)
                m_stream.recvClosed = true;
        }
        return true;
    }

    case FrameData: {
        QByteArray payload = frame.payload;
        if (!stripDataPadding(frame.flags, &payload, error))
            return false;
        // The cap is on the *undelivered* backlog, not on the meeting: the
        // caller drains whole messages out of this buffer as they complete, so
        // reaching it means the far side is answering faster than we read.
        if (m_stream.buffer.size() + payload.size() > kMaxResponseBytes) {
            *error = QStringLiteral("stream backlog exceeded %1 bytes").arg(kMaxResponseBytes);
            LOG_ERROR(applog::cat::Http2) << *error << "- aborting stream" << m_stream.id;
            return false;
        }
        m_stream.buffer.append(payload);
        if (frame.length > 0) {
            if (!sendWindowUpdate(0, frame.length, clock, timeoutMs, error))
                return false;
            if (!(frame.flags & FlagEndStream)
                && !sendWindowUpdate(m_stream.id, frame.length, clock, timeoutMs, error))
                return false;
        }
        if (frame.flags & FlagEndStream)
            m_stream.recvClosed = true;
        return true;
    }

    case FrameRstStream: {
        quint32 code = 0;
        if (frame.payload.size() >= 4)
            code = readUint32(frame.payload, 0);
        *error = QStringLiteral("server reset the stream (code %1)").arg(code);
        LOG_WARN(applog::cat::Http2) << "RST_STREAM on stream" << m_stream.id << "code" << code
                                     << "-" << m_stream.buffer.size() << "bytes still unread";
        // Nothing more will arrive, and there is nothing left to cancel.
        m_stream.recvClosed = true;
        m_stream.open = false;
        return false;
    }

    default:
        return handleConnectionFrame(frame, clock, timeoutMs, error);
    }
}

bool Http2Connection::pumpStream(int waitMs, QString *error)
{
    if (!m_stream.open || m_stream.recvClosed)
        return true;
    if (!m_socket) {
        *error = QStringLiteral("socket is closed");
        return false;
    }

    QElapsedTimer clock;
    clock.start();
    // Once a frame header is in hand its payload is already buffered too (that
    // is what frameReady() checked), so this budget only ever covers the small
    // writes a frame can provoke - a SETTINGS ack or a WINDOW_UPDATE.
    const int replyBudget = 5000;

    while (!m_stream.recvClosed) {
        if (!frameReady()) {
            const qint64 spent = clock.elapsed();
            const int left = qint64(waitMs) > spent ? int(qint64(waitMs) - spent) : 0;
            // waitForReadyRead(0) still asks the OS, which is what makes a poll
            // see bytes that arrived but have not been copied into Qt's buffer.
            if (!m_socket->waitForReadyRead(left)
                && m_socket->state() != QAbstractSocket::ConnectedState) {
                *error = QStringLiteral("connection closed by server");
                return false;
            }
            if (!frameReady()) {
                // Nothing to read is the normal answer to a poll, not a
                // failure: the caller decides whether it wants to wait longer.
                if (left <= 0)
                    break;
                continue;
            }
        }

        QElapsedTimer frameClock;
        frameClock.start();
        Frame frame;
        if (!readFrame(&frame, frameClock, replyBudget, error))
            return false;
        if (!absorbStreamFrame(frame, frameClock, replyBudget, error))
            return false;
        if (m_goaway && !m_stream.recvClosed) {
            *error = m_goawayReason;
            return false;
        }
    }
    return true;
}

void Http2Connection::closeStream()
{
    if (m_stream.open && !m_stream.recvClosed && m_socket
        && m_socket->state() == QAbstractSocket::ConnectedState) {
        QElapsedTimer clock;
        clock.start();
        QByteArray payload;
        appendUint32(payload, 8); // CANCEL
        QString error;
        // Best effort.  A failure here only means the peer will notice when the
        // connection goes, and the connection is the caller's to keep or drop.
        sendFrame(FrameRstStream, 0, m_stream.id, payload, clock, 2000, &error);
        LOG_DEBUG(applog::cat::Http2) << "stream" << m_stream.id << "cancelled";
    }
    m_stream = StreamState();
}

} // namespace http2
