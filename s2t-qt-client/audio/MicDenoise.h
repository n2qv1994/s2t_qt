// Hardware denoise control for the reSpeaker XVF3800 USB array, plus the
// A/B evidence recorder that goes with it.
//
// There is no software AEC/AGC anywhere in this product - the only real
// signal-conditioning knob is this hardware toggle, so comparing it off/on is
// the honest thing to capture.  Fixed commands and values only: nothing here
// can invoke an arbitrary host-control command, and SAVE_CONFIGURATION is
// never called, so the change is temporary by construction.
#ifndef MICDENOISE_H
#define MICDENOISE_H

#include "AudioCapture.h"

#include <QByteArray>
#include <QObject>
#include <QString>

namespace mic {

enum class DenoiseState { Unknown, Enabled, Disabled };

QString denoiseStateKey(DenoiseState state);

// Runs PP_MIN_NS/PP_MIN_NN through the xvf3800 host-control tool
// (xvf_host.exe on Windows, xvf_host on Linux).  Returns false and fills
// *error on a missing executable, a non-zero exit, or a timeout.
bool setDenoise(const QString &appPath, bool enabled, QString *output, QString *error);

// Probes the device directly: the host can retain a stale audio endpoint
// after the USB array is gone, and a zero exit code alone is not proof - the
// tool prints "Found device" only when it really opened one.
bool probeDevicePresent(const QString &appPath, QString *error);

struct DenoiseAbResult
{
    QByteArray offWav;
    QByteArray onWav;
    // Honest reporting of whether the pre-capture state could be put back.
    // "restored" reflects whether the restore command actually SUCCEEDED, not
    // merely whether it was attempted - claiming a restore that did not
    // happen is exactly the kind of wrong evidence this dashboard exists to
    // avoid.
    bool restoreAttempted = false;
    bool restored = false;
    QString priorState;
    QString finalState;
    QString restoreError;
    QString error;
};

// Records two short clips back to back, denoise off then on, so an operator
// can hear the difference instead of trusting a state label.  Lives on the
// capture thread and is driven by timers, never by sleeping, so the audio
// event loop keeps running underneath it.
class DenoiseAbRecorder : public QObject
{
    Q_OBJECT

public:
    explicit DenoiseAbRecorder(QObject *parent = nullptr);

public slots:
    void start(const AudioDeviceChoice &choice, const QString &appPath, double seconds,
               int priorState);

signals:
    void completed(const mic::DenoiseAbResult &result);

private:
    void beginClip(bool denoiseEnabled);
    void finishClip();
    void fail(const QString &message);
    void restoreAndEmit();

    AudioCapture *m_capture = nullptr;
    AudioDeviceChoice m_choice;
    QString m_appPath;
    double m_seconds = 3.0;
    DenoiseState m_prior = DenoiseState::Unknown;
    DenoiseAbResult m_result;
    QByteArray m_buffer;
    bool m_capturingOn = false;
    bool m_busy = false;
};

} // namespace mic

Q_DECLARE_METATYPE(mic::DenoiseAbResult)

#endif // MICDENOISE_H
