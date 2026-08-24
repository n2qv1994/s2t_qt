#include "WavIo.h"

#include <QFile>
#include <QtGlobal>

#include <cmath>

namespace wav {
namespace {

quint32 le32(const QByteArray &buf, int offset)
{
    return quint32(quint8(buf.at(offset))) | (quint32(quint8(buf.at(offset + 1))) << 8)
        | (quint32(quint8(buf.at(offset + 2))) << 16) | (quint32(quint8(buf.at(offset + 3))) << 24);
}

quint16 le16(const QByteArray &buf, int offset)
{
    return quint16(quint16(quint8(buf.at(offset))) | (quint16(quint8(buf.at(offset + 1))) << 8));
}

void appendLe32(QByteArray &out, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((value >> (8 * i)) & 0xff));
}

void appendLe16(QByteArray &out, quint16 value)
{
    for (int i = 0; i < 2; ++i)
        out.append(char((value >> (8 * i)) & 0xff));
}

} // namespace

Pcm parseWav(const QByteArray &bytes, QString *error)
{
    Pcm out;
    if (bytes.size() < 44 || bytes.left(4) != "RIFF" || bytes.mid(8, 4) != "WAVE") {
        *error = QStringLiteral("không phải tệp WAV hợp lệ");
        return out;
    }
    int offset = 12;
    int bitsPerSample = 0;
    int format = 0;
    QByteArray data;
    bool sawFmt = false;
    while (offset + 8 <= bytes.size()) {
        const QByteArray chunkId = bytes.mid(offset, 4);
        const quint32 chunkSize = le32(bytes, offset + 4);
        const int body = offset + 8;
        if (chunkId == "fmt " && body + 16 <= bytes.size()) {
            format = le16(bytes, body);
            out.channels = le16(bytes, body + 2);
            out.sampleRate = int(le32(bytes, body + 4));
            bitsPerSample = le16(bytes, body + 14);
            sawFmt = true;
        } else if (chunkId == "data") {
            // A streamed WAV can carry a placeholder size of 0 or 0xffffffff;
            // trust what is actually in the file over the declared length.
            const int available = bytes.size() - body;
            const int length = (chunkSize == 0 || chunkSize > quint32(available))
                ? available
                : int(chunkSize);
            data = bytes.mid(body, length);
        }
        // Chunks are word aligned: an odd size carries one pad byte.
        offset = body + int(chunkSize) + (chunkSize & 1u ? 1 : 0);
        if (chunkSize == 0 && chunkId != "data")
            break;
    }
    if (!sawFmt) {
        *error = QStringLiteral("tệp WAV thiếu khối 'fmt '");
        out = Pcm();
        return out;
    }
    if (format != 1 || bitsPerSample != 16) {
        *error = QStringLiteral("chỉ hỗ trợ WAV PCM 16-bit (đang là %1-bit, format %2)")
                     .arg(bitsPerSample).arg(format);
        return Pcm();
    }
    if (out.channels < 1 || out.channels > 2 || out.sampleRate <= 0) {
        *error = QStringLiteral("WAV phải là mono hoặc stereo với sample rate hợp lệ");
        return Pcm();
    }
    // Truncate a half frame rather than letting it shift every later sample.
    const int frameBytes = out.channels * 2;
    out.frames = data.left(data.size() - (data.size() % frameBytes));
    return out;
}

Pcm readWav(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("không mở được %1: %2").arg(path, file.errorString());
        return Pcm();
    }
    return parseWav(file.readAll(), error);
}

QByteArray buildWav(const QByteArray &frames, int sampleRate, int channels)
{
    const int safeChannels = qMax(1, channels);
    const int safeRate = sampleRate > 0 ? sampleRate : 16000;
    const quint32 dataSize = quint32(frames.size());
    QByteArray out;
    out.reserve(frames.size() + 44);
    out.append("RIFF");
    appendLe32(out, 36 + dataSize);
    out.append("WAVE");
    out.append("fmt ");
    appendLe32(out, 16);
    appendLe16(out, 1); // PCM
    appendLe16(out, quint16(safeChannels));
    appendLe32(out, quint32(safeRate));
    appendLe32(out, quint32(safeRate * safeChannels * 2));
    appendLe16(out, quint16(safeChannels * 2));
    appendLe16(out, 16);
    out.append("data");
    appendLe32(out, dataSize);
    out.append(frames);
    return out;
}

bool writeWav(const QString &path, const QByteArray &frames, int sampleRate, int channels,
              QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("không ghi được %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray payload = buildWav(frames, sampleRate, channels);
    if (file.write(payload) != payload.size()) {
        *error = QStringLiteral("ghi %1 không đầy đủ").arg(path);
        return false;
    }
    file.close();
    return true;
}

QByteArray toMono16k(const QByteArray &frames, int sampleRate, int channels)
{
    if (sampleRate <= 0 || channels <= 0)
        return QByteArray();

    const int frameBytes = channels * 2;
    const int frameCount = frames.size() / frameBytes;
    const auto *samples = reinterpret_cast<const qint16 *>(frames.constData());

    QByteArray mono;
    mono.resize(frameCount * 2);
    auto *monoOut = reinterpret_cast<qint16 *>(mono.data());
    for (int i = 0; i < frameCount; ++i) {
        qint32 sum = 0;
        for (int c = 0; c < channels; ++c)
            sum += samples[i * channels + c];
        monoOut[i] = qint16(sum / channels);
    }
    if (sampleRate == 16000)
        return mono;

    // Linear interpolation.  This audio is speech that a VAD and an embedding
    // model consume, not something anyone listens to for fidelity, and a
    // polyphase resampler here would be a dependency for no audible gain.
    const double ratio = 16000.0 / double(sampleRate);
    const int outCount = int(std::floor(frameCount * ratio));
    QByteArray out;
    out.resize(qMax(0, outCount) * 2);
    auto *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < outCount; ++i) {
        const double src = double(i) / ratio;
        const int i0 = int(src);
        const int i1 = qMin(i0 + 1, frameCount - 1);
        const double frac = src - double(i0);
        const double value = double(monoOut[i0]) * (1.0 - frac) + double(monoOut[i1]) * frac;
        dst[i] = qint16(qBound(-32768.0, value, 32767.0));
    }
    return out;
}

} // namespace wav
