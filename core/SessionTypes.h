// Value types shared between the worker threads and the UI.
#ifndef SESSIONTYPES_H
#define SESSIONTYPES_H

#include <QList>
#include <QMetaType>
#include <QString>

// Mirrors the mic_status vocabulary the bridge exposed, so the operator-facing
// labels and the evidence dashboard keep the same states they were validated
// against.
enum class MicStatus {
    Idle,
    Starting,
    Recording,
    Paused,
    NetworkReconnecting,
    DeviceReconnecting,
    Finalizing,
    Stopped,
    Error,
};

QString micStatusKey(MicStatus status);
QString micStatusLabel(MicStatus status);

struct MicStatusTransition
{
    double ts = 0.0;
    QString from;
    QString to;
};

// One row per audio packet actually sent (~160 ms cadence).  A running record
// of every stage measured, not just whatever the latest values happen to be
// when someone next looks - a queue trending up across consecutive rows is
// the signal that the pipeline is falling behind real time.
struct LatencySample
{
    double ts = 0.0;
    double rpcMs = 0.0;
    double aiWaitMs = 0.0;
    double transportMs = 0.0;
    double localQueueSec = 0.0;
    double serverQueueSec = 0.0;
};

struct SessionTelemetry
{
    double capturedSec = 0.0;
    double sentSec = 0.0;
    // Captured but not yet ACKed by the server.
    double localQueueSec = 0.0;
    // ACKed (durably spooled) but not yet consumed by the AI.  ACK means
    // "stored", never "inferred" - these two are deliberately separate.
    double serverQueueSec = 0.0;
    double rpcLastMs = 0.0;
    double rpcMaxMs = 0.0;
    double aiWaitLastMs = 0.0;
    double transportRoundTripMs = 0.0;
    double transportOneWayEstMs = 0.0;
    double statePollLastMs = 0.0;
    double statePollMaxMs = 0.0;
    int droppedChunks = 0;
    QString pollError;
};

struct SessionMeta
{
    QString title;
    QStringList participants;
    QString securityLevel; // "" | thuong | mat | toi_mat | tuyet_mat
    QString mode = QStringLiteral("record_and_s2t"); // or record_only
};

struct FinishedSession
{
    QString sessionId;
    QString sourceName;
    double durationSec = 0.0;
    quint64 revision = 0;
};

Q_DECLARE_METATYPE(MicStatus)
Q_DECLARE_METATYPE(LatencySample)
Q_DECLARE_METATYPE(SessionTelemetry)
Q_DECLARE_METATYPE(SessionMeta)
Q_DECLARE_METATYPE(FinishedSession)

#endif // SESSIONTYPES_H
