// One meeting held in the Server buffer: the queue between a client and the
// inference tier, plus the thread that drains it.
//
// This is the piece the split exists for.  It used to be the client's
// SessionWorker that owned the push loop, the seq idempotency and the
// transport-only retry, which meant a network blip on a workstation stalled
// that workstation's microphone.  Here the same rules run on the server, one
// hop closer to the pipeline, so a blip costs queue depth instead of audio and
// the client is free the moment a packet is durably in this buffer.
//
// Threading, and why:
//
//   - the forwarder runs on this QThread and owns one channel.  push_audio and
//     the final stop_session share it, because stop_session is a drain barrier
//     that must come *after* the last packet, and one thread is the only way
//     to guarantee that ordering.
//   - the live-state read owns a second channel, guarded by its own mutex.
//     SessionState grows with the meeting; serialising it must never queue in
//     front of a 160 ms audio packet.  That is the same separation the client
//     kept, moved here.
//   - push(), liveState() and stop() are all called from HTTP/2 connection
//     threads, concurrently, so everything they touch is under a lock.
#ifndef SESSIONBUFFER_H
#define SESSIONBUFFER_H

#include "RpcLane.h"
#include "proto/BufferAdmin.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

class SessionBuffer : public QThread
{
public:
    struct Settings
    {
        QString upstreamTarget;
        QString upstreamToken;
        // Peer that called start_session.  Recorded so an operator reading the
        // admin screen can tell two workstations apart.
        QString client;
        // How much un-forwarded audio may pile up before push_audio refuses.
        // Bounded on purpose: an unbounded queue turns a stalled pipeline into
        // an out-of-memory kill, and the client already knows how to stop
        // loudly when a push is rejected.
        qint64 capacityBytes = 0;
        int upstreamTimeoutMs = 30000;
        int statePollMs = 200;
        // Empty disables the archive.  When set, every accepted packet is also
        // appended to <spoolDir>/<session>.pcm, so the audio the pipeline was
        // given still exists after the fact.
        QString spoolDir;
    };

    SessionBuffer(const QString &sessionId, const asr::StartSessionResponse &started,
                  const Settings &settings);
    ~SessionBuffer() override;

    QString sessionId() const { return m_sessionId; }
    const asr::StartSessionResponse &startResponse() const { return m_started; }

    // ---- called from connection threads ------------------------------------

    // Accepts one packet into the queue and acknowledges it.  The ACK means
    // "durably in this buffer", never "the pipeline has seen it" - the two are
    // reported separately, and sourceSeenSec in the reply is still the
    // pipeline's own number, so the client's "hàng đợi server AI" keeps
    // meaning what it meant.
    grpc::Status push(const asr::PushAudioRequest &request, asr::PushAudioResponse *out);

    // Cached; a refresh older than statePollMs triggers exactly one upstream
    // read no matter how many clients are watching.
    grpc::Status liveState(asr::StateResponse *out);

    // Drains the queue, then calls stop_session upstream on the same thread
    // that sent the audio, and returns the pipeline's own answer.
    grpc::Status stop(int timeoutMs, asr::StopSessionResponse *out);

    // Ends the thread without a stop_session: used when the whole server is
    // going down, where pretending to close the meeting cleanly would be a lie.
    void shutdown();

    bool isFinished() const;
    double idleSeconds() const;
    double finishedSecondsAgo() const;
    buf::BufferedSession snapshot() const;

protected:
    void run() override;

private:
    struct Packet
    {
        QByteArray pcm;
        quint32 sampleRate = 0;
        quint32 channels = 0;
        QString audioFormat;
        bool reset = false;
        quint32 vadChunkMs = 0;
        quint64 seq = 0;
    };

    // Sends one packet, retrying the same seq through transport failures only.
    // Returns false when the session must end; *fatal then holds the status
    // that push() will hand the client.
    bool forward(AsrClient &client, const Packet &packet, grpc::Status *fatal);
    void recordForwardMs(double ms);
    void noteError(const grpc::Status &status);
    void openSpool();
    void archive(const QByteArray &pcm);

    QString m_sessionId;
    asr::StartSessionResponse m_started;
    Settings m_settings;

    // ---- queue -------------------------------------------------------------
    mutable QMutex m_mutex;
    QWaitCondition m_notEmpty;   // forwarder waits on this
    QWaitCondition m_stopped;    // stop() waits on this
    QList<Packet> m_queue;
    qint64 m_pendingBytes = 0;
    QAtomicInt m_stopRequested{0};
    QAtomicInt m_shutdownRequested{0};

    // ---- what the client has been told -------------------------------------
    quint64 m_lastAcceptedSeq = 0;
    asr::PushAudioResponse m_lastAck;
    // The pipeline's own progress, carried into every ACK so the client's
    // queue reading stays honest about how far behind the pipeline is.
    double m_upstreamSourceSeenSec = 0.0;
    double m_upstreamSpeechSeenSec = 0.0;
    asr::Timing m_upstreamTiming;
    quint64 m_stateVersion = 0;

    // ---- counters ----------------------------------------------------------
    quint64 m_acceptedPackets = 0;
    quint64 m_acceptedBytes = 0;
    quint64 m_forwardedPackets = 0;
    quint64 m_forwardedBytes = 0;
    quint64 m_droppedPackets = 0;
    quint64 m_retries = 0;
    quint64 m_spooledBytes = 0;
    quint64 m_statePolls = 0;
    quint64 m_stateReaders = 0;
    QList<double> m_forwardMs;
    QString m_lastError;
    double m_lastErrorAt = 0.0;
    grpc::Status m_fatal;
    bool m_haveFatal = false;

    double m_startedAt = 0.0;
    double m_updatedAt = 0.0;
    double m_finishedAt = 0.0;
    bool m_finished = false;
    QString m_title;
    QString m_lastAudioFormat;
    quint32 m_sampleRate = 0;
    quint32 m_channels = 0;

    // ---- the drain barrier -------------------------------------------------
    asr::StopSessionResponse m_stopResponse;
    grpc::Status m_stopStatus;
    bool m_stopDone = false;

    // ---- live-state cache, on its own channel and its own lock -------------
    // Lock order, everywhere in this class: m_stateMutex before m_mutex.
    // Never the other way round.
    mutable QMutex m_stateMutex;
    RpcLane *m_stateLane = nullptr;
    asr::StateResponse m_state;
    bool m_haveState = false;
    QElapsedTimer m_stateAge;

    QFile m_spool;
};

#endif // SESSIONBUFFER_H
