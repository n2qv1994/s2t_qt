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
//   - the forwarder runs on this QThread and owns the BackendSession.  Audio
//     and the final flush share it, because the flush is a drain barrier that
//     must come *after* the last packet, and one thread is the only way to
//     guarantee that ordering.
//   - the transcript is guarded by its own mutex, separate from the queue's.
//     SessionState grows with the meeting; serialising it must never queue in
//     front of a 160 ms audio packet.  That separation predates this file
//     owning the transcript, and is why it survived the change.
//   - push(), liveState() and stop() are all called from HTTP/2 connection
//     threads, concurrently, so everything they touch is under a lock.
//
// Since 2026-08-25 the meeting's state lives *here*, in LiveTranscript, rather
// than being fetched from a pipeline that kept one.  Riva and Triton both
// answer per chunk and remember nothing, so this class is now the authority on
// what the meeting says.
#ifndef SESSIONBUFFER_H
#define SESSIONBUFFER_H

#include "LiveTranscript.h"
#include "SessionJournal.h"
#include "backend/InferenceBackend.h"
#include "proto/BufferAdmin.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>

#include <memory>

class SessionBuffer : public QThread
{
public:
    struct Settings
    {
        // The inference tier.  Borrowed, not owned - BufferHub outlives every
        // session, and a session that outlived the backend would be holding a
        // dangling pointer at exactly the moment it tried to flush.
        InferenceBackend *backend = nullptr;
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
        // Empty disables durability: the queue then lives only in RAM and a
        // restart loses every open session, which is how this server behaved
        // before journalling existed.  When set, every accepted packet is
        // written here before the client is ACKed, and a restart picks the
        // meeting back up - see SessionJournal.h.
        QString journalDir;
        jrn::Durability durability = jrn::Durability::Os;
        jrn::Keep journalKeep = jrn::Keep::Queue;
        qint64 segmentBytes = 16 * 1024 * 1024;
    };

    SessionBuffer(const QString &sessionId, qint64 streamId, const BackendSessionConfig &config,
                  const Settings &settings);
    // Rebuilds a session from its journal after a restart.  The backlog is put
    // straight into the queue, so the forwarder sends it before anything the
    // client pushes next - order across the restart is preserved.
    SessionBuffer(const jrn::Recovered &recovered, const Settings &settings);
    ~SessionBuffer() override;

    // Opens the backend session and waits for the answer.
    //
    // Separate from the constructor because of thread affinity: a
    // BackendSession owns a socket and must be created on the thread that will
    // drive it, which is the forwarder this class *is*.  So the constructor
    // starts the thread, the thread opens the session, and this blocks the
    // caller - a connection thread answering start_session - until it knows
    // whether the tier accepted the meeting.
    grpc::Status openBackend();

    QString sessionId() const { return m_sessionId; }
    QString journalHandle() const { return m_handle; }
    const asr::StartSessionResponse &startResponse() const { return m_started; }
    // True until the client touches this session again.  A recovered session
    // nobody comes back for has to be closed eventually, or a server that
    // restarts a few times accumulates meetings that will never end.
    bool awaitingClient() const;

    // ---- called from connection threads ------------------------------------

    // Accepts one packet into the queue and acknowledges it.  The ACK means
    // "durably in this buffer", never "the pipeline has seen it" - the two are
    // reported separately, and sourceSeenSec in the reply is still the
    // pipeline's own number, so the client's "hàng đợi server AI" keeps
    // meaning what it meant.
    grpc::Status push(const asr::PushAudioRequest &request, asr::PushAudioResponse *out);

    // The meeting as this server understands it.  No longer a cached upstream
    // read: since the tier stopped keeping session state, LiveTranscript here
    // *is* the authority, so this is a lock and a copy.
    grpc::Status liveState(asr::StateResponse *out);

    // The same, restricted to a time window, plus the canonical transcript that
    // an editor works against.  These are what get_review_state,
    // apply_text_edit and rename_speaker are answered from.
    grpc::Status reviewState(double viewStartSec, double viewEndSec, asr::StateResponse *out);
    grpc::Status applyTextEdit(const asr::TextEditRequest &request, asr::ReviewEditResponse *out);
    grpc::Status renameSpeaker(const asr::RenameSpeakerRequest &request,
                               asr::ReviewEditResponse *out);
    asr::SessionSummary summary() const;

    // Drains the queue, then calls stop_session upstream on the same thread
    // that sent the audio, and returns the pipeline's own answer.
    grpc::Status stop(int timeoutMs, asr::StopSessionResponse *out);

    // The same thing without waiting for it.  Used by the reaper, which runs on
    // the thread that accepts connections and must never block there for the
    // length of an upstream call.
    void requestStop();

    // Ends the thread without a stop_session: used when the whole server is
    // going down, where pretending to close the meeting cleanly would be a lie.
    // The journal is left intact, which is exactly what makes the next start
    // able to pick the meeting up again.
    void shutdown();

    bool isFinished() const;
    double idleSeconds() const;
    double finishedSecondsAgo() const;
    buf::BufferedSession snapshot() const;

protected:
    void run() override;

private:
    // The queue element and the journal record are the same type on purpose: a
    // packet read back after a restart is then indistinguishable from one that
    // just arrived, and the forwarder needs no idea which it is holding.
    using Packet = jrn::Packet;

    // Sends one packet, retrying the same seq through transport failures only.
    // Returns false when the session must end; *fatal then holds the status
    // that push() will hand the client.
    bool forward(BackendSession &session, const Packet &packet, grpc::Status *fatal);
    void recordForwardMs(double ms);
    void noteError(const grpc::Status &status);

    QString m_sessionId;
    QString m_handle;
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

    // ---- the backend handshake ---------------------------------------------
    // Created and destroyed on the forwarder thread, never touched from
    // anywhere else.  openBackend() waits on m_opened for the result.
    std::unique_ptr<BackendSession> m_backend;
    QWaitCondition m_opened;
    grpc::Status m_openStatus;
    bool m_openDone = false;

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

    // ---- the transcript, and its own lock ----------------------------------
    //
    // Lock order, everywhere in this class: m_stateMutex before m_mutex.
    // Never the other way round.
    //
    // It used to guard a cache of what the pipeline had said.  It now guards
    // the transcript itself, because nothing upstream keeps one any more.  The
    // separate lock is still worth having for the original reason: serialising
    // a long meeting's state must never queue in front of a 160 ms packet.
    mutable QMutex m_stateMutex;
    LiveTranscript m_live;
    BackendSessionConfig m_config;
    qint64 m_backendStreamId = 0;

    // Written under m_mutex, like everything else it protects.
    jrn::Journal m_journal;
    bool m_journalFailed = false;
    bool m_recovered = false;
    bool m_clientReturned = false;
};

#endif // SESSIONBUFFER_H
