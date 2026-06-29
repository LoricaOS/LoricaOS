#!/usr/bin/env python3
"""Convert a PNG wallpaper to raw pixel data for Aegis lumen compositor.

Output format:
  - 4 bytes: width  (uint32 little-endian)
  - 4 bytes: height (uint32 little-endian)
  - width * height * 4 bytes: pixels as uint32 LE in 0x00RRGGBB format

Usage:
  python3 tools/convert-wallpaper.py input.png output.raw
"""

import array
import struct
import sys

from PIL import Image


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.raw>", file=sys.stderr)
        sys.exit(1)

    src_path = sys.argv[1]
    dst_path = sys.argv[2]

    img = Image.open(src_path).convert("RGBA")
    w, h = img.size

    raw = img.tobytes()  # R, G, B, A per pixel, tightly packed

    # Build a uint32 array of 0x00RRGGBB pixels.
    # On little-endian hosts (x86/ARM), each uint32 is stored LE in memory,
    # which is exactly what the Aegis framebuffer expects.
    n = w * h
    out = array.array("I", [0]) * n
    for i in range(n):
        off = i * 4
        out[i] = (raw[off] << 16) | (raw[off + 1] << 8) | raw[off + 2]

    with open(dst_path, "wb") as f:
        f.write(struct.pack("<II", w, h))
        out.tofile(f)

    expected = 8 + n * 4
    print(f"{w}x{h} -> {dst_path} ({expected} bytes)")


if __name__ == "__main__":
    main()
