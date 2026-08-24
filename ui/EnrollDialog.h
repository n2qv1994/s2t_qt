// Speaker enrolment and per-session speaker publishing.
//
// Two tabs, matching the two halves of the old enrolment page:
//  * record a dedicated reading and enrol it into the global CAM++ DB;
//  * review the speakers a finished meeting told apart, then decide for each
//    one whether it stays session-local or gets published globally.
//
// The recording itself never touches the network - it is captured into memory
// and only the finished WAV is sent - so losing the connection mid-reading
// does not corrupt anything, and the dialog says so instead of making someone
// start over for nothing.
#ifndef ENROLLDIALOG_H
#define ENROLLDIALOG_H

#include "../core/SessionController.h"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTimer;
QT_END_NAMESPACE

class AudioCapture;

class EnrollDialog : public QDialog
{
    Q_OBJECT

public:
    EnrollDialog(SessionController *controller, const QString &editorId, QWidget *parent = nullptr);
    ~EnrollDialog() override;

private slots:
    void toggleRecording();
    void submitRecording();
    void loadScript();
    void loadRoster();
    void loadSessionSpeakers();
    void saveSelections();

private:
    QWidget *buildEnrollTab();
    QWidget *buildSessionTab();
    void setStatus(const QString &kind, const QString &text);

    SessionController *m_controller = nullptr;
    QString m_editorId;

    QTextEdit *m_script = nullptr;
    QLabel *m_guidance = nullptr;
    QLineEdit *m_speakerName = nullptr;
    QPushButton *m_recordButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QLabel *m_timerLabel = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_allowBelow = nullptr;
    QListWidget *m_belowPolicy = nullptr;
    QListWidget *m_roster = nullptr;

    QLineEdit *m_sessionInput = nullptr;
    QLabel *m_registryStatus = nullptr;
    QTableWidget *m_speakers = nullptr;
    QLabel *m_saveResults = nullptr;

    AudioCapture *m_capture = nullptr;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    QByteArray m_recorded;
    // Kept after a failed upload so the reading is not lost with the request.
    QByteArray m_pendingWav;
    QString m_pendingName;
    bool m_recording = false;
    int m_sampleRate = 48000;
    int m_channels = 1;
};

#endif // ENROLLDIALOG_H
