// Phụ đề trực tiếp: play a recording and watch the transcript appear on it.
//
// This is the demo flow end to end - microphone or file in, gRPC to the Server
// buffer, Riva or Triton behind that, text back - with the one thing the main
// window does not do: the text is placed against a **playback clock** instead
// of being appended as it arrives.
//
// That distinction is the whole design.  Subtitles chosen by arrival order
// drift the moment the pipeline is faster or slower than real time, and it is
// always one or the other.  Every word the server returns carries start_sec and
// end_sec, so the right question is not "what came back last" but "what was
// being said at the position the player is at now".  Push the audio as fast as
// the link allows and the subtitles still land on the right frames.
//
// Two panes, because the request asked for either and they answer different
// questions: an overlay on the picture for watching, and a running text pane
// beside it for reading back.
#ifndef SUBTITLEWINDOW_H
#define SUBTITLEWINDOW_H

#include "core/SessionTypes.h"
#include "core/TranscriptModel.h"

#include <QList>
#include <QString>
#include <QWidget>

class AppConfig;
class SessionController;

QT_BEGIN_NAMESPACE
class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QVideoWidget;
QT_END_NAMESPACE

// The translucent caption drawn over the picture.  A widget of its own rather
// than a styled QLabel because it has to size itself to the text and stay
// pinned to the bottom of whatever it is covering.
class SubtitleOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit SubtitleOverlay(QWidget *parent = nullptr);

    // `settled` is what the pipeline has committed, `moving` is the interim
    // edge.  They are drawn differently: the edge is the part that is still
    // allowed to change, and showing it as though it were final is how a demo
    // ends up looking like it made a mistake it later "corrected".
    void setText(const QString &settled, const QString &moving);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_settled;
    QString m_moving;
};

class SubtitleWindow : public QWidget
{
    Q_OBJECT

public:
    SubtitleWindow(SessionController *controller, AppConfig *config, QWidget *parent = nullptr);
    ~SubtitleWindow() override;

    // Starts the demo on `path` without going through the file dialog.  Public
    // because the only way to verify a widget here is to drive it - see the
    // throwaway-driver recipe in the handover notes - and a modal dialog is
    // exactly what a driver cannot get past.
    void loadFile(const QString &path);

private slots:
    void openFile();
    void startMicrophone();
    void stopEverything();
    void onModelUpdated();
    void onPlayerPosition(qint64 ms);
    void onSeek(int value);

protected:
    // The caption is a child of the video surface, so it has to be told when
    // that surface changes size - a child does not resize with its parent.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Mode { Idle, File, Microphone };

    void setMode(Mode mode);
    void refreshStatus();
    // Chooses the caption for `atSec` and repaints both panes.
    void renderAt(double atSec);
    // Every word the model holds, flattened and sorted by time.  Rebuilt on
    // each update: the row list is small and re-sorting it is far cheaper than
    // reasoning about which rows a correction rewrote.
    void rebuildWords();

    SessionController *m_controller = nullptr;
    AppConfig *m_config = nullptr;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QVideoWidget *m_video = nullptr;
    QStackedWidget *m_stage = nullptr;
    QLabel *m_audioOnly = nullptr;
    SubtitleOverlay *m_overlay = nullptr;
    QPlainTextEdit *m_transcript = nullptr;
    QSlider *m_seek = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_clock = nullptr;
    QPushButton *m_fileButton = nullptr;
    QPushButton *m_micButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    Mode m_mode = Mode::Idle;
    QString m_sourceName;
    double m_durationSec = 0.0;
    // Sorted by startSec.  The caption search is a scan over this.
    QList<WordItem> m_words;
    QString m_movingText;
    // Set while the operator is dragging the seek bar, so the player's own
    // position updates do not fight the handle.
    bool m_seeking = false;
};

#endif // SUBTITLEWINDOW_H
