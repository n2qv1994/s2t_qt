#include "MicDenoise.h"

#include "WavIo.h"

#include <QFileInfo>
#include <QProcess>
#include <QTimer>

namespace mic {

QString denoiseStateKey(DenoiseState state)
{
    switch (state) {
    case DenoiseState::Enabled: return QStringLiteral("enabled");
    case DenoiseState::Disabled: return QStringLiteral("disabled");
    case DenoiseState::Unknown: break;
    }
    return QStringLiteral("unknown");
}

bool setDenoise(const QString &appPath, bool enabled, QString *output, QString *error)
{
    const QFileInfo info(appPath.trimmed());
    if (appPath.trimmed().isEmpty() || !info.isFile()) {
        *error = QStringLiteral("không tìm thấy xvf3800 host-control trên máy này");
        return false;
    }
    // On Linux the tool is copied off the vendor image and routinely arrives
    // without the execute bit.  QProcess would report only a generic start
    // failure, so name the real cause while the operator can still fix it.
    if (!info.isExecutable()) {
        *error = QStringLiteral("tệp host-control không có quyền thực thi: chmod +x %1")
                     .arg(info.absoluteFilePath());
        return false;
    }
    // The two supported modes, nothing else.  Never persisted to the device.
    const QString ns = enabled ? QStringLiteral("0.15") : QStringLiteral("1.0");
    const QString nn = enabled ? QStringLiteral("0.51") : QStringLiteral("1.0");
    QStringList collected;
    const QList<QPair<QString, QString>> commands = {
        {QStringLiteral("PP_MIN_NS"), ns},
        {QStringLiteral("PP_MIN_NN"), nn},
    };
    for (const auto &command : commands) {
        QProcess process;
        process.start(appPath, {command.first, command.second});
        if (!process.waitForStarted(3000)) {
            // isFile() above is a TOCTOU check, not a guarantee: the drive can
            // be unplugged or the binary quarantined between the two.
            *error = QStringLiteral("không chạy được lệnh điều khiển (%1): %2")
                         .arg(command.first, process.errorString());
            return false;
        }
        if (!process.waitForFinished(8000)) {
            process.kill();
            *error = QStringLiteral("lệnh điều khiển microphone bị timeout");
            return false;
        }
        const QString text = QString::fromLocal8Bit(process.readAllStandardOutput()
                                                    + "\n"
                                                    + process.readAllStandardError())
                                 .trimmed();
        if (process.exitCode() != 0) {
            *error = QStringLiteral("%1 lỗi: %2").arg(command.first, text.right(400));
            return false;
        }
        collected << text.right(300);
    }
    if (output)
        *output = collected.join(QLatin1Char('\n'));
    return true;
}

bool probeDevicePresent(const QString &appPath, QString *error)
{
    const QFileInfo info(appPath.trimmed());
    if (appPath.trimmed().isEmpty() || !info.isFile() || !info.isExecutable())
        return true; // not configured or not runnable: nothing to probe here
    QProcess process;
    process.start(appPath, {QStringLiteral("PP_MIN_NS")});
    if (!process.waitForStarted(2000) || !process.waitForFinished(3000)) {
        process.kill();
        *error = QStringLiteral("không liên lạc được với reSpeaker USB");
        return false;
    }
    const QString text = QString::fromLocal8Bit(process.readAllStandardOutput()
                                                + "\n" + process.readAllStandardError());
    if (process.exitCode() != 0 || !text.contains(QStringLiteral("Found device"))) {
        *error = QStringLiteral("reSpeaker USB không còn phản hồi");
        return false;
    }
    return true;
}

DenoiseAbRecorder::DenoiseAbRecorder(QObject *parent) : QObject(parent) {}

void DenoiseAbRecorder::start(const AudioDeviceChoice &choice, const QString &appPath,
                              double seconds, int priorState)
{
    if (m_busy) {
        DenoiseAbResult busy;
        busy.error = QStringLiteral("đang ghi mẫu đối chứng khác - thử lại sau vài giây");
        emit completed(busy);
        return;
    }
    m_busy = true;
    m_choice = choice;
    m_appPath = appPath;
    m_seconds = qBound(0.5, seconds, 10.0);
    m_prior = DenoiseState(priorState);
    m_result = DenoiseAbResult();
    m_result.priorState = denoiseStateKey(m_prior);
    m_capturingOn = false;
    beginClip(false);
}

void DenoiseAbRecorder::beginClip(bool denoiseEnabled)
{
    QString output;
    QString error;
    if (!setDenoise(m_appPath, denoiseEnabled, &output, &error)) {
        fail(error);
        return;
    }
    m_capturingOn = denoiseEnabled;
    m_buffer.clear();

    if (!m_capture) {
        m_capture = new AudioCapture(this);
        connect(m_capture, &AudioCapture::chunk, this,
                [this](const QByteArray &pcm) { m_buffer.append(pcm); });
        connect(m_capture, &AudioCapture::failed, this,
                [this](const QString &message) { fail(message); });
    }
    // Let the DSP settle after the control command lands before capture
    // starts, or the first clip contains the tail of the previous mode.
    QTimer::singleShot(400, this, [this]() {
        if (!m_busy)
            return;
        m_capture->start(m_choice);
        QTimer::singleShot(int(m_seconds * 1000.0), this, [this]() {
            if (m_busy)
                finishClip();
        });
    });
}

void DenoiseAbRecorder::finishClip()
{
    m_capture->stop();
    const QByteArray wavBytes =
        wav::buildWav(m_buffer, m_choice.sampleRate, m_choice.channels);
    if (m_capturingOn)
        m_result.onWav = wavBytes;
    else
        m_result.offWav = wavBytes;

    if (!m_capturingOn) {
        beginClip(true);
        return;
    }
    restoreAndEmit();
}

void DenoiseAbRecorder::fail(const QString &message)
{
    if (!m_busy)
        return;
    if (m_capture)
        m_capture->stop();
    m_result.error = message;
    restoreAndEmit();
}

void DenoiseAbRecorder::restoreAndEmit()
{
    // Restore only a state this process actually knows was active before the
    // capture.  The host-control tool has no documented way to read the
    // current PP_MIN_NS/PP_MIN_NN back, so on a freshly started client the
    // prior state is genuinely unknown and guessing "enabled" would report a
    // restore that may well be wrong.
    m_result.restoreAttempted = (m_prior != DenoiseState::Unknown);
    if (m_result.restoreAttempted) {
        QString output;
        QString error;
        if (setDenoise(m_appPath, m_prior == DenoiseState::Enabled, &output, &error)) {
            m_result.restored = true;
            m_result.finalState = denoiseStateKey(m_prior);
        } else {
            m_result.restoreError = error;
            m_result.finalState = denoiseStateKey(DenoiseState::Enabled);
        }
    } else {
        m_result.finalState = denoiseStateKey(DenoiseState::Enabled);
    }
    m_busy = false;
    emit completed(m_result);
}

} // namespace mic
