// Reopen a finished (or still running) meeting: read its durable transcript,
// listen back to any point in it, correct the text, and relabel speakers.
//
// The transcript is loaded in bounded time windows rather than in one call.
// Asking for a whole meeting used to work until state.json outgrew gRPC's
// 4 MiB limit and a long session failed outright with RESOURCE_EXHAUSTED; the
// adapter caches its parse, so each extra window costs almost nothing.
//
// Editing is optimistic-concurrency guarded: every save carries the revision
// the panel loaded at, and the server rejects it if the transcript moved
// underneath - that is what stops a late correction from eating a fix.
#ifndef REVIEWPANEL_H
#define REVIEWPANEL_H

#include "../core/SessionController.h"

#include <QHash>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAudioOutput;
class QBuffer;
class QComboBox;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
QT_END_NAMESPACE

class ReviewPanel : public QWidget
{
    Q_OBJECT

public:
    ReviewPanel(SessionController *controller, QWidget *parent = nullptr);

    void setEditorId(const QString &editorId) { m_editorId = editorId; }
    void openSession(const QString &sessionId);
    void refreshSessionList();

signals:
    void statusMessage(const QString &message);

private slots:
    void loadFromInputs();
    void onCellChanged(int row, int column);
    void onCellDoubleClicked(int row, int column);
    void refreshTail();

private:
    void loadWindows(const QString &sessionId, double fromSec);
    void requestWindow(const QString &sessionId, double cursor, double covered, int windowIndex);
    void renderRows();
    void playAround(double centreSec);
    void renameSpeaker(const QString &fromSpeaker, const QString &currentName);

    SessionController *m_controller = nullptr;
    QComboBox *m_picker = nullptr;
    QLineEdit *m_sessionInput = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_audioInfo = nullptr;
    QPushButton *m_playPause = nullptr;
    QTableWidget *m_table = nullptr;
    QTimer *m_tailTimer = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer *m_audioBuffer = nullptr;
    int m_audioSeq = 0;

    QString m_editorId;
    QString m_sessionId;
    QHash<QString, asr::DisplayRow> m_rows;
    QList<asr::DisplayRow> m_ordered;
    QHash<QString, asr::SessionSummary> m_summaries;
    quint64 m_revision = 0;
    bool m_final = false;
    double m_commitBoundarySec = -1.0;
    bool m_populating = false;
    bool m_loading = false;
};

#endif // REVIEWPANEL_H
