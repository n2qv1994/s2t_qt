// Acceptance / evidence dashboard.
//
// Deliberately a separate window from the live view, the same way the old
// /evidence page was separate from /: it exists to answer "prove it", and the
// answers are read live rather than labelled.  Model state comes from
// Triton's own repository index, queue depth from the running session, and
// the denoise comparison is two real recordings rather than a state label.
#ifndef EVIDENCEWINDOW_H
#define EVIDENCEWINDOW_H

#include "../core/SessionController.h"

#include <QWidget>

#include <functional>

QT_BEGIN_NAMESPACE
class QAudioOutput;
class QBuffer;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPushButton;
class QTableWidget;
class QTimer;
QT_END_NAMESPACE

// Min/max envelope per pixel column - real waveform evidence, not just a
// player to press play on.
class WaveformWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WaveformWidget(QWidget *parent = nullptr);
    void setWav(const QByteArray &wavBytes);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QList<QPair<float, float>> m_envelope;
};

class EvidenceWindow : public QWidget
{
    Q_OBJECT

public:
    EvidenceWindow(SessionController *controller, QWidget *parent = nullptr);

    void setSession(const QString &sessionId);

private slots:
    void refreshDevice();
    void refreshModels();
    void refreshLatency();
    void loadSessionEvidence();
    void startDenoiseAb();
    void onDenoiseAbReady(const mic::DenoiseAbResult &result);

private:
    void countStage(const QString &sessionId, const QString &stage, int generation,
                    std::function<void(const QList<asr::PipelineTraceEvent> &)> done);

    SessionController *m_controller = nullptr;
    QLineEdit *m_session = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_architecture = nullptr;
    QTableWidget *m_models = nullptr;
    QLabel *m_modelSummary = nullptr;
    QLabel *m_device = nullptr;
    QTableWidget *m_latency = nullptr;
    QLabel *m_vad = nullptr;
    QLabel *m_campp = nullptr;
    QTableWidget *m_speakers = nullptr;

    QPushButton *m_denoiseButton = nullptr;
    QLabel *m_denoiseStatus = nullptr;
    QLabel *m_denoiseRestore = nullptr;
    WaveformWidget *m_waveOff = nullptr;
    WaveformWidget *m_waveOn = nullptr;
    QPushButton *m_playOff = nullptr;
    QPushButton *m_playOn = nullptr;
    QByteArray m_offWav;
    QByteArray m_onWav;

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QBuffer *m_buffer = nullptr;

    QTimer *m_deviceTimer = nullptr;
    QTimer *m_modelTimer = nullptr;
    int m_generation = 0;
};

#endif // EVIDENCEWINDOW_H
