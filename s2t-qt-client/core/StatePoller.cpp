#include "StatePoller.h"

#include "core/Logger.h"
#include "grpc/AsrClient.h"

#include <QElapsedTimer>
#include <QMutexLocker>

namespace {
const int kPollIntervalMs = 200;
const int kPollTimeoutMs = 20000;
}

StatePoller::StatePoller(const QString &target, const QString &token, QObject *parent)
    : QThread(parent), m_target(target), m_token(token)
{
    setObjectName(QStringLiteral("state-poller"));
}

StatePoller::~StatePoller()
{
    requestStop();
    if (!wait(5000)) {
        LOG_ERROR(applog::cat::Poll) << "state-poller did not stop within 5 s - forcing terminate()";
        terminate();
    }
}

void StatePoller::setSession(const QString &sessionId)
{
    QMutexLocker lock(&m_mutex);
    if (sessionId != m_sessionId) {
        LOG_INFO(applog::cat::Poll)
            << "poller switching to session"
            << (sessionId.isEmpty() ? QStringLiteral("(parked)") : sessionId);
    }
    m_sessionId = sessionId;
    m_wake.wakeAll();
}

void StatePoller::requestStop()
{
    m_stop.storeRelease(1);
    QMutexLocker lock(&m_mutex);
    m_wake.wakeAll();
}

void StatePoller::run()
{
    AsrClient client(m_target, m_token);
    LOG_INFO(applog::cat::Poll) << "state-poller thread running - target=" << m_target
                                << "interval" << kPollIntervalMs << "ms";
    while (!m_stop.loadAcquire()) {
        QString sessionId;
        {
            QMutexLocker lock(&m_mutex);
            sessionId = m_sessionId;
            if (sessionId.isEmpty()) {
                m_wake.wait(&m_mutex, QDeadlineTimer(kPollIntervalMs));
                continue;
            }
        }

        QElapsedTimer clock;
        clock.start();
        asr::SessionRequest request;
        request.sessionId = sessionId;
        asr::StateResponse response;
        const grpc::Status status = client.getLiveState(request, &response, kPollTimeoutMs);
        const double pollMs = double(clock.nsecsElapsed()) / 1e6;
        if (!status.ok()) {
            // A poll failure is reported but never fatal: the audio path has
            // its own channel and its own retry rules, and a session must not
            // end because a read-side query timed out.
            if (status.isTransport())
                client.reset();
            // The controller keeps a repeated failure from spamming the log;
            // here only the transport-level reset is worth a line.
            LOG_TRACE(applog::cat::Poll) << "get_live_state failed:" << status.toString();
            emit pollFailed(status.toString());
        } else {
            emit stateReceived(response, pollMs);
        }

        QMutexLocker lock(&m_mutex);
        if (m_stop.loadAcquire())
            break;
        m_wake.wait(&m_mutex, QDeadlineTimer(kPollIntervalMs));
    }
    LOG_INFO(applog::cat::Poll) << "state-poller thread stopped";
}
