/* audio.h — minimal audio file playback for Aegis userspace.
 *
 * Decodes an audio file and streams it to a PCM sink fd that expects
 * 48 kHz / 16-bit / stereo (the HDA driver's fixed format, e.g.
 * /dev/audio). Source sample rates are linearly resampled to 48 kHz and
 * mono is expanded to stereo, so any standard WAV/MP3 plays at correct
 * pitch.
 *
 * Supported: WAV (16-bit PCM, any rate, mono/stereo) and MP3 (via the
 * vendored public-domain minimp3 decoder). Returns 0 on success, -1 on a
 * bad/unsupported file or a sink error.
 *
 * The caller owns the sink fd and must close it afterwards — closing
 * /dev/audio is what flushes the DMA and triggers playback.
 */
#include <sys/types.h>

#ifndef LIBAUDIO_H
#define LIBAUDIO_H

/* Dispatch on the filename extension (.mp3 → MP3, else WAV). */
int audio_play_file(int audio_fd, const char *path);

int audio_play_wav(int audio_fd, const char *path);
int audio_play_mp3(int audio_fd, const char *path);

/* Estimate the track length in milliseconds (0 if unknown). WAV is exact from
 * the header; MP3 is estimated from file size and the first frame's bitrate. */
int audio_duration_ms(const char *path);

/* Play `path` to /dev/audio in a forked child and return its pid (so the
 * caller can kill it to stop, or waitpid it). Returns -1 on fork failure.
 * /dev/audio streams with backpressure, so the child lives for the track's
 * duration — this keeps the caller's UI responsive. */
pid_t audio_play_file_async(const char *path);

#endif /* LIBAUDIO_H */
