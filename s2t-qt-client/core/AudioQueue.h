// Bounded hand-off between the capture thread and the gRPC sender thread.
//
// Bounded on purpose: the point is to stop loudly rather than silently delete
// audio when the server cannot keep up.  Pause discards at push time (not at
// send time) so speech spoken while paused is never delivered later as if it
// had been live - the same rule the bridge enforced by clearing its queue on
// every pause tick.
#ifndef AUDIOQUEUE_H
#define AUDIOQUEUE_H

#include <QByteArray>
#include <QMutex>
#include <QWaitCondition>

class AudioQueue
{
public:
    void configure(qint64 capacityBytes);

    // Returns false when the queue is full; the caller counts that as a
    // dropped chunk and the session ends rather than continuing with a hole.
    bool push(const QByteArray &pcm);

    // Blocks up to timeoutMs for data.  Returns everything buffered, which
    // the sender then splits into fixed transport packets.
    QByteArray take(int timeoutMs);
    QByteArray takeAllNow();

    qint64 pendingBytes() const;
    void clear();
    // Wakes any blocked take() so a stopping sender does not sit out its
    // timeout after the capture side has already gone away.
    void wake();

    void setPaused(bool paused);
    bool isPaused() const;

private:
    mutable QMutex m_mutex;
    QWaitCondition m_notEmpty;
    QByteArray m_buffer;
    qint64 m_capacity = 0;
    bool m_paused = false;
};

#endif // AUDIOQUEUE_H
