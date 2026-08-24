// HPACK (RFC 7541) header compression, enough of it to be a correct HTTP/2
// client.
//
// The decoder is full: static table, dynamic table with eviction, all four
// literal representations, dynamic-table-size updates, and Huffman string
// decoding.  None of that is optional - gRPC's C-core server Huffman-encodes
// header values and does use its dynamic table, so a decoder that skipped
// either would fail on the second response of a connection, not the first.
//
// The encoder deliberately stays minimal: every header goes out as a literal
// that is never added to a table.  That keeps our peer's decoder dynamic
// table permanently empty, so there is no encoder-side table state that could
// drift out of sync with it.  `authorization` uses the never-indexed form,
// which is what that representation exists for.
#ifndef HPACK_H
#define HPACK_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace hpack {

struct Header
{
    QByteArray name;
    QByteArray value;
};

class Decoder
{
public:
    Decoder() = default;

    // Applied when the peer's SETTINGS_HEADER_TABLE_SIZE changes.  A dynamic
    // table size update inside a header block can lower it further but never
    // above this.
    void setMaxDynamicTableSize(quint32 size);

    // Appends to *out.  Returns false and fills *error on a malformed block;
    // the connection must then be torn down, since HPACK state is stateful
    // across the whole connection and cannot be resynchronised.
    bool decode(const QByteArray &block, QList<Header> *out, QString *error);

private:
    bool lookup(quint64 index, Header *out, QString *error) const;
    void insert(const Header &header);
    void evictToFit();

    QList<Header> m_dynamic; // index 0 is the newest entry
    quint32 m_limit = 4096;
    quint32 m_size = 0;
};

class Encoder
{
public:
    static QByteArray encode(const QList<Header> &headers);
};

// Exposed for the unit-style self check in GrpcChannel; also handy when
// debugging a capture by hand.
QByteArray huffmanDecode(const QByteArray &input, bool *ok);
void writeInteger(QByteArray &out, quint8 prefixMask, int prefixBits, quint64 value);

} // namespace hpack

#endif // HPACK_H
