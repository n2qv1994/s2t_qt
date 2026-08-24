// Proto3 wire-format codec, hand written.
//
// The Qt kit on this machine ships neither Qt::Protobuf nor protoc, and the
// project must build with nothing but Qt + MinGW.  Proto3 encoding is small
// enough to own outright: varints, four wire types, and length-delimited
// submessages.  Everything under proto/ is generated-by-hand equivalent of
// what protoc would emit for asr_session.proto / speaker_registry.proto.
#ifndef PROTOWIRE_H
#define PROTOWIRE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace pw {

enum WireType {
    VarintType = 0,
    Fixed64Type = 1,
    LengthDelimitedType = 2,
    StartGroupType = 3,
    EndGroupType = 4,
    Fixed32Type = 5,
};

// Append-only encoder.  Proto3 default values are never emitted (that is what
// the server's own protobuf runtime does, and it keeps push_audio requests as
// small as the Python client's were).
class Writer
{
public:
    Writer() = default;

    const QByteArray &data() const { return m_buf; }
    QByteArray take() { return m_buf; }
    void clear() { m_buf.clear(); }

    static void appendVarint(QByteArray &out, quint64 value);
    void putVarint(quint64 value) { appendVarint(m_buf, value); }
    void putTag(int field, WireType type) { putVarint((quint64(field) << 3) | quint64(type)); }

    void putInt32(int field, qint32 value);
    void putInt64(int field, qint64 value);
    void putUInt32(int field, quint32 value);
    void putUInt64(int field, quint64 value);
    void putBool(int field, bool value);
    void putEnum(int field, int value);
    void putFloat(int field, float value);
    void putDouble(int field, double value);
    void putString(int field, const QString &value);
    void putBytes(int field, const QByteArray &value);
    void putSubMessage(int field, const QByteArray &encoded);

    // Repeated scalars are read back packed by the adapter's protobuf runtime,
    // so write them packed too.
    void putPackedFloat(int field, const QList<float> &values);
    void putPackedInt32(int field, const QList<qint32> &values);
    void putPackedInt64(int field, const QList<qint64> &values);

    template <typename T>
    void putRepeatedMessage(int field, const QList<T> &items)
    {
        for (const T &item : items)
            putSubMessage(field, item.serialize());
    }

    void putRepeatedString(int field, const QList<QString> &values)
    {
        for (const QString &value : values)
            putString(field, value);
    }

private:
    QByteArray m_buf;
};

// Single-pass decoder over a borrowed buffer.  `ok()` goes false and stays
// false on any malformed input, so a caller can check once at the end instead
// of at every field.
class Reader
{
public:
    Reader(const char *begin, const char *end) : m_p(begin), m_end(end) {}
    explicit Reader(const QByteArray &buf) : m_p(buf.constData()), m_end(buf.constData() + buf.size()) {}
    // The buffer is borrowed, not copied.  Binding to a temporary would leave
    // the reader pointing at freed memory the moment the full expression ends
    // - `Reader r(msg.serialize())` reads garbage rather than failing, so make
    // it a compile error instead of a very quiet bug.
    explicit Reader(QByteArray &&) = delete;

    bool atEnd() const { return m_p >= m_end; }
    bool ok() const { return m_ok; }
    void fail() { m_ok = false; m_p = m_end; }

    // Reads the next tag.  Returns false at end of buffer or on error.
    bool nextField(int *field, WireType *type);

    quint64 readVarint();
    qint32 readInt32() { return qint32(qint64(readVarint())); }
    qint64 readInt64() { return qint64(readVarint()); }
    quint32 readUInt32() { return quint32(readVarint()); }
    quint64 readUInt64() { return readVarint(); }
    bool readBool() { return readVarint() != 0; }
    int readEnum() { return int(qint64(readVarint())); }
    float readFloat();
    double readDouble();
    QByteArray readBytes();
    QString readString();

    // Repeated scalar fields may arrive packed or unpacked depending on the
    // encoder; both shapes have to be accepted for the same field number.
    void readPackedFloat(WireType type, QList<float> *out);
    void readPackedInt32(WireType type, QList<qint32> *out);
    void readPackedInt64(WireType type, QList<qint64> *out);

    template <typename T>
    void readMessageInto(T *out)
    {
        const QByteArray sub = readBytes();
        if (!m_ok)
            return;
        Reader nested(sub);
        out->parse(nested);
        if (!nested.ok())
            m_ok = false;
    }

    template <typename T>
    void appendMessage(QList<T> *out)
    {
        T item;
        readMessageInto(&item);
        if (m_ok)
            out->append(item);
    }

    void skip(WireType type);

private:
    const char *m_p = nullptr;
    const char *m_end = nullptr;
    bool m_ok = true;
};

} // namespace pw

#endif // PROTOWIRE_H
