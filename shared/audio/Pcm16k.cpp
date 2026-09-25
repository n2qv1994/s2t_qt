#include "Pcm16k.h"

#include <QtGlobal>

#include <cmath>

namespace audio {

QByteArray toPipelineFormat(const QByteArray &pcm, int sampleRate, int channels)
{
    const int rate = sampleRate > 0 ? sampleRate : kPipelineSampleRate;
    const int chans = channels > 0 ? channels : 1;
    if (rate == kPipelineSampleRate && chans == 1)
        return pcm;
    if (pcm.isEmpty())
        return pcm;

    const auto *in = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype samples = pcm.size() / 2;
    const qsizetype frames = samples / chans;
    if (frames <= 0)
        return QByteArray();

    // Down-mix first, then resample: averaging after the interpolation would
    // interpolate between two different speakers' channels.
    QList<float> mono;
    mono.reserve(int(frames));
    for (qsizetype frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (int channel = 0; channel < chans; ++channel)
            sum += float(in[frame * chans + channel]);
        mono.append(sum / float(chans));
    }

    if (rate == kPipelineSampleRate) {
        QByteArray out(int(frames) * 2, Qt::Uninitialized);
        auto *dst = reinterpret_cast<qint16 *>(out.data());
        for (qsizetype frame = 0; frame < frames; ++frame)
            dst[frame] = qint16(qBound(-32768.0f, std::round(mono.at(int(frame))), 32767.0f));
        return out;
    }

    // Same arithmetic as np.interp over a linear position ramp: output length
    // is round(n * 16000 / rate), and position i maps back to i * rate / 16000
    // in the input.  A packet shorter than one output sample still yields one,
    // so no audio is silently dropped at a packet boundary.
    const qsizetype outFrames =
        qMax<qsizetype>(1, qsizetype(std::llround(double(frames) * double(kPipelineSampleRate)
                                                 / double(rate))));
    QByteArray out(int(outFrames) * 2, Qt::Uninitialized);
    auto *dst = reinterpret_cast<qint16 *>(out.data());
    const double step = double(rate) / double(kPipelineSampleRate);
    for (qsizetype i = 0; i < outFrames; ++i) {
        const double position = double(i) * step;
        const qsizetype left = qsizetype(position);
        const double fraction = position - double(left);
        const float a = mono.at(int(qBound<qsizetype>(0, left, frames - 1)));
        const float b = mono.at(int(qBound<qsizetype>(0, left + 1, frames - 1)));
        const double value = double(a) + (double(b) - double(a)) * fraction;
        dst[i] = qint16(qBound(-32768.0, std::round(value), 32767.0));
    }
    return out;
}

} // namespace audio
