#include "BufferAdmin.h"

namespace buf {

using pw::Reader;
using pw::Writer;
using pw::WireType;

QByteArray PingRequest::serialize() const
{
    Writer w;
    w.putDouble(1, clientTs);
    return w.take();
}

void PingRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: clientTs = r.readDouble(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray PingResponse::serialize() const
{
    Writer w;
    w.putDouble(1, clientTs);
    w.putDouble(2, serverTs);
    w.putString(3, serverVersion);
    w.putBool(4, upstreamReady);
    return w.take();
}

void PingResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: clientTs = r.readDouble(); break;
        case 2: serverTs = r.readDouble(); break;
        case 3: serverVersion = r.readString(); break;
        case 4: upstreamReady = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray UpstreamStatus::serialize() const
{
    Writer w;
    w.putString(1, target);
    w.putBool(2, reachable);
    w.putDouble(3, latencyMs);
    w.putString(4, detail);
    w.putDouble(5, checkedAt);
    w.putUInt64(6, consecutiveFailures);
    return w.take();
}

void UpstreamStatus::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: target = r.readString(); break;
        case 2: reachable = r.readBool(); break;
        case 3: latencyMs = r.readDouble(); break;
        case 4: detail = r.readString(); break;
        case 5: checkedAt = r.readDouble(); break;
        case 6: consecutiveFailures = r.readUInt64(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray BufferedSession::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    w.putString(2, client);
    w.putString(3, title);
    w.putDouble(4, startedAt);
    w.putDouble(5, updatedAt);
    w.putBool(6, running);
    w.putUInt64(7, acceptedPackets);
    w.putUInt64(8, acceptedBytes);
    w.putUInt64(9, forwardedPackets);
    w.putUInt64(10, forwardedBytes);
    w.putUInt64(11, pendingPackets);
    w.putUInt64(12, pendingBytes);
    w.putUInt64(13, spooledBytes);
    w.putUInt64(14, droppedPackets);
    w.putUInt64(15, retries);
    w.putDouble(16, lagSec);
    w.putDouble(17, forwardP50Ms);
    w.putDouble(18, forwardP95Ms);
    w.putString(19, lastError);
    w.putDouble(20, lastErrorAt);
    w.putUInt64(21, statePolls);
    w.putDouble(22, stateAgeSec);
    w.putUInt64(23, stateReaders);
    return w.take();
}

void BufferedSession::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        case 2: client = r.readString(); break;
        case 3: title = r.readString(); break;
        case 4: startedAt = r.readDouble(); break;
        case 5: updatedAt = r.readDouble(); break;
        case 6: running = r.readBool(); break;
        case 7: acceptedPackets = r.readUInt64(); break;
        case 8: acceptedBytes = r.readUInt64(); break;
        case 9: forwardedPackets = r.readUInt64(); break;
        case 10: forwardedBytes = r.readUInt64(); break;
        case 11: pendingPackets = r.readUInt64(); break;
        case 12: pendingBytes = r.readUInt64(); break;
        case 13: spooledBytes = r.readUInt64(); break;
        case 14: droppedPackets = r.readUInt64(); break;
        case 15: retries = r.readUInt64(); break;
        case 16: lagSec = r.readDouble(); break;
        case 17: forwardP50Ms = r.readDouble(); break;
        case 18: forwardP95Ms = r.readDouble(); break;
        case 19: lastError = r.readString(); break;
        case 20: lastErrorAt = r.readDouble(); break;
        case 21: statePolls = r.readUInt64(); break;
        case 22: stateAgeSec = r.readDouble(); break;
        case 23: stateReaders = r.readUInt64(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray BufferStatusRequest::serialize() const
{
    Writer w;
    w.putString(1, sessionId);
    return w.take();
}

void BufferStatusRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: sessionId = r.readString(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray BufferStatusResponse::serialize() const
{
    Writer w;
    w.putString(1, serverVersion);
    w.putDouble(2, uptimeSec);
    w.putSubMessage(3, upstream.serialize());
    w.putUInt32(4, activeConnections);
    w.putUInt64(5, totalConnections);
    w.putUInt64(6, totalCalls);
    w.putUInt64(7, rejectedCalls);
    w.putUInt64(8, queueCapacityBytes);
    w.putUInt64(9, queueUsedBytes);
    w.putString(10, spoolDir);
    w.putBool(11, spoolEnabled);
    w.putRepeatedMessage(12, sessions);
    return w.take();
}

void BufferStatusResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: serverVersion = r.readString(); break;
        case 2: uptimeSec = r.readDouble(); break;
        case 3: r.readMessageInto(&upstream); break;
        case 4: activeConnections = r.readUInt32(); break;
        case 5: totalConnections = r.readUInt64(); break;
        case 6: totalCalls = r.readUInt64(); break;
        case 7: rejectedCalls = r.readUInt64(); break;
        case 8: queueCapacityBytes = r.readUInt64(); break;
        case 9: queueUsedBytes = r.readUInt64(); break;
        case 10: spoolDir = r.readString(); break;
        case 11: spoolEnabled = r.readBool(); break;
        case 12: r.appendMessage(&sessions); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray BufferSessionsRequest::serialize() const
{
    Writer w;
    w.putUInt32(1, limit);
    w.putBool(2, includeFinished);
    return w.take();
}

void BufferSessionsRequest::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: limit = r.readUInt32(); break;
        case 2: includeFinished = r.readBool(); break;
        default: r.skip(type); break;
        }
    }
}

QByteArray BufferSessionsResponse::serialize() const
{
    Writer w;
    w.putRepeatedMessage(1, sessions);
    return w.take();
}

void BufferSessionsResponse::parse(Reader &r)
{
    int field = 0;
    WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: r.appendMessage(&sessions); break;
        default: r.skip(type); break;
        }
    }
}

} // namespace buf
