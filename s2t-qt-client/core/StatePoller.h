// Live state polling on its own thread and its own channel.
//
// SessionState grows with the meeting and must never block the audio upload
// loop.  Keeping this separate is what lets the UI refresh at its own cadence
// without coupling microphone backpressure to state serialization or network
// transfer.
#ifndef STATEPOLLER_H
#define STATEPOLLER_H

#include "proto/AsrSession.h"

#include <QAtomicInt>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

class StatePoller : public QThread
{
    Q_OBJECT

public:
    StatePoller(const QString &target, const QString &token, QObject *parent = nullptr);
    ~StatePoller() override;

    // Empty id parks the loop without tearing the thread down, so switching
    // meetings does not cost a reconnect.
    void setSession(const QString &sessionId);
    void requestStop();

signals:
    void stateReceived(const asr::StateResponse &state, double pollMs);
    void pollFailed(const QString &message);

protected:
    void run() override;

private:
    QString m_target;
    QString m_token;
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    QString m_sessionId;
    QAtomicInt m_stop{0};
};

#endif // STATEPOLLER_H
