#include "RunJournal.h"

#include "Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QSysInfo>

namespace runjournal {
namespace {

// How many journals are kept.  One per run, and a tester working through a
// checklist starts the program many times in an afternoon, so this is a couple
// of days of that - enough that "the run where it went wrong" is still there
// tomorrow, bounded enough that nobody ever has to sweep the folder.
const int kKeepFiles = 40;

// A step line is short by construction, but a program left running for a week
// with a stuck retry loop could still fill a disk.  At this size the file is
// closed with a line saying so, and further steps go only to the ordinary log,
// which rotates.
const qint64 kMaxBytes = 8 * 1024 * 1024;

struct State
{
    QMutex mutex;
    QFile file;
    QString path;
    QString dir;
    bool stepsOpened = false;
    bool finished = false;
    bool complained = false;
    qint64 bytes = 0;
    QStringList lines;
};

State &state()
{
    static State s;
    return s;
}

QString stamp()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

// Caller holds the mutex.
void writeLocked(const QString &line)
{
    State &s = state();
    s.lines.append(line);
    // Bounded, like the logger's own ring: the diagnostics window shows this,
    // and a long run must not make that window the biggest thing in memory.
    if (s.lines.size() > 4000)
        s.lines.remove(0, 1000);
    if (!s.file.isOpen())
        return;
    const QByteArray bytes = line.toUtf8() + '\n';
    s.file.write(bytes);
    // Flushed per line, for the same reason the debug log is: the run this
    // file exists to explain is quite often the run that ended badly.
    s.file.flush();
    s.bytes += bytes.size();
    if (s.bytes >= kMaxBytes) {
        s.file.write(QStringLiteral("[%1] --- nhật ký đã đạt giới hạn %2 MB, dừng ghi tệp này ---\n")
                         .arg(stamp())
                         .arg(kMaxBytes / (1024 * 1024))
                         .toUtf8());
        s.file.flush();
        s.file.close();
    }
}

// Caller holds the mutex.  Oldest first, so what is dropped is what has been
// least useful for longest.
void pruneLocked()
{
    State &s = state();
    QDir dir(s.dir);
    QFileInfoList files = dir.entryInfoList({QStringLiteral("quy-trinh-*.log")}, QDir::Files,
                                            QDir::Time | QDir::Reversed);
    while (files.size() > kKeepFiles) {
        QFile::remove(files.first().absoluteFilePath());
        files.removeFirst();
    }
}

} // namespace

QString directory()
{
    {
        QMutexLocker lock(&state().mutex);
        if (!state().dir.isEmpty())
            return state().dir;
    }
    // Beside the debug log on purpose: one folder to open, one folder to zip
    // up, and the diagnostics window already knows how to open that one.
    return applog::logDirectory();
}

QString path()
{
    QMutexLocker lock(&state().mutex);
    return state().path;
}

QStringList recent()
{
    QMutexLocker lock(&state().mutex);
    return state().lines;
}

void start(const QString &program, const QString &version)
{
    State &s = state();
    {
        QMutexLocker lock(&s.mutex);
        if (s.file.isOpen())
            return;
        s.dir = applog::logDirectory();
        QDir().mkpath(s.dir);
        // Named after the moment the run began, and carrying the pid: two
        // copies of the client started in the same second on one machine -
        // which a tester comparing two configurations will do - must not end
        // up writing into the same file.
        const QString name =
            QStringLiteral("quy-trinh-%1-%2-%3.log")
                .arg(program,
                     QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
                     QString::number(QCoreApplication::applicationPid()));
        s.path = QDir(s.dir).filePath(name);
        s.file.setFileName(s.path);
        if (!s.file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (!s.complained) {
                s.complained = true;
                LOG_ERROR(applog::cat::App)
                    << "cannot open the run journal" << s.path << "-" << s.file.errorString()
                    << "- the steps will only be in the debug log";
            }
            s.path.clear();
            return;
        }
        s.bytes = 0;
        pruneLocked();

        writeLocked(QStringLiteral("================================================"));
        writeLocked(QStringLiteral(" NHẬT KÝ QUY TRÌNH - %1 %2").arg(program, version));
        writeLocked(QStringLiteral(" Bắt đầu lúc: %1").arg(stamp()));
        writeLocked(QStringLiteral(" Tệp này ghi TỪNG BƯỚC đã chạy, theo đúng thứ tự."));
        writeLocked(QStringLiteral(" Khi có sự cố, gửi nguyên tệp này cho đội phát triển."));
        writeLocked(QStringLiteral("================================================"));
        writeLocked(QString());
        writeLocked(QStringLiteral("=== MÔI TRƯỜNG ==="));
    }

    field(QStringLiteral("Chương trình"), QStringLiteral("%1 %2").arg(program, version));
    field(QStringLiteral("Biên dịch lúc"),
          QStringLiteral("%1 %2").arg(QString::fromLatin1(__DATE__), QString::fromLatin1(__TIME__)));
    field(QStringLiteral("Qt"), QString::fromLatin1(qVersion()));
    field(QStringLiteral("Hệ điều hành"),
          QStringLiteral("%1 (nhân %2, %3)")
              .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                   QSysInfo::currentCpuArchitecture()));
    field(QStringLiteral("Máy"), QHostInfo::localHostName());
    field(QStringLiteral("Ngôn ngữ hệ thống"), QLocale::system().name());
    field(QStringLiteral("Thư mục làm việc"), QDir::currentPath());
    field(QStringLiteral("Tiến trình (pid)"), QString::number(QCoreApplication::applicationPid()));
    field(QStringLiteral("Dòng lệnh"), QCoreApplication::arguments().join(QLatin1Char(' ')));
    field(QStringLiteral("Nhật ký gỡ lỗi"),
          applog::logFilePath().isEmpty() ? applog::logDirectory() : applog::logFilePath());

    LOG_INFO(applog::cat::App) << "run journal:" << path();
}

void section(const QString &title)
{
    QMutexLocker lock(&state().mutex);
    writeLocked(QString());
    writeLocked(QStringLiteral("--- %1 ---").arg(title));
}

void field(const QString &name, const QString &value)
{
    QMutexLocker lock(&state().mutex);
    // Padded so the values line up: the header is read by eye, top to bottom,
    // and a ragged left edge is what makes somebody stop reading it.
    writeLocked(QStringLiteral("  %1 : %2").arg(name, -28).arg(value));
}

void step(const QString &event, const QString &detail)
{
    const QString line =
        QStringLiteral("[%1] %2  %3").arg(stamp()).arg(event, -24).arg(detail);
    {
        QMutexLocker lock(&state().mutex);
        // The steps section opens on the first step rather than in start(),
        // so the header stays one uninterrupted block whatever order the
        // callers fill it in.
        if (!state().stepsOpened) {
            state().stepsOpened = true;
            writeLocked(QString());
            writeLocked(QStringLiteral("=== CÁC BƯỚC ĐÃ CHẠY ==="));
        }
        writeLocked(line);
    }
    // Into the ordinary log as well, so the two files can be read side by side
    // when a step needs the frames around it to make sense.
    LOG_INFO(applog::cat::App) << "STEP" << event << "|" << detail;
}

void finish(const QString &reason)
{
    State &s = state();
    QMutexLocker lock(&s.mutex);
    // Once.  The first caller is the one that knows why the program is
    // stopping; everything after it is teardown, and teardown must not be
    // allowed to overwrite the reason.
    if (s.finished)
        return;
    s.finished = true;
    writeLocked(QString());
    writeLocked(QStringLiteral("=== KẾT THÚC ==="));
    writeLocked(QStringLiteral("  Lý do  : %1").arg(reason));
    writeLocked(QStringLiteral("  Lúc    : %1").arg(stamp()));
    writeLocked(QStringLiteral("  Số bước: %1").arg(s.lines.size()));
    writeLocked(QString());
    // The sentence that tells a reader what a MISSING ending means.  A journal
    // that stops in the middle of the steps was not written by a program that
    // shut down - it was killed, or it crashed, and that is worth knowing
    // before anybody starts looking for a logic error.
    writeLocked(QStringLiteral("  (Không có mục KẾT THÚC nghĩa là tiến trình bị giết hoặc sập.)"));
    if (s.file.isOpen()) {
        s.file.flush();
        s.file.close();
    }
}

} // namespace runjournal
