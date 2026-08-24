// A small pool of upstream lanes for relayed, request-scoped RPCs.
//
// Everything the Server buffer does not buffer - review, audio playback,
// edits, enrolment, trace, model status - is a straight relay, and a relay is
// one upstream call per client call.  A pool bounds how many of those may be
// in flight at once and keeps their connections warm between calls.
//
// The live audio path and the live-state read deliberately do NOT come through
// here.  Each SessionBuffer owns dedicated lanes for those, for the same
// reason the client kept them off its own RPC pool: a 120-second EnrollSpeaker
// must never sit in front of a 160 ms audio packet.
#ifndef UPSTREAMPOOL_H
#define UPSTREAMPOOL_H

#include "RpcLane.h"

#include <QList>
#include <QMutex>
#include <QSemaphore>
#include <QString>

#include <functional>

class UpstreamPool
{
public:
    UpstreamPool(int lanes, const QString &target, const QString &token);
    ~UpstreamPool();

    UpstreamPool(const UpstreamPool &) = delete;
    UpstreamPool &operator=(const UpstreamPool &) = delete;

    // Waits for a free lane, then runs `work` on it.  There is no timeout on
    // the wait for a lane, on purpose: the call `work` is about to make has its
    // own deadline, so queueing here shows up as latency on that call rather
    // than as a second failure mode with its own error message.
    grpc::Status call(const std::function<grpc::Status(AsrClient &)> &work);

    QString target() const { return m_target; }
    int lanes() const { return m_lanes.size(); }
    // How many lanes are in use right now.  A pool permanently at its limit is
    // the signal that relayed calls are queueing behind each other, so the
    // admin RPC reports it.
    int busy() const;

private:
    QString m_target;
    QList<RpcLane *> m_lanes;
    QSemaphore m_free;
    mutable QMutex m_mutex;
    QList<int> m_available;
};

#endif // UPSTREAMPOOL_H
