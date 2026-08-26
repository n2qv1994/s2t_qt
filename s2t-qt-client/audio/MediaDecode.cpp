#include "MediaDecode.h"

#include "core/Logger.h"

#include <QAudioDecoder>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

namespace audio {

wav::Pcm decodeMedia(const QString &path, QString *error, int sampleRate, int channels,
                     int timeoutMs)
{
    wav::Pcm out;
    if (error)
        error->clear();

    const QFileInfo info(path);
    if (!info.exists() || !info.isReadable()) {
        if (error)
            *error = QStringLiteral("không đọc được tệp '%1'").arg(path);
        return out;
    }

    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDecoder decoder;
    decoder.setAudioFormat(format);
    decoder.setSource(QUrl::fromLocalFile(info.absoluteFilePath()));

    QByteArray frames;
    QString failure;
    QEventLoop loop;

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &decoder, [&] {
        const QAudioBuffer buffer = decoder.read();
        if (buffer.isValid())
            frames.append(reinterpret_cast<const char *>(buffer.constData<char>()),
                          buffer.byteCount());
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    // Qt 6 spells this `error`, not `errorOccurred` - the latter is
    // QMediaPlayer.  The name is also taken by the getter, so the signal has to
    // be disambiguated by signature rather than taken by address.
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
                     &decoder, [&](QAudioDecoder::Error) {
                         failure = decoder.errorString();
                         loop.quit();
                     });

    // A decoder that neither finishes nor errors would hang the UI thread for
    // good; a long file legitimately takes a while, so the bound is generous
    // rather than tight.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &decoder, [&] {
        failure = QStringLiteral("giải mã quá %1 giây mà chưa xong").arg(timeoutMs / 1000);
        decoder.stop();
        loop.quit();
    });
    guard.start(timeoutMs);

    QElapsedTimer clock;
    clock.start();
    decoder.start();
    loop.exec();
    guard.stop();

    if (!failure.isEmpty()) {
        if (error)
            *error = failure;
        LOG_WARN(applog::cat::Audio) << "decoding" << info.fileName() << "failed:" << failure;
        return out;
    }
    if (frames.isEmpty()) {
        // The common shape of this is a video with no audio track, and saying
        // so beats "file is empty" - the picture plays perfectly well.
        if (error) {
            *error = QStringLiteral("tệp '%1' không có dữ liệu âm thanh nào để nhận dạng")
                         .arg(info.fileName());
        }
        return out;
    }

    out.frames = frames;
    out.sampleRate = sampleRate;
    out.channels = channels;
    LOG_INFO(applog::cat::Audio)
        << "decoded" << info.fileName() << "->" << out.durationSec() << "s at" << sampleRate
        << "Hz /" << channels << "ch in" << clock.elapsed() << "ms";
    return out;
}

bool hasVideoTrack(const QString &path)
{
    QMediaPlayer player;
    QEventLoop loop;
    bool settled = false;

    // Track metadata only becomes known once the media is loaded, which is
    // asynchronous even for a local file.
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &player,
                     [&](QMediaPlayer::MediaStatus status) {
                         if (status == QMediaPlayer::LoadedMedia
                             || status == QMediaPlayer::BufferedMedia
                             || status == QMediaPlayer::InvalidMedia) {
                             settled = true;
                             loop.quit();
                         }
                     });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &player, [&] {
        settled = true;
        loop.quit();
    });

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(5000);

    player.setSource(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
    if (!settled)
        loop.exec();
    guard.stop();

    return !player.videoTracks().isEmpty();
}

} // namespace audio
