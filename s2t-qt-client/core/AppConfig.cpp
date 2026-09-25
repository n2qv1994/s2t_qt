#include "AppConfig.h"

#include <QFile>
#include <QSettings>

namespace {
QSettings settings()
{
    return QSettings(QStringLiteral("s2t"), QStringLiteral("s2t_qt"));
}
} // namespace

void AppConfig::load()
{
    QSettings store = settings();
    serverTarget = store.value(QStringLiteral("server/target"), serverTarget).toString();
    apiToken = store.value(QStringLiteral("server/token"), apiToken).toString();
    inputDeviceId = store.value(QStringLiteral("mic/deviceId"), inputDeviceId).toByteArray();
    expectedDeviceName =
        store.value(QStringLiteral("mic/expectedName"), expectedDeviceName).toString();
    sampleRate = store.value(QStringLiteral("mic/sampleRate"), sampleRate).toInt();
    channels = store.value(QStringLiteral("mic/channels"), channels).toInt();
    bufferSec = store.value(QStringLiteral("mic/bufferSec"), bufferSec).toDouble();
    micControlApp = store.value(QStringLiteral("mic/controlApp"), micControlApp).toString();
    pipelineTrace = store.value(QStringLiteral("session/pipelineTrace"), pipelineTrace).toBool();
    paceFileReplay = store.value(QStringLiteral("session/paceFileReplay"), paceFileReplay).toBool();
    // Default to whatever the logger is already doing, not to a fixed value:
    // with nothing stored, applying this back to the logger must be a no-op
    // rather than an undo of the build default or a --log-mode on the command
    // line.
    logMode = applog::modeFromString(
        store.value(QStringLiteral("log/mode"), applog::modeName(applog::mode())).toString());
    logLevel = applog::levelFromString(
        store.value(QStringLiteral("log/level"), applog::levelName(applog::level())).toString());
    // operatorId is deliberately NOT restored.  This name goes into the audit
    // trail as the person answerable for an edit or an enrolment; a field that
    // refills itself with whoever used this machine last would file one
    // person's work under another's name.  Typed once per run, on purpose.
    operatorId.clear();

    LOG_INFO(applog::cat::Config)
        << "config loaded: server=" << serverTarget << "token="
        << (apiToken.isEmpty() ? QStringLiteral("(empty)")
                               : QStringLiteral("(%1 chars)").arg(apiToken.size()))
        << "mic=" << (expectedDeviceName.isEmpty() ? QStringLiteral("(any)") : expectedDeviceName)
        << QStringLiteral("id=%1").arg(inputDeviceId.isEmpty()
                                           ? QStringLiteral("(system default)")
                                           : QString::fromUtf8(inputDeviceId))
        << QStringLiteral("%1Hz/%2ch").arg(sampleRate).arg(channels)
        << "buffer=" << bufferSec << "s"
        << "pipelineTrace=" << pipelineTrace << "paceFileReplay=" << paceFileReplay
        << "log=" << applog::modeName(logMode) << "/" << applog::levelName(logLevel);
}

void AppConfig::save() const
{
    QSettings store = settings();
    store.setValue(QStringLiteral("server/target"), serverTarget);
    store.setValue(QStringLiteral("server/token"), apiToken);
    store.setValue(QStringLiteral("mic/deviceId"), inputDeviceId);
    store.setValue(QStringLiteral("mic/expectedName"), expectedDeviceName);
    store.setValue(QStringLiteral("mic/sampleRate"), sampleRate);
    store.setValue(QStringLiteral("mic/channels"), channels);
    store.setValue(QStringLiteral("mic/bufferSec"), bufferSec);
    store.setValue(QStringLiteral("mic/controlApp"), micControlApp);
    store.setValue(QStringLiteral("session/pipelineTrace"), pipelineTrace);
    store.setValue(QStringLiteral("session/paceFileReplay"), paceFileReplay);
    store.setValue(QStringLiteral("log/mode"), applog::modeName(logMode));
    store.setValue(QStringLiteral("log/level"), applog::levelName(logLevel));
    LOG_INFO(applog::cat::Config)
        << "config saved: server=" << serverTarget
        << QStringLiteral("%1Hz/%2ch").arg(sampleRate).arg(channels)
        << "log=" << applog::modeName(logMode) << "/" << applog::levelName(logLevel);
}

QString AppConfig::tokenFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("không đọc được tệp token %1: %2").arg(path, file.errorString());
        LOG_WARN(applog::cat::Config)
            << "could not read the token file:" << path << "-" << file.errorString();
        return QString();
    }
    LOG_INFO(applog::cat::Config) << "token read from file" << path;
    return QString::fromUtf8(file.readAll()).trimmed();
}

QList<QPair<QString, QString>> AppConfig::describe() const
{
    const auto orUnset = [](const QString &value, const QString &unset) {
        return value.isEmpty() ? unset : value;
    };
    const auto yesNo = [](bool value) {
        return value ? QStringLiteral("bật") : QStringLiteral("tắt");
    };
    return {
        {QStringLiteral("Máy chủ"), orUnset(serverTarget, QStringLiteral("(CHƯA ĐẶT)"))},
        {QStringLiteral("Token"),
         apiToken.isEmpty() ? QStringLiteral("CHƯA ĐẶT - mọi lệnh sẽ bị từ chối")
                            : QStringLiteral("đã đặt")},
        {QStringLiteral("Micro (id đã lưu)"),
         inputDeviceId.isEmpty() ? QStringLiteral("(mặc định hệ thống)")
                                 : QString::fromUtf8(inputDeviceId)},
        {QStringLiteral("Tên thiết bị bắt buộc chứa"),
         orUnset(expectedDeviceName, QStringLiteral("(bất kỳ)"))},
        {QStringLiteral("Định dạng thu"),
         QStringLiteral("%1 Hz / %2 kênh").arg(sampleRate).arg(channels)},
        {QStringLiteral("Bộ đệm tại máy"), QStringLiteral("%1 s").arg(bufferSec)},
        {QStringLiteral("Ứng dụng điều khiển mic"),
         orUnset(micControlApp, QStringLiteral("(không)"))},
        {QStringLiteral("Pipeline trace"), yesNo(pipelineTrace)},
        {QStringLiteral("Phát tệp theo thời gian thực"), yesNo(paceFileReplay)},
        {QStringLiteral("Log (đã lưu)"),
         QStringLiteral("%1 / %2").arg(applog::modeName(logMode), applog::levelName(logLevel))},
    };
}
