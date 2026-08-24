// Decoding for the container formats that are not plain PCM WAV.
//
// Everything downstream of the file picker understands only 16-bit PCM, so an
// .m4a/AAC recording - which the deployed UI accepted - has to be decoded
// first.  ffmpeg is the only thing that understands AAC here, and it is
// optional: when it is absent the failure says so plainly instead of
// presenting a corrupt-file error for a perfectly good recording.
#ifndef TRANSCODE_H
#define TRANSCODE_H

#include <QString>

namespace audio {

// Returns the path of a 16 kHz mono 16-bit PCM WAV, decoding `source` into a
// temporary file when it is not already usable.  Returns an empty string and
// fills *error on failure.  When the returned path differs from `source`, the
// caller owns the temporary and should delete it once the session has read it.
QString ensurePcmWav(const QString &source, QString *error);

bool ffmpegAvailable();

} // namespace audio

#endif // TRANSCODE_H
