#!/usr/bin/env python3
"""Convert a PNG logo to raw BGRA format for Bastion display manager.

Output format: 4-byte width (LE) + 4-byte height (LE) + width*height*4 bytes BGRA pixels.
"""
import sys, struct
from PIL import Image

if len(sys.argv) != 3:
    print(f"usage: {sys.argv[0]} input.png output.raw", file=sys.stderr)
    sys.exit(1)

img = Image.open(sys.argv[1]).convert("RGBA")
w, h = img.size
pixels = img.tobytes()  # RGBA order

with open(sys.argv[2], "wb") as f:
    f.write(struct.pack("<II", w, h))
    # Convert RGBA to BGRA
    for i in range(0, len(pixels), 4):
        r, g, b, a = pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]
        f.write(bytes([b, g, r, a]))

print(f"Logo converted: {w}x{h} -> {sys.argv[2]}")
