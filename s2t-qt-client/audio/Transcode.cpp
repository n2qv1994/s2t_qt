#include "Transcode.h"

#include "WavIo.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

namespace audio {
namespace {

QString ffmpegPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

} // namespace

bool ffmpegAvailable()
{
    return !ffmpegPath().isEmpty();
}

QString ensurePcmWav(const QString &source, QString *error)
{
    const QFileInfo info(source);
    if (!info.isFile()) {
        *error = QStringLiteral("không tìm thấy tệp: %1").arg(source);
        return QString();
    }

    // A .wav that is already 16-bit PCM needs nothing; one that is not (IEEE
    // float, 24-bit, ADPCM, ...) goes through ffmpeg the same as an .m4a.
    if (info.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
        QString probeError;
        const wav::Pcm probe = wav::readWav(source, &probeError);
        if (probe.isValid())
            return source;
    }

    const QString executable = ffmpegPath();
    if (executable.isEmpty()) {
        *error = QStringLiteral(
            "tệp không phải WAV PCM 16-bit và không tìm thấy ffmpeg trên máy này để giải mã. "
            "Cài ffmpeg (thêm vào PATH) hoặc chuyển tệp sang WAV PCM 16-bit trước.");
        return QString();
    }

    const QString target = QDir(QDir::tempPath())
                               .filePath(QStringLiteral("s2t_qt_%1.wav")
                                             .arg(QUuid::createUuid().toString(QUuid::Id128)));
    QProcess process;
    // -f wav is explicit because the target name is generated: ffmpeg's own
    // extension sniffing is not something to rely on for muxer choice.
    process.start(executable, {QStringLiteral("-y"), QStringLiteral("-i"), source,
                               QStringLiteral("-ar"), QStringLiteral("16000"),
                               QStringLiteral("-ac"), QStringLiteral("1"),
                               QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"),
                               QStringLiteral("-f"), QStringLiteral("wav"), target});
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("không chạy được ffmpeg: %1").arg(process.errorString());
        return QString();
    }
    if (!process.waitForFinished(120000)) {
        process.kill();
        *error = QStringLiteral("giải mã audio bị timeout");
        QFile::remove(target);
        return QString();
    }
    if (process.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        *error = QStringLiteral("giải mã audio thất bại: %1").arg(detail.right(400));
        QFile::remove(target);
        return QString();
    }

    QString verifyError;
    const wav::Pcm decoded = wav::readWav(target, &verifyError);
    if (!decoded.isValid()) {
        *error = QStringLiteral("ffmpeg không tạo ra WAV 16-bit hợp lệ: %1").arg(verifyError);
        QFile::remove(target);
        return QString();
    }
    return target;
}

} // namespace audio
