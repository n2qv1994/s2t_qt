#include "SessionJournal.h"

#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace jrn {
namespace {

const char kMagic[4] = {'S', '2', 'T', 'J'};
const quint32 kVersion = 1;
const int kHeaderBytes = 16;

enum RecordType : quint8 {
    RecMeta = 1,
    RecPacket = 2,
    RecProgress = 3,
    RecStopped = 4,
};

// A record header is 5 bytes and the trailing CRC is 4.  Anything shorter than
// the pair is a torn tail, not a record.
const int kRecordOverhead = 9;

// Refuse a record larger than this rather than trying to allocate it.  The
// biggest thing here is one audio packet; gRPC's own request cap is 64 MiB and
// this sits just above it, so a plausible record always fits and a corrupt
// length never allocates a gigabyte.
const int kMaxRecordBytes = 80 * 1024 * 1024;

quint32 crc32(const char *data, int length, quint32 seed = 0)
{
    static quint32 table[256];
    static bool built = false;
    if (!built) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    quint32 c = seed ^ 0xffffffffu;
    for (int i = 0; i < length; ++i)
        c = table[(c ^ quint8(data[i])) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(char((value >> 24) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

quint32 readU32(const QByteArray &buf, int offset)
{
    return (quint32(quint8(buf.at(offset))) << 24) | (quint32(quint8(buf.at(offset + 1))) << 16)
        | (quint32(quint8(buf.at(offset + 2))) << 8) | quint32(quint8(buf.at(offset + 3)));
}

QString segmentName(const QString &handle, int index)
{
    return QStringLiteral("%1.%2.jrn").arg(handle).arg(index, 6, 10, QLatin1Char('0'));
}

// Pushes the file all the way to the platter.  QFileDevice::flush() only gets
// Qt's own buffer into the OS, which is not the same promise.
bool syncToDisk(QFile &file)
{
    if (!file.flush())
        return false;
    const int handle = int(file.handle());
    if (handle < 0)
        return false;
#ifdef Q_OS_WIN
    return _commit(handle) == 0;
#else
    return fsync(handle) == 0;
#endif
}

// ---- payload encoding ------------------------------------------------------
//
// Reuses the project's proto3 codec.  These field numbers are internal to the
// journal - nothing outside this process reads them - so they are free to
// change with the version number in the segment header.

QByteArray encodeMeta(const Meta &meta)
{
    pw::Writer w;
    w.putString(1, meta.sessionId);
    w.putString(2, meta.client);
    w.putDouble(3, meta.startedAt);
    w.putString(4, meta.configJson);
    w.putBytes(5, meta.startResponse);
    return w.take();
}

bool decodeMeta(const QByteArray &payload, Meta *out)
{
    pw::Reader r(payload);
    int field = 0;
    pw::WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: out->sessionId = r.readString(); break;
        case 2: out->client = r.readString(); break;
        case 3: out->startedAt = r.readDouble(); break;
        case 4: out->configJson = r.readString(); break;
        case 5: out->startResponse = r.readBytes(); break;
        default: r.skip(type); break;
        }
    }
    return r.ok() && !out->sessionId.isEmpty();
}

QByteArray encodePacket(const Packet &packet)
{
    pw::Writer w;
    w.putUInt64(1, packet.seq);
    w.putBytes(2, packet.pcm);
    w.putBool(3, packet.reset);
    w.putUInt32(4, packet.vadChunkMs);
    w.putUInt32(5, packet.sampleRate);
    w.putUInt32(6, packet.channels);
    w.putString(7, packet.audioFormat);
    return w.take();
}

bool decodePacket(const QByteArray &payload, Packet *out)
{
    pw::Reader r(payload);
    int field = 0;
    pw::WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: out->seq = r.readUInt64(); break;
        case 2: out->pcm = r.readBytes(); break;
        case 3: out->reset = r.readBool(); break;
        case 4: out->vadChunkMs = r.readUInt32(); break;
        case 5: out->sampleRate = r.readUInt32(); break;
        case 6: out->channels = r.readUInt32(); break;
        case 7: out->audioFormat = r.readString(); break;
        default: r.skip(type); break;
        }
    }
    return r.ok();
}

QByteArray encodeProgress(const Progress &progress)
{
    pw::Writer w;
    w.putUInt64(1, progress.seq);
    w.putDouble(2, progress.sourceSeenSec);
    w.putDouble(3, progress.speechSeenSec);
    w.putUInt64(4, progress.stateVersion);
    return w.take();
}

bool decodeProgress(const QByteArray &payload, Progress *out)
{
    pw::Reader r(payload);
    int field = 0;
    pw::WireType type = pw::VarintType;
    while (r.nextField(&field, &type)) {
        switch (field) {
        case 1: out->seq = r.readUInt64(); break;
        case 2: out->sourceSeenSec = r.readDouble(); break;
        case 3: out->speechSeenSec = r.readDouble(); break;
        case 4: out->stateVersion = r.readUInt64(); break;
        default: r.skip(type); break;
        }
    }
    return r.ok();
}

// Reads one record.  `partial` comes back true for a torn tail, which is the
// expected shape after a crash and must not be reported as corruption.
enum class ReadResult { Ok, End, Partial, Bad };

ReadResult readRecord(QFile &file, quint8 *type, QByteArray *payload)
{
    const QByteArray head = file.read(5);
    if (head.isEmpty())
        return ReadResult::End;
    if (head.size() < 5)
        return ReadResult::Partial;
    *type = quint8(head.at(0));
    const quint32 length = readU32(head, 1);
    if (length > kMaxRecordBytes)
        return ReadResult::Bad;
    const QByteArray body = file.read(qint64(length) + 4);
    if (body.size() < int(length) + 4)
        return ReadResult::Partial;
    const quint32 stored = readU32(body, int(length));
    // Over header-then-payload as one stream, matching how write() builds it.
    QByteArray whole = head;
    whole.append(body.constData(), int(length));
    if (crc32(whole.constData(), whole.size()) != stored) {
        // Either a half-written record from a crash or a damaged disk.  The
        // reader cannot tell the two apart and treats both as end of data,
        // which is right for the first and the safest available answer for the
        // second - it never hands back a record it is not sure of.
        return ReadResult::Partial;
    }
    *payload = body.left(int(length));
    return ReadResult::Ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

Journal::Journal() = default;

Journal::~Journal()
{
    close();
}

void Journal::close()
{
    if (m_file.isOpen())
        m_file.close();
}

bool Journal::create(const QString &dir, const QString &handle, const Meta &meta,
                     Durability durability, Keep keep, qint64 segmentBytes, QString *error)
{
    m_dir = dir;
    m_handle = handle;
    m_durability = durability;
    m_keep = keep;
    m_segmentBytes = qMax(qint64(256 * 1024), segmentBytes);
    m_metaPayload = encodeMeta(meta);

    QDir target(dir);
    if (!target.exists() && !target.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("không tạo được thư mục nhật ký '%1'").arg(dir);
        return false;
    }
    // A handle left behind by an earlier session with the same id would be
    // read back at the next recovery and mixed with this one.
    store::remove(dir, handle);
    return openSegment(1, error);
}

bool Journal::reopen(const QString &dir, const QString &handle, const Meta &meta,
                     Durability durability, Keep keep, qint64 segmentBytes, QString *error)
{
    m_dir = dir;
    m_handle = handle;
    m_durability = durability;
    m_keep = keep;
    m_segmentBytes = qMax(qint64(256 * 1024), segmentBytes);
    // Restated at the head of the new segment, so a later recovery can start
    // from any surviving segment even after the earlier ones were retired.
    m_metaPayload = encodeMeta(meta);

    // Continue past the highest segment on disk rather than appending to it.
    // The last one may end in a torn record, and writing after that would bury
    // the tear in the middle of a segment where the reader stops at it and
    // silently loses everything after.
    int highest = 0;
    const QDir target(dir);
    const QStringList names =
        target.entryList(QStringList{handle + QStringLiteral(".*.jrn")}, QDir::Files);
    for (const QString &name : names) {
        const QStringList parts = name.split(QLatin1Char('.'));
        if (parts.size() >= 3)
            highest = qMax(highest, parts.at(parts.size() - 2).toInt());
    }
    if (highest == 0) {
        *error = QStringLiteral("không còn phân đoạn nào cho '%1'").arg(handle);
        return false;
    }
    return openSegment(highest + 1, error);
}

bool Journal::openSegment(int index, QString *error)
{
    if (m_file.isOpen())
        m_file.close();
    m_segment = index;
    m_file.setFileName(QDir(m_dir).filePath(segmentName(m_handle, index)));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        *error = QStringLiteral("không mở được '%1' - %2")
                     .arg(m_file.fileName(), m_file.errorString());
        return false;
    }
    if (m_file.size() == 0) {
        QByteArray header(kMagic, 4);
        appendU32(header, kVersion);
        appendU32(header, quint32(index));
        appendU32(header, 0);
        if (m_file.write(header) != header.size()) {
            *error = QStringLiteral("không ghi được đầu phân đoạn '%1'").arg(m_file.fileName());
            return false;
        }
    }
    m_segmentHighSeq.append(qMakePair(index, quint64(0)));
    // Every segment carries the Meta record, so recovery does not depend on the
    // first segment still existing after the queue-mode cleanup has run.
    if (!m_metaPayload.isEmpty() && !write(RecMeta, m_metaPayload, error))
        return false;
    return true;
}

bool Journal::write(quint8 type, const QByteArray &payload, QString *error)
{
    QByteArray record;
    record.reserve(payload.size() + kRecordOverhead);
    record.append(char(type));
    appendU32(record, quint32(payload.size()));
    record.append(payload);
    appendU32(record, crc32(record.constData(), record.size()));

    if (m_file.write(record) != record.size()) {
        *error = QStringLiteral("ghi nhật ký thất bại - %1").arg(m_file.errorString());
        return false;
    }
    if (m_durability == Durability::Fsync) {
        if (!syncToDisk(m_file)) {
            *error = QStringLiteral("fsync nhật ký thất bại - %1").arg(m_file.errorString());
            return false;
        }
    } else if (!m_file.flush()) {
        // Qt buffers writes; without this the "durable" record can still be
        // sitting in this process when it dies, which is the one failure this
        // whole file exists to prevent.
        *error = QStringLiteral("flush nhật ký thất bại - %1").arg(m_file.errorString());
        return false;
    }
    m_bytesWritten += quint64(record.size());
    return true;
}

bool Journal::appendPacket(const Packet &packet, QString *error)
{
    if (!m_file.isOpen()) {
        *error = QStringLiteral("nhật ký chưa mở");
        return false;
    }
    if (m_file.size() >= m_segmentBytes && !openSegment(m_segment + 1, error))
        return false;
    if (!write(RecPacket, encodePacket(packet), error))
        return false;
    if (!m_segmentHighSeq.isEmpty())
        m_segmentHighSeq.last().second = packet.seq;
    return true;
}

bool Journal::appendProgress(const Progress &progress, QString *error)
{
    if (!m_file.isOpen()) {
        *error = QStringLiteral("nhật ký chưa mở");
        return false;
    }
    if (!write(RecProgress, encodeProgress(progress), error))
        return false;
    retireSegmentsBelow(progress.seq);
    return true;
}

bool Journal::appendStopped(bool ok, QString *error)
{
    if (!m_file.isOpen()) {
        *error = QStringLiteral("nhật ký chưa mở");
        return false;
    }
    pw::Writer w;
    w.putDouble(1, 0.0);
    w.putBool(2, ok);
    // Always fsynced, whatever the mode: this is the record that says the
    // meeting is over, and replaying a finished session on the next start is
    // worse than the cost of one sync.
    const bool result = write(RecStopped, w.take(), error);
    syncToDisk(m_file);
    return result;
}

void Journal::retireSegmentsBelow(quint64 seq)
{
    if (m_keep != Keep::Queue)
        return;
    // A segment can go once the pipeline has acknowledged everything in it.
    // The newest segment is never retired - it is the one being written.
    while (m_segmentHighSeq.size() > 1) {
        const auto oldest = m_segmentHighSeq.first();
        if (oldest.second == 0 || oldest.second > seq)
            break;
        m_segmentHighSeq.removeFirst();
        const QString path = QDir(m_dir).filePath(segmentName(m_handle, oldest.first));
        if (QFile::remove(path)) {
            ++m_retired;
            LOG_DEBUG(applog::cat::Session)
                << "retired journal segment" << path << "- everything up to seq" << oldest.second
                << "is upstream";
        }
    }
}

// ---------------------------------------------------------------------------
// store
// ---------------------------------------------------------------------------

namespace store {

QString handleFor(const QString &sessionId)
{
    QString safe;
    safe.reserve(sessionId.size());
    for (const QChar ch : sessionId) {
        const bool plain = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || ch == QLatin1Char('-')
            || ch == QLatin1Char('_');
        safe.append(plain ? ch : QLatin1Char('_'));
    }
    safe.truncate(48);
    // The digest is what keeps two ids that sanitise to the same stem apart,
    // and it makes the mapping reproducible across restarts.
    const QByteArray digest =
        QCryptographicHash::hash(sessionId.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return safe + QLatin1Char('-') + QString::fromLatin1(digest);
}

QStringList handles(const QString &dir)
{
    QStringList out;
    const QDir target(dir);
    if (!target.exists())
        return out;
    const QStringList names = target.entryList(QStringList{QStringLiteral("*.jrn")}, QDir::Files);
    for (const QString &name : names) {
        const QStringList parts = name.split(QLatin1Char('.'));
        if (parts.size() < 3)
            continue;
        const QString handle = parts.mid(0, parts.size() - 2).join(QLatin1Char('.'));
        if (!handle.isEmpty() && !out.contains(handle))
            out.append(handle);
    }
    out.sort();
    return out;
}

void remove(const QString &dir, const QString &handle)
{
    const QDir target(dir);
    const QStringList names =
        target.entryList(QStringList{handle + QStringLiteral(".*.jrn")}, QDir::Files);
    for (const QString &name : names)
        QFile::remove(target.filePath(name));
}

bool recover(const QString &dir, const QString &handle, Recovered *out, QString *error)
{
    out->handle = handle;
    const QDir target(dir);
    QStringList names =
        target.entryList(QStringList{handle + QStringLiteral(".*.jrn")}, QDir::Files);
    if (names.isEmpty()) {
        *error = QStringLiteral("không có phân đoạn nào cho '%1'").arg(handle);
        return false;
    }
    // Segment order is the record order; the zero-padded index sorts correctly
    // as text, which is why it is padded.
    names.sort();

    bool haveMeta = false;
    // First pass: everything except packet payloads.  Packets are collected
    // too, but only their headers are needed until the watermark is known - so
    // they are kept and filtered at the end rather than read twice.
    QList<Packet> packets;

    for (const QString &name : names) {
        QFile file(target.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            LOG_WARN(applog::cat::Session)
                << "cannot read journal segment" << file.fileName() << "-" << file.errorString();
            out->truncated = true;
            continue;
        }
        const QByteArray header = file.read(kHeaderBytes);
        if (header.size() < kHeaderBytes || !header.startsWith(QByteArray(kMagic, 4))) {
            LOG_WARN(applog::cat::Session) << "not a journal segment:" << file.fileName();
            out->truncated = true;
            continue;
        }
        if (readU32(header, 4) != kVersion) {
            *error = QStringLiteral("phân đoạn '%1' thuộc phiên bản nhật ký khác").arg(name);
            return false;
        }

        bool done = false;
        while (!done) {
            quint8 type = 0;
            QByteArray payload;
            switch (readRecord(file, &type, &payload)) {
            case ReadResult::End:
                done = true;
                continue;
            case ReadResult::Partial:
                // A crash mid-write.  Everything before this point is good and
                // everything after is gone; that is the contract.
                out->truncated = true;
                done = true;
                continue;
            case ReadResult::Bad:
                out->truncated = true;
                done = true;
                continue;
            case ReadResult::Ok:
                break;
            }

            switch (type) {
            case RecMeta:
                if (!haveMeta && decodeMeta(payload, &out->meta)) {
                    haveMeta = true;
                    pw::Reader reader(out->meta.startResponse);
                    out->started.parse(reader);
                }
                break;
            case RecPacket: {
                Packet packet;
                if (decodePacket(payload, &packet)) {
                    packets.append(packet);
                    out->lastAcceptedSeq = qMax(out->lastAcceptedSeq, packet.seq);
                    ++out->acceptedPackets;
                    out->acceptedBytes += quint64(packet.pcm.size());
                }
                break;
            }
            case RecProgress: {
                Progress progress;
                if (decodeProgress(payload, &progress) && progress.seq >= out->progress.seq)
                    out->progress = progress;
                break;
            }
            case RecStopped:
                out->stopped = true;
                break;
            default:
                break;
            }
        }
    }

    if (!haveMeta) {
        *error = QStringLiteral("phiên '%1' không có bản ghi Meta").arg(handle);
        return false;
    }

    for (const Packet &packet : packets) {
        if (packet.seq > out->progress.seq)
            out->backlog.append(packet);
        else
            ++out->forwardedPackets;
    }
    std::sort(out->backlog.begin(), out->backlog.end(),
              [](const Packet &a, const Packet &b) { return a.seq < b.seq; });
    return true;
}

} // namespace store

} // namespace jrn
