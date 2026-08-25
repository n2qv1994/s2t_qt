#include "ProtoWire.h"

#include <cstring>

namespace pw {

void Writer::appendVarint(QByteArray &out, quint64 value)
{
    char scratch[10];
    int n = 0;
    do {
        char byte = char(value & 0x7f);
        value >>= 7;
        if (value)
            byte = char(quint8(byte) | 0x80);
        scratch[n++] = byte;
    } while (value);
    out.append(scratch, n);
}

void Writer::putInt32(int field, qint32 value)
{
    if (value == 0)
        return;
    putTag(field, VarintType);
    // Negative int32 is sign-extended to 64 bits on the wire, exactly as the
    // reference implementation does; the adapter has no negative int32 fields
    // today but Word/DisplayRow indices could grow one.
    putVarint(quint64(qint64(value)));
}

void Writer::putInt64(int field, qint64 value)
{
    if (value == 0)
        return;
    putTag(field, VarintType);
    putVarint(quint64(value));
}

void Writer::putUInt32(int field, quint32 value)
{
    if (value == 0)
        return;
    putTag(field, VarintType);
    putVarint(value);
}

void Writer::putUInt64(int field, quint64 value)
{
    if (value == 0)
        return;
    putTag(field, VarintType);
    putVarint(value);
}

void Writer::putBool(int field, bool value)
{
    if (!value)
        return;
    putTag(field, VarintType);
    putVarint(1);
}

void Writer::putEnum(int field, int value)
{
    if (value == 0)
        return;
    putTag(field, VarintType);
    putVarint(quint64(qint64(value)));
}

void Writer::putFloat(int field, float value)
{
    if (value == 0.0f)
        return;
    putTag(field, Fixed32Type);
    quint32 bits = 0;
    std::memcpy(&bits, &value, 4);
    char raw[4];
    for (int i = 0; i < 4; ++i)
        raw[i] = char((bits >> (8 * i)) & 0xff);
    m_buf.append(raw, 4);
}

void Writer::putOptionalInt32(int field, qint32 value)
{
    putTag(field, VarintType);
    putVarint(quint64(qint64(value)));
}

void Writer::putOptionalFloat(int field, float value)
{
    putTag(field, Fixed32Type);
    quint32 bits = 0;
    std::memcpy(&bits, &value, 4);
    char raw[4];
    for (int i = 0; i < 4; ++i)
        raw[i] = char((bits >> (8 * i)) & 0xff);
    m_buf.append(raw, 4);
}

void Writer::putDouble(int field, double value)
{
    if (value == 0.0)
        return;
    putTag(field, Fixed64Type);
    quint64 bits = 0;
    std::memcpy(&bits, &value, 8);
    char raw[8];
    for (int i = 0; i < 8; ++i)
        raw[i] = char((bits >> (8 * i)) & 0xff);
    m_buf.append(raw, 8);
}

void Writer::putString(int field, const QString &value)
{
    if (value.isEmpty())
        return;
    putBytes(field, value.toUtf8());
}

void Writer::putBytes(int field, const QByteArray &value)
{
    if (value.isEmpty())
        return;
    putTag(field, LengthDelimitedType);
    putVarint(quint64(value.size()));
    m_buf.append(value);
}

void Writer::putSubMessage(int field, const QByteArray &encoded)
{
    // An empty submessage still has to go on the wire: `Timing{}` present and
    // `Timing` absent mean different things to a reader that checks presence.
    putTag(field, LengthDelimitedType);
    putVarint(quint64(encoded.size()));
    m_buf.append(encoded);
}

void Writer::putPackedFloat(int field, const QList<float> &values)
{
    if (values.isEmpty())
        return;
    QByteArray payload;
    payload.reserve(values.size() * 4);
    for (float value : values) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, 4);
        for (int i = 0; i < 4; ++i)
            payload.append(char((bits >> (8 * i)) & 0xff));
    }
    putBytes(field, payload);
}

void Writer::putPackedInt32(int field, const QList<qint32> &values)
{
    if (values.isEmpty())
        return;
    QByteArray payload;
    for (qint32 value : values)
        appendVarint(payload, quint64(qint64(value)));
    putBytes(field, payload);
}

void Writer::putPackedInt64(int field, const QList<qint64> &values)
{
    if (values.isEmpty())
        return;
    QByteArray payload;
    for (qint64 value : values)
        appendVarint(payload, quint64(value));
    putBytes(field, payload);
}

bool Reader::nextField(int *field, WireType *type)
{
    if (!m_ok || m_p >= m_end)
        return false;
    const quint64 tag = readVarint();
    if (!m_ok)
        return false;
    const int number = int(tag >> 3);
    const int wire = int(tag & 0x07);
    if (number <= 0 || wire > 5) {
        fail();
        return false;
    }
    *field = number;
    *type = WireType(wire);
    return true;
}

quint64 Reader::readVarint()
{
    quint64 value = 0;
    int shift = 0;
    while (m_p < m_end) {
        const quint8 byte = quint8(*m_p++);
        // Ten groups of seven bits is the most a 64-bit varint can occupy; a
        // longer run means the buffer is corrupt, not that the value is huge.
        if (shift > 63) {
            fail();
            return 0;
        }
        value |= quint64(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return value;
        shift += 7;
    }
    fail();
    return 0;
}

float Reader::readFloat()
{
    if (m_end - m_p < 4) {
        fail();
        return 0.0f;
    }
    quint32 bits = 0;
    for (int i = 0; i < 4; ++i)
        bits |= quint32(quint8(m_p[i])) << (8 * i);
    m_p += 4;
    float value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

double Reader::readDouble()
{
    if (m_end - m_p < 8) {
        fail();
        return 0.0;
    }
    quint64 bits = 0;
    for (int i = 0; i < 8; ++i)
        bits |= quint64(quint8(m_p[i])) << (8 * i);
    m_p += 8;
    double value = 0.0;
    std::memcpy(&value, &bits, 8);
    return value;
}

QByteArray Reader::readBytes()
{
    const quint64 length = readVarint();
    if (!m_ok)
        return QByteArray();
    if (quint64(m_end - m_p) < length) {
        fail();
        return QByteArray();
    }
    QByteArray out(m_p, int(length));
    m_p += length;
    return out;
}

QString Reader::readString()
{
    return QString::fromUtf8(readBytes());
}

void Reader::readPackedFloat(WireType type, QList<float> *out)
{
    if (type != LengthDelimitedType) {
        if (type == Fixed32Type)
            out->append(readFloat());
        else
            skip(type);
        return;
    }
    const QByteArray payload = readBytes();
    Reader nested(payload);
    while (!nested.atEnd() && nested.ok())
        out->append(nested.readFloat());
}

void Reader::readPackedInt32(WireType type, QList<qint32> *out)
{
    if (type != LengthDelimitedType) {
        if (type == VarintType)
            out->append(readInt32());
        else
            skip(type);
        return;
    }
    const QByteArray payload = readBytes();
    Reader nested(payload);
    while (!nested.atEnd() && nested.ok())
        out->append(nested.readInt32());
}

void Reader::readPackedInt64(WireType type, QList<qint64> *out)
{
    if (type != LengthDelimitedType) {
        if (type == VarintType)
            out->append(readInt64());
        else
            skip(type);
        return;
    }
    const QByteArray payload = readBytes();
    Reader nested(payload);
    while (!nested.atEnd() && nested.ok())
        out->append(nested.readInt64());
}

void Reader::skip(WireType type)
{
    switch (type) {
    case VarintType:
        readVarint();
        break;
    case Fixed64Type:
        if (m_end - m_p < 8)
            fail();
        else
            m_p += 8;
        break;
    case LengthDelimitedType:
        readBytes();
        break;
    case Fixed32Type:
        if (m_end - m_p < 4)
            fail();
        else
            m_p += 4;
        break;
    case StartGroupType:
    case EndGroupType:
    default:
        // Proto3 has no groups; anything claiming to be one is corrupt.
        fail();
        break;
    }
}

void Writer::putStringMap(int field, const QMap<QString, QString> &values)
{
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        Writer entry;
        entry.putString(1, it.key());
        entry.putString(2, it.value());
        putSubMessage(field, entry.data());
    }
}

void Reader::readStringMapEntry(QMap<QString, QString> *out)
{
    const QByteArray sub = readBytes();
    if (!m_ok)
        return;
    Reader nested(sub);
    QString key;
    QString value;
    int field = 0;
    WireType type = VarintType;
    while (nested.nextField(&field, &type)) {
        switch (field) {
        case 1: key = nested.readString(); break;
        case 2: value = nested.readString(); break;
        default: nested.skip(type); break;
        }
    }
    if (!nested.ok()) {
        m_ok = false;
        return;
    }
    out->insert(key, value);
}

} // namespace pw
