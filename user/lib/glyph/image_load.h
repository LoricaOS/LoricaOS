/* image_load.h — decode image files to 0x00RRGGBB pixel buffers.
 *
 * Wraps vendored stb_image (PNG/BMP/JPEG). Proxy windows have no per-pixel
 * alpha, so the loader flattens RGBA→0x00RRGGBB (alpha dropped). Pixels are
 * malloc'd; free with glyph_pixbuf_free.
 */
#ifndef GLYPH_PIXBUF_LOAD_H
#define GLYPH_PIXBUF_LOAD_H

#include <stdint.h>

typedef struct {
    uint32_t *px;   /* w*h pixels, 0x00RRGGBB, row-major; NULL when empty */
    int       w, h;
} glyph_pixbuf_t;

/* Decode the file at `path` into *out. Returns 0 on success, or a negative
 * errno (-ENOENT, -EIO, -ENOMEM, -EINVAL for undecodable/oversized).
 * Rejects images larger than GLYPH_PIXBUF_MAX_PIXELS to bound memory. */
#define GLYPH_PIXBUF_MAX_PIXELS (64 * 1024 * 1024)  /* 64 MPix hard cap */
int  glyph_pixbuf_load_file(const char *path, glyph_pixbuf_t *out);

/* Free pixels and zero the struct. Safe on an all-zero struct. */
void glyph_pixbuf_free(glyph_pixbuf_t *img);

#endif /* GLYPH_PIXBUF_LOAD_H */
