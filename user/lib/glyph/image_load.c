/* image_load.c — glyph_pixbuf_load_file via vendored stb_image. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include "image_load.h"

/* stb_image config: memory-only (we read the file ourselves), the three
 * formats that matter, no SIMD/threads. STBI_ASSERT → no-op to match
 * font.c's handling of the other stb header (avoid abort() in the GUI). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM           /* we could add PNM cheaply, but keep it tight */
#define STBI_NO_HDR
#define STBI_NO_LINEAR        /* with NO_HDR, drops the only <math.h>/pow use → no libm */
#define STBI_NO_TGA
#define STBI_NO_PSD
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz)        malloc(sz)
#define STBI_REALLOC(p,sz)     realloc(p,sz)
#define STBI_FREE(p)           free(p)
#include "stb_image.h"

/* Read an entire file into a malloc'd buffer. *len gets the size.
 * Returns the buffer (caller frees) or NULL with errno set. */
static unsigned char *
read_whole_file(const char *path, long *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0 || end > (off_t)(256 * 1024 * 1024)) {  /* 256MB file cap */
        close(fd); errno = EINVAL; return NULL;
    }
    lseek(fd, 0, SEEK_SET);

    unsigned char *buf = malloc((size_t)end);
    if (!buf) { close(fd); errno = ENOMEM; return NULL; }

    long got = 0;
    while (got < end) {
        ssize_t n = read(fd, buf + got, (size_t)(end - got));
        if (n < 0)  { free(buf); close(fd); errno = EIO; return NULL; }
        if (n == 0) break;  /* short file — decode what we have */
        got += n;
    }
    close(fd);
    *len = got;
    return buf;
}

int
glyph_pixbuf_load_file(const char *path, glyph_pixbuf_t *out)
{
    if (!path || !out) return -EINVAL;
    out->px = NULL; out->w = out->h = 0;

    long flen = 0;
    unsigned char *file = read_whole_file(path, &flen);
    if (!file) return -errno;

    int w = 0, h = 0, comp = 0;
    /* Force 4 channels (RGBA); we drop A below. */
    unsigned char *rgba = stbi_load_from_memory(file, (int)flen, &w, &h,
                                                &comp, 4);
    free(file);
    if (!rgba) return -EINVAL;                 /* undecodable */
    if (w <= 0 || h <= 0 ||
        (long long)w * h > GLYPH_PIXBUF_MAX_PIXELS) {
        stbi_image_free(rgba);
        return -EINVAL;
    }

    uint32_t *px = malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px) { stbi_image_free(rgba); return -ENOMEM; }

    /* RGBA bytes → 0x00RRGGBB. stb gives R,G,B,A in memory order. */
    long n = (long)w * h;
    for (long i = 0; i < n; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        px[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    stbi_image_free(rgba);

    out->px = px;
    out->w  = w;
    out->h  = h;
    return 0;
}

void
glyph_pixbuf_free(glyph_pixbuf_t *img)
{
    if (!img) return;
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}
