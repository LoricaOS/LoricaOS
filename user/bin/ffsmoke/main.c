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

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-probe") == 0)
        return probe(argc > 2 ? argv[2] : "/usr/share/ffsmoke/bbb.mp4");
    /* No arg = the vigil-service invocation; media staged by ffsmoke-test.sh. */
    const char *path = argc > 1 ? argv[1] : "/usr/share/ffsmoke/bbb.mp4";
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
