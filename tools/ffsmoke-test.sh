#!/usr/bin/env bash
# LoricaOS FFmpeg-port smoke test.
#
# Proves the ported static FFmpeg libs (tools/build-ffmpeg.sh) work ON AEGIS.
# Stages a real H.264+AAC mp4 into the rootfs and boots the ffsmoke ISO
# (kernel cmdline `ffsmoke` → vigil runs /bin/ffsmoke) WITH an emulated HDA
# device, so one boot exercises both stages:
#   video: demux + threaded H.264 decode + swscale       → "[FFSMOKE] PASS"
#   audio: decode AAC + swresample → /dev/audio + the A/V clock (syscall 505)
#          → "[FFSMOKE] audio: PASS"
# Run from the repo root; builds ffmpeg + the ISO itself if needed.
set -u
REPO="$(git rev-parse --show-toplevel)"
cd "$REPO"

MEDIA_DIR="build/ffsmoke-media"
MEDIA="$MEDIA_DIR/clip.mp4"
# A small clip carrying BOTH an H.264 video track and an AAC audio track.
MEDIA_URL="https://download.samplelib.com/mp4/sample-5s.mp4"

[ -f build/ffmpeg-install/lib/libavcodec.a ] || bash tools/build-ffmpeg.sh
if [ ! -f "$MEDIA" ]; then
    mkdir -p "$MEDIA_DIR"
    wget -q -O "$MEDIA" "$MEDIA_URL" || { echo "[ffsmoke] FAIL: media fetch"; exit 1; }
fi

make ffsmoke-iso || exit 1

LOG="$(mktemp)"
timeout 150 qemu-system-x86_64 -machine pc -cdrom build/loricaos-ffsmoke.iso \
    -boot order=d -display none -vga std -nodefaults -serial stdio \
    -no-reboot -m 2048M -smp 4 \
    -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
    > "$LOG" 2>&1 || true

video_ok=0; audio_ok=0
grep -q "\[FFSMOKE\] PASS" "$LOG" && video_ok=1
grep -q "\[FFSMOKE\] audio: PASS" "$LOG" && audio_ok=1

if [ "$video_ok" = 1 ] && [ "$audio_ok" = 1 ]; then
    echo "[ffsmoke] PASS: FFmpeg decoded H.264 + drove the A/V clock on Aegis"
    grep "\[FFSMOKE\]" "$LOG" | sed 's/^/  /'
    rm -f "$LOG"
    exit 0
fi

echo "[ffsmoke] FAIL: video_ok=$video_ok audio_ok=$audio_ok"
grep "\[FFSMOKE\]" "$LOG" | sed 's/^/  /'
echo "----- last 30 serial lines -----"; tail -30 "$LOG"
rm -f "$LOG"
exit 1
