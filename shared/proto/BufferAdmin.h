// C++ mirror of buffer_admin.proto (package s2t.buffer.v1).
//
// This one is *ours*.  ProductASRService and SpeakerRegistryService describe
// the inference tier and s2t-qt-server only relays them; the three RPCs here
// describe the Server buffer itself - how much audio is waiting, how far
// behind the upstream pipeline is, and whether it is reachable at all.
//
// Written in the same hand-mirrored style as the other two, and for the same
// reason (no protoc on either build host).  The appendix of
// docs/danh-sach-api.md carries the .proto these numbers come from; changing
// a number here is a change to that document in the same commit.
#ifndef BUFFERADMIN_H
#define BUFFERADMIN_H

#include "ProtoWire.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace buf {

struct PingRequest
{
    // Unix seconds as the caller saw them.  Echoed back untouched so a client
    // can measure the round trip without trusting the server's clock.
    double clientTs = 0.0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct PingResponse
{
    double clientTs = 0.0;
    double serverTs = 0.0;
    QString serverVersion;
    // Whether the buffer can currently reach the inference tier.  A buffer
    // that answers while the pipeline is down is a real state, not a fault:
    // audio keeps being accepted and spooled.  The client shows the two
    // separately rather than collapsing them into one "connected" light.
    bool upstreamReady = false;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct UpstreamStatus
{
    QString target;
    bool reachable = false;
    double latencyMs = 0.0;
    QString detail;
    double checkedAt = 0.0;
    quint64 consecutiveFailures = 0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct BufferedSession
{
    // The id the client knows.  It is the upstream one as well: the buffer
    // relays start_session rather than inventing an id of its own, so a
    // session can be looked up on the adapter by the same string an operator
    // reads off this screen.
    QString sessionId;
    QString client;
    QString title;
    double startedAt = 0.0;
    double updatedAt = 0.0;
    bool running = false;
    // ACKed to the client, i.e. durably in this buffer.  Never inferred.
    quint64 acceptedPackets = 0;
    quint64 acceptedBytes = 0;
    // Actually handed to the inference tier and answered.
    quint64 forwardedPackets = 0;
    quint64 forwardedBytes = 0;
    quint64 pendingPackets = 0;
    quint64 pendingBytes = 0;
    quint64 spooledBytes = 0;
    quint64 droppedPackets = 0;
    quint64 retries = 0;
    // Audio seconds accepted from the client but not yet acknowledged
    // upstream.  This is the number that says whether the pipeline is keeping
    // up, and it is what the client's dashboard shows as "trễ".
    double lagSec = 0.0;
    double forwardP50Ms = 0.0;
    double forwardP95Ms = 0.0;
    QString lastError;
    double lastErrorAt = 0.0;
    quint64 statePolls = 0;
    // Age of the cached live state.  Many clients watching one meeting cost
    // one upstream poll, so this is how stale the answer they share may be.
    double stateAgeSec = 0.0;
    quint64 stateReaders = 0;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct BufferStatusRequest
{
    // Empty means every session.
    QString sessionId;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct BufferStatusResponse
{
    QString serverVersion;
    double uptimeSec = 0.0;
    UpstreamStatus upstream;
    quint32 activeConnections = 0;
    quint64 totalConnections = 0;
    quint64 totalCalls = 0;
    quint64 rejectedCalls = 0;
    quint64 queueCapacityBytes = 0;
    quint64 queueUsedBytes = 0;
    QString spoolDir;
    bool spoolEnabled = false;
    QList<BufferedSession> sessions;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct BufferSessionsRequest
{
    quint32 limit = 0;
    bool includeFinished = false;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

struct BufferSessionsResponse
{
    QList<BufferedSession> sessions;
    QByteArray serialize() const;
    void parse(pw::Reader &reader);
};

} // namespace buf

#endif // BUFFERADMIN_H
