// Debug logging for every runtime flow in the client, with one flag that
// decides where the lines go.
//
//   Mode::Debug   -> the console.  The GUI binary is linked for the windows
//                    subsystem and has no console of its own, so this borrows
//                    the launching shell's one (and falls back to the
//                    debugger's OutputDebugString when there is none).
//   Mode::Develop -> a rotating file under the user's app-data directory.
//
// The flag is resolved once at startup, first match wins:
//   1. --log-mode debug|develop      (command line)
//   2. S2T_LOG_MODE=debug|develop    (environment)
//   3. the value saved in "Cấu hình" (AppConfig, applied by MainWindow)
//   4. the compile-time default      (qmake CONFIG+=develop)
// A later setMode() - the settings dialog - always wins over all of them,
// because that is an operator asking for it right now.
//
// Level is the separate knob: it decides how much is written, not where.
// Trace is the per-audio-packet firehose and is off unless asked for.
//
// Every sink is mutex-protected on purpose: capture, the session worker, the
// state poller and three RPC lanes all log from their own threads.
#ifndef LOGGER_H
#define LOGGER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>

namespace applog {

// The flag.  Debug -> console, Develop -> file.
enum class Mode { Debug, Develop };

enum class Level { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

// Subsystem tags.  Kept short and fixed-width in the output so a whole
// session's log can be filtered with a plain `findstr`/`grep` on one column.
namespace cat {
constexpr const char *App = "app";
constexpr const char *Config = "config";
constexpr const char *Ui = "ui";
constexpr const char *Session = "session";
constexpr const char *Worker = "worker";
constexpr const char *Audio = "audio";
constexpr const char *Queue = "queue";
constexpr const char *Rpc = "rpc";
constexpr const char *Grpc = "grpc";
constexpr const char *Http2 = "http2";
constexpr const char *Poll = "poll";
constexpr const char *Model = "model";
constexpr const char *Qt = "qt";

// Every tag above, in one place, so a filter offering "all subsystems" cannot
// silently omit one that was added here later.
QStringList all();
} // namespace cat

// Every level a person may pick, coarsest last.  Off is not in it: it is a
// sink state, not something to offer in a filter.
QVector<Level> selectableLevels();

// Resolves the flag from argv/env/compile default, opens the sink and routes
// Qt's own qDebug/qWarning messages into it.  Safe to call once, from main().
void initFromArguments(const QStringList &args);

// Applies what the operator saved in the settings, unless the command line or
// the environment already forced a mode - an explicit run-time override must
// not be silently undone by a stored preference.
void applyStoredPreference(Mode mode, Level level);

// Live switch: takes effect on the very next line, closing or opening the
// file sink as needed.
void setMode(Mode mode);
void setLevel(Level level);

Mode mode();
Level level();
bool isEnabled(Level lvl);
// Empty until the file sink has actually been opened.
QString logFilePath();
// The directory the log file lives in - or would live in, when the current
// mode is Debug and no file has been opened.  Never empty.
QString logDirectory();
// Flushes and closes the file sink; call before returning from main().
void shutdown();

QString modeName(Mode value);
QString levelName(Level value);
Mode modeFromString(const QString &text, bool *ok = nullptr);
Level levelFromString(const QString &text, bool *ok = nullptr);

// Borrows the parent console on Windows so console output is visible at all.
// A no-op elsewhere, and a no-op when stdout is already redirected.
void ensureConsole();

// One emitted line, kept in a form a viewer can filter on without the logger
// having to know anything about the UI.
struct Entry
{
    Level level = Level::Info;
    QString category;
    QString line; // fully formatted, exactly as it went to the console or file
};

// Live feed for the in-app log viewer.  The object lives on the GUI thread, so
// a line logged from the capture, worker, poller or an RPC lane reaches the
// widget through a queued connection and never touches it directly.
class Bus : public QObject
{
    Q_OBJECT

public:
    explicit Bus(QObject *parent = nullptr) : QObject(parent) {}

    // Called by the logger from whatever thread emitted the line.
    void publish(const Entry &entry) { emit appended(entry); }

signals:
    void appended(const applog::Entry &entry);
};

Bus *bus();

// The last few thousand lines, so a viewer opened halfway through a session
// starts with the history rather than an empty page.
QVector<Entry> recent();
void clearRecent();

// One log line, assembled on the stack and emitted by the destructor.
class Record
{
public:
    Record(Level level, const char *category, const char *file, int line);
    ~Record();

    Record(const Record &) = delete;
    Record &operator=(const Record &) = delete;

    // Separated by a space the way qDebug() does it, because every call site
    // is written expecting that; QTextStream on its own would run the pieces
    // together into "mode=debuglevel=debug".
    template <typename T>
    Record &operator<<(const T &value)
    {
        if (!m_first) {
            m_stream.flush();
            // ... except right after a '=' or an opening bracket, so that
            // `<< "rev=" << 7` reads "rev=7" and not "rev= 7".
            const QChar last = m_text.isEmpty() ? QChar() : m_text.at(m_text.size() - 1);
            if (last != QLatin1Char('=') && last != QLatin1Char('('))
                m_stream << ' ';
        }
        m_first = false;
        m_stream << value;
        return *this;
    }

    Record &stream() { return *this; }

private:
    Level m_level;
    const char *m_category;
    const char *m_file;
    int m_line;
    bool m_first = true;
    QString m_text;
    QTextStream m_stream;
};

} // namespace applog

// Crosses a thread boundary as a queued signal argument, so Qt has to know it.
Q_DECLARE_METATYPE(applog::Entry)

// A loop that runs at most once, rather than an if: the macro is routinely
// written as the unbraced body of an if, and any expansion containing an `if`
// there trips -Wdangling-else, which this project builds with.  A `for` binds
// to nothing and cannot swallow an outer else.  Arguments are not evaluated
// when the level is disabled, and the Record is destroyed - i.e. the line is
// emitted - at the semicolon.
#define S2T_LOG_AT(lvl, category)                                                                  \
    for (bool s2tLogOnce_ = applog::isEnabled(lvl); s2tLogOnce_; s2tLogOnce_ = false)              \
    applog::Record(lvl, category, __FILE__, __LINE__).stream()

#define LOG_TRACE(category) S2T_LOG_AT(applog::Level::Trace, category)
#define LOG_DEBUG(category) S2T_LOG_AT(applog::Level::Debug, category)
#define LOG_INFO(category) S2T_LOG_AT(applog::Level::Info, category)
#define LOG_WARN(category) S2T_LOG_AT(applog::Level::Warn, category)
#define LOG_ERROR(category) S2T_LOG_AT(applog::Level::Error, category)

#endif // LOGGER_H
