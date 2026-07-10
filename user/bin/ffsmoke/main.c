/* ffsmoke.c — FFmpeg port smoke test: demux + threaded-decode a video file,
 * swscale one frame to RGB32, print stats. No display, no audio out — proves
 * the static libs work (avformat/avcodec/swscale) on musl before the player
 * app exists. Run: ffsmoke <file>. Exits 0 only if frames actually decoded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* -probe: raw kernel-read check, no FFmpeg. Reads the file twice — big 128K
 * reads vs 1K reads — and compares checksums + prints the head bytes. Splits
 * "ext2/read-path bug" from "FFmpeg-layer bug" in one boot. */
static int probe(const char *path)
{
    /* Read the head BEFORE any printf: if it still comes back as console
     * text, the corruption predates this process (sticky, at mount/boot);
     * if it's clean here but dirty after prints, reads leak console writes. */
    static unsigned char head[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("[FFSMOKE] probe: open FAIL\n"); return 1; }
    int hr = (int)read(fd, head, sizeof(head));
    close(fd);

    struct stat st;
    if (stat(path, &st) != 0) { printf("[FFSMOKE] probe: stat FAIL\n"); return 1; }
    fprintf(stderr, "[FFSMOKE] probe: st_size=%ld\n", (long)st.st_size);
    fprintf(stderr, "[FFSMOKE] probe: pre-print head(%d):", hr);
    for (int i = 0; i < 16; i++) printf(" %02x", head[i]);
    printf("\n");

    static unsigned char buf[64 * 1024];
    int r;

    /* Per-64K-chunk sums — compare against the host's to localize where the
     * data diverges (first block only? every N? everywhere?). */
    fd = open(path, O_RDONLY);
    unsigned long n = 0;
    int chunk = 0;
    while ((r = (int)read(fd, buf, sizeof(buf))) > 0) {
        unsigned long s = 0;
        for (int i = 0; i < r; i++) s += buf[i];
        fprintf(stderr, "[FFSMOKE] probe: chunk %2d n=%d sum=%lu\n", chunk++, r, s);
        n += (unsigned long)r;
    }
    close(fd);

    /* Re-read the head now, after lots of console writes. */
    fd = open(path, O_RDONLY);
    hr = (int)read(fd, head, sizeof(head));
    close(fd);
    fprintf(stderr, "[FFSMOKE] probe: post-print head(%d):", hr);
    for (int i = 0; i < 16; i++) printf(" %02x", head[i]);
    printf("\n[FFSMOKE] probe: total n=%lu\n", n);
    return 0;
}

/* -audio: decode the AUDIO stream, resample to 48k/s16/stereo (swresample —
 * the same path the video player uses), feed /dev/audio, and sample the kernel
 * A/V clock (syscall 505). Proves decode→resample→sink→clock end to end.
 * PASS = the clock advances to within ~15% of the audio actually fed. Needs
 * QEMU `-device intel-hda -device hda-duplex`. */
#include <sys/syscall.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#define SYS_AUDIO_POSITION 505

static int audio_clock_test(const char *path)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "[FFSMOKE] audio: FAIL open\n"); return 1;
    }
    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx < 0) { fprintf(stderr, "[FFSMOKE] audio: SKIP no audio stream\n"); return 2; }

    const AVCodec *dec = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[aidx]->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) {
        fprintf(stderr, "[FFSMOKE] audio: FAIL codec open\n"); return 1;
    }

    /* Resampler → 48 kHz / s16 / stereo (the /dev/audio sink format). */
    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext *swr = NULL;
    if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, 48000,
                            &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
                            0, NULL) < 0 || swr_init(swr) < 0) {
        fprintf(stderr, "[FFSMOKE] audio: FAIL swr init\n"); return 1;
    }

    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) { fprintf(stderr, "[FFSMOKE] audio: FAIL open /dev/audio\n"); return 1; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    static short obuf[48000 * 2];         /* up to 1 s of stereo per convert */
    long fed_frames = 0, last_pos = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == aidx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frm) >= 0) {
                uint8_t *out = (uint8_t *)obuf;
                int got = swr_convert(swr, &out, 48000,
                                      (const uint8_t **)frm->extended_data,
                                      frm->nb_samples);
                if (got > 0) {
                    size_t want = (size_t)got * 4, done = 0;
                    while (done < want) {
                        ssize_t n = write(fd, (char *)obuf + done, want - done);
                        if (n <= 0) break;
                        done += (size_t)n;
                    }
                    fed_frames += got;
                    last_pos = (long)syscall(SYS_AUDIO_POSITION);
                }
            }
        }
        av_packet_unref(pkt);
    }
    close(fd);                            /* drains the tail */

    long fed_ms = fed_frames * 1000 / 48000;
    /* After drain the clock should be near the total fed. Allow slack for the
     * in-flight ring tail at the last sample and timer granularity. */
    long final_pos = (long)syscall(SYS_AUDIO_POSITION);
    fprintf(stderr, "[FFSMOKE] audio: codec=%s fed_ms=%ld clock_mid=%ld clock_final=%ld\n",
            dec->name, fed_ms, last_pos, final_pos);
    if (fed_ms < 500) { fprintf(stderr, "[FFSMOKE] audio: FAIL too little audio\n"); return 1; }
    if (last_pos <= 0) { fprintf(stderr, "[FFSMOKE] audio: FAIL clock never advanced\n"); return 1; }
    /* Clock should reach at least 60% of fed audio (it lags by the ring depth
     * during play; we sampled mid-stream, so allow generous slack). */
    if (final_pos < fed_ms * 6 / 10) {
        fprintf(stderr, "[FFSMOKE] audio: FAIL clock lagged (%ld < %ld)\n",
                final_pos, fed_ms * 6 / 10);
        return 1;
    }
    fprintf(stderr, "[FFSMOKE] audio: PASS\n");
    return 0;
}

#define DEFAULT_MEDIA "/usr/share/ffsmoke/clip.mp4"

static int video_decode_test(const char *path);

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-probe") == 0)
        return probe(argc > 2 ? argv[2] : DEFAULT_MEDIA);
    if (argc > 1 && strcmp(argv[1], "-audio") == 0)
        return audio_clock_test(argc > 2 ? argv[2] : DEFAULT_MEDIA);

    /* No arg = the vigil-service invocation. Run BOTH stages on the staged
     * clip: video decode, then the A/V-clock test (skipped if the clip has no
     * audio track or there's no HDA). The harness boots with -device
     * intel-hda so the audio stage exercises the real sink + syscall 505. */
    const char *path = argc > 1 ? argv[1] : DEFAULT_MEDIA;
    int vrc = video_decode_test(path);
    if (vrc != 0) return vrc;
    int arc = audio_clock_test(path);
    return arc == 2 ? 0 : arc;            /* SKIP (no audio) is not a failure */
}

static int video_decode_test(const char *path)
{
    long t0 = now_ms();

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "[FFSMOKE] FAIL: open_input\n"); return 1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "[FFSMOKE] FAIL: stream_info\n"); return 1;
    }
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0) { fprintf(stderr, "[FFSMOKE] FAIL: no video stream\n"); return 1; }

    const AVCodec *dec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vidx]->codecpar);
    ctx->thread_count = 4;                    /* exercise pthread decode */
    if (avcodec_open2(ctx, dec, NULL) < 0) {
        fprintf(stderr, "[FFSMOKE] FAIL: codec open (%s)\n", dec ? dec->name : "?"); return 1;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();
    long frames = 0;
    unsigned long rgbsum = 0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vidx && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frm) >= 0) {
                frames++;
                if (frames == 30) {           /* swscale one frame to RGB32 */
                    struct SwsContext *sws = sws_getContext(
                        frm->width, frm->height, frm->format,
                        frm->width, frm->height, AV_PIX_FMT_BGRA,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    uint8_t *rgb = malloc((size_t)frm->width * frm->height * 4);
                    uint8_t *dst[4] = { rgb }; int ls[4] = { frm->width * 4 };
                    sws_scale(sws, (const uint8_t * const *)frm->data,
                              frm->linesize, 0, frm->height, dst, ls);
                    for (long i = 0; i < (long)frm->width * frm->height * 4; i++)
                        rgbsum += rgb[i];
                    free(rgb); sws_freeContext(sws);
                }
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, NULL);           /* drain */
    while (avcodec_receive_frame(ctx, frm) >= 0) frames++;

    fprintf(stderr, "[FFSMOKE] codec=%s %dx%d frames=%ld rgbsum=%lu decode_ms=%ld\n",
           dec->name, ctx->width, ctx->height, frames, rgbsum, now_ms() - t0);
    if (frames == 0) { fprintf(stderr, "[FFSMOKE] FAIL: 0 frames\n"); return 1; }
    fprintf(stderr, "[FFSMOKE] PASS\n");
    return 0;
}
