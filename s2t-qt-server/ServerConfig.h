// Configuration for the Server buffer.
//
// A service, not a desktop app, so this is an INI file plus command-line
// overrides rather than QSettings in the registry: the file can be reviewed,
// diffed and put under configuration management, and the same binary can be
// run twice on one host against two upstreams without either instance
// rewriting the other's settings.
//
// Every key can be overridden on the command line, and the command line always
// wins - a value typed by an operator right now outranks one saved earlier.
#ifndef SERVERCONFIG_H
#define SERVERCONFIG_H

#include "SessionJournal.h"
#include "core/Logger.h"

#include <QString>
#include <QStringList>

// Reported over BufferAdminService/ping so a client can say what it is talking
// to without an out-of-band question.
#define S2T_SERVER_VERSION "1.0"

struct ServerConfig
{
    // ---- listening side (what s2t-qt-client connects to) -------------------
    QString listenAddress = QStringLiteral("0.0.0.0");
    quint16 listenPort = 8800;
    // Bearer token clients must present.  Empty accepts everyone, which is
    // logged as a warning at startup rather than left to be discovered.
    QString listenToken;
    int maxConnections = 128;
    // A client RPC lane can genuinely be idle for the length of a meeting, so
    // this is deliberately long.
    int idleTimeoutMs = 300000;

    // ---- upstream side (the inference tier) --------------------------------
    //
    // Until 2026-08-25 this was grpc_session_adapter.py, a Python service that
    // owned the whole pipeline and answered asr.ui.v1 directly.  It is gone:
    // the Server buffer now talks to the inference tier itself, and there are
    // two of them.  `backend` picks which, and it changes what `target` means:
    //
    //   triton  inference.GRPCInferenceService, KServe v2.  The model
    //           repository as deployed - asr_diar_session and the ten models
    //           behind it.  Default port 8011.
    //   riva    nvidia.riva.asr.RivaSpeechRecognition.  Default port 50051.
    //
    // Anything else is refused at startup rather than left to fail on the first
    // packet: a typo here would otherwise look like an unreachable server.
    QString backend = QStringLiteral("triton");
    QString upstreamTarget = QStringLiteral("192.168.1.47:8011");
    QString upstreamToken;
    // Which model to decode with.  Empty means the backend's own default, which
    // for Triton is asr_diar_session and for Riva is whatever that server was
    // deployed with.
    QString model;
    // Only Riva uses this; Triton's model repository already implies it.
    QString language = QStringLiteral("vi-VN");
    // Channels kept open for relayed, request-scoped RPCs (review, audio,
    // edit, enrolment, trace).  The audio path and the live-state poll are
    // deliberately NOT in this pool - each session owns dedicated channels for
    // those, so a slow review query can never sit in front of an audio packet.
    int upstreamLanes = 4;
    int upstreamTimeoutMs = 30000;
    // How often the upstream reachability probe runs when nothing else is
    // talking to it.  Backs the "upstream_ready" flag in ping.
    int upstreamProbeMs = 5000;

    // ---- buffering ---------------------------------------------------------
    // How much audio one session may hold waiting for the pipeline before
    // push_audio starts refusing.  Refusing is the point: an unbounded buffer
    // turns a stalled pipeline into an out-of-memory kill, and the client
    // already knows how to stop loudly.
    double bufferSeconds = 300.0;
    // Where the per-session journal lives.  Empty means the queue is RAM only
    // and a restart loses every open meeting; set it and a meeting survives the
    // server restarting under it.  See SessionJournal.h.
    QString journalDir;
    // How far a packet is pushed before the client is told it is safe.
    // Os survives this process dying; Fsync survives the machine losing power,
    // at the cost of one fsync per packet.
    jrn::Durability durability = jrn::Durability::Os;
    // Whether a journal segment is deleted once the pipeline has acknowledged
    // everything in it (Queue, the default) or kept until the session is
    // forgotten (Session, which makes the journal an archive of the meeting and
    // costs the whole meeting in disk).
    jrn::Keep journalKeep = jrn::Keep::Queue;
    qint64 segmentBytes = 16 * 1024 * 1024;
    // A recovered session nobody comes back for is stopped after this long.
    // Without it, a server that restarts a few times accumulates meetings that
    // will never end.  0 disables it.
    int orphanTimeoutSec = 1800;
    // Cache lifetime for get_live_state.  Below this age every client watching
    // a meeting is answered from one upstream poll; above it, the first caller
    // refreshes and the rest wait for that same refresh.
    int statePollMs = 200;
    // A stopped session is kept this long so a client can still read its final
    // state and buffer counters before it is forgotten.
    int finishedRetentionSec = 900;

    // ---- logging -----------------------------------------------------------
    applog::Mode logMode = applog::Mode::Debug;
    applog::Level logLevel = applog::Level::Info;

    // Where it was loaded from; empty when nothing was read.
    QString sourcePath;

    // Reads the INI file if it exists, then applies argv.  Returns false with
    // *error set only for an argument that is wrong - a missing config file is
    // not an error, because every value here has a working default.
    bool load(const QStringList &args, QString *error);
    void save(const QString &path, QString *error) const;

    // The default file: /etc/s2t-qt-server.conf on Linux, next to the binary
    // on Windows.  Named here rather than in main() so --write-config and the
    // loader cannot disagree about it.
    static QString defaultPath();

    // One block, for the startup log and for --show-config.  The tokens are
    // reduced to "đã đặt"/"chưa đặt" - a service log is read by more people
    // than the config file is.
    QStringList describe() const;

    qint64 bufferBytesPerSession(int sampleRate, int channels) const;
};

#endif // SERVERCONFIG_H
