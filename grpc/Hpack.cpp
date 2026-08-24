#include "Hpack.h"

namespace hpack {
namespace {

struct StaticEntry
{
    const char *name;
    const char *value;
};

// RFC 7541 Appendix A.  Index 0 is unused (HPACK indices are 1-based), so the
// padding entry keeps the arithmetic below index-for-index with the spec.
const StaticEntry kStaticTable[] = {
    {"", ""},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

const int kStaticCount = int(sizeof(kStaticTable) / sizeof(kStaticTable[0])) - 1; // 61

struct HuffCode
{
    quint32 code;
    quint8 bits;
};

// RFC 7541 Appendix B, all 257 symbols (256 octets plus EOS at index 256).
const HuffCode kHuffman[257] = {
    {0x1ff8u, 13},    {0x7fffd8u, 23},  {0xfffffe2u, 28}, {0xfffffe3u, 28},
    {0xfffffe4u, 28}, {0xfffffe5u, 28}, {0xfffffe6u, 28}, {0xfffffe7u, 28},
    {0xfffffe8u, 28}, {0xffffeau, 24},  {0x3ffffffcu, 30},{0xfffffe9u, 28},
    {0xfffffeau, 28}, {0x3ffffffdu, 30},{0xfffffebu, 28}, {0xfffffecu, 28},
    {0xfffffedu, 28}, {0xfffffeeu, 28}, {0xfffffefu, 28}, {0xffffff0u, 28},
    {0xffffff1u, 28}, {0xffffff2u, 28}, {0x3ffffffeu, 30},{0xffffff3u, 28},
    {0xffffff4u, 28}, {0xffffff5u, 28}, {0xffffff6u, 28}, {0xffffff7u, 28},
    {0xffffff8u, 28}, {0xffffff9u, 28}, {0xffffffau, 28}, {0xffffffbu, 28},
    {0x14u, 6},       {0x3f8u, 10},     {0x3f9u, 10},     {0xffau, 12},
    {0x1ff9u, 13},    {0x15u, 6},       {0xf8u, 8},       {0x7fau, 11},
    {0x3fau, 10},     {0x3fbu, 10},     {0xf9u, 8},       {0x7fbu, 11},
    {0xfau, 8},       {0x16u, 6},       {0x17u, 6},       {0x18u, 6},
    {0x0u, 5},        {0x1u, 5},        {0x2u, 5},        {0x19u, 6},
    {0x1au, 6},       {0x1bu, 6},       {0x1cu, 6},       {0x1du, 6},
    {0x1eu, 6},       {0x1fu, 6},       {0x5cu, 7},       {0xfbu, 8},
    {0x7ffcu, 15},    {0x20u, 6},       {0xffbu, 12},     {0x3fcu, 10},
    {0x1ffau, 13},    {0x21u, 6},       {0x5du, 7},       {0x5eu, 7},
    {0x5fu, 7},       {0x60u, 7},       {0x61u, 7},       {0x62u, 7},
    {0x63u, 7},       {0x64u, 7},       {0x65u, 7},       {0x66u, 7},
    {0x67u, 7},       {0x68u, 7},       {0x69u, 7},       {0x6au, 7},
    {0x6bu, 7},       {0x6cu, 7},       {0x6du, 7},       {0x6eu, 7},
    {0x6fu, 7},       {0x70u, 7},       {0x71u, 7},       {0x72u, 7},
    {0xfcu, 8},       {0x73u, 7},       {0xfdu, 8},       {0x1ffbu, 13},
    {0x7fff0u, 19},   {0x1ffcu, 13},    {0x3ffcu, 14},    {0x22u, 6},
    {0x7ffdu, 15},    {0x3u, 5},        {0x23u, 6},       {0x4u, 5},
    {0x24u, 6},       {0x5u, 5},        {0x25u, 6},       {0x26u, 6},
    {0x27u, 6},       {0x6u, 5},        {0x74u, 7},       {0x75u, 7},
    {0x28u, 6},       {0x29u, 6},       {0x2au, 6},       {0x7u, 5},
    {0x2bu, 6},       {0x76u, 7},       {0x2cu, 6},       {0x8u, 5},
    {0x9u, 5},        {0x2du, 6},       {0x77u, 7},       {0x78u, 7},
    {0x79u, 7},       {0x7au, 7},       {0x7bu, 7},       {0x7ffeu, 15},
    {0x7fcu, 11},     {0x3ffdu, 14},    {0x1ffdu, 13},    {0xffffffcu, 28},
    {0xfffe6u, 20},   {0x3fffd2u, 22},  {0xfffe7u, 20},   {0xfffe8u, 20},
    {0x3fffd3u, 22},  {0x3fffd4u, 22},  {0x3fffd5u, 22},  {0x7fffd9u, 23},
    {0x3fffd6u, 22},  {0x7fffdau, 23},  {0x7fffdbu, 23},  {0x7fffdcu, 23},
    {0x7fffddu, 23},  {0x7fffdeu, 23},  {0xffffebu, 24},  {0x7fffdfu, 23},
    {0xffffecu, 24},  {0xffffedu, 24},  {0x3fffd7u, 22},  {0x7fffe0u, 23},
    {0xffffeeu, 24},  {0x7fffe1u, 23},  {0x7fffe2u, 23},  {0x7fffe3u, 23},
    {0x7fffe4u, 23},  {0x1fffdcu, 21},  {0x3fffd8u, 22},  {0x7fffe5u, 23},
    {0x3fffd9u, 22},  {0x7fffe6u, 23},  {0x7fffe7u, 23},  {0xffffefu, 24},
    {0x3fffdau, 22},  {0x1fffddu, 21},  {0xfffe9u, 20},   {0x3fffdbu, 22},
    {0x3fffdcu, 22},  {0x7fffe8u, 23},  {0x7fffe9u, 23},  {0x1fffdeu, 21},
    {0x7fffeau, 23},  {0x3fffddu, 22},  {0x3fffdeu, 22},  {0xfffff0u, 24},
    {0x1fffdfu, 21},  {0x3fffdfu, 22},  {0x7fffebu, 23},  {0x7fffecu, 23},
    {0x1fffe0u, 21},  {0x1fffe1u, 21},  {0x3fffe0u, 22},  {0x1fffe2u, 21},
    {0x7fffedu, 23},  {0x3fffe1u, 22},  {0x7fffeeu, 23},  {0x7fffefu, 23},
    {0xfffeau, 20},   {0x3fffe2u, 22},  {0x3fffe3u, 22},  {0x3fffe4u, 22},
    {0x7ffff0u, 23},  {0x3fffe5u, 22},  {0x3fffe6u, 22},  {0x7ffff1u, 23},
    {0x3ffffe0u, 26}, {0x3ffffe1u, 26}, {0xfffebu, 20},   {0x7fff1u, 19},
    {0x3fffe7u, 22},  {0x7ffff2u, 23},  {0x3fffe8u, 22},  {0x1ffffecu, 25},
    {0x3ffffe2u, 26}, {0x3ffffe3u, 26}, {0x3ffffe4u, 26}, {0x7ffffdeu, 27},
    {0x7ffffdfu, 27}, {0x3ffffe5u, 26}, {0xfffff1u, 24},  {0x1ffffedu, 25},
    {0x7fff2u, 19},   {0x1fffe3u, 21},  {0x3ffffe6u, 26}, {0x7ffffe0u, 27},
    {0x7ffffe1u, 27}, {0x3ffffe7u, 26}, {0x7ffffe2u, 27}, {0xfffff2u, 24},
    {0x1fffe4u, 21},  {0x1fffe5u, 21},  {0x3ffffe8u, 26}, {0x3ffffe9u, 26},
    {0xffffffdu, 28}, {0x7ffffe3u, 27}, {0x7ffffe4u, 27}, {0x7ffffe5u, 27},
    {0xfffecu, 20},   {0xfffff3u, 24},  {0xfffedu, 20},   {0x1fffe6u, 21},
    {0x3fffe9u, 22},  {0x1fffe7u, 21},  {0x1fffe8u, 21},  {0x7ffff3u, 23},
    {0x3fffeau, 22},  {0x3fffebu, 22},  {0x1ffffeeu, 25}, {0x1ffffefu, 25},
    {0xfffff4u, 24},  {0xfffff5u, 24},  {0x3ffffeau, 26}, {0x7ffff4u, 23},
    {0x3ffffebu, 26}, {0x7ffffe6u, 27}, {0x3ffffecu, 26}, {0x3ffffedu, 26},
    {0x7ffffe7u, 27}, {0x7ffffe8u, 27}, {0x7ffffe9u, 27}, {0x7ffffeau, 27},
    {0x7ffffebu, 27}, {0xffffffeu, 28}, {0x7ffffecu, 27}, {0x7ffffedu, 27},
    {0x7ffffeeu, 27}, {0x7ffffefu, 27}, {0x7fffff0u, 27}, {0x3ffffeeu, 26},
    {0x3fffffffu, 30},
};

// Canonical-code decode tree, built once.  Each node is two child indices;
// -1 means "no such branch", a value >= 0 in `symbol` means "leaf".
struct HuffNode
{
    int child[2] = {-1, -1};
    int symbol = -1;
};

class HuffTree
{
public:
    HuffTree()
    {
        m_nodes.append(HuffNode());
        for (int symbol = 0; symbol < 257; ++symbol) {
            const HuffCode &entry = kHuffman[symbol];
            int node = 0;
            for (int bit = entry.bits - 1; bit >= 0; --bit) {
                const int branch = int((entry.code >> bit) & 1u);
                if (m_nodes[node].child[branch] < 0) {
                    m_nodes.append(HuffNode());
                    m_nodes[node].child[branch] = m_nodes.size() - 1;
                }
                node = m_nodes[node].child[branch];
            }
            m_nodes[node].symbol = symbol;
        }
    }

    const QList<HuffNode> &nodes() const { return m_nodes; }

private:
    QList<HuffNode> m_nodes;
};

const HuffTree &huffTree()
{
    static const HuffTree tree;
    return tree;
}

const quint32 kEntryOverhead = 32; // RFC 7541 section 4.1

quint32 entrySize(const Header &header)
{
    return quint32(header.name.size() + header.value.size()) + kEntryOverhead;
}

class BitCursor
{
public:
    BitCursor(const QByteArray &buf) : m_buf(buf) {}

    bool atEnd() const { return m_bit >= quint64(m_buf.size()) * 8; }

    int nextBit()
    {
        const quint64 byteIndex = m_bit >> 3;
        if (byteIndex >= quint64(m_buf.size()))
            return -1;
        const int shift = 7 - int(m_bit & 7);
        ++m_bit;
        return (quint8(m_buf.at(int(byteIndex))) >> shift) & 1;
    }

    quint64 bitsLeft() const { return quint64(m_buf.size()) * 8 - m_bit; }

private:
    const QByteArray &m_buf;
    quint64 m_bit = 0;
};

// Reads an HPACK integer whose first `prefixBits` live in `first`.
bool readInteger(const QByteArray &block, int *pos, quint8 first, int prefixBits, quint64 *out)
{
    const quint64 mask = (1u << prefixBits) - 1u;
    quint64 value = quint64(first) & mask;
    if (value < mask) {
        *out = value;
        return true;
    }
    quint64 shift = 0;
    while (true) {
        if (*pos >= block.size())
            return false;
        const quint8 byte = quint8(block.at(*pos));
        ++(*pos);
        // A continuation longer than nine groups cannot fit a 64-bit value;
        // treat it as corruption rather than silently wrapping.
        if (shift > 56)
            return false;
        value += quint64(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            break;
        shift += 7;
    }
    *out = value;
    return true;
}

bool readString(const QByteArray &block, int *pos, QByteArray *out, QString *error)
{
    if (*pos >= block.size()) {
        *error = QStringLiteral("hpack: truncated string header");
        return false;
    }
    const quint8 first = quint8(block.at(*pos));
    ++(*pos);
    const bool huffman = (first & 0x80) != 0;
    quint64 length = 0;
    if (!readInteger(block, pos, first, 7, &length)) {
        *error = QStringLiteral("hpack: bad string length");
        return false;
    }
    if (length > quint64(block.size() - *pos)) {
        *error = QStringLiteral("hpack: string runs past end of block");
        return false;
    }
    const QByteArray raw = block.mid(*pos, int(length));
    *pos += int(length);
    if (!huffman) {
        *out = raw;
        return true;
    }
    bool ok = false;
    *out = huffmanDecode(raw, &ok);
    if (!ok) {
        *error = QStringLiteral("hpack: invalid huffman string");
        return false;
    }
    return true;
}

int staticIndexForName(const QByteArray &name)
{
    for (int i = 1; i <= kStaticCount; ++i) {
        if (name == kStaticTable[i].name)
            return i;
    }
    return 0;
}

} // namespace

QByteArray huffmanDecode(const QByteArray &input, bool *ok)
{
    *ok = true;
    QByteArray out;
    out.reserve(input.size() * 8 / 5 + 1);
    const QList<HuffNode> &nodes = huffTree().nodes();
    BitCursor cursor(input);
    int node = 0;
    int partialBits = 0;
    bool partialAllOnes = true;
    while (!cursor.atEnd()) {
        const int bit = cursor.nextBit();
        if (bit < 0)
            break;
        const int next = nodes.at(node).child[bit];
        if (next < 0) {
            *ok = false;
            return QByteArray();
        }
        node = next;
        ++partialBits;
        if (!bit)
            partialAllOnes = false;
        const int symbol = nodes.at(node).symbol;
        if (symbol >= 0) {
            if (symbol == 256) {
                // EOS must never appear inside a literal; RFC 7541 5.2 says a
                // decoder that sees it must treat the block as an error.
                *ok = false;
                return QByteArray();
            }
            out.append(char(quint8(symbol)));
            node = 0;
            partialBits = 0;
            partialAllOnes = true;
        }
    }
    // A trailing partial path is only legal as EOS padding: fewer than eight
    // bits, all ones.  Anything else is a truncated or corrupt string, not a
    // string whose last character we may quietly drop.
    if (node != 0 && (partialBits >= 8 || !partialAllOnes)) {
        *ok = false;
        return QByteArray();
    }
    return out;
}

void writeInteger(QByteArray &out, quint8 prefixMask, int prefixBits, quint64 value)
{
    const quint64 max = (1u << prefixBits) - 1u;
    if (value < max) {
        out.append(char(prefixMask | quint8(value)));
        return;
    }
    out.append(char(prefixMask | quint8(max)));
    quint64 rest = value - max;
    while (rest >= 0x80) {
        out.append(char(quint8((rest & 0x7f) | 0x80)));
        rest >>= 7;
    }
    out.append(char(quint8(rest)));
}

void Decoder::setMaxDynamicTableSize(quint32 size)
{
    m_limit = size;
    evictToFit();
}

void Decoder::evictToFit()
{
    while (m_size > m_limit && !m_dynamic.isEmpty()) {
        m_size -= entrySize(m_dynamic.last());
        m_dynamic.removeLast();
    }
}

void Decoder::insert(const Header &header)
{
    const quint32 size = entrySize(header);
    if (size > m_limit) {
        // RFC 7541 4.4: an entry larger than the whole table empties it and is
        // not inserted.  Not an error.
        m_dynamic.clear();
        m_size = 0;
        return;
    }
    m_dynamic.prepend(header);
    m_size += size;
    evictToFit();
}

bool Decoder::lookup(quint64 index, Header *out, QString *error) const
{
    if (index == 0) {
        *error = QStringLiteral("hpack: index 0 is not a valid table entry");
        return false;
    }
    if (index <= quint64(kStaticCount)) {
        out->name = QByteArray(kStaticTable[index].name);
        out->value = QByteArray(kStaticTable[index].value);
        return true;
    }
    const quint64 dynamicIndex = index - quint64(kStaticCount) - 1;
    if (dynamicIndex >= quint64(m_dynamic.size())) {
        *error = QStringLiteral("hpack: dynamic table index %1 out of range").arg(index);
        return false;
    }
    *out = m_dynamic.at(int(dynamicIndex));
    return true;
}

bool Decoder::decode(const QByteArray &block, QList<Header> *out, QString *error)
{
    int pos = 0;
    while (pos < block.size()) {
        const quint8 first = quint8(block.at(pos));
        ++pos;

        if (first & 0x80) { // 1xxxxxxx - indexed header field
            quint64 index = 0;
            if (!readInteger(block, &pos, first, 7, &index)) {
                *error = QStringLiteral("hpack: bad indexed field");
                return false;
            }
            Header header;
            if (!lookup(index, &header, error))
                return false;
            out->append(header);
            continue;
        }

        if ((first & 0xe0) == 0x20) { // 001xxxxx - dynamic table size update
            quint64 size = 0;
            if (!readInteger(block, &pos, first, 5, &size)) {
                *error = QStringLiteral("hpack: bad table size update");
                return false;
            }
            // The peer may only shrink below the value we advertised in
            // SETTINGS; a larger one means the streams have desynchronised.
            if (size > m_limit) {
                *error = QStringLiteral("hpack: table size update above negotiated maximum");
                return false;
            }
            m_limit = quint32(size);
            evictToFit();
            continue;
        }

        int prefixBits = 4;
        bool indexing = false;
        if ((first & 0xc0) == 0x40) { // 01xxxxxx - literal with incremental indexing
            prefixBits = 6;
            indexing = true;
        }
        // Otherwise 0000xxxx (without indexing) or 0001xxxx (never indexed);
        // both are 4-bit prefixes and neither touches the dynamic table.

        quint64 nameIndex = 0;
        if (!readInteger(block, &pos, first, prefixBits, &nameIndex)) {
            *error = QStringLiteral("hpack: bad literal name index");
            return false;
        }
        Header header;
        if (nameIndex != 0) {
            Header named;
            if (!lookup(nameIndex, &named, error))
                return false;
            header.name = named.name;
        } else if (!readString(block, &pos, &header.name, error)) {
            return false;
        }
        if (!readString(block, &pos, &header.value, error))
            return false;
        if (indexing)
            insert(header);
        out->append(header);
    }
    return true;
}

QByteArray Encoder::encode(const QList<Header> &headers)
{
    QByteArray out;
    for (const Header &header : headers) {
        // A bearer token must not be placed in any compression context that
        // could later be probed; that is exactly what "never indexed" is for.
        const bool sensitive = header.name == "authorization";
        const quint8 pattern = sensitive ? 0x10 : 0x00;
        const int index = staticIndexForName(header.name);
        if (index > 0) {
            writeInteger(out, pattern, 4, quint64(index));
        } else {
            writeInteger(out, pattern, 4, 0);
            writeInteger(out, 0x00, 7, quint64(header.name.size()));
            out.append(header.name);
        }
        // Literal (not Huffman) values: a client's headers are tiny and this
        // removes a whole class of encoder bugs from the send path.
        writeInteger(out, 0x00, 7, quint64(header.value.size()));
        out.append(header.value);
    }
    return out;
}

} // namespace hpack
