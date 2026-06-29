/* selftest.c — host self-check for the audio lib's WAV parse + resampler.
 *
 * Build + run on the build host (NOT in Aegis):
 *     make -C user/lib/audio selftest && ./user/lib/audio/selftest
 *
 * Writes synthetic inputs to /tmp and plays them through audio_play_*()
 * with a regular file as the "sink", then asserts the output is 48 kHz
 * stereo of the expected length. Exercises the resampler (the novel
 * logic); minimp3 itself is trusted upstream. If $1 is a real .mp3 it is
 * decoded too (no encoder on the build host, so MP3 is opt-in).
 */
#include "audio.h"
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void le32(unsigned char *p, unsigned v)
{ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void le16(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; }

/* Write a `secs`-second 16-bit mono WAV at `rate` Hz (a quiet sine). */
static void make_wav(const char *path, int rate, int secs)
{
    int frames = rate * secs;
    int data = frames * 2;                 /* mono 16-bit */
    unsigned char h[44];
    memcpy(h, "RIFF", 4);      le32(h+4, 36 + data);
    memcpy(h+8, "WAVE", 4);
    memcpy(h+12, "fmt ", 4);   le32(h+16, 16);
    le16(h+20, 1); le16(h+22, 1);          /* PCM, mono */
    le32(h+24, rate); le32(h+28, rate*2);
    le16(h+32, 2); le16(h+34, 16);
    memcpy(h+36, "data", 4);   le32(h+40, data);
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    assert(fd >= 0);
    write(fd, h, 44);
    for (int i = 0; i < frames; i++) {
        int ph = i % (rate / 220 + 1);
        int16_t s = (int16_t)((ph & 1) ? 4000 : -4000);
        write(fd, &s, 2);
    }
    close(fd);
}

static long play_to_file(int (*fn)(int,const char*), const char *in, const char *out)
{
    int fd = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    assert(fd >= 0);
    int rc = fn(fd, in);
    close(fd);
    if (rc != 0) return -1;
    struct stat st; stat(out, &st);
    return st.st_size;
}

int main(int argc, char **argv)
{
    /* 44.1 kHz mono → expect 48 kHz stereo, length scaled by 48000/44100. */
    make_wav("/tmp/at_44k.wav", 44100, 2);
    long b = play_to_file(audio_play_wav, "/tmp/at_44k.wav", "/tmp/at_44k.raw");
    long want_frames = 2L * 48000;                  /* 2 s @ 48k */
    long got_frames = b / 4;                        /* stereo 16-bit */
    long diff = got_frames - want_frames;
    if (diff < 0) diff = -diff;
    printf("44.1k→48k: %ld frames (want ~%ld, diff %ld)\n",
           got_frames, want_frames, diff);
    assert(b > 0 && diff < 48000 / 10);             /* within ~0.1 s */

    /* 48 kHz mono → 1:1 frames, mono expanded to stereo. */
    make_wav("/tmp/at_48k.wav", 48000, 1);
    b = play_to_file(audio_play_wav, "/tmp/at_48k.wav", "/tmp/at_48k.raw");
    printf("48k passthrough: %ld frames (want 48000)\n", b / 4);
    assert(b > 0 && (b / 4) >= 48000 - 2 && (b / 4) <= 48000 + 2);

    /* Real startup.wav, if present alongside. */
    if (access("rootfs/usr/share/startup.wav", R_OK) == 0) {
        b = play_to_file(audio_play_file, "rootfs/usr/share/startup.wav",
                         "/tmp/startup.raw");
        printf("startup.wav: %ld bytes -> %s\n", b, b > 0 ? "OK" : "FAIL");
        assert(b > 0);
    }

    /* Opt-in MP3 (build host has no encoder): pass a real .mp3 as argv[1]. */
    if (argc > 1) {
        b = play_to_file(audio_play_mp3, argv[1], "/tmp/mp3.raw");
        printf("mp3 %s: %ld bytes (%ld frames @48k = %.2fs) -> %s\n",
               argv[1], b, b/4, (b/4)/48000.0, b > 0 ? "OK" : "FAIL");
        assert(b > 0);
    }

    printf("audio selftest: PASS\n");
    return 0;
}
