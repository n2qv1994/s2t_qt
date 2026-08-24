#include "AudioQueue.h"

#include <QMutexLocker>

void AudioQueue::configure(qint64 capacityBytes)
{
    QMutexLocker lock(&m_mutex);
    m_capacity = capacityBytes > 0 ? capacityBytes : 0;
    m_buffer.clear();
    m_paused = false;
}

bool AudioQueue::push(const QByteArray &pcm)
{
    if (pcm.isEmpty())
        return true;
    QMutexLocker lock(&m_mutex);
    // Discarding here rather than at send time is what makes Resume continue
    // the same meeting without a block of paused speech appearing after it.
    if (m_paused)
        return true;
    if (m_capacity > 0 && m_buffer.size() + pcm.size() > m_capacity)
        return false;
    m_buffer.append(pcm);
    m_notEmpty.wakeAll();
    return true;
}

QByteArray AudioQueue::take(int timeoutMs)
{
    QMutexLocker lock(&m_mutex);
    if (m_buffer.isEmpty())
        m_notEmpty.wait(&m_mutex, QDeadlineTimer(timeoutMs));
    QByteArray out;
    out.swap(m_buffer);
    return out;
}

QByteArray AudioQueue::takeAllNow()
{
    QMutexLocker lock(&m_mutex);
    QByteArray out;
    out.swap(m_buffer);
    return out;
}

qint64 AudioQueue::pendingBytes() const
{
    QMutexLocker lock(&m_mutex);
    return m_buffer.size();
}

void AudioQueue::clear()
{
    QMutexLocker lock(&m_mutex);
    m_buffer.clear();
}

void AudioQueue::wake()
{
    QMutexLocker lock(&m_mutex);
    m_notEmpty.wakeAll();
}

void AudioQueue::setPaused(bool paused)
{
    QMutexLocker lock(&m_mutex);
    m_paused = paused;
    if (paused) {
        // Drop the partial packet too, not just future callbacks: a chunk
        // that raced the pause click must not be delivered after resume.
        m_buffer.clear();
    }
}

bool AudioQueue::isPaused() const
{
    QMutexLocker lock(&m_mutex);
    return m_paused;
}
