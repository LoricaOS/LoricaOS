#!/usr/bin/env bash
# LoricaOS FFmpeg-port smoke test.
#
# Proves the ported static FFmpeg libs (tools/build-ffmpeg.sh) work ON AEGIS:
# stages a real H.264 mp4 into the rootfs, builds the ffsmoke ISO (kernel
# cmdline `ffsmoke` → vigil runs /bin/ffsmoke, which demuxes + thread-decodes
# + swscales the file), boots it headless and expects "[FFSMOKE] PASS" on
# serial. Run from the repo root; builds ffmpeg + the ISO itself if needed.
set -u
REPO="$(git rev-parse --show-toplevel)"
cd "$REPO"

MEDIA_DIR="build/ffsmoke-media"
MEDIA="$MEDIA_DIR/bbb.mp4"
MEDIA_URL="https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/360/Big_Buck_Bunny_360_10s_1MB.mp4"

[ -f build/ffmpeg-install/lib/libavcodec.a ] || bash tools/build-ffmpeg.sh
if [ ! -f "$MEDIA" ]; then
    mkdir -p "$MEDIA_DIR"
    wget -q -O "$MEDIA" "$MEDIA_URL" || { echo "[ffsmoke] FAIL: media fetch"; exit 1; }
fi

make ffsmoke-iso || exit 1

LOG="$(mktemp)"
timeout 120 qemu-system-x86_64 -machine pc -cdrom build/loricaos-ffsmoke.iso \
    -boot order=d -display none -vga std -nodefaults -serial stdio \
    -no-reboot -m 2048M -smp 4 > "$LOG" 2>&1 || true

if grep -q "\[FFSMOKE\] PASS" "$LOG"; then
    echo "[ffsmoke] PASS: FFmpeg decoded H.264 on Aegis"
    grep "\[FFSMOKE\]" "$LOG" | sed 's/^/  /'
    rm -f "$LOG"
    exit 0
fi

echo "[ffsmoke] FAIL: no [FFSMOKE] PASS on serial"
grep "\[FFSMOKE\]" "$LOG" | sed 's/^/  /'
echo "----- last 30 serial lines -----"; tail -30 "$LOG"
rm -f "$LOG"
exit 1
