// The small modal pieces of the client: starting a session, renaming a
// speaker, editing one sentence, reading the audit trail, and pointing the
// app at a server.
#ifndef DIALOGS_H
#define DIALOGS_H

#include "../core/SessionController.h"
#include "../core/SessionTypes.h"
#include "../proto/AsrSession.h"

#include <QDialog>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
QT_END_NAMESPACE

// Roster + metadata, shared by "start microphone" and "replay a file" exactly
// as the one HTML panel served both.
class StartSessionDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Purpose { Microphone, File };

    StartSessionDialog(SessionController *controller, Purpose purpose, QWidget *parent = nullptr);

    // Tri-state result.  restrictSpeakers()==false omits expected_speakers
    // entirely; true with an empty list sends an explicit empty allow-list,
    // which means "recognise no registered name" - a different instruction,
    // not a fallback to the first.
    bool restrictSpeakers() const;
    QStringList selectedSpeakers() const;
    SessionMeta meta() const;

private slots:
    void reloadRoster();
    void updateSummary();

private:
    SessionController *m_controller = nullptr;
    Purpose m_purpose;
    QLineEdit *m_title = nullptr;
    QLineEdit *m_participants = nullptr;
    QComboBox *m_security = nullptr;
    QComboBox *m_mode = nullptr;
    QRadioButton *m_unrestricted = nullptr;
    QRadioButton *m_restricted = nullptr;
    QListWidget *m_roster = nullptr;
    QLabel *m_info = nullptr;
    QPushButton *m_confirm = nullptr;
};

// Give a diarization slot a display name, or fold it into another slot the
// meeting already saw (diarization sometimes splits one person in two).
class SpeakerRenameDialog : public QDialog
{
    Q_OBJECT

public:
    SpeakerRenameDialog(const QString &fromSpeaker, const QString &currentName,
                        const QStringList &otherSpeakers, QWidget *parent = nullptr);

    QString displayName() const;
    QString mergeTarget() const;

private:
    QString m_fromSpeaker;
    QLineEdit *m_name = nullptr;
    QComboBox *m_merge = nullptr;
};

// One sentence, token by token.  A token past the commit boundary is shown
// but not editable: the server refuses it with edit_range_not_committed
// because a later correction pass can still rewrite it.
class SentenceEditDialog : public QDialog
{
    Q_OBJECT

public:
    SentenceEditDialog(SessionController *controller, const asr::DisplayRow &row,
                       const QString &sessionId, quint64 baseRevision, bool transcriptFinal,
                       double commitBoundarySec, const QString &editorId, QWidget *parent = nullptr);

signals:
    void transcriptChanged(const asr::SessionState &state, quint64 revision);

private slots:
    void commitItem(class QListWidgetItem *item);

private:
    void rebuild();

    SessionController *m_controller = nullptr;
    asr::DisplayRow m_row;
    QString m_sessionId;
    QString m_editorId;
    quint64 m_revision = 0;
    bool m_final = false;
    double m_commitBoundarySec = -1.0;
    QListWidget *m_tokens = nullptr;
    QLabel *m_info = nullptr;
    bool m_rebuilding = false;
};

// Read side of audit.jsonl: who changed what, and when.
class AuditHistoryDialog : public QDialog
{
    Q_OBJECT

public:
    AuditHistoryDialog(SessionController *controller, const QString &sessionId,
                       QWidget *parent = nullptr);

private slots:
    void reload();

private:
    SessionController *m_controller = nullptr;
    QLineEdit *m_session = nullptr;
    QLabel *m_info = nullptr;
    QListWidget *m_rows = nullptr;
};

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    SettingsDialog(AppConfig *config, QWidget *parent = nullptr);

    void applyToConfig() const;

private slots:
    void browseTokenFile();
    void browseControlApp();
    void updateLogHint();

private:
    AppConfig *m_config = nullptr;
    QLineEdit *m_server = nullptr;
    QLineEdit *m_token = nullptr;
    QComboBox *m_device = nullptr;
    QLineEdit *m_expectedName = nullptr;
    QSpinBox *m_sampleRate = nullptr;
    QSpinBox *m_channels = nullptr;
    QDoubleSpinBox *m_bufferSec = nullptr;
    QLineEdit *m_controlApp = nullptr;
    QCheckBox *m_pipelineTrace = nullptr;
    QCheckBox *m_paceReplay = nullptr;
    // The debug-log flag: where the log goes, and how much of it there is.
    QComboBox *m_logMode = nullptr;
    QComboBox *m_logLevel = nullptr;
    QLabel *m_logPath = nullptr;
};

#endif // DIALOGS_H
