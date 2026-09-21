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

#include "core/SessionController.h"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>

QT_BEGIN_NAMESPACE
class QAudioOutput;
class QBuffer;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMediaPlayer;
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
    void loadFromFile();
    void submitRecording();
    void previewRecording();
    void loadScript();
    void loadRoster();
    void loadSessionSpeakers();
    void saveSelections();
    void loadGlobalSpeakers();

private:
    QWidget *buildEnrollTab();
    QWidget *buildSessionTab();
    QWidget *buildGlobalTab();
    void setStatus(const QString &kind, const QString &text);
    // Plays a complete WAV through the dialog's own player.  Used for the
    // enrolment preview, which is the only way to HEAR what would be enrolled
    // before doing something that cannot be undone.
    void playWav(const QByteArray &wav);
    // `verb` is "deactivate" | "activate" | "delete".  One function for all
    // three: they differ in the confirmation they demand, not in what they do
    // afterwards.
    void runGlobalAction(const QString &verb);
    QString selectedGlobalSpeaker(QString *name) const;

    SessionController *m_controller = nullptr;
    QString m_editorId;

    QTextEdit *m_script = nullptr;
    QLabel *m_guidance = nullptr;
    QLineEdit *m_speakerName = nullptr;
    QPushButton *m_recordButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_fileButton = nullptr;
    QLabel *m_timerLabel = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_allowBelow = nullptr;
    QListWidget *m_belowPolicy = nullptr;
    QListWidget *m_roster = nullptr;

    QLineEdit *m_sessionInput = nullptr;
    QLabel *m_registryStatus = nullptr;
    QTableWidget *m_speakers = nullptr;
    QLabel *m_saveResults = nullptr;

    // ---- the global database tab -------------------------------------------
    QTableWidget *m_global = nullptr;
    QLabel *m_globalStatus = nullptr;
    QLineEdit *m_globalReason = nullptr;
    QPushButton *m_deactivateButton = nullptr;
    QPushButton *m_activateButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_previewButton = nullptr;
    QLabel *m_previewInfo = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer *m_audioBuffer = nullptr;

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
