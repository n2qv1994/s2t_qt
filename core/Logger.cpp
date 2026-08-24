#include "Logger.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace applog {
namespace {

// qmake CONFIG+=develop flips the built-in default; without it a plain build
// logs to the console, which is what someone attaching a debugger wants.
#ifdef S2T_LOG_DEFAULT_DEVELOP
const Mode kDefaultMode = Mode::Develop;
#else
const Mode kDefaultMode = Mode::Debug;
#endif

const Level kDefaultLevel = Level::Debug;

// One file is rolled aside at this size and two generations are kept, so a
// machine left recording overnight cannot fill the disk while still holding
// enough history to cover the session that went wrong.
const qint64 kMaxFileBytes = 8 * 1024 * 1024;
const int kKeptGenerations = 2;

// What the in-app viewer can show of the past.  At trace level a live session
// produces a few lines a second, so this is roughly the last ten minutes -
// enough to open the window after something went wrong and still see it.
const int kRecentMax = 4000;
// Trimmed a block at a time rather than one entry per line: at trace level the
// audio path logs several times a second, and dropping a single entry would
// memmove the whole buffer on every one of them while the sink lock is held.
const int kRecentTrimBlock = 1000;

struct State
{
    QMutex mutex;
    Mode mode = kDefaultMode;
    // Read without the mutex on every single log call, so it is atomic rather
    // than plain: the hot path must not take a lock just to decide to do
    // nothing.
    QAtomicInt level{int(kDefaultLevel)};
    // Set when --log-mode or S2T_LOG_MODE decided the mode; a stored
    // preference must not quietly override an explicit run-time choice.
    bool modeForced = false;
    bool levelForced = false;
    bool consoleReady = false;
    QString filePath;
    QFile file;
    qint64 fileBytes = 0;
    QVector<Entry> recent;
    // Created on the GUI thread by initFromArguments, before any worker
    // thread exists, so publishing to it from one is a plain queued emit.
    Bus *bus = nullptr;
};

State &state()
{
    static State instance;
    return instance;
}

const char *levelTag(Level value)
{
    switch (value) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Off: return "OFF  ";
    }
    return "?????";
}

// Qt names the worker threads (audio-capture, session-worker, state-poller,
// rpc-lane-N); the GUI thread has no name, and "main" is more useful in a log
// than a raw pointer.
QString threadTag()
{
    QThread *current = QThread::currentThread();
    if (!current)
        return QStringLiteral("?");
    const QString name = current->objectName();
    // Qt calls the GUI thread "Qt mainThread"; "main" is shorter and is what
    // the rest of these logs call it.
    if (name.isEmpty() || name == QLatin1String("Qt mainThread"))
        return QStringLiteral("main");
    return name;
}

QString baseName(const char *path)
{
    if (!path)
        return QString();
    const char *cut = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            cut = p + 1;
    }
    return QString::fromLatin1(cut);
}

QString defaultLogDirectory()
{
    // Depends on the organization/application name, which main() sets before
    // this is ever reached.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.s2t_qt");
    return base + QStringLiteral("/logs");
}

void writeConsole(const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
#ifdef Q_OS_WIN
    // Launched from Explorer there is no console to borrow at all; the
    // debugger's output pane is then the only place a Debug-mode line can go.
    if (!state().consoleReady) {
        OutputDebugStringA(bytes.constData());
        OutputDebugStringA("\n");
    }
#endif
}

// Caller holds the mutex.
void rotateLocked()
{
    State &s = state();
    s.file.close();
    // Drop the oldest, then shift each generation down one.
    QFile::remove(s.filePath + QStringLiteral(".%1").arg(kKeptGenerations));
    for (int i = kKeptGenerations - 1; i >= 1; --i) {
        QFile::rename(s.filePath + QStringLiteral(".%1").arg(i),
                      s.filePath + QStringLiteral(".%1").arg(i + 1));
    }
    QFile::rename(s.filePath, s.filePath + QStringLiteral(".1"));
    s.fileBytes = 0;
}

// Caller holds the mutex.  Complains once and then stays quiet, so an
// unwritable directory cannot turn every log call into a console warning.
bool openFileLocked()
{
    State &s = state();
    if (s.file.isOpen())
        return true;
    if (s.filePath.isEmpty()) {
        const QString dir = defaultLogDirectory();
        QDir().mkpath(dir);
        s.filePath = dir + QStringLiteral("/s2t_qt.log");
    }
    QDir().mkpath(QFileInfo(s.filePath).absolutePath());
    s.file.setFileName(s.filePath);
    if (!s.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        static bool complained = false;
        if (!complained) {
            complained = true;
            writeConsole(
                QStringLiteral("[log] cannot open the log file %1 (%2); falling back to console")
                    .arg(s.filePath, s.file.errorString()));
        }
        s.mode = Mode::Debug;
        return false;
    }
    s.fileBytes = s.file.size();
    return true;
}

void emitLine(Level level, const char *category, const QString &line)
{
    Entry entry;
    entry.level = level;
    entry.category = QString::fromLatin1(category ? category : "");
    entry.line = line;

    State &s = state();
    Bus *bus = nullptr;
    {
        QMutexLocker lock(&s.mutex);
        bus = s.bus;
        s.recent.append(entry);
        if (s.recent.size() > kRecentMax + kRecentTrimBlock)
            s.recent.remove(0, kRecentTrimBlock);

        if (s.mode == Mode::Debug || !openFileLocked()) {
            writeConsole(line);
        } else {
            const QByteArray bytes = line.toUtf8() + '\n';
            s.file.write(bytes);
            // Flushed per line: the reason to keep a file at all is to still
            // have the last few lines after a crash or a pulled power cable.
            s.file.flush();
            s.fileBytes += bytes.size();
            if (s.fileBytes >= kMaxFileBytes) {
                rotateLocked();
                openFileLocked();
            }
        }
    }
    // Published outside the lock on purpose: the viewer's slot is free to log,
    // and a queued emit should never happen while the sink is held.
    if (bus)
        bus->publish(entry);
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Level lvl = Level::Debug;
    switch (type) {
    case QtDebugMsg: lvl = Level::Debug; break;
    case QtInfoMsg: lvl = Level::Info; break;
    case QtWarningMsg: lvl = Level::Warn; break;
    case QtCriticalMsg: lvl = Level::Error; break;
    case QtFatalMsg: lvl = Level::Error; break;
    }
    if (!isEnabled(lvl))
        return;
    // The Record writes and flushes from its destructor, which runs before
    // this handler returns - so even a qFatal line reaches the sink before Qt
    // aborts the process.
    Record record(lvl, cat::Qt, context.file ? context.file : "", context.line);
    record.stream() << message;
}

// Accepts "--log-mode develop" and "--log-mode=develop" alike, because both
// spellings are what people actually type.
QString optionValue(const QStringList &args, const QString &name)
{
    const QString prefix = name + QLatin1Char('=');
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == name && i + 1 < args.size())
            return args.at(i + 1);
        if (arg.startsWith(prefix))
            return arg.mid(prefix.size());
    }
    return QString();
}

} // namespace

QStringList cat::all()
{
    return {QString::fromLatin1(cat::App),     QString::fromLatin1(cat::Config),
            QString::fromLatin1(cat::Ui),      QString::fromLatin1(cat::Session),
            QString::fromLatin1(cat::Worker),  QString::fromLatin1(cat::Audio),
            QString::fromLatin1(cat::Queue),   QString::fromLatin1(cat::Rpc),
            QString::fromLatin1(cat::Grpc),    QString::fromLatin1(cat::Http2),
            QString::fromLatin1(cat::Poll),    QString::fromLatin1(cat::Model),
            QString::fromLatin1(cat::Qt)};
}

QVector<Level> selectableLevels()
{
    return {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error};
}

QString modeName(Mode value)
{
    return value == Mode::Develop ? QStringLiteral("develop") : QStringLiteral("debug");
}

QString levelName(Level value)
{
    switch (value) {
    case Level::Trace: return QStringLiteral("trace");
    case Level::Debug: return QStringLiteral("debug");
    case Level::Info: return QStringLiteral("info");
    case Level::Warn: return QStringLiteral("warn");
    case Level::Error: return QStringLiteral("error");
    case Level::Off: return QStringLiteral("off");
    }
    return QStringLiteral("debug");
}

Mode modeFromString(const QString &text, bool *ok)
{
    const QString key = text.trimmed().toLower();
    if (ok)
        *ok = true;
    if (key == QLatin1String("develop") || key == QLatin1String("file"))
        return Mode::Develop;
    if (key == QLatin1String("debug") || key == QLatin1String("console"))
        return Mode::Debug;
    if (ok)
        *ok = false;
    return kDefaultMode;
}

Level levelFromString(const QString &text, bool *ok)
{
    const QString key = text.trimmed().toLower();
    if (ok)
        *ok = true;
    if (key == QLatin1String("trace"))
        return Level::Trace;
    if (key == QLatin1String("debug"))
        return Level::Debug;
    if (key == QLatin1String("info"))
        return Level::Info;
    if (key == QLatin1String("warn") || key == QLatin1String("warning"))
        return Level::Warn;
    if (key == QLatin1String("error"))
        return Level::Error;
    if (key == QLatin1String("off") || key == QLatin1String("none"))
        return Level::Off;
    if (ok)
        *ok = false;
    return kDefaultLevel;
}

Mode mode()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.mode;
}

Level level()
{
    return Level(state().level.loadAcquire());
}

bool isEnabled(Level lvl)
{
    return lvl != Level::Off && int(lvl) >= state().level.loadAcquire();
}

void setLevel(Level lvl)
{
    state().level.storeRelease(int(lvl));
}

void setMode(Mode value)
{
    State &s = state();
    {
        QMutexLocker lock(&s.mutex);
        if (s.mode == value)
            return;
        s.mode = value;
        if (value == Mode::Debug && s.file.isOpen()) {
            s.file.flush();
            s.file.close();
        }
    }
    if (value == Mode::Debug)
        ensureConsole();
    LOG_INFO(cat::App) << "log mode ->" << modeName(value)
                       << (value == Mode::Develop ? "(file)" : "(console)");
    if (value == Mode::Develop) {
        // Emitting one line is what actually creates and names the file, so
        // the path reported here is never a guess.
        LOG_INFO(cat::App) << "log file:" << logFilePath();
    }
}

QString logFilePath()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.filePath;
}

QString logDirectory()
{
    const QString path = logFilePath();
    return path.isEmpty() ? defaultLogDirectory() : QFileInfo(path).absolutePath();
}

Bus *bus()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.bus;
}

QVector<Entry> recent()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    return s.recent;
}

void clearRecent()
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    s.recent.clear();
}

void ensureConsole()
{
#ifdef Q_OS_WIN
    State &s = state();
    // If stdout was already redirected to a pipe or a file, that handle is
    // inherited and works as is - reopening CONOUT$ here would hijack the
    // redirect and send the output to the console instead of to whoever asked
    // for it.  Only borrow a console when there is genuinely nowhere to write.
    const HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    if (existing && existing != INVALID_HANDLE_VALUE) {
        s.consoleReady = true;
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    // Vietnamese in log lines is UTF-8; the console defaults to the OEM code
    // page and would render it as rubbish.
    SetConsoleOutputCP(CP_UTF8);
    s.consoleReady = true;
#else
    state().consoleReady = true;
#endif
}

void initFromArguments(const QStringList &args)
{
    State &s = state();

    Mode resolvedMode = kDefaultMode;
    Level resolvedLevel = kDefaultLevel;
    bool modeForced = false;
    bool levelForced = false;

    const QString envMode = qEnvironmentVariable("S2T_LOG_MODE");
    if (!envMode.isEmpty()) {
        bool ok = false;
        const Mode parsed = modeFromString(envMode, &ok);
        if (ok) {
            resolvedMode = parsed;
            modeForced = true;
        }
    }
    const QString envLevel = qEnvironmentVariable("S2T_LOG_LEVEL");
    if (!envLevel.isEmpty()) {
        bool ok = false;
        const Level parsed = levelFromString(envLevel, &ok);
        if (ok) {
            resolvedLevel = parsed;
            levelForced = true;
        }
    }
    QString filePath = qEnvironmentVariable("S2T_LOG_FILE");

    // The command line is the last word over the environment.
    const QString argMode = optionValue(args, QStringLiteral("--log-mode"));
    if (!argMode.isEmpty()) {
        bool ok = false;
        const Mode parsed = modeFromString(argMode, &ok);
        if (ok) {
            resolvedMode = parsed;
            modeForced = true;
        }
    }
    const QString argLevel = optionValue(args, QStringLiteral("--log-level"));
    if (!argLevel.isEmpty()) {
        bool ok = false;
        const Level parsed = levelFromString(argLevel, &ok);
        if (ok) {
            resolvedLevel = parsed;
            levelForced = true;
        }
    }
    const QString argFile = optionValue(args, QStringLiteral("--log-file"));
    if (!argFile.isEmpty())
        filePath = argFile;
    // Naming a destination file only makes sense as develop mode; asking for
    // one and still getting console-only output would be a trap.
    if (!filePath.isEmpty() && argMode.isEmpty() && envMode.isEmpty()) {
        resolvedMode = Mode::Develop;
        modeForced = true;
    }

    // Registered and created before the first line and before any worker
    // thread exists: from here on the bus can be published to from anywhere.
    qRegisterMetaType<Entry>("applog::Entry");
    {
        QMutexLocker lock(&s.mutex);
        s.mode = resolvedMode;
        s.modeForced = modeForced;
        s.levelForced = levelForced;
        s.filePath = filePath;
        if (!s.bus)
            s.bus = new Bus();
    }
    s.level.storeRelease(int(resolvedLevel));

    // The console is prepared even in develop mode: a failure to open the log
    // file has to be visible somewhere.
    ensureConsole();
    qInstallMessageHandler(messageHandler);

    LOG_INFO(cat::App) << "logger started - mode=" << modeName(resolvedMode)
                       << "level=" << levelName(resolvedLevel)
                       << (modeForced ? "(forced by command line/environment)" : "(default)");
    if (resolvedMode == Mode::Develop)
        LOG_INFO(cat::App) << "log file:" << logFilePath();
}

void applyStoredPreference(Mode storedMode, Level storedLevel)
{
    State &s = state();
    bool modeForced = false;
    bool levelForced = false;
    {
        QMutexLocker lock(&s.mutex);
        modeForced = s.modeForced;
        levelForced = s.levelForced;
    }
    if (!levelForced)
        setLevel(storedLevel);
    if (!modeForced)
        setMode(storedMode);
}

void shutdown()
{
    LOG_INFO(cat::App) << "logger stopping";
    qInstallMessageHandler(nullptr);
    State &s = state();
    QMutexLocker lock(&s.mutex);
    if (s.file.isOpen()) {
        s.file.flush();
        s.file.close();
    }
}

Record::Record(Level lvl, const char *category, const char *file, int line)
    : m_level(lvl), m_category(category), m_file(file), m_line(line), m_stream(&m_text)
{
}

Record::~Record()
{
    // Fixed columns for level, category and thread: the whole point is that
    // `grep " worker "` picks out one subsystem cleanly.
    const QString head =
        QStringLiteral("%1 %2 %3 %4 %5:%6 | ")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 QString::fromLatin1(levelTag(m_level)),
                 QString::fromLatin1(m_category ? m_category : "").leftJustified(8),
                 threadTag().leftJustified(14),
                 baseName(m_file))
            .arg(m_line);
    m_stream.flush();
    emitLine(m_level, m_category, head + m_text);
}

} // namespace applog
