// Thread pool for request-scoped RPCs (review, audio, edit, enrolment, ...).
//
// Each lane owns its own AsrClient, i.e. its own TCP connection, because a
// gRPC channel here is single-threaded by construction.  Three lanes is the
// smallest number that keeps a 120-second EnrollSpeaker from blocking the
// session picker and the model-status poll behind it.
//
// The live audio push and the live state poll deliberately do NOT go through
// this pool - they own dedicated channels of their own, so a slow review
// query can never sit in front of a 160 ms audio packet.
#ifndef RPCEXECUTOR_H
#define RPCEXECUTOR_H

#include "Logger.h"
#include "../grpc/AsrClient.h"

#include <QList>
#include <QObject>
#include <QThread>

#include <functional>

class RpcExecutor : public QObject
{
    Q_OBJECT

public:
    explicit RpcExecutor(QObject *parent = nullptr);
    ~RpcExecutor() override;

    void configure(const QString &target, const QString &token);

    // Runs `work` on a pool thread and delivers `done` on `receiver`'s thread.
    // If `receiver` is destroyed first, Qt drops the queued result, so a
    // dialog that closes mid-flight cannot be written to after deletion.
    template <typename T>
    void call(QObject *receiver,
              std::function<grpc::Status(AsrClient &, T &)> work,
              std::function<void(const grpc::Status &, const T &)> done)
    {
        if (m_lanes.isEmpty()) {
            LOG_ERROR(applog::cat::Rpc) << "no lanes available - dropping the RPC";
            return;
        }
        const int index = m_next % m_lanes.size();
        Lane &lane = m_lanes[index];
        m_next = (m_next + 1) % m_lanes.size();
        AsrClient *client = lane.client;
        // Which method it is gets logged inside grpc::Channel::invoke, where
        // the path is actually known; this only records the hand-off.
        LOG_TRACE(applog::cat::Rpc) << "dispatching to rpc-lane-" << index;
        QMetaObject::invokeMethod(
            lane.anchor,
            [client, receiver, work, done]() {
                T result;
                const grpc::Status status = work(*client, result);
                QMetaObject::invokeMethod(
                    receiver, [done, status, result]() { done(status, result); },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

private:
    struct Lane
    {
        QThread *thread = nullptr;
        QObject *anchor = nullptr;
        AsrClient *client = nullptr;
    };

    QList<Lane> m_lanes;
    int m_next = 0;
};

#endif // RPCEXECUTOR_H
