// Microphone capture, and the device-identity guard that goes with it.
//
// An audio endpoint can be re-used by the host after a USB microphone is
// unplugged, so "the stream is still open" is not evidence that the right
// microphone is still attached - the bridge this replaces learned that the
// hard way and checked the bound device name on a timer.  This keeps that:
// name check, presence check, and a callback-progress check, because a stale
// endpoint can survive the audio interface disappearing and quietly deliver
// silence.  Both back ends do this: WASAPI re-uses endpoint ids, and a
// PipeWire/PulseAudio source keeps its node name across a re-plug.
//
// Lives in its own thread with an event loop (QAudioSource needs one).
#ifndef AUDIOCAPTURE_H
#define AUDIOCAPTURE_H

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

QT_BEGIN_NAMESPACE
class QAudioSource;
class QIODevice;
QT_END_NAMESPACE

struct AudioDeviceChoice
{
    QByteArray deviceId;  // QAudioDevice::id() of the configured input
    QString expectedName; // substring the bound device description must contain
    int sampleRate = 48000;
    int channels = 1;
};

class AudioCapture : public QObject
{
    Q_OBJECT

public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    // Resolves a configured choice to a real input, or returns an invalid id.
    // Exposed so the settings dialog and the pre-flight check use exactly the
    // same matching rule the capture itself will.
    static QByteArray resolveDeviceId(const AudioDeviceChoice &choice, QString *resolvedName,
                                      QString *error);

public slots:
    void start(const AudioDeviceChoice &choice);
    void stop();

signals:
    void started(const QString &deviceName);
    void failed(const QString &message);
    // Interleaved little-endian int16, at the configured rate and channel
    // count.  Packetisation into the 160 ms transport unit happens upstream.
    void chunk(const QByteArray &pcm);
    // The device stopped being the one we opened, or stopped delivering.  The
    // session stays alive; the controller decides whether to wait for it back.
    void deviceLost(const QString &reason);

private slots:
    void onReadyRead();
    void checkHealth();

private:
    void teardown();

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QTimer m_health;
    AudioDeviceChoice m_choice;
    QByteArray m_boundId;
    QString m_boundName;
    qint64 m_capturedBytes = 0;
    qint64 m_lastHealthBytes = 0;
    bool m_running = false;
};

Q_DECLARE_METATYPE(AudioDeviceChoice)

#endif // AUDIOCAPTURE_H
