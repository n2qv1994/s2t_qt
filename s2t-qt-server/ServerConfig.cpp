#include "ServerConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace {

// Reads "--name value" out of args.  Returns false when the flag is present
// but has nothing after it, which is worth refusing rather than defaulting:
// `--upstream` with a missing argument almost always means the operator meant
// to type one and the shell ate it.
bool takeValue(const QStringList &args, const QString &flag, QString *value, bool *seen,
               QString *error)
{
    const int index = args.indexOf(flag);
    if (index < 0)
        return true;
    if (index + 1 >= args.size()) {
        *error = QStringLiteral("thiếu giá trị cho %1").arg(flag);
        return false;
    }
    *value = args.at(index + 1);
    *seen = true;
    return true;
}

bool takeInt(const QStringList &args, const QString &flag, int *value, QString *error)
{
    QString text;
    bool seen = false;
    if (!takeValue(args, flag, &text, &seen, error))
        return false;
    if (!seen)
        return true;
    bool ok = false;
    const int parsed = text.toInt(&ok);
    if (!ok) {
        *error = QStringLiteral("%1 cần một số nguyên, nhận được '%2'").arg(flag, text);
        return false;
    }
    *value = parsed;
    return true;
}

bool takeDouble(const QStringList &args, const QString &flag, double *value, QString *error)
{
    QString text;
    bool seen = false;
    if (!takeValue(args, flag, &text, &seen, error))
        return false;
    if (!seen)
        return true;
    bool ok = false;
    const double parsed = text.toDouble(&ok);
    if (!ok) {
        *error = QStringLiteral("%1 cần một số, nhận được '%2'").arg(flag, text);
        return false;
    }
    *value = parsed;
    return true;
}

} // namespace

QString ServerConfig::defaultPath()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("s2t-qt-server.conf"));
#else
    return QStringLiteral("/etc/s2t-qt-server.conf");
#endif
}

bool ServerConfig::load(const QStringList &args, QString *error)
{
    QString path = defaultPath();
    bool seen = false;
    if (!takeValue(args, QStringLiteral("--config"), &path, &seen, error))
        return false;

    if (QFileInfo::exists(path)) {
        QSettings ini(path, QSettings::IniFormat);
        listenAddress = ini.value(QStringLiteral("listen/address"), listenAddress).toString();
        listenPort = quint16(ini.value(QStringLiteral("listen/port"), listenPort).toUInt());
        listenToken = ini.value(QStringLiteral("listen/token"), listenToken).toString();
        maxConnections = ini.value(QStringLiteral("listen/max_connections"), maxConnections).toInt();
        idleTimeoutMs = ini.value(QStringLiteral("listen/idle_timeout_ms"), idleTimeoutMs).toInt();

        upstreamTarget = ini.value(QStringLiteral("upstream/target"), upstreamTarget).toString();
        upstreamToken = ini.value(QStringLiteral("upstream/token"), upstreamToken).toString();
        upstreamLanes = ini.value(QStringLiteral("upstream/lanes"), upstreamLanes).toInt();
        upstreamTimeoutMs =
            ini.value(QStringLiteral("upstream/timeout_ms"), upstreamTimeoutMs).toInt();
        upstreamProbeMs = ini.value(QStringLiteral("upstream/probe_ms"), upstreamProbeMs).toInt();

        bufferSeconds = ini.value(QStringLiteral("buffer/seconds"), bufferSeconds).toDouble();
        spoolDir = ini.value(QStringLiteral("buffer/spool_dir"), spoolDir).toString();
        statePollMs = ini.value(QStringLiteral("buffer/state_poll_ms"), statePollMs).toInt();
        finishedRetentionSec =
            ini.value(QStringLiteral("buffer/finished_retention_sec"), finishedRetentionSec).toInt();

        bool modeOk = false;
        const applog::Mode mode = applog::modeFromString(
            ini.value(QStringLiteral("log/mode"), applog::modeName(logMode)).toString(), &modeOk);
        if (modeOk)
            logMode = mode;
        bool levelOk = false;
        const applog::Level level = applog::levelFromString(
            ini.value(QStringLiteral("log/level"), applog::levelName(logLevel)).toString(),
            &levelOk);
        if (levelOk)
            logLevel = level;

        sourcePath = path;
    } else if (seen) {
        // An explicitly named file that is not there is a mistake worth
        // stopping for; the implicit default simply not existing is not.
        *error = QStringLiteral("không mở được tệp cấu hình '%1'").arg(path);
        return false;
    }

    // ---- command line, which always wins ----------------------------------
    QString text;
    bool got = false;
    if (!takeValue(args, QStringLiteral("--listen"), &text, &got, error))
        return false;
    if (got) {
        const int colon = text.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            *error = QStringLiteral("--listen cần dạng địa-chỉ:cổng, nhận được '%1'").arg(text);
            return false;
        }
        listenAddress = text.left(colon);
        bool ok = false;
        const uint port = text.mid(colon + 1).toUInt(&ok);
        if (!ok || port == 0 || port > 65535) {
            *error = QStringLiteral("cổng không hợp lệ trong '%1'").arg(text);
            return false;
        }
        listenPort = quint16(port);
    }

    got = false;
    if (!takeValue(args, QStringLiteral("--token"), &listenToken, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--upstream"), &upstreamTarget, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--upstream-token"), &upstreamToken, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--spool-dir"), &spoolDir, &got, error))
        return false;

    if (!takeInt(args, QStringLiteral("--max-connections"), &maxConnections, error))
        return false;
    if (!takeInt(args, QStringLiteral("--upstream-lanes"), &upstreamLanes, error))
        return false;
    if (!takeInt(args, QStringLiteral("--state-poll-ms"), &statePollMs, error))
        return false;
    if (!takeDouble(args, QStringLiteral("--buffer-seconds"), &bufferSeconds, error))
        return false;

    // ---- sanity, once, here rather than at every use ----------------------
    if (listenPort == 0) {
        *error = QStringLiteral("cổng lắng nghe không hợp lệ");
        return false;
    }
    if (upstreamTarget.trimmed().isEmpty()) {
        *error = QStringLiteral("chưa cấu hình địa chỉ tầng suy luận (upstream/target)");
        return false;
    }
    maxConnections = qBound(1, maxConnections, 4096);
    upstreamLanes = qBound(1, upstreamLanes, 64);
    upstreamTimeoutMs = qBound(1000, upstreamTimeoutMs, 600000);
    upstreamProbeMs = qBound(500, upstreamProbeMs, 300000);
    // Below a second of buffer the queue cannot hold even one 320 ms packet
    // plus its successor, and the "bounded on purpose" behaviour turns into
    // "drops everything".
    bufferSeconds = qBound(1.0, bufferSeconds, 24.0 * 3600.0);
    statePollMs = qBound(20, statePollMs, 60000);
    finishedRetentionSec = qBound(0, finishedRetentionSec, 24 * 3600);
    idleTimeoutMs = qBound(5000, idleTimeoutMs, 24 * 3600 * 1000);
    return true;
}

void ServerConfig::save(const QString &path, QString *error) const
{
    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(QStringLiteral("listen/address"), listenAddress);
    ini.setValue(QStringLiteral("listen/port"), listenPort);
    ini.setValue(QStringLiteral("listen/token"), listenToken);
    ini.setValue(QStringLiteral("listen/max_connections"), maxConnections);
    ini.setValue(QStringLiteral("listen/idle_timeout_ms"), idleTimeoutMs);
    ini.setValue(QStringLiteral("upstream/target"), upstreamTarget);
    ini.setValue(QStringLiteral("upstream/token"), upstreamToken);
    ini.setValue(QStringLiteral("upstream/lanes"), upstreamLanes);
    ini.setValue(QStringLiteral("upstream/timeout_ms"), upstreamTimeoutMs);
    ini.setValue(QStringLiteral("upstream/probe_ms"), upstreamProbeMs);
    ini.setValue(QStringLiteral("buffer/seconds"), bufferSeconds);
    ini.setValue(QStringLiteral("buffer/spool_dir"), spoolDir);
    ini.setValue(QStringLiteral("buffer/state_poll_ms"), statePollMs);
    ini.setValue(QStringLiteral("buffer/finished_retention_sec"), finishedRetentionSec);
    ini.setValue(QStringLiteral("log/mode"), applog::modeName(logMode));
    ini.setValue(QStringLiteral("log/level"), applog::levelName(logLevel));
    ini.sync();
    if (ini.status() != QSettings::NoError)
        *error = QStringLiteral("không ghi được '%1'").arg(path);
}

QStringList ServerConfig::describe() const
{
    const QString listenTokenState = listenToken.trimmed().isEmpty()
        ? QStringLiteral("chưa đặt (chấp nhận mọi client)")
        : QStringLiteral("đã đặt");
    const QString upstreamTokenState =
        upstreamToken.trimmed().isEmpty() ? QStringLiteral("chưa đặt") : QStringLiteral("đã đặt");
    QStringList lines;
    lines << QStringLiteral("tệp cấu hình     : %1")
                 .arg(sourcePath.isEmpty() ? QStringLiteral("(không có, dùng mặc định)")
                                           : sourcePath)
          << QStringLiteral("lắng nghe        : %1:%2").arg(listenAddress).arg(listenPort)
          << QStringLiteral("token client     : %1").arg(listenTokenState)
          << QStringLiteral("kết nối tối đa   : %1").arg(maxConnections)
          << QStringLiteral("tầng suy luận    : %1").arg(upstreamTarget)
          << QStringLiteral("token suy luận   : %1").arg(upstreamTokenState)
          << QStringLiteral("kênh dùng lại    : %1").arg(upstreamLanes)
          << QStringLiteral("đệm mỗi phiên    : %1 giây").arg(bufferSeconds, 0, 'f', 0)
          << QStringLiteral("thư mục spool    : %1")
                 .arg(spoolDir.isEmpty() ? QStringLiteral("(tắt)") : spoolDir)
          << QStringLiteral("nhịp đọc trạng thái: %1 ms").arg(statePollMs)
          << QStringLiteral("giữ phiên đã dừng : %1 giây").arg(finishedRetentionSec)
          << QStringLiteral("nhật ký          : %1 / %2")
                 .arg(applog::modeName(logMode), applog::levelName(logLevel));
    return lines;
}

qint64 ServerConfig::bufferBytesPerSession(int sampleRate, int channels) const
{
    const qint64 rate = qMax(1, sampleRate);
    const qint64 ch = qMax(1, channels);
    // 16-bit PCM is the only format on this path; the adapter rejects anything
    // else, so sizing for it is not an assumption but the contract.
    return qint64(bufferSeconds * double(rate * ch * 2));
}
