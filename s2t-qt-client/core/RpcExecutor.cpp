#include "RpcExecutor.h"

#include "core/Logger.h"

namespace {
const int kLaneCount = 3;
}

RpcExecutor::RpcExecutor(QObject *parent) : QObject(parent)
{
    for (int i = 0; i < kLaneCount; ++i) {
        Lane lane;
        lane.thread = new QThread(this);
        lane.thread->setObjectName(QStringLiteral("rpc-lane-%1").arg(i));
        lane.anchor = new QObject();
        lane.client = new AsrClient(QString(), QString());
        lane.anchor->moveToThread(lane.thread);
        // The client is owned by the lane, not parented into the object tree:
        // it must be destroyed on the lane's own thread, after that thread has
        // finished, which the connection below arranges.
        QObject::connect(lane.thread, &QThread::finished, lane.anchor, &QObject::deleteLater);
        AsrClient *client = lane.client;
        QObject::connect(lane.thread, &QThread::finished, lane.anchor,
                         [client]() { delete client; });
        lane.thread->start();
        m_lanes.append(lane);
    }
    LOG_INFO(applog::cat::Rpc) << "started" << kLaneCount
                               << "RPC lanes, one connection each";
}

RpcExecutor::~RpcExecutor()
{
    LOG_INFO(applog::cat::Rpc) << "shutting down" << m_lanes.size() << "RPC lanes";
    for (Lane &lane : m_lanes) {
        lane.thread->quit();
        // A lane can be mid-call on a long RPC (EnrollSpeaker allows 120 s).
        // Wait long enough for a normal call to land, then stop waiting: at
        // shutdown a stuck socket must not hold the whole application open.
        if (!lane.thread->wait(5000)) {
            LOG_WARN(applog::cat::Rpc)
                << "lane" << lane.thread->objectName()
                << "still stuck in an RPC after 5 s - forcing terminate()";
            lane.thread->terminate();
        }
    }
    m_lanes.clear();
}

void RpcExecutor::configure(const QString &target, const QString &token)
{
    LOG_INFO(applog::cat::Rpc) << "pointing" << m_lanes.size() << "RPC lanes at" << target;
    for (Lane &lane : m_lanes) {
        AsrClient *client = lane.client;
        QMetaObject::invokeMethod(
            lane.anchor, [client, target, token]() { client->setTarget(target, token); },
            Qt::QueuedConnection);
    }
}
