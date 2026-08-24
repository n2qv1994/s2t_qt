// The two things this client can say about itself: what it is doing right now
// (the log) and whether its parts still work (the diagnostics).
//
// Both already existed and neither had a surface.  The log went to a console
// the operator cannot reach - the GUI binary is linked for the windows
// subsystem - or to a file somewhere under AppData; the protocol self-test and
// the server probe were --selftest / --probe only.  On a deployed workstation
// with no shell that meant the answers to "what happened" and "is the server
// reachable" were both out of reach.  This window is where they live.
#ifndef DIAGNOSTICSWINDOW_H
#define DIAGNOSTICSWINDOW_H

#include "../core/AppConfig.h"
#include "../core/Logger.h"

#include <QThread>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
QT_END_NAMESPACE

class SessionController;

// The diagnostics block on network I/O for seconds at a time (the probe alone
// allows a 10 s deadline per RPC), so they run here instead of freezing the
// GUI thread.  One at a time: selftest's report capture is process-wide.
class DiagnosticsRunner : public QThread
{
    Q_OBJECT

public:
    enum class Job { Codec, Probe, Network };

    DiagnosticsRunner(Job job, const QString &target, const QString &token,
                      QObject *parent = nullptr);

signals:
    void completed(const QString &report, int code);

protected:
    void run() override;

private:
    Job m_job;
    QString m_target;
    QString m_token;
};

class DiagnosticsWindow : public QWidget
{
    Q_OBJECT

public:
    DiagnosticsWindow(SessionController *controller, AppConfig *config, QWidget *parent = nullptr);
    ~DiagnosticsWindow() override;

private slots:
    void onLogLine(const applog::Entry &entry);
    void onModeChanged();
    void onLevelChanged();
    void onFilterChanged();
    void clearLog();
    void saveLogAs();
    void openLogFolder();
    void startCodecTest();
    void startProbe();
    void startNetworkTest();
    void onJobCompleted(const QString &report, int code);
    void onConnectionUpdated();

private:
    QWidget *buildLogTab();
    QWidget *buildDiagnosticsTab();
    bool passesFilter(const applog::Entry &entry) const;
    void cacheFilters();
    void appendToView(const applog::Entry &entry);
    void scrollToTail();
    void rerender();
    void countEntry(const applog::Entry &entry, int delta);
    void recountAll();
    void refreshCounters();
    void refreshLogPath();
    void startJob(DiagnosticsRunner::Job job, const QString &title);
    void setJobRunning(bool running);

    SessionController *m_controller = nullptr;
    AppConfig *m_config = nullptr;

    // ---- log tab ----------------------------------------------------------
    QComboBox *m_mode = nullptr;
    QComboBox *m_level = nullptr;
    QComboBox *m_minShown = nullptr;
    QComboBox *m_category = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_follow = nullptr;
    QCheckBox *m_hold = nullptr;
    QPlainTextEdit *m_view = nullptr;
    QLabel *m_logPath = nullptr;
    QLabel *m_counters = nullptr;
    // Everything received, filtered or not, so changing a filter re-renders
    // from what is already here instead of losing the history.
    QVector<applog::Entry> m_entries;
    // Tracked as entries arrive and leave rather than recounted per line.
    int m_warnings = 0;
    int m_errors = 0;
    // Read out of the widgets once per filter change, not once per entry: a
    // re-render walks every held entry, and pulling a QVariant out of a combo
    // twenty thousand times to learn the same answer is pure waste.
    applog::Level m_floor = applog::Level::Trace;
    QString m_categoryFilter;
    QString m_searchFilter;

    // ---- diagnostics tab --------------------------------------------------
    QLineEdit *m_target = nullptr;
    QLineEdit *m_token = nullptr;
    QPushButton *m_codecButton = nullptr;
    QPushButton *m_probeButton = nullptr;
    QPushButton *m_networkButton = nullptr;
    QPushButton *m_recheckButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QLabel *m_jobStatus = nullptr;
    QPlainTextEdit *m_report = nullptr;
    DiagnosticsRunner *m_runner = nullptr;
};

#endif // DIAGNOSTICSWINDOW_H
