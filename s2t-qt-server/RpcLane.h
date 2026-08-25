// One upstream channel that lives on its own thread, and a blocking way to
// use it from any other thread.
//
// This exists because of a Qt rule that is easy to forget: QTcpSocket has
// thread affinity.  grpc::Channel opens its socket lazily inside the first
// call, so a channel shared between two HTTP/2 connection threads would end up
// with a socket created in one thread and read from another - which works
// often enough to pass a smoke test and is undefined behaviour all the same.
//
// So the channel never leaves its thread.  Callers hand work *to* it through a
// BlockingQueuedConnection and wait, which is the same shape the client's
// RpcExecutor uses, minus the callback: a handler here has nothing useful to
// do while its upstream call is in flight, because it owes an answer on the
// connection it was called on.
#ifndef RPCLANE_H
#define RPCLANE_H

#include "grpc/GrpcChannel.h"

#include <QObject>
#include <QString>

#include <functional>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

class RpcLane
{
public:
    // `name` becomes the thread name, so `thread apply all bt` under gdb reads
    // as the lane table rather than as a list of numbers.
    RpcLane(const QString &name, const QString &target, const QString &token);
    ~RpcLane();

    RpcLane(const RpcLane &) = delete;
    RpcLane &operator=(const RpcLane &) = delete;

    // Runs `work` on the lane's thread and returns what it returned.  Blocks
    // the caller for the length of the upstream call, deliberately: the caller
    // is an HTTP/2 connection thread that owes a reply on that same call.
    grpc::Status call(const std::function<grpc::Status(grpc::Channel &)> &work);

    // Drops the TCP connection so the next call dials again.  Used after a
    // transport failure instead of waiting out HTTP/2's own reconnect.
    void reset();

    QString name() const { return m_name; }

private:
    QString m_name;
    QThread *m_thread = nullptr;
    QObject *m_anchor = nullptr;
    grpc::Channel *m_channel = nullptr;
};

#endif // RPCLANE_H
