// One rule, in one place: the inference tier is fed 16 kHz mono and nothing
// else.
//
// `asr_diar_session` takes a bare float tensor with no sample rate beside it,
// so it treats whatever it is handed as 16 kHz.  Feeding it 48 kHz does not
// fail - it produces fluent-sounding nonsense, three times too slow, and the
// stored audio plays back at a third speed while the database says 16000.
// That was measured on 2026-09-24: the same 20 s of speech gave 58 words at
// 16 kHz and 2 words at 48 kHz.
//
// The reference adapter normalises every packet the same way before it reaches
// Triton (`_normalize_http_audio` in realtime_ui.py) and archives the
// normalised audio, not the original - so the archive and the transcript are
// always describing the same samples.  This is that function.
#ifndef PCM16K_H
#define PCM16K_H

#include <QByteArray>
#include <QList>

namespace audio {

// Target format of everything downstream of push_audio.
constexpr int kPipelineSampleRate = 16000;

// Interleaved signed 16-bit little-endian in, 16 kHz mono signed 16-bit
// little-endian out.  Channels are averaged and the rate is converted by
// linear interpolation, which is what the adapter does (`np.interp`) - matching
// it matters more here than a better filter would, because a difference in
// resampling shows up as a difference in the transcript.
//
// Returns `pcm` unchanged when it is already 16 kHz mono, so the common path
// copies nothing.
QByteArray toPipelineFormat(const QByteArray &pcm, int sampleRate, int channels);

} // namespace audio

#endif // PCM16K_H
