#!/bin/bash
# tools/fetch-ffmpeg.sh — fetch the FFmpeg source tarball into references/ffmpeg/.
# FFmpeg is built as trimmed static decode libs by tools/build-ffmpeg.sh for the
# lumen-video player (libavformat/avcodec/swscale/swresample/avutil only).
set -e
REPO="$(git rev-parse --show-toplevel)"
VER="${FFMPEG_VERSION:-6.1.2}"
URL="https://ffmpeg.org/releases/ffmpeg-${VER}.tar.xz"
TMP="/tmp/ffmpeg-${VER}.tar.xz"

if [ -f "$REPO/references/ffmpeg/configure" ]; then
    echo "[ffmpeg] source already present at references/ffmpeg/ — skip"
    exit 0
fi

wget -O "$TMP" "$URL"
mkdir -p "$REPO/references"
cd "$REPO/references"
tar xJf "$TMP"
rm -rf ffmpeg
mv "ffmpeg-${VER}" ffmpeg
echo "[ffmpeg] source extracted to references/ffmpeg/ (${VER})"
