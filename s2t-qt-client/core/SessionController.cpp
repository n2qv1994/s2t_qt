#include "SessionController.h"

#include "core/Logger.h"
#include "SessionWorker.h"
#include "StatePoller.h"
#include "audio/MediaDecode.h"
#include "audio/Transcode.h"
#include "audio/WavIo.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace {

const int kLatencyHistoryMax = 300;
const int kStatusHistoryMax = 50;
const int kDeviceRetryMs = 500;
const int kConnectionProbeMs = 3000;

double nowSeconds()
{
    return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

} // namespace

SessionController::SessionController(AppConfig *config, QObject *parent)
    : QObject(parent), m_config(config)
{
    m_rpc = new RpcExecutor(this);
    m_rpc->configure(m_config->serverTarget, m_config->apiToken);

    m_captureThread = new QThread(this);
    m_captureThread->setObjectName(QStringLiteral("audio-capture"));
    m_capture = new AudioCapture();
    m_denoiseRecorder = new mic::DenoiseAbRecorder();
    m_capture->moveToThread(m_captureThread);
    m_denoiseRecorder->moveToThread(m_captureThread);
    connect(m_captureThread, &QThread::finished, m_capture, &QObject::deleteLater);
    connect(m_captureThread, &QThread::finished, m_denoiseRecorder, &QObject::deleteLater);

    connect(m_capture, &AudioCapture::started, this, &SessionController::onCaptureStarted);
    connect(m_capture, &AudioCapture::failed, this, &SessionController::onCaptureFailed);
    connect(m_capture, &AudioCapture::deviceLost, this, &SessionController::onDeviceLost);
    // Direct connection on purpose: this runs on the capture thread and only
    // touches the lock-protected queue, so audio never takes a trip through
    // the GUI event loop to reach the sender.
    connect(m_capture, &AudioCapture::chunk, this,
            [this](const QByteArray &pcm) {
                if (m_queue.push(pcm))
                    return;
                // Runs on the capture thread with no lock held here; the queue
                // only rejects when it is genuinely full, so this cannot
                // become a per-chunk log.
                LOG_ERROR(applog::cat::Queue)
                    << "audio queue full - dropping" << pcm.size()
                    << "bytes; the session will stop";
                if (m_worker)
                    m_worker->noteDroppedChunk();
            },
            Qt::DirectConnection);
    connect(m_denoiseRecorder, &mic::DenoiseAbRecorder::completed, this,
            [this](const mic::DenoiseAbResult &result) {
                if (result.restored || !result.restoreAttempted) {
                    m_denoise = result.finalState == QStringLiteral("disabled")
                        ? mic::DenoiseState::Disabled
                        : mic::DenoiseState::Enabled;
                }
                emit denoiseAbReady(result);
                emit statusUpdated();
            });
    m_captureThread->start();

    m_poller = new StatePoller(m_config->serverTarget, m_config->apiToken, this);
    connect(m_poller, &StatePoller::stateReceived, this, &SessionController::onLiveState);
    connect(m_poller, &StatePoller::pollFailed, this, &SessionController::onPollFailed);
    m_poller->start();

    m_deviceRetry.setInterval(kDeviceRetryMs);
    connect(&m_deviceRetry, &QTimer::timeout, this, &SessionController::retryDevice);

    m_connectionTimer.setInterval(kConnectionProbeMs);
    connect(&m_connectionTimer, &QTimer::timeout, this, &SessionController::refreshConnection);
    m_connectionTimer.start();
    LOG_INFO(applog::cat::Session)
        << "controller ready - server=" << m_config->serverTarget
        << "| threads: audio-capture, state-poller, 3 rpc lanes";
    refreshConnection();
}

SessionController::~SessionController()
{
    LOG_INFO(applog::cat::Session) << "controller shutting down - stopping worker, poller, capture";
    teardownWorker();
    // The worker is parented here, so it must not still be running when the
    // QObject destructor gets to it - QThread aborts on that.  It can be
    // inside a long stop_session drain, hence the generous wait.
    if (m_workerPendingDelete && m_workerPendingDelete->isRunning())
        m_workerPendingDelete->wait(10000);
    if (m_poller) {
        m_poller->requestStop();
        m_poller->wait(3000);
    }
    if (m_captureThread) {
        // Blocking only while the thread is genuinely running: a blocking
        // queued call into a stopped event loop never returns.
        if (m_captureThread->isRunning())
            QMetaObject::invokeMethod(m_capture, "stop", Qt::BlockingQueuedConnection);
        m_captureThread->quit();
        m_captureThread->wait(3000);
    }
    LOG_INFO(applog::cat::Session) << "controller shutdown complete";
}

AudioDeviceChoice SessionController::inputDevice() const
{
    return deviceChoice();
}

void SessionController::applyConfig()
{
    LOG_INFO(applog::cat::Session)
        << "applying new config - server=" << m_config->serverTarget
        << "| recreating the state-poller and re-pointing the rpc lanes";
    m_rpc->configure(m_config->serverTarget, m_config->apiToken);
    // The poller holds its channel for the life of its thread, so a new
    // server means a new poller rather than a re-target of the old one.
    if (m_poller) {
        m_poller->requestStop();
        m_poller->wait(3000);
        m_poller->deleteLater();
    }
    m_poller = new StatePoller(m_config->serverTarget, m_config->apiToken, this);
    connect(m_poller, &StatePoller::stateReceived, this, &SessionController::onLiveState);
    connect(m_poller, &StatePoller::pollFailed, this, &SessionController::onPollFailed);
    m_poller->start();
    if (!m_sessionId.isEmpty())
        m_poller->setSession(m_sessionId);
    refreshConnection();
}

AudioDeviceChoice SessionController::deviceChoice() const
{
    AudioDeviceChoice choice;
    choice.deviceId = m_config->inputDeviceId;
    choice.expectedName = m_config->expectedDeviceName;
    choice.sampleRate = m_config->sampleRate;
    choice.channels = m_config->channels;
    return choice;
}

void SessionController::setMicStatus(MicStatus status)
{
    if (status == m_micStatus)
        return; // re-asserting a state must not restart the clock or spam the log
    MicStatusTransition transition;
    transition.ts = nowSeconds();
    transition.from = micStatusKey(m_micStatus);
    transition.to = micStatusKey(status);
    m_micStatus = status;
    m_micStatusSince = transition.ts;
    LOG_INFO(applog::cat::Session)
        << "mic status:" << transition.from << "->" << transition.to;
    m_micStatusHistory.append(transition);
    while (m_micStatusHistory.size() > kStatusHistoryMax)
        m_micStatusHistory.removeFirst();
    emit statusUpdated();
}

void SessionController::setError(const QString &message)
{
    if (m_error == message)
        return;
    if (message.isEmpty())
        LOG_INFO(applog::cat::Session) << "error state cleared";
    else
        LOG_ERROR(applog::cat::Session) << "session error:" << message;
    m_error = message;
    if (!message.isEmpty())
        emit errorMessage(message);
    emit statusUpdated();
}

bool SessionController::beginSession(SessionWorker *worker)
{
    m_worker = worker;
    connect(worker, &SessionWorker::sessionStarted, this, &SessionController::onSessionStarted);
    connect(worker, &SessionWorker::statusChanged, this, &SessionController::onWorkerStatus);
    connect(worker, &SessionWorker::telemetryChanged, this, &SessionController::onWorkerTelemetry);
    connect(worker, &SessionWorker::latencySample, this, &SessionController::onLatencySample);
    connect(worker, &SessionWorker::errorChanged, this, &SessionController::onWorkerError);
    connect(worker, &SessionWorker::finished, this, &SessionController::onWorkerFinished);
    connect(worker, &SessionWorker::failed, this, &SessionController::onWorkerFailed);
    m_telemetry = SessionTelemetry();
    m_latencyHistory.clear();
    setError(QString());
    LOG_INFO(applog::cat::Session) << "starting the session-worker for source" << m_sourceName;
    worker->start();
    return true;
}

void SessionController::startMicrophone(bool restrict, const QStringList &expected,
                                        const SessionMeta &meta)
{
    if (m_worker) {
        LOG_WARN(applog::cat::Session) << "refusing startMicrophone: a session is already running";
        emit errorMessage(QStringLiteral("Đang có phiên chạy - dừng phiên hiện tại trước."));
        return;
    }
    LOG_INFO(applog::cat::Session)
        << "microphone session requested - restrict=" << restrict
        << "expected=[" << expected.join(QStringLiteral(", ")) << "]"
        << "mode=" << meta.mode << "title=" << meta.title;
    m_expectedSpeakers = expected;
    m_speakerFilter = !restrict ? QStringLiteral("unrestricted")
                                : (expected.isEmpty() ? QStringLiteral("none")
                                                      : QStringLiteral("restricted"));
    m_sourceName = QStringLiteral("Microphone trực tiếp");
    m_deviceWarning.clear();
    setMicStatus(MicStatus::Starting);

    const double bytesPerSec =
        double(qMax(1, m_config->sampleRate * m_config->channels * 2));
    const qint64 capacity = qint64(qMax(10.0, m_config->bufferSec) * bytesPerSec);
    m_queue.configure(capacity);
    LOG_DEBUG(applog::cat::Queue) << "audio queue capacity:" << capacity << "bytes ="
                                  << qMax(10.0, m_config->bufferSec) << "s";

    SessionWorker::Settings settings;
    settings.target = m_config->serverTarget;
    settings.token = m_config->apiToken;
    settings.source = SessionWorker::Source::Microphone;
    settings.sampleRate = m_config->sampleRate;
    settings.channels = m_config->channels;
    settings.pipelineTrace = m_config->pipelineTrace;
    settings.restrictSpeakers = restrict;
    settings.expectedSpeakers = expected;
    settings.meta = meta;

    // Open AND start the device before creating a server session.  A driver
    // failure must read as "cannot record" rather than leaving an empty
    // meeting open on the server with nothing ever arriving for it - so the
    // worker is not launched until onCaptureStarted confirms real capture.
    m_pendingSettings = settings;
    m_hasPendingSettings = true;
    m_captureWanted = true;
    LOG_INFO(applog::cat::Audio)
        << "opening the microphone before creating the session -" << settings.sampleRate << "Hz /"
        << settings.channels << "ch, expected device=" << m_config->expectedDeviceName;
    QMetaObject::invokeMethod(m_capture, "start", Qt::QueuedConnection,
                              Q_ARG(AudioDeviceChoice, deviceChoice()));
}

void SessionController::startFile(const QString &path, bool restrict, const QStringList &expected,
                                  const SessionMeta &meta)
{
    if (m_worker) {
        LOG_WARN(applog::cat::Session) << "refusing startFile: a session is already running";
        emit errorMessage(QStringLiteral("Đang có phiên chạy - dừng phiên hiện tại trước."));
        return;
    }
    LOG_INFO(applog::cat::Session)
        << "file replay requested" << path << "- restrict=" << restrict
        << "expected=[" << expected.join(QStringLiteral(", ")) << "]" << "mode=" << meta.mode;
    QString error;
    wav::Pcm pcm;
    // .m4a and non-PCM .wav go through ffmpeg first; a plain 16-bit PCM WAV
    // comes straight back and no temporary is created.
    const QString decodedPath = audio::ensurePcmWav(path, &error);
    if (!decodedPath.isEmpty()) {
        if (decodedPath != path)
            LOG_DEBUG(applog::cat::Session) << "transcoded through ffmpeg ->" << decodedPath;
        pcm = wav::readWav(decodedPath, &error);
        if (decodedPath != path)
            QFile::remove(decodedPath); // already fully read into memory
    }
    if (!pcm.isValid()) {
        // No ffmpeg binary, or a container it would not take: fall back to the
        // FFmpeg Qt itself ships with, which is the same decoder QMediaPlayer
        // uses and needs nothing installed on the workstation.
        //
        // This is not a nicety.  The RHEL host has no ffmpeg on PATH at all, so
        // until 2026-09-02 the replay path there could open a plain PCM WAV and
        // nothing else - an operator picking the .mp4 of their own meeting got
        // "không tìm thấy ffmpeg" and no way forward, while the subtitle window
        // beside it played the same file.
        QString mediaError;
        pcm = audio::decodeMedia(path, &mediaError);
        if (pcm.isValid()) {
            LOG_INFO(applog::cat::Session)
                << "decoded through Qt Multimedia (no ffmpeg binary needed)" << path;
        } else if (!mediaError.isEmpty()) {
            error = mediaError;
        }
    }
    if (!pcm.isValid()) {
        LOG_ERROR(applog::cat::Session) << "decoding the media file failed:" << error;
        emit errorMessage(error.isEmpty() ? QStringLiteral("không đọc được tệp audio") : error);
        return;
    }
    LOG_INFO(applog::cat::Session)
        << "file loaded:" << pcm.sampleRate << "Hz /" << pcm.channels << "ch /"
        << pcm.durationSec() << "s /" << pcm.frames.size() << "bytes";

    // Off means "feed it as fast as the pipeline accepts", which is what a
    // reprocessing run wants; on reproduces the meeting's real timing, which
    // is what a rehearsal or a latency measurement wants.
    startPcm(pcm, QFileInfo(path).fileName(), restrict, expected, meta,
             m_config->paceFileReplay);
}

void SessionController::startPcm(const wav::Pcm &pcm, const QString &sourceName, bool restrict,
                                 const QStringList &expected, const SessionMeta &meta, bool paced)
{
    if (m_worker) {
        LOG_WARN(applog::cat::Session) << "refusing startPcm: a session is already running";
        emit errorMessage(QStringLiteral("Đang có phiên chạy - dừng phiên hiện tại trước."));
        return;
    }
    if (!pcm.isValid() || pcm.frames.isEmpty()) {
        emit errorMessage(QStringLiteral("Không có dữ liệu âm thanh để gửi."));
        return;
    }

    m_expectedSpeakers = expected;
    m_speakerFilter = !restrict ? QStringLiteral("unrestricted")
                                : (expected.isEmpty() ? QStringLiteral("none")
                                                      : QStringLiteral("restricted"));
    m_sourceName = sourceName;

    SessionWorker::Settings settings;
    settings.target = m_config->serverTarget;
    settings.token = m_config->apiToken;
    settings.source = SessionWorker::Source::File;
    settings.pipelineTrace = m_config->pipelineTrace;
    settings.restrictSpeakers = restrict;
    settings.expectedSpeakers = expected;
    settings.meta = meta;
    settings.file = pcm;
    settings.fileName = m_sourceName;
    settings.pacedToSourceClock = paced;

    m_captureWanted = false;
    setMicStatus(MicStatus::Starting);
    beginSession(new SessionWorker(settings, nullptr, this));
}

void SessionController::stop()
{
    if (!m_worker) {
        LOG_WARN(applog::cat::Session) << "stop pressed with no session running";
        emit errorMessage(QStringLiteral("Không có phiên nào đang chạy."));
        return;
    }
    LOG_INFO(applog::cat::Session)
        << "stop requested for session" << m_sessionId << "-" << m_queue.pendingBytes()
        << "bytes still queued";
    // Stop capture first so the worker's drain of the queue is finite and
    // still contains every frame spoken before the click.
    if (m_captureWanted) {
        m_captureWanted = false;
        m_deviceRetry.stop();
        QMetaObject::invokeMethod(m_capture, "stop", Qt::QueuedConnection);
    }
    m_worker->requestStop();
    emit notice(QStringLiteral("Đang kết thúc: gửi nốt audio và flush correction..."));
}

void SessionController::setPaused(bool paused)
{
    if (!m_worker)
        return;
    LOG_INFO(applog::cat::Session) << (paused ? "pausing" : "resuming") << "session" << m_sessionId;
    m_worker->setPaused(paused);
    setMicStatus(paused ? MicStatus::Paused : MicStatus::Recording);
    emit notice(paused
                    ? QStringLiteral("Đã tạm dừng microphone; không gửi audio nào tới AI.")
                    : QStringLiteral("Đã tiếp tục microphone; vẫn cùng một phiên."));
}

void SessionController::setDenoise(bool enabled)
{
    QString output;
    QString error;
    LOG_INFO(applog::cat::Audio) << (enabled ? "enabling" : "disabling") << "denoise through"
                                 << m_config->micControlApp;
    if (!mic::setDenoise(m_config->micControlApp, enabled, &output, &error)) {
        LOG_ERROR(applog::cat::Audio) << "denoise control failed:" << error
                                      << "| output:" << output.simplified();
        emit errorMessage(QStringLiteral("Lỗi điều khiển lọc nhiễu: %1").arg(error));
        return;
    }
    LOG_DEBUG(applog::cat::Audio) << "xvf_host returned:" << output.simplified();
    m_denoise = enabled ? mic::DenoiseState::Enabled : mic::DenoiseState::Disabled;
    emit notice(enabled ? QStringLiteral("Đã bật lọc nhiễu tạm thời trên microphone")
                        : QStringLiteral("Đã tắt lọc nhiễu tạm thời trên microphone"));
    emit statusUpdated();
}

void SessionController::captureDenoiseAb(double seconds)
{
    if (m_worker) {
        LOG_WARN(applog::cat::Audio) << "refusing the denoise A/B capture: a mic session is running";
        emit errorMessage(QStringLiteral(
            "Phiên mic đang chạy - dừng phiên trước khi ghi mẫu đối chứng lọc nhiễu."));
        return;
    }
    LOG_INFO(applog::cat::Audio) << "starting the denoise A/B capture -" << seconds
                                 << "s per side";
    QMetaObject::invokeMethod(m_denoiseRecorder, "start", Qt::QueuedConnection,
                              Q_ARG(AudioDeviceChoice, deviceChoice()),
                              Q_ARG(QString, m_config->micControlApp), Q_ARG(double, seconds),
                              Q_ARG(int, int(m_denoise)));
}

void SessionController::refreshConnection()
{
    if (m_connectionPending)
        return;
    m_connectionPending = true;
    buf::PingRequest request;
    request.clientTs = nowSeconds();
    m_rpc->call<buf::PingResponse>(
        this,
        [request](AsrClient &client, buf::PingResponse &out) {
            // The buffer's own RPC, not a relayed one, and a real call rather
            // than a TCP connect: "connected" has to mean the server answers
            // *and* the token is accepted, or the badge goes green against a
            // server that rejects everything.
            //
            // It also answers the second question in the same round trip -
            // whether the buffer can reach the inference tier - which a
            // relayed get_model_status could not distinguish from the buffer
            // itself being down.  The two failures need different fixes, so
            // they are shown as different states.
            return client.bufferPing(request, &out, 5000);
        },
        [this](const grpc::Status &status, const buf::PingResponse &response) {
            m_connectionPending = false;
            const bool wasConnected = m_connected;
            const bool wasUpstream = m_upstreamReady;
            m_connected = status.ok();
            m_upstreamReady = status.ok() && response.upstreamReady;
            m_serverVersion = status.ok() ? response.serverVersion : QString();
            if (status.code == grpc::Unimplemented) {
                // Almost always a client pointed straight at the adapter,
                // which answers ProductASRService but has never heard of
                // BufferAdminService.  Say which mistake it is.
                m_connectionDetail =
                    QStringLiteral("%1 trả lời nhưng không phải Server buffer - kiểm tra lại cổng "
                                   "trong Cấu hình (mặc định 8800, không phải 8700)")
                        .arg(m_config->serverTarget);
            } else if (status.ok()) {
                m_connectionDetail = m_upstreamReady
                    ? QStringLiteral("%1 (Server buffer %2), tầng suy luận sẵn sàng")
                          .arg(m_config->serverTarget, response.serverVersion)
                    : QStringLiteral("%1 (Server buffer %2) - tầng suy luận chưa sẵn sàng; audio "
                                     "vẫn được nhận và xếp hàng trên server")
                          .arg(m_config->serverTarget, response.serverVersion);
            } else {
                m_connectionDetail = status.toString();
            }
            // Probed every 3 s; only a change is worth a line, or the log is
            // 20 identical entries per minute.
            if (wasConnected != m_connected) {
                if (m_connected)
                    LOG_INFO(applog::cat::Session) << "buffer server connected:" << m_connectionDetail;
                else
                    LOG_WARN(applog::cat::Session) << "buffer server unreachable:" << m_connectionDetail;
            }
            if (m_connected && wasUpstream != m_upstreamReady) {
                LOG_WARN(applog::cat::Session)
                    << "inference tier is now" << (m_upstreamReady ? "reachable" : "unreachable")
                    << "according to the buffer";
            }
            emit connectionUpdated();
        });
}

void SessionController::onCaptureStarted(const QString &deviceName)
{
    LOG_INFO(applog::cat::Audio) << "microphone open:" << deviceName
                                 << (m_hasPendingSettings ? "- creating the session on the server"
                                                          : "- reconnected mid-session");
    m_deviceName = deviceName;
    m_deviceWarning.clear();
    m_deviceRetry.stop();
    if (m_hasPendingSettings) {
        // Capture is real, so the meeting may now exist on the server.
        m_hasPendingSettings = false;
        beginSession(new SessionWorker(m_pendingSettings, &m_queue, this));
        emit statusUpdated();
        return;
    }
    if (m_worker && m_micStatus == MicStatus::DeviceReconnecting)
        setMicStatus(m_worker->isPaused() ? MicStatus::Paused : MicStatus::Recording);
    emit statusUpdated();
}

void SessionController::onCaptureFailed(const QString &message)
{
    LOG_ERROR(applog::cat::Audio) << "opening the microphone failed:" << message
                                  << "| sessionPending=" << m_hasPendingSettings
                                  << "workerRunning=" << (m_worker != nullptr);
    m_deviceWarning = message;
    if (m_hasPendingSettings) {
        // Never opened: no session was created, so there is nothing to keep
        // alive and nothing to stop - just report why.
        m_hasPendingSettings = false;
        m_captureWanted = false;
        m_deviceRetry.stop();
        setMicStatus(MicStatus::Error);
        setError(message);
        return;
    }
    if (m_captureWanted && m_worker) {
        // Mid-session: keep the meeting open and keep trying the same device.
        setMicStatus(MicStatus::DeviceReconnecting);
        m_deviceRetry.start();
    } else {
        setMicStatus(MicStatus::Error);
    }
    setError(message);
}

void SessionController::onDeviceLost(const QString &reason)
{
    LOG_ERROR(applog::cat::Audio)
        << "microphone lost mid-session:" << reason << "- keeping session" << m_sessionId
        << "and discarding" << m_queue.pendingBytes() << "unsent bytes";
    const QString warning = QStringLiteral(
                                "Microphone bị ngắt khi đang ghi. Phiên được tạm dừng; "
                                "cắm lại mic để tiếp tục cùng phiên. %1")
                                .arg(reason);
    m_deviceWarning = warning;
    setError(warning);
    // Never insert silence and never end the session: audio that happened
    // while the device was gone cannot be recovered because it was never
    // captured, but everything before and after it belongs to one meeting.
    m_queue.clear();
    setMicStatus(MicStatus::DeviceReconnecting);
    if (m_captureWanted)
        m_deviceRetry.start();
}

void SessionController::retryDevice()
{
    if (!m_captureWanted || !m_worker) {
        m_deviceRetry.stop();
        return;
    }
    // Fires twice a second while a mic is unplugged, so it stays at trace.
    LOG_TRACE(applog::cat::Audio) << "retrying the microphone";
    QMetaObject::invokeMethod(m_capture, "start", Qt::QueuedConnection,
                              Q_ARG(AudioDeviceChoice, deviceChoice()));
}

void SessionController::onSessionStarted(const QString &sessionId)
{
    LOG_INFO(applog::cat::Session)
        << "session created on the server: id=" << sessionId << "source=" << m_sourceName
        << "speakerFilter=" << m_speakerFilter;
    m_sessionId = sessionId;
    m_model.resetForSession(sessionId);
    if (m_poller)
        m_poller->setSession(sessionId);
    emit sessionStarted(sessionId);
    emit statusUpdated();
    emit modelUpdated();
}

void SessionController::onWorkerStatus(MicStatus status)
{
    // A device-level reconnect outranks the worker's own view: the worker
    // only knows that no audio is arriving, not why.
    if (m_micStatus == MicStatus::DeviceReconnecting && status == MicStatus::Recording)
        return;
    setMicStatus(status);
}

void SessionController::onWorkerTelemetry(const SessionTelemetry &telemetry)
{
    const QString keepPollError = m_telemetry.pollError;
    const double keepPollLast = m_telemetry.statePollLastMs;
    const double keepPollMax = m_telemetry.statePollMaxMs;
    m_telemetry = telemetry;
    m_telemetry.pollError = keepPollError;
    m_telemetry.statePollLastMs = keepPollLast;
    m_telemetry.statePollMaxMs = keepPollMax;
    emit statusUpdated();
}

void SessionController::onLatencySample(const LatencySample &sample)
{
    m_latencyHistory.append(sample);
    while (m_latencyHistory.size() > kLatencyHistoryMax)
        m_latencyHistory.removeFirst();
}

void SessionController::onWorkerError(const QString &message)
{
    setError(message);
}

void SessionController::onWorkerFinished(const FinishedSession &summary)
{
    LOG_INFO(applog::cat::Session)
        << "session finished cleanly: id=" << summary.sessionId
        << "source=" << summary.sourceName << "duration=" << summary.durationSec
        << "s rev=" << summary.revision;
    m_finishedSessions.removeIf(
        [&summary](const FinishedSession &item) { return item.sessionId == summary.sessionId; });
    m_finishedSessions.append(summary);
    teardownWorker();
    emit sessionFinished(summary);
    emit statusUpdated();
}

void SessionController::onWorkerFailed(const QString &message)
{
    LOG_ERROR(applog::cat::Session) << "session" << m_sessionId << "failed:" << message;
    setError(message);
    teardownWorker();
    emit statusUpdated();
}

void SessionController::teardownWorker()
{
    if (m_captureWanted) {
        m_captureWanted = false;
        m_deviceRetry.stop();
        QMetaObject::invokeMethod(m_capture, "stop", Qt::QueuedConnection);
    }
    if (!m_worker)
        return;
    LOG_DEBUG(applog::cat::Session) << "tearing down the session-worker of session" << m_sessionId;
    SessionWorker *worker = m_worker;
    m_worker = nullptr;
    m_workerPendingDelete = worker;
    worker->requestStop();
    worker->disconnect(this);
    // The worker can still be inside a 3600 s stop_session drain; hand it to
    // the event loop rather than blocking the UI on it.
    connect(worker, &QThread::finished, this, [this, worker]() {
        if (m_workerPendingDelete == worker)
            m_workerPendingDelete = nullptr;
        worker->deleteLater();
    });
    if (worker->isFinished()) {
        m_workerPendingDelete = nullptr;
        worker->deleteLater();
    }
}

void SessionController::onLiveState(const asr::StateResponse &state, double pollMs)
{
    // Five of these a second: a per-poll line only belongs at trace, and a
    // revision change is the one thing worth a normal line.
    LOG_TRACE(applog::cat::Poll)
        << "state" << state.sessionId << "rev=" << state.transcriptRevision
        << "rows=" << state.state.rows.size() << "provisional=" << state.state.provisionalRows.size()
        << "seen=" << state.state.sourceSeenSec << "s poll=" << pollMs << "ms";
    if (state.transcriptRevision != m_lastLoggedRevision) {
        m_lastLoggedRevision = state.transcriptRevision;
        LOG_DEBUG(applog::cat::Poll)
            << "transcript revision ->" << state.transcriptRevision << "("
            << state.state.rows.size() << "rows," << state.state.nLow << "low-confidence words)";
    }
    m_telemetry.statePollLastMs = pollMs;
    m_telemetry.statePollMaxMs = qMax(m_telemetry.statePollMaxMs, pollMs);
    m_telemetry.pollError.clear();
    // ACK means "durably stored", not "already inferred".  Recomputing the
    // server queue here keeps it falling after a reconnect burst has finished
    // uploading and there are no more ACKs left to recalculate it from.
    if (m_worker)
        m_telemetry.serverQueueSec = qMax(0.0, m_telemetry.sentSec - state.state.sourceSeenSec);
    if (m_model.isReview() && m_model.sessionId() != state.sessionId)
        return; // reviewing another meeting; do not overwrite it with live tail
    m_model.applyLiveState(state);
    emit modelUpdated();
    emit statusUpdated();
}

void SessionController::onPollFailed(const QString &message)
{
    // The poller retries five times a second; log the first occurrence of a
    // given failure and then stay quiet until it changes or clears.
    if (message != m_telemetry.pollError)
        LOG_WARN(applog::cat::Poll) << "get_live_state poll failing:" << message;
    m_telemetry.pollError = message;
    emit statusUpdated();
}
