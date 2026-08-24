// Owns one meeting end to end on its own thread and its own gRPC channel:
// start_session, the push_audio loop, the stop drain, and the final revision
// read.
//
// The whole lifecycle stays on this thread on purpose.  stop_session is a
// drain barrier that must come *after* the last captured packet has been
// sent, and the only way to guarantee that ordering is to keep both on the
// same thread rather than coordinating two of them.
//
// Two sources, one loop: a live microphone feeding an AudioQueue, or a WAV
// replayed from disk.  They differ only in packet size (160 ms live, 320 ms
// replay - the pipeline's native diar hop) and in whether the loop paces
// itself against the source clock.
#ifndef SESSIONWORKER_H
#define SESSIONWORKER_H

#include "AudioQueue.h"
#include "SessionTypes.h"
#include "audio/WavIo.h"

#include <QAtomicInt>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QThread>

class AsrClient;

class SessionWorker : public QThread
{
    Q_OBJECT

public:
    enum class Source { Microphone, File };

    struct Settings
    {
        QString target;
        QString token;
        Source source = Source::Microphone;
        int sampleRate = 48000;
        int channels = 1;
        bool pipelineTrace = true;
        // Tri-state, exactly as start_session's config_json defines it:
        //   unset      -> key omitted, match against the whole global registry
        //   empty list -> key present and empty, assign no registered name
        //   names      -> only those names may reach the verifier
        bool restrictSpeakers = false;
        QStringList expectedSpeakers;
        SessionMeta meta;
        // File replay only.
        wav::Pcm file;
        QString fileName;
        bool pacedToSourceClock = true;
    };

    explicit SessionWorker(const Settings &settings, AudioQueue *queue, QObject *parent = nullptr);
    ~SessionWorker() override;

    QString sessionId() const;

    void requestStop();
    void setPaused(bool paused);
    bool isPaused() const;
    // Set by the capture side when the bounded queue rejected a chunk.  The
    // loop then stops loudly instead of continuing with a hole in the audio.
    void noteDroppedChunk();

signals:
    void sessionStarted(const QString &sessionId);
    void statusChanged(MicStatus status);
    void telemetryChanged(const SessionTelemetry &telemetry);
    void latencySample(const LatencySample &sample);
    void errorChanged(const QString &message);
    void finished(const FinishedSession &summary);
    void failed(const QString &message);

protected:
    void run() override;

private:
    QString buildConfigJson() const;
    // Sends one packet, retrying the *same* seq through transport failures.
    // Returns false only when the session must end.
    bool pushPacket(AsrClient &client, const QByteArray &pcm, bool reset, int vadChunkMs,
                    QString *error);
    void publishTelemetry();
    void runMicrophone(AsrClient &client);
    void runFile(AsrClient &client);
    bool drainAndStop(AsrClient &client, const QByteArray &tail);

    Settings m_settings;
    AudioQueue *m_queue = nullptr;

    mutable QMutex m_mutex;
    QString m_sessionId;

    QAtomicInt m_stopRequested{0};
    QAtomicInt m_paused{0};
    QAtomicInt m_dropped{0};

    quint64 m_seq = 0;
    qint64 m_sentBytes = 0;
    qint64 m_capturedBytes = 0;
    SessionTelemetry m_telemetry;
    bool m_networkReconnecting = false;
    double m_lastSourceSeenSec = 0.0;
};

#endif // SESSIONWORKER_H
