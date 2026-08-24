#include "UpstreamPool.h"

#include "core/Logger.h"

UpstreamPool::UpstreamPool(int lanes, const QString &target, const QString &token)
    : m_target(target), m_free(qMax(1, lanes))
{
    const int count = qMax(1, lanes);
    for (int i = 0; i < count; ++i) {
        m_lanes.append(new RpcLane(QStringLiteral("relay-lane-%1").arg(i), target, token));
        m_available.append(i);
    }
    LOG_INFO(applog::cat::Rpc) << "relay pool:" << count << "lanes to" << target;
}

UpstreamPool::~UpstreamPool()
{
    qDeleteAll(m_lanes);
}

grpc::Status UpstreamPool::call(const std::function<grpc::Status(AsrClient &)> &work)
{
    m_free.acquire();
    int index = 0;
    {
        QMutexLocker lock(&m_mutex);
        // Take the most recently released lane: its TCP connection is the one
        // most likely to still be open.
        index = m_available.takeLast();
    }
    const grpc::Status status = m_lanes.at(index)->call(work);
    // A transport failure leaves a socket that will fail again on the next
    // call; dropping it here means the lane redials rather than handing the
    // next caller the same dead connection.
    if (status.isTransport())
        m_lanes.at(index)->reset();
    {
        QMutexLocker lock(&m_mutex);
        m_available.append(index);
    }
    m_free.release();
    return status;
}

int UpstreamPool::busy() const
{
    QMutexLocker lock(&m_mutex);
    return m_lanes.size() - m_available.size();
}
