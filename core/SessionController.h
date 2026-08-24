// Main-thread façade over everything with a thread behind it: capture, the
// session worker, the live-state poller and the RPC pool.
//
// The UI talks only to this.  It owns the physical input device for the whole
// lifetime of a session, which is what stops a denoise A/B capture and a live
// meeting from trying to open the same microphone at once.
#ifndef SESSIONCONTROLLER_H
#define SESSIONCONTROLLER_H

#include "AppConfig.h"
#include "AudioQueue.h"
#include "RpcExecutor.h"
#include "SessionTypes.h"
#include "TranscriptModel.h"
#include "SessionWorker.h"
#include "../audio/AudioCapture.h"
#include "../audio/MicDenoise.h"

#include <QObject>
#include <QString>
#include <QTimer>

class StatePoller;
QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

class SessionController : public QObject
{
    Q_OBJECT

public:
    explicit SessionController(AppConfig *config, QObject *parent = nullptr);
    ~SessionController() override;

    RpcExecutor *rpc() { return m_rpc; }
    TranscriptModel &model() { return m_model; }
    const TranscriptModel &model() const { return m_model; }

    // Re-points every channel after the settings dialog changes the server.
    void applyConfig();

    bool isRunning() const { return m_worker != nullptr; }
    MicStatus micStatus() const { return m_micStatus; }
    double micStatusSince() const { return m_micStatusSince; }
    const QList<MicStatusTransition> &micStatusHistory() const { return m_micStatusHistory; }
    const SessionTelemetry &telemetry() const { return m_telemetry; }
    const QList<LatencySample> &latencyHistory() const { return m_latencyHistory; }
    const QList<FinishedSession> &finishedSessions() const { return m_finishedSessions; }
    QString sessionId() const { return m_sessionId; }
    QString deviceName() const { return m_deviceName; }
    QString deviceWarning() const { return m_deviceWarning; }
    QString errorText() const { return m_error; }
    QString speakerFilterLabel() const { return m_speakerFilter; }
    QStringList expectedSpeakers() const { return m_expectedSpeakers; }
    QString denoiseStateKey() const { return mic::denoiseStateKey(m_denoise); }
    QString sourceName() const { return m_sourceName; }
    bool connected() const { return m_connected; }
    double connectionLatencyMs() const { return m_connectionLatencyMs; }
    QString connectionDetail() const { return m_connectionDetail; }
    // The configured input, so a one-shot recorder (enrolment) opens the same
    // physical microphone the meeting would, not whatever the host defaults to.
    AudioDeviceChoice inputDevice() const;

public slots:
    // `restrict == false` omits expected_speakers entirely (match the whole
    // registry).  `restrict == true` with an empty list sends an explicit
    // empty allow-list, which means "assign no registered name" - a different
    // thing, and the difference is deliberate.
    void startMicrophone(bool restrict, const QStringList &expected, const SessionMeta &meta);
    void startFile(const QString &path, bool restrict, const QStringList &expected,
                   const SessionMeta &meta);
    void stop();
    void setPaused(bool paused);
    void setDenoise(bool enabled);
    void captureDenoiseAb(double seconds);
    void refreshConnection();

signals:
    void modelUpdated();
    void statusUpdated();
    void errorMessage(const QString &message);
    void notice(const QString &message);
    void sessionStarted(const QString &sessionId);
    void sessionFinished(const FinishedSession &summary);
    void denoiseAbReady(const mic::DenoiseAbResult &result);
    void connectionUpdated();

private slots:
    void onCaptureStarted(const QString &deviceName);
    void onCaptureFailed(const QString &message);
    void onDeviceLost(const QString &reason);
    void onWorkerStatus(MicStatus status);
    void onWorkerTelemetry(const SessionTelemetry &telemetry);
    void onLatencySample(const LatencySample &sample);
    void onWorkerError(const QString &message);
    void onWorkerFinished(const FinishedSession &summary);
    void onWorkerFailed(const QString &message);
    void onSessionStarted(const QString &sessionId);
    void onLiveState(const asr::StateResponse &state, double pollMs);
    void onPollFailed(const QString &message);
    void retryDevice();

private:
    void setMicStatus(MicStatus status);
    void setError(const QString &message);
    void teardownWorker();
    AudioDeviceChoice deviceChoice() const;
    bool beginSession(SessionWorker *worker);

    AppConfig *m_config = nullptr;
    RpcExecutor *m_rpc = nullptr;
    TranscriptModel m_model;

    QThread *m_captureThread = nullptr;
    AudioCapture *m_capture = nullptr;
    mic::DenoiseAbRecorder *m_denoiseRecorder = nullptr;

    AudioQueue m_queue;
    // Held between "start microphone" and the device actually opening: the
    // server session is only created once capture is confirmed real.
    SessionWorker::Settings m_pendingSettings;
    bool m_hasPendingSettings = false;
    SessionWorker *m_worker = nullptr;
    // A worker that has been asked to stop but may still be draining; kept so
    // shutdown can wait for it rather than deleting a running QThread.
    SessionWorker *m_workerPendingDelete = nullptr;
    StatePoller *m_poller = nullptr;

    QTimer m_deviceRetry;
    QTimer m_connectionTimer;

    MicStatus m_micStatus = MicStatus::Idle;
    double m_micStatusSince = 0.0;
    QList<MicStatusTransition> m_micStatusHistory;
    SessionTelemetry m_telemetry;
    QList<LatencySample> m_latencyHistory;
    QList<FinishedSession> m_finishedSessions;

    QString m_sessionId;
    QString m_sourceName;
    QString m_deviceName;
    QString m_deviceWarning;
    QString m_error;
    QString m_speakerFilter = QStringLiteral("unrestricted");
    QStringList m_expectedSpeakers;
    mic::DenoiseState m_denoise = mic::DenoiseState::Unknown;
    bool m_captureWanted = false;
    bool m_connected = false;
    bool m_connectionPending = false;
    double m_connectionLatencyMs = 0.0;
    QString m_connectionDetail;
    // Logging only: live state arrives five times a second, so the revision
    // is logged when it moves rather than on every poll.
    quint64 m_lastLoggedRevision = 0;
};

#endif // SESSIONCONTROLLER_H
