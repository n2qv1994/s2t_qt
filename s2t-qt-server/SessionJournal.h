// The on-disk record that lets a meeting survive the Server buffer restarting.
//
// Without this the queue lives only in RAM: restart the server and every open
// session is gone, `push_audio` answers NOT_FOUND, and an operator has to start
// a new meeting.  With it, a restart costs the client a few retries - which its
// transport-retry loop already absorbs - and nothing else.
//
// Shape: one append-only journal per session, split into segments.
//
//   <dir>/<handle>.000001.jrn
//   <dir>/<handle>.000002.jrn
//
//   segment header  "S2TJ" | version u32 | segment u32 | reserved u32
//   record          type u8 | length u32 | payload | crc32 u32
//
// Four record types.  Meta is written once at the head of the first segment;
// Packet is appended before the client is ACKed; Progress is appended after the
// inference tier acknowledges a packet; Stopped marks a clean end.
//
// Two rules make recovery correct, and both are about ordering:
//
//   1. The Packet record is written *before* the client is ACKed.  An ACK means
//      "durably in this buffer", and that has to stay true across a restart or
//      it was never true at all.
//   2. The Progress record is written *after* the forward succeeds.  A crash in
//      between replays that seq upstream, and the inference tier's own seq
//      idempotency turns the duplicate into a no-op.  A Progress record written
//      first would instead lose the packet.
//
// A torn final record is normal, not corruption: a crash mid-write leaves one.
// The CRC catches it and the reader treats it as end of data.
#ifndef SESSIONJOURNAL_H
#define SESSIONJOURNAL_H

#include "proto/AsrSession.h"

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>

namespace jrn {

// How far a write is pushed before the client is told the packet is safe.
enum class Durability {
    // write() returned.  Survives the process dying and the server being
    // restarted - which is what this feature is for - but not the machine
    // losing power.
    Os,
    // fsync() returned.  Survives power loss, and costs one fsync per packet.
    Fsync,
};

// What happens to a segment once everything in it has reached the pipeline.
enum class Keep {
    // Delete it.  Disk use then tracks the queue depth, which the buffer
    // already bounds - a healthy session holds one or two segments.
    Queue,
    // Keep it until the session is forgotten.  The journal doubles as an
    // archive of the audio the pipeline was given, at the cost of the whole
    // meeting on disk.
    Session,
};

// One packet exactly as the client sent it.  Also the element type of the
// in-memory queue, so a recovered packet and a live one are the same thing.
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

struct Meta
{
    QString sessionId;
    QString client;
    double startedAt = 0.0;
    QString configJson;
    // The whole StartSessionResponse, serialized.  Recovery gets stream_id and
    // the pipeline's first state back exactly, rather than approximating them.
    QByteArray startResponse;
};

// The pipeline's own progress, as of the last packet it acknowledged.
struct Progress
{
    quint64 seq = 0;
    double sourceSeenSec = 0.0;
    double speechSeenSec = 0.0;
    quint64 stateVersion = 0;
};

// Everything needed to rebuild a SessionBuffer after a restart.
struct Recovered
{
    QString handle; // filename stem, not the session id
    Meta meta;
    asr::StartSessionResponse started;
    Progress progress;
    // Accepted but never acknowledged upstream, in seq order.  These are
    // re-sent first, before anything new the client pushes.
    QList<Packet> backlog;
    quint64 lastAcceptedSeq = 0;
    quint64 acceptedPackets = 0;
    quint64 acceptedBytes = 0;
    quint64 forwardedPackets = 0;
    bool stopped = false;
    // True when a record failed its CRC or ran off the end of a segment. Normal
    // after a crash; worth reporting once, because it is also what a damaged
    // disk looks like.
    bool truncated = false;
};

// The writer.  One per session, owned by its SessionBuffer, and only ever
// touched with that buffer's mutex held.
class Journal
{
public:
    Journal();
    ~Journal();

    Journal(const Journal &) = delete;
    Journal &operator=(const Journal &) = delete;

    // Opens a new journal and writes the Meta record.  Returns false with
    // *error set if the directory or the first segment cannot be created - and
    // that is fatal to the durability promise, so the caller must refuse the
    // session rather than run without it.
    bool create(const QString &dir, const QString &handle, const Meta &meta,
                Durability durability, Keep keep, qint64 segmentBytes, QString *error);
    // Re-opens after recovery, appending to a fresh segment.  `meta` is the one
    // that came back from store::recover(), passed in rather than re-read: it
    // is restated at the head of the new segment, and re-reading the journal
    // here would double the I/O of every recovery.
    bool reopen(const QString &dir, const QString &handle, const Meta &meta,
                Durability durability, Keep keep, qint64 segmentBytes, QString *error);

    bool isOpen() const { return m_file.isOpen(); }
    void close();

    // Each returns false on a write error; the caller must then refuse the
    // packet, because a silent failure here turns the ACK into a lie.
    bool appendPacket(const Packet &packet, QString *error);
    bool appendProgress(const Progress &progress, QString *error);
    bool appendStopped(bool ok, QString *error);

    quint64 bytesWritten() const { return m_bytesWritten; }
    // Segments deleted so far because everything in them was forwarded.
    int segmentsRetired() const { return m_retired; }

private:
    bool openSegment(int index, QString *error);
    bool write(quint8 type, const QByteArray &payload, QString *error);
    void retireSegmentsBelow(quint64 seq);

    QString m_dir;
    QString m_handle;
    QFile m_file;
    Durability m_durability = Durability::Os;
    Keep m_keep = Keep::Queue;
    qint64 m_segmentBytes = 16 * 1024 * 1024;
    int m_segment = 0;
    quint64 m_bytesWritten = 0;
    int m_retired = 0;
    // Highest seq in each segment still on disk, so a Progress record can
    // retire whole segments without re-reading them.
    QList<QPair<int, quint64>> m_segmentHighSeq;
    QByteArray m_metaPayload;
};

// Directory-level operations.
namespace store {

// Every session handle with at least one segment in `dir`.
QStringList handles(const QString &dir);

// Reads one session back.  Skips packet payloads that are already forwarded, so
// this is a seek over the journal rather than a read of it.
bool recover(const QString &dir, const QString &handle, Recovered *out, QString *error);

// Deletes every segment of one session.
void remove(const QString &dir, const QString &handle);

// The filename stem for a session id.  Session ids come from the inference
// tier, so they are not trusted to be filename-safe: unsafe characters are
// replaced and a short digest of the original is appended, which also keeps two
// ids that sanitise the same from colliding.
QString handleFor(const QString &sessionId);

} // namespace store

} // namespace jrn

#endif // SESSIONJOURNAL_H
