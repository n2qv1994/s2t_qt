// 16-bit PCM WAV reading and writing.
//
// The client only ever handles uncompressed PCM: enrollment recordings go out
// as WAV bytes over EnrollSpeaker, get_audio_range comes back as raw PCM that
// has to be wrapped before QMediaPlayer will touch it, and file replay reads
// a meeting recording back off disk.  Anything else (m4a/AAC) is handed to
// ffmpeg first - see Transcode.
#ifndef WAVIO_H
#define WAVIO_H

#include <QByteArray>
#include <QString>

namespace wav
{

struct Pcm
{
    QByteArray frames;      // interleaved little-endian int16
    int sampleRate = 0;
    int channels = 0;
    bool isValid() const { return sampleRate > 0 && channels > 0; }
    double durationSec() const
    {
        if (!isValid())
            return 0.0;
        return double(frames.size()) / double(sampleRate * channels * 2);
    }
};

// Parses a RIFF/WAVE container.  Returns an invalid Pcm and fills *error for
// anything that is not 16-bit PCM, because every consumer downstream of this
// (enrollment, replay, playback) assumes exactly that.
Pcm readWav(const QString &path, QString *error);
Pcm parseWav(const QByteArray &bytes, QString *error);

QByteArray buildWav(const QByteArray &frames, int sampleRate, int channels);
bool writeWav(const QString &path, const QByteArray &frames, int sampleRate, int channels,
              QString *error);

// Downmix to mono and drop to 16 kHz.  The server does this itself for
// push_audio, so it is only used where the client has to hand over finished
// audio: enrollment WAVs, which the CAM++ pipeline wants at its own rate.
QByteArray toMono16k(const QByteArray &frames, int sampleRate, int channels);

} // namespace wav

#endif // WAVIO_H
