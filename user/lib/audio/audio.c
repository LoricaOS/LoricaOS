/* audio.c — WAV/MP3 playback to a 48 kHz/16-bit/stereo PCM sink. See audio.h.
 *
 * Extracted from bastion's startup-sound code and generalized: it now reads
 * the WAV sample rate (bastion assumed 48 kHz) and resamples anything that
 * isn't 48 kHz, and it gained MP3 support via minimp3. The resampler is a
 * streaming linear interpolator with state carried across input chunks.
 */
#define _GNU_SOURCE
#include "audio.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD              /* portable scalar build, plenty fast for playback */
#include "minimp3.h"

#define SINK_RATE   48000
#define MP3_MAX     (32 * 1024 * 1024)   /* refuse absurd files */

/* ── Streaming linear resampler → 48 kHz stereo ──────────────────────────── */

typedef struct {
    int      fd;
    int      channels;          /* source channels (1 or 2) */
    uint32_t step;              /* input frames per output frame, Q16.16 */
    uint32_t acc;               /* fractional position prev→cur, Q16.16 */
    int16_t  pl, pr;            /* previous input frame (stereo) */
    int      primed;            /* pl/pr valid */
    int16_t  out[512 * 2];      /* staged stereo output */
    int      n;                 /* frames staged */
} resamp_t;

static void rs_init(resamp_t *r, int fd, int src_hz, int channels)
{
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->channels = (channels == 1) ? 1 : 2;
    if (src_hz <= 0) src_hz = SINK_RATE;
    r->step = (uint32_t)(((uint64_t)src_hz << 16) / SINK_RATE);
}

static void rs_flush(resamp_t *r)
{
    if (r->n > 0) {
        (void)write(r->fd, r->out, (size_t)r->n * 4);
        r->n = 0;
    }
}

/* Emit one output frame: linear-interpolate prev→cur at fractional pos acc. */
static void rs_emit(resamp_t *r, int cl, int cr)
{
    r->out[r->n * 2]     = (int16_t)(r->pl + (int)(((long long)(cl - r->pl) * r->acc) >> 16));
    r->out[r->n * 2 + 1] = (int16_t)(r->pr + (int)(((long long)(cr - r->pr) * r->acc) >> 16));
    if (++r->n == 512) rs_flush(r);
}

/* Push `frames` interleaved 16-bit input frames. */
static void rs_push(resamp_t *r, const int16_t *pcm, int frames)
{
    for (int i = 0; i < frames; i++) {
        int16_t cl, cr;
        if (r->channels == 1) {
            cl = cr = pcm[i];
        } else {
            cl = pcm[i * 2];
            cr = pcm[i * 2 + 1];
        }
        if (!r->primed) { r->pl = cl; r->pr = cr; r->primed = 1; continue; }
        /* Emit every output frame whose source position lies in [prev, cur). */
        while (r->acc < 0x10000u) {
            rs_emit(r, cl, cr);
            r->acc += r->step;
        }
        r->acc -= 0x10000u;
        r->pl = cl; r->pr = cr;
    }
}

/* ── WAV ─────────────────────────────────────────────────────────────────── */

static void skip_bytes(int fd, long n)
{
    unsigned char tmp[256];
    while (n > 0) {
        int want = n > (long)sizeof(tmp) ? (int)sizeof(tmp) : (int)n;
        int got = (int)read(fd, tmp, want);
        if (got <= 0) break;
        n -= got;
    }
}

int audio_play_wav(int fd, const char *path)
{
    int wf = open(path, O_RDONLY);
    if (wf < 0) return -1;

    unsigned char id[12];
    if (read(wf, id, 12) != 12 ||
        memcmp(id, "RIFF", 4) != 0 || memcmp(id + 8, "WAVE", 4) != 0) {
        close(wf);
        return -1;
    }

    int channels = 2, bits = 16, rate = SINK_RATE;
    long data_size = -1;
    for (;;) {
        unsigned char ch[8];
        if (read(wf, ch, 8) != 8) break;
        unsigned sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((unsigned)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            unsigned want = sz < 16 ? sz : 16;
            if ((unsigned)read(wf, fmt, want) != want) { close(wf); return -1; }
            channels = fmt[2] | (fmt[3] << 8);
            rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((unsigned)fmt[7] << 24);
            bits     = fmt[14] | (fmt[15] << 8);
            if (sz > want) skip_bytes(wf, (long)(sz - want));
        } else if (memcmp(ch, "data", 4) == 0) {
            data_size = (long)sz;
            break;                              /* file offset now at the PCM */
        } else {
            skip_bytes(wf, (long)sz + (sz & 1));    /* chunks are word-aligned */
        }
    }
    if (data_size < 0 || bits != 16 || (channels != 1 && channels != 2)) {
        close(wf);
        return -1;
    }

    resamp_t rs;
    rs_init(&rs, fd, rate, channels);

    unsigned char rb[4096];
    long left = data_size;
    while (left > 0) {
        int want = left > (long)sizeof(rb) ? (int)sizeof(rb) : (int)left;
        int got = (int)read(wf, rb, want);
        if (got <= 0) break;
        left -= got;
        rs_push(&rs, (const int16_t *)rb, got / (2 * channels));
    }
    rs_flush(&rs);
    close(wf);
    return 0;
}

/* ── MP3 ─────────────────────────────────────────────────────────────────── */

int audio_play_mp3(int fd, const char *path)
{
    int mf = open(path, O_RDONLY);
    if (mf < 0) return -1;

    /* minimp3 wants the encoded stream in memory; MP3s are small. Grow-read. */
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { close(mf); return -1; }
    for (;;) {
        if (len == cap) {
            if (cap >= MP3_MAX) { free(buf); close(mf); return -1; }
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(mf); return -1; }
            buf = nb;
        }
        int got = (int)read(mf, buf + len, cap - len);
        if (got <= 0) break;
        len += (size_t)got;
    }
    close(mf);
    if (len == 0) { free(buf); return -1; }

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    resamp_t rs;
    int inited = 0, ok = 0;
    unsigned char *p = buf;
    int avail = (int)len;
    while (avail > 0) {
        int samples = mp3dec_decode_frame(&dec, p, avail, pcm, &info);
        if (info.frame_bytes == 0) break;       /* no sync / done */
        p += info.frame_bytes;
        avail -= info.frame_bytes;
        if (samples > 0) {
            if (!inited) { rs_init(&rs, fd, info.hz, info.channels); inited = 1; }
            rs_push(&rs, pcm, samples);
            ok = 1;
        }
    }
    if (inited) rs_flush(&rs);
    free(buf);
    return ok ? 0 : -1;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

static int ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    if (ls < lf) return 0;
    const char *t = s + ls - lf;
    for (size_t i = 0; i < lf; i++) {
        char a = t[i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

int audio_play_file(int fd, const char *path)
{
    if (ends_with_ci(path, ".mp3"))
        return audio_play_mp3(fd, path);
    return audio_play_wav(fd, path);
}

int audio_duration_ms(const char *path)
{
    if (ends_with_ci(path, ".mp3")) {
        /* Accurate: read the file and count decoded samples (a CPU-only pass,
         * no audio output). The file-size/bitrate heuristic is wrong for VBR. */
        int mf = open(path, O_RDONLY);
        if (mf < 0) return 0;
        size_t cap = 1 << 20, len = 0;
        unsigned char *buf = malloc(cap);
        if (!buf) { close(mf); return 0; }
        for (;;) {
            if (len == cap) {
                if (cap >= MP3_MAX) { free(buf); close(mf); return 0; }
                cap *= 2;
                unsigned char *nb = realloc(buf, cap);
                if (!nb) { free(buf); close(mf); return 0; }
                buf = nb;
            }
            int got = (int)read(mf, buf + len, cap - len);
            if (got <= 0) break;
            len += (size_t)got;
        }
        close(mf);
        mp3dec_t dec; mp3dec_init(&dec);
        mp3dec_frame_info_t info;
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        unsigned char *p = buf;
        int avail = (int)len, hz = 0;
        long long frames = 0;
        while (avail > 0) {
            int samples = mp3dec_decode_frame(&dec, p, avail, pcm, &info);
            if (info.frame_bytes == 0) break;
            if (samples > 0) { frames += samples; if (!hz) hz = info.hz; }
            p += info.frame_bytes;
            avail -= info.frame_bytes;
        }
        free(buf);
        return hz > 0 ? (int)(frames * 1000 / hz) : 0;
    }

    int wf = open(path, O_RDONLY);
    if (wf < 0) return 0;
    unsigned char id[12];
    if (read(wf, id, 12) != 12 ||
        memcmp(id, "RIFF", 4) != 0 || memcmp(id + 8, "WAVE", 4) != 0) {
        close(wf); return 0;
    }
    int rate = SINK_RATE, channels = 2, bits = 16;
    long data_size = -1;
    for (;;) {
        unsigned char ch[8];
        if (read(wf, ch, 8) != 8) break;
        unsigned sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((unsigned)ch[7] << 24);
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            unsigned want = sz < 16 ? sz : 16;
            if ((unsigned)read(wf, fmt, want) != want) { close(wf); return 0; }
            channels = fmt[2] | (fmt[3] << 8);
            rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((unsigned)fmt[7] << 24);
            bits     = fmt[14] | (fmt[15] << 8);
            if (sz > want) skip_bytes(wf, (long)(sz - want));
        } else if (memcmp(ch, "data", 4) == 0) {
            data_size = (long)sz; break;
        } else {
            skip_bytes(wf, (long)sz + (sz & 1));
        }
    }
    close(wf);
    if (data_size < 0 || rate <= 0 || channels <= 0 || bits < 8) return 0;
    return (int)((long long)data_size * 1000 / ((long long)rate * channels * (bits / 8)));
}

pid_t audio_play_file_async(const char *path)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;                      /* parent, or -1 on fork failure */
    int fd = open("/dev/audio", O_WRONLY);
    if (fd >= 0) {
        audio_play_file(fd, path);
        close(fd);                       /* drains the buffered tail */
    }
    _exit(0);
}
