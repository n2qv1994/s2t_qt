#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "core/AppConfig.h"
#include "core/SessionController.h"

#include <QMainWindow>

QT_BEGIN_NAMESPACE
class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QTextEdit;
class QTimer;
QT_END_NAMESPACE

class TimelineView;
class ReviewPanel;
class EnrollDialog;
class TraceWindow;
class SubtitleWindow;
class EvidenceWindow;
class DiagnosticsWindow;
class StatusPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void startMicrophone();
    void startFileReplay();
    void stopSession();
    void togglePause();
    void openReview();
    void openAuditHistory();
    void openEnrollment();
    void openTrace();
    void openSubtitles();
    void openEvidence();
    void openDiagnostics();
    void openSettings();
    void toggleTicker(bool enabled);
    void onModelUpdated();
    void onStatusUpdated();
    void onConnectionUpdated();
    void onWordActivated(double startSec, double endSec, const QPoint &globalPos);

private:
    // buildUi() is split by *what the operator is doing* rather than by widget
    // type: the actions exist once and are placed twice - once in a menu, so
    // every capability is discoverable and has a shortcut, and once on the
    // toolbar if it is something reached for during a live meeting.
    void buildUi();
    void buildActions();
    void buildMenus();
    void buildToolBar();
    void buildStatusBar();
    void buildCentral();
    void refreshHighlights();
    void refreshDelayBox();
    void refreshTicker();
    void refreshActions();

    AppConfig m_config;
    SessionController *m_controller = nullptr;

    TimelineView *m_timeline = nullptr;
    QTextEdit *m_ticker = nullptr;
    QStackedWidget *m_stack = nullptr;
    StatusPanel *m_status = nullptr;
    QLabel *m_connectionPill = nullptr;
    QLabel *m_deviceLabel = nullptr;
    QLabel *m_sessionLabel = nullptr;
    QLineEdit *m_operatorId = nullptr;

    QAction *m_micAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_fileAction = nullptr;
    QAction *m_liveAction = nullptr;
    QAction *m_textAction = nullptr;
    QAction *m_tickerAction = nullptr;
    QAction *m_denoiseOnAction = nullptr;
    QAction *m_denoiseOffAction = nullptr;
    QAction *m_lowConfAction = nullptr;
    QAction *m_reviewAction = nullptr;
    QAction *m_historyAction = nullptr;
    QAction *m_subtitleAction = nullptr;
    QAction *m_traceAction = nullptr;
    QAction *m_evidenceAction = nullptr;
    QAction *m_enrollAction = nullptr;
    QAction *m_logAction = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_quitAction = nullptr;

    QDockWidget *m_reviewDock = nullptr;
    ReviewPanel *m_reviewPanel = nullptr;
    TraceWindow *m_traceWindow = nullptr;
    SubtitleWindow *m_subtitleWindow = nullptr;
    EvidenceWindow *m_evidenceWindow = nullptr;
    DiagnosticsWindow *m_diagnosticsWindow = nullptr;
    QTimer *m_statusTimer = nullptr;
};

#endif // MAINWINDOW_H
