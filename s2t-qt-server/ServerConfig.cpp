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

// Every flag this binary understands, and whether it consumes the token after
// it.  Kept next to the parser rather than only in usage(), because the point
// of it is to reject what is *not* here.
//
// Silently ignoring an unknown argument is the wrong default for a service
// whose flags include a durability mode: `--durabilty fsync` would start
// happily and leave an operator believing they had power-loss durability that
// they do not have.
struct KnownFlag
{
    const char *name;
    bool takesValue;
};

static const KnownFlag kFlags[] = {
    {"--config", true},          {"--listen", true},
    {"--token", true},           {"--upstream", true},
    {"--upstream-token", true},  {"--upstream-lanes", true},
    {"--backend", true},         {"--model", true},
    {"--enroll-url", true},      {"--enroll-timeout-ms", true},
    {"--database-dir", true},
    {"--language", true},
    {"--journal-dir", true},     {"--durability", true},
    {"--journal-keep", true},    {"--orphan-timeout-sec", true},
    {"--buffer-seconds", true},  {"--state-poll-ms", true},
    {"--max-connections", true}, {"--write-config", true},
    {"--probe", true},           {"--log-mode", true},
    {"--log-level", true},       {"--log-file", true},
    {"--help", false},           {"-h", false},
    {"--version", false},        {"--selftest", false},
    {"--selftest-codec", false}, {"--show-config", false},
};

static bool checkArguments(const QStringList &args, QString *error)
{
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (!arg.startsWith(QLatin1Char('-')))
            continue;
        const KnownFlag *found = nullptr;
        for (const KnownFlag &flag : kFlags) {
            if (arg == QLatin1String(flag.name)) {
                found = &flag;
                break;
            }
        }
        if (!found) {
            *error = QStringLiteral("tham số không hiểu: %1 (xem --help)").arg(arg);
            return false;
        }
        if (found->takesValue)
            ++i; // its value may itself look like a flag; skip it
    }
    return true;
}

bool ServerConfig::load(const QStringList &args, QString *error)
{
    if (!checkArguments(args, error))
        return false;

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
        backend = ini.value(QStringLiteral("upstream/backend"), backend).toString();
        enrollUrl = ini.value(QStringLiteral("enroll/url"), enrollUrl).toString();
        databaseDir = ini.value(QStringLiteral("database/dir"), databaseDir).toString();
        enrollTimeoutMs = ini.value(QStringLiteral("enroll/timeout_ms"), enrollTimeoutMs).toInt();
        model = ini.value(QStringLiteral("upstream/model"), model).toString();
        language = ini.value(QStringLiteral("upstream/language"), language).toString();
        upstreamLanes = ini.value(QStringLiteral("upstream/lanes"), upstreamLanes).toInt();
        upstreamTimeoutMs =
            ini.value(QStringLiteral("upstream/timeout_ms"), upstreamTimeoutMs).toInt();
        upstreamProbeMs = ini.value(QStringLiteral("upstream/probe_ms"), upstreamProbeMs).toInt();

        bufferSeconds = ini.value(QStringLiteral("buffer/seconds"), bufferSeconds).toDouble();
        journalDir = ini.value(QStringLiteral("buffer/journal_dir"), journalDir).toString();
        const QString durabilityText =
            ini.value(QStringLiteral("buffer/durability"), QStringLiteral("os"))
                .toString()
                .trimmed()
                .toLower();
        if (durabilityText == QLatin1String("fsync"))
            durability = jrn::Durability::Fsync;
        else if (durabilityText == QLatin1String("os"))
            durability = jrn::Durability::Os;
        else if (!durabilityText.isEmpty()) {
            *error = QStringLiteral("buffer/durability phải là os hoặc fsync, nhận được '%1'")
                         .arg(durabilityText);
            return false;
        }
        const QString keepText = ini.value(QStringLiteral("buffer/journal_keep"),
                                           QStringLiteral("queue"))
                                     .toString()
                                     .trimmed()
                                     .toLower();
        if (keepText == QLatin1String("session"))
            journalKeep = jrn::Keep::Session;
        else if (keepText == QLatin1String("queue"))
            journalKeep = jrn::Keep::Queue;
        else if (!keepText.isEmpty()) {
            *error = QStringLiteral("buffer/journal_keep phải là queue hoặc session, nhận được '%1'")
                         .arg(keepText);
            return false;
        }
        segmentBytes = ini.value(QStringLiteral("buffer/segment_bytes"),
                                 qlonglong(segmentBytes))
                           .toLongLong();
        orphanTimeoutSec =
            ini.value(QStringLiteral("buffer/orphan_timeout_sec"), orphanTimeoutSec).toInt();
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
    if (!takeValue(args, QStringLiteral("--enroll-url"), &enrollUrl, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--database-dir"), &databaseDir, &got, error))
        return false;
    if (!takeInt(args, QStringLiteral("--enroll-timeout-ms"), &enrollTimeoutMs, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--backend"), &backend, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--model"), &model, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--language"), &language, &got, error))
        return false;
    got = false;
    if (!takeValue(args, QStringLiteral("--journal-dir"), &journalDir, &got, error))
        return false;

    QString durabilityArg;
    got = false;
    if (!takeValue(args, QStringLiteral("--durability"), &durabilityArg, &got, error))
        return false;
    if (got) {
        const QString value = durabilityArg.trimmed().toLower();
        if (value == QLatin1String("fsync"))
            durability = jrn::Durability::Fsync;
        else if (value == QLatin1String("os"))
            durability = jrn::Durability::Os;
        else {
            *error = QStringLiteral("--durability phải là os hoặc fsync, nhận được '%1'")
                         .arg(durabilityArg);
            return false;
        }
    }

    QString keepArg;
    got = false;
    if (!takeValue(args, QStringLiteral("--journal-keep"), &keepArg, &got, error))
        return false;
    if (got) {
        const QString value = keepArg.trimmed().toLower();
        if (value == QLatin1String("session"))
            journalKeep = jrn::Keep::Session;
        else if (value == QLatin1String("queue"))
            journalKeep = jrn::Keep::Queue;
        else {
            *error = QStringLiteral("--journal-keep phải là queue hoặc session, nhận được '%1'")
                         .arg(keepArg);
            return false;
        }
    }

    if (!takeInt(args, QStringLiteral("--orphan-timeout-sec"), &orphanTimeoutSec, error))
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
    // Same rule as the unknown-flag table: a typo here would otherwise start
    // happily and look like an unreachable inference tier for the rest of the
    // meeting, which is a much more expensive way to find out.
    backend = backend.trimmed().toLower();
    if (backend != QStringLiteral("triton") && backend != QStringLiteral("riva")) {
        *error = QStringLiteral("backend '%1' không hợp lệ (upstream/backend phải là 'triton' "
                                "hoặc 'riva')")
                     .arg(backend);
        return false;
    }
    if (model.trimmed().isEmpty() && backend == QStringLiteral("triton"))
        model = QStringLiteral("asr_diar_session");
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
    // Below a quarter megabyte a segment barely holds a dozen packets and the
    // roll cost swamps the writes; above a gigabyte one segment outlives the
    // whole queue and nothing is ever retired.
    segmentBytes = qBound(qint64(256 * 1024), segmentBytes, qint64(1024) * 1024 * 1024);
    orphanTimeoutSec = qBound(0, orphanTimeoutSec, 24 * 3600);
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
    ini.setValue(QStringLiteral("upstream/backend"), backend);
    ini.setValue(QStringLiteral("enroll/url"), enrollUrl);
    ini.setValue(QStringLiteral("database/dir"), databaseDir);
    ini.setValue(QStringLiteral("enroll/timeout_ms"), enrollTimeoutMs);
    ini.setValue(QStringLiteral("upstream/model"), model);
    ini.setValue(QStringLiteral("upstream/language"), language);
    ini.setValue(QStringLiteral("upstream/lanes"), upstreamLanes);
    ini.setValue(QStringLiteral("upstream/timeout_ms"), upstreamTimeoutMs);
    ini.setValue(QStringLiteral("upstream/probe_ms"), upstreamProbeMs);
    ini.setValue(QStringLiteral("buffer/seconds"), bufferSeconds);
    ini.setValue(QStringLiteral("buffer/journal_dir"), journalDir);
    ini.setValue(QStringLiteral("buffer/durability"),
                 durability == jrn::Durability::Fsync ? QStringLiteral("fsync")
                                                      : QStringLiteral("os"));
    ini.setValue(QStringLiteral("buffer/journal_keep"),
                 journalKeep == jrn::Keep::Session ? QStringLiteral("session")
                                                   : QStringLiteral("queue"));
    ini.setValue(QStringLiteral("buffer/segment_bytes"), qlonglong(segmentBytes));
    ini.setValue(QStringLiteral("buffer/orphan_timeout_sec"), orphanTimeoutSec);
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
          << QStringLiteral("tầng suy luận    : %1 (%2)").arg(upstreamTarget, backend)
          << QStringLiteral("kho phiên        : %1").arg(databaseDir.isEmpty() ? QStringLiteral("(tắt)") : databaseDir)
          << QStringLiteral("đăng ký giọng    : %1").arg(enrollUrl.isEmpty() ? QStringLiteral("(tắt)") : enrollUrl)
          << QStringLiteral("mô hình          : %1").arg(model.isEmpty() ? QStringLiteral("(mặc định)") : model)
          << QStringLiteral("token suy luận   : %1").arg(upstreamTokenState)
          << QStringLiteral("kênh dùng lại    : %1").arg(upstreamLanes)
          << QStringLiteral("đệm mỗi phiên    : %1 giây").arg(bufferSeconds, 0, 'f', 0)
          << QStringLiteral("nhật ký phiên    : %1")
                 .arg(journalDir.isEmpty()
                          ? QStringLiteral("(tắt - phiên KHÔNG sống qua lần khởi động lại)")
                          : QStringLiteral("%1  [%2, giữ %3, phân đoạn %4 MB]")
                                .arg(journalDir,
                                     durability == jrn::Durability::Fsync
                                         ? QStringLiteral("fsync")
                                         : QStringLiteral("os"),
                                     journalKeep == jrn::Keep::Session
                                         ? QStringLiteral("cả phiên")
                                         : QStringLiteral("hàng đợi"))
                                .arg(segmentBytes / (1024 * 1024)))
          << QStringLiteral("bỏ phiên mồ côi  : %1")
                 .arg(orphanTimeoutSec > 0 ? QStringLiteral("%1 giây").arg(orphanTimeoutSec)
                                           : QStringLiteral("(tắt)"))
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
