#include "AudioCapture.h"

#include "core/Logger.h"
#include "core/RunJournal.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

namespace {

// How often the bound device is checked for still being the right one and
// still delivering.  Two seconds is the reconnect deadline the acceptance
// report asks for: past that, a silent microphone has to be visible.
const int kHealthIntervalMs = 2000;

// How often the device's buffer is drained on top of readyRead.  20 ms is one
// capture buffer, so a missed signal costs nothing.
const int kDrainIntervalMs = 20;

// How much the device may buffer before frames are lost.  200 ms, not the 20
// of the callback cadence: the old size left no headroom at all for a
// scheduling hiccup on a loaded machine, and the frames a full buffer drops
// are gone with nothing anywhere to say so.
const int kDeviceBufferMs = 200;

QAudioFormat makeFormat(const AudioDeviceChoice &choice)
{
    QAudioFormat format;
    format.setSampleRate(choice.sampleRate > 0 ? choice.sampleRate : 48000);
    format.setChannelCount(choice.channels > 0 ? choice.channels : 1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

} // namespace

AudioCapture::AudioCapture(QObject *parent) : QObject(parent)
{
    // Both timers are PARENTED to this object, and that is the whole fix for
    // the watchdog never running.
    //
    // A QTimer declared as a plain member is not a child of the object that
    // holds it, so moveToThread() leaves its affinity on the thread this was
    // constructed on - the GUI thread.  start() then runs on the capture
    // thread and Qt refuses it with "QObject::startTimer: Timers cannot be
    // started from another thread", on stderr, once, at mic open.  The health
    // check therefore never ticked: a microphone unplugged mid-meeting was
    // never noticed, the status stayed "đang ghi", and 6 minutes 33 seconds of
    // a meeting went by with no audio and no warning (measured 2026-09-24).
    m_health.setParent(this);
    m_health.setInterval(kHealthIntervalMs);
    connect(&m_health, &QTimer::timeout, this, &AudioCapture::checkHealth);

    // QAudioSource in pull mode only emits readyRead while its own buffer has
    // something in it, and a buffer that overruns between two signals loses
    // what it held.  Draining on a timer as well as on the signal is what
    // stops that: on the virtual PipeWire source only 47% of the frames
    // played reached the client - 11.4 s of audio out of 24.3 s of recording -
    // while pw-record on the same device captured all of them.
    m_drain.setParent(this);
    m_drain.setInterval(kDrainIntervalMs);
    connect(&m_drain, &QTimer::timeout, this, &AudioCapture::onReadyRead);
}

AudioCapture::~AudioCapture()
{
    teardown();
}

QByteArray AudioCapture::resolveDeviceId(const AudioDeviceChoice &choice, QString *resolvedName,
                                         QString *error)
{
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    if (applog::isEnabled(applog::Level::Debug)) {
        QStringList names;
        for (const QAudioDevice &device : inputs)
            names << device.description();
        LOG_DEBUG(applog::cat::Audio)
            << "the OS reports" << inputs.size() << "capture devices:"
            << names.join(QStringLiteral(" | "));
    }
    if (inputs.isEmpty()) {
        *error = QStringLiteral("Hệ điều hành không báo cáo thiết bị thu âm nào.");
        return QByteArray();
    }

    // An explicitly configured device wins, but only if it is still present
    // AND still answers to the expected name - that pair is the whole point.
    if (!choice.deviceId.isEmpty()) {
        for (const QAudioDevice &device : inputs) {
            if (device.id() != choice.deviceId)
                continue;
            const QString name = device.description();
            if (!choice.expectedName.trimmed().isEmpty()
                && !name.contains(choice.expectedName.trimmed(), Qt::CaseInsensitive)) {
                *error = QStringLiteral(
                             "Thiết bị đã cấu hình không còn đúng microphone "
                             "(cần tên chứa: %1; đang thấy: %2).")
                             .arg(choice.expectedName.trimmed(), name);
                return QByteArray();
            }
            *resolvedName = name;
            return device.id();
        }
        *error = QStringLiteral("Microphone đã cấu hình không còn khả dụng (có thể đã bị rút).");
        return QByteArray();
    }

    // No id pinned: fall back to the name filter, then to the system default.
    // Never silently open an unrelated input when a name was asked for.
    const QString wanted = choice.expectedName.trimmed();
    if (!wanted.isEmpty()) {
        for (const QAudioDevice &device : inputs) {
            if (device.description().contains(wanted, Qt::CaseInsensitive)) {
                *resolvedName = device.description();
                return device.id();
            }
        }
        *error = QStringLiteral("Không tìm thấy microphone có tên chứa \"%1\".").arg(wanted);
        return QByteArray();
    }

    const QAudioDevice fallback = QMediaDevices::defaultAudioInput();
    if (fallback.isNull()) {
        *error = QStringLiteral("Không có thiết bị thu âm mặc định.");
        return QByteArray();
    }
    *resolvedName = fallback.description();
    return fallback.id();
}

void AudioCapture::start(const AudioDeviceChoice &choice)
{
    if (m_running) {
        LOG_WARN(applog::cat::Audio) << "start requested while already capturing on" << m_boundName;
        emit failed(QStringLiteral("microphone đang được sử dụng"));
        return;
    }
    m_choice = choice;
    m_capturedBytes = 0;
    m_lastHealthBytes = 0;

    QString resolvedName;
    QString error;
    const QByteArray id = resolveDeviceId(choice, &resolvedName, &error);
    if (id.isEmpty()) {
        LOG_ERROR(applog::cat::Audio) << "no capture device could be selected:" << error;
        emit failed(error);
        return;
    }
    LOG_INFO(applog::cat::Audio) << "capture device selected:" << resolvedName;

    QAudioDevice selected;
    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        if (device.id() == id) {
            selected = device;
            break;
        }
    }
    if (selected.isNull()) {
        LOG_ERROR(applog::cat::Audio) << "the device disappeared between selection and open";
        emit failed(QStringLiteral("không mở được thiết bị thu âm đã chọn"));
        return;
    }

    QAudioFormat format = makeFormat(choice);
    if (!selected.isFormatSupported(format)) {
        LOG_ERROR(applog::cat::Audio)
            << "the device does not support the requested format:" << format.sampleRate() << "Hz /"
            << format.channelCount() << "ch / Int16";
        // Say what was asked for rather than silently accepting whatever the
        // driver prefers: the server is told the rate/channels we claim to be
        // sending, so a substituted format would corrupt every timestamp.
        emit failed(QStringLiteral("Thiết bị không hỗ trợ %1 Hz / %2 kênh / 16-bit.")
                        .arg(format.sampleRate())
                        .arg(format.channelCount()));
        return;
    }

    m_source = new QAudioSource(selected, format, this);
    m_source->setBufferSize(format.sampleRate() * format.channelCount() * 2 * kDeviceBufferMs
                            / 1000);
    m_io = m_source->start();
    if (!m_io || m_source->error() != QAudio::NoError) {
        const QString reason = m_source ? QStringLiteral("mã lỗi %1").arg(int(m_source->error()))
                                        : QStringLiteral("không tạo được luồng");
        LOG_ERROR(applog::cat::Audio) << "QAudioSource::start() failed -" << reason;
        teardown();
        emit failed(QStringLiteral("Không mở được microphone (%1). Kiểm tra USB mic còn cắm, "
                                   "đúng thiết bị đầu vào và tiến trình khác không giữ "
                                   "thiết bị, rồi bắt đầu phiên mới.")
                        .arg(reason));
        return;
    }
    connect(m_io, &QIODevice::readyRead, this, &AudioCapture::onReadyRead);
    m_boundId = selected.id();
    m_boundName = resolvedName;
    m_running = true;
    m_health.start();
    m_drain.start();
    if (!m_health.isActive()) {
        // Only reachable if this object's timers still belong to another
        // thread - the exact failure this class hit in production.  Saying so
        // beats recording a whole meeting with no watchdog behind it.
        LOG_ERROR(applog::cat::Audio)
            << "the microphone watchdog did not start - AudioCapture's timers are on another "
               "thread; a device that stops delivering will NOT be reported";
    }
    LOG_INFO(applog::cat::Audio)
        << "microphone capturing:" << m_boundName << format.sampleRate() << "Hz /"
        << format.channelCount() << "ch, buffer" << m_source->bufferSize() << "bytes (~"
        << kDeviceBufferMs << "ms), watchdog every" << kHealthIntervalMs << "ms";
    LOG_STEP("mic.open",
             QStringLiteral("mở microphone '%1' · %2 Hz / %3 kênh · bộ đệm %4 ms · bộ canh %5")
                 .arg(m_boundName)
                 .arg(format.sampleRate())
                 .arg(format.channelCount())
                 .arg(kDeviceBufferMs)
                 .arg(m_health.isActive() ? QStringLiteral("đang chạy") : QStringLiteral("KHÔNG CHẠY")));
    emit started(m_boundName);
}

void AudioCapture::stop()
{
    if (!m_running)
        return;
    LOG_INFO(applog::cat::Audio)
        << "closing the microphone" << m_boundName << "-" << m_capturedBytes << "bytes captured";
    // Drain whatever the device already handed us before closing: those are
    // frames the operator spoke before pressing stop, and dropping them would
    // silently truncate the meeting.
    onReadyRead();
    teardown();
}

void AudioCapture::teardown()
{
    m_health.stop();
    m_drain.stop();
    m_running = false;
    if (m_io) {
        disconnect(m_io, nullptr, this, nullptr);
        m_io = nullptr;
    }
    if (m_source) {
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
}

void AudioCapture::onReadyRead()
{
    if (!m_io)
        return;
    const QByteArray data = m_io->readAll();
    if (data.isEmpty())
        return;
    m_capturedBytes += data.size();
    emit chunk(data);
}

void AudioCapture::checkHealth()
{
    if (!m_running || !m_source)
        return;

    if (m_source->error() != QAudio::NoError) {
        const QString reason = QStringLiteral("luồng audio báo lỗi (mã %1)")
                                   .arg(int(m_source->error()));
        LOG_ERROR(applog::cat::Audio) << "health check:" << reason;
        teardown();
        emit deviceLost(reason);
        return;
    }

    bool present = false;
    QString currentName;
    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        if (device.id() == m_boundId) {
            present = true;
            currentName = device.description();
            break;
        }
    }
    if (!present) {
        LOG_ERROR(applog::cat::Audio) << "health check: device" << m_boundName
                                      << "is no longer listed by the OS";
        teardown();
        emit deviceLost(QStringLiteral("microphone đã bị rút hoặc không còn khả dụng"));
        return;
    }
    const QString wanted = m_choice.expectedName.trimmed();
    if (!wanted.isEmpty() && !currentName.contains(wanted, Qt::CaseInsensitive)) {
        LOG_ERROR(applog::cat::Audio) << "health check: endpoint renamed" << m_boundName << "->"
                                      << currentName << "(must contain" << wanted << ")";
        teardown();
        emit deviceLost(QStringLiteral("thiết bị đổi tên thành \"%1\" - không còn là microphone đã chọn")
                            .arg(currentName));
        return;
    }

    // A stale endpoint can outlive the hardware and keep the stream
    // "running" while delivering nothing.  During active capture, bytes must
    // advance between two health ticks.
    if (m_capturedBytes <= m_lastHealthBytes) {
        LOG_ERROR(applog::cat::Audio)
            << "health check: no new bytes in 2 s (stuck at" << m_capturedBytes
            << "bytes) - the endpoint is stale";
        LOG_STEP("mic.silent",
                 QStringLiteral("microphone '%1' CÒN TRONG DANH SÁCH nhưng không ra dữ liệu quá "
                                "%2 s (đứng ở %3 byte) - coi như mất thiết bị")
                     .arg(m_boundName)
                     .arg(kHealthIntervalMs / 1000)
                     .arg(m_capturedBytes));
        teardown();
        emit deviceLost(QStringLiteral("luồng audio của microphone không còn dữ liệu"));
        return;
    }
    LOG_TRACE(applog::cat::Audio) << "health check OK -" << (m_capturedBytes - m_lastHealthBytes)
                                  << "bytes in 2 s";
    m_lastHealthBytes = m_capturedBytes;
}
