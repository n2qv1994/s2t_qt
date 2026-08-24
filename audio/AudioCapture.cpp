#include "AudioCapture.h"

#include "../core/Logger.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>

namespace {

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
    m_health.setInterval(2000);
    connect(&m_health, &QTimer::timeout, this, &AudioCapture::checkHealth);
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
    // ~20 ms of audio per buffer, the same callback cadence the PortAudio
    // client used before the 160 ms transport packet is assembled upstream.
    m_source->setBufferSize(format.sampleRate() * format.channelCount() * 2 * 20 / 1000);
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
    LOG_INFO(applog::cat::Audio)
        << "microphone capturing:" << m_boundName << format.sampleRate() << "Hz /"
        << format.channelCount() << "ch, buffer" << m_source->bufferSize() << "bytes (~20ms)";
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
        teardown();
        emit deviceLost(QStringLiteral("luồng audio của microphone không còn dữ liệu"));
        return;
    }
    LOG_TRACE(applog::cat::Audio) << "health check OK -" << (m_capturedBytes - m_lastHealthBytes)
                                  << "bytes in 2 s";
    m_lastHealthBytes = m_capturedBytes;
}
