#include "RpcLane.h"

#include "core/Logger.h"

#include <QThread>

RpcLane::RpcLane(const QString &name, const QString &target, const QString &token) : m_name(name)
{
    m_thread = new QThread();
    m_thread->setObjectName(name);
    m_anchor = new QObject();
    m_anchor->moveToThread(m_thread);
    m_thread->start();

    // Created *inside* the lane thread, which is the whole point of this
    // class - see the header.
    QMetaObject::invokeMethod(
        m_anchor, [this, target, token]() { m_client = new AsrClient(target, token); },
        Qt::BlockingQueuedConnection);
    LOG_DEBUG(applog::cat::Rpc) << "lane" << name << "ready for" << target;
}

RpcLane::~RpcLane()
{
    if (m_anchor) {
        QMetaObject::invokeMethod(
            m_anchor,
            [this]() {
                delete m_client;
                m_client = nullptr;
            },
            Qt::BlockingQueuedConnection);
        delete m_anchor;
        m_anchor = nullptr;
    }
    if (m_thread) {
        m_thread->quit();
        if (!m_thread->wait(10000)) {
            LOG_WARN(applog::cat::Rpc) << "lane" << m_name << "did not stop - terminating it";
            m_thread->terminate();
            m_thread->wait(2000);
        }
        delete m_thread;
        m_thread = nullptr;
    }
}

grpc::Status RpcLane::call(const std::function<grpc::Status(AsrClient &)> &work)
{
    grpc::Status status;
    if (!m_anchor || !m_thread || !m_thread->isRunning()) {
        status.code = grpc::Unavailable;
        status.message = QStringLiteral("kênh %1 đã đóng").arg(m_name);
        return status;
    }
    QMetaObject::invokeMethod(
        m_anchor,
        [this, &work, &status]() {
            if (m_client)
                status = work(*m_client);
            else {
                status.code = grpc::Unavailable;
                status.message = QStringLiteral("kênh %1 chưa sẵn sàng").arg(m_name);
            }
        },
        Qt::BlockingQueuedConnection);
    return status;
}

void RpcLane::reset()
{
    if (!m_anchor)
        return;
    QMetaObject::invokeMethod(
        m_anchor,
        [this]() {
            if (m_client)
                m_client->reset();
        },
        Qt::BlockingQueuedConnection);
}
