// Opt-in per-stage pipeline trace: what each stage received and what it did
// with it, paged by the immutable event seq.
//
// Two modes, deliberately separate.  Realtime keeps only the most recent
// cards, so a long session cannot grow the list without bound; history pages
// backwards through older events on demand without that trim, so browsing the
// past does not fight with staying bounded live.
#ifndef TRACEWINDOW_H
#define TRACEWINDOW_H

#include "core/SessionController.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QAudioOutput;
class QBuffer;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMediaPlayer;
class QPushButton;
class QSpinBox;
class QTimer;
QT_END_NAMESPACE

class TraceWindow : public QWidget
{
    Q_OBJECT

public:
    TraceWindow(SessionController *controller, QWidget *parent = nullptr);

    void setSession(const QString &sessionId);

private slots:
    void reload();
    void poll();
    void loadHistoryPage();
    void playSelectedRange();
    void stitchSelectedSpans();
    void syncMode();

private:
    void appendEvents(const QList<asr::PipelineTraceEvent> &events);
    void playWav(const QByteArray &wavBytes, const QString &caption);

    SessionController *m_controller = nullptr;
    QLineEdit *m_session = nullptr;
    QComboBox *m_stage = nullptr;
    QCheckBox *m_follow = nullptr;
    QSpinBox *m_afterSeq = nullptr;
    QPushButton *m_loadPage = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_cards = nullptr;
    QTimer *m_timer = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer *m_buffer = nullptr;

    quint64 m_afterLive = 0;
    // Bumped whenever the session, stage or mode changes; an in-flight reply
    // whose generation no longer matches is dropped instead of being appended
    // to a list it no longer belongs to.
    int m_generation = 0;
    bool m_busyLive = false;
    bool m_busyHistory = false;
};

#endif // TRACEWINDOW_H
