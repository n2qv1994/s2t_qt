// Decoding any media file the platform can read, using the FFmpeg that Qt
// Multimedia already ships with.
//
// audio/Transcode.h does the same job by shelling out to an ffmpeg binary, and
// that is still the path a plain replay takes: it is older, it is proven, and
// it costs nothing when the file is already PCM WAV.  This exists for the
// subtitle demo, where the file may be a **video** - an .mp4 whose audio track
// has to reach the pipeline while QMediaPlayer draws the picture - and where
// requiring an ffmpeg binary on an operator's workstation would be a new
// deployment dependency for a feature that is meant to be a demonstration.
//
// QAudioDecoder is the same decoder QMediaPlayer uses, so a file that plays is
// a file that decodes.  That equivalence is the point: the demo cannot end up
// showing a picture it has no transcript for.
#ifndef MEDIADECODE_H
#define MEDIADECODE_H

#include "WavIo.h"

#include <QString>

namespace audio {

// Decodes the whole file to interleaved 16-bit PCM.  Blocking: it spins a
// local event loop until the decoder finishes, because every caller here has a
// file already on disk and nothing useful to do until it is read.
//
// `sampleRate` and `channels` are what the result is resampled to.  **16 kHz
// mono, and that is not a preference.**  asr_diar_session takes a bare float
// tensor with no rate alongside it, so it decodes whatever it is handed as
// though it were 16 kHz - hand it 48 kHz and the transcript comes back as
// fluent-looking nonsense rather than as an error.  audio/Transcode.h's
// ensurePcmWav has always produced 16 kHz mono for exactly this reason.
//
// Returns an invalid Pcm and fills *error on failure - including the case that
// matters most in practice, a video file with no audio track.
wav::Pcm decodeMedia(const QString &path, QString *error, int sampleRate = 16000,
                     int channels = 1, int timeoutMs = 120000);

// True when the file has a video track worth showing.  The demo uses it to
// decide between a video surface and a plain waveform-less audio player, so an
// .mp3 does not open a black rectangle.
bool hasVideoTrack(const QString &path);

} // namespace audio

#endif // MEDIADECODE_H
