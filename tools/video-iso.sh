#!/usr/bin/env bash
# video-iso.sh — build a desktop ISO that autoplays a test clip with the
# lumen-video player. A one-off test/demo artifact (not a release image): it
# takes the already-built desktop rootfs, injects the player binary + the clip
# + an autoplay vigil service via debugfs, and wraps it in an ISO whose kernel
# cmdline autologins and fires the `videoplay` service.
#
#   VIDEO_ELF=<path to built video.elf>  CLIP=<path to test .mp4>  \
#       bash tools/video-iso.sh
#
# Requires: build/rootfs-desktop.img (make desktop-iso once) + the kernel + Limine.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VIDEO_ELF="${VIDEO_ELF:?set VIDEO_ELF to the built video.elf}"
CLIP="${CLIP:?set CLIP to the test .mp4}"
DESKTOP=build/rootfs-desktop.img
OUT=build/rootfs-video.img
ISO=build/loricaos-video.iso
DEBUGFS="${DEBUGFS:-debugfs}"
STRIP="${STRIP_TOOL:-$(command -v x86_64-linux-gnu-strip || command -v strip || true)}"
FS_KIB=98304   # 96 MiB logical — room for the desktop overlay + the clip
CLIP_NAME="$(basename "$CLIP")"

[ -f "$DESKTOP" ]   || { echo "video-iso: need $DESKTOP (run: make desktop-iso)"; exit 1; }
[ -f "$VIDEO_ELF" ] || { echo "video-iso: need VIDEO_ELF ($VIDEO_ELF)"; exit 1; }
[ -f "$CLIP" ]      || { echo "video-iso: need CLIP ($CLIP)"; exit 1; }

# Grow a copy of the desktop rootfs to fit the clip.
cp "$DESKTOP" "$OUT"
truncate -s "${FS_KIB}K" "$OUT"
e2fsck -fy "$OUT" >/dev/null 2>&1 || true
resize2fs "$OUT" "${FS_KIB}K" >/dev/null 2>&1 || { echo "video-iso: resize2fs failed"; exit 1; }

declare -A MKDIRD
ensure_dir() {
    local d="$1"
    [ -n "${MKDIRD[$d]:-}" ] && return
    local p; p="$(dirname "$d")"
    [ "$p" != "/" ] && [ "$p" != "." ] && ensure_dir "$p"
    $DEBUGFS -w -R "mkdir $d" "$OUT" >/dev/null 2>&1 || true
    MKDIRD["$d"]=1
}
put() {   # <src> <dest-abs> <octal-mode>
    ensure_dir "$(dirname "$2")"
    $DEBUGFS -w -R "rm $2" "$OUT" >/dev/null 2>&1 || true
    $DEBUGFS -w -R "write $1 $2" "$OUT" >/dev/null 2>&1
    $DEBUGFS -w -R "set_inode_field $2 mode $3" "$OUT" >/dev/null 2>&1
}

# 1. The player binary (stripped) + its app bundle + cap profile.
tmp="$(mktemp)"
"$STRIP" -o "$tmp" "$VIDEO_ELF" 2>/dev/null || cp "$VIDEO_ELF" "$tmp"
put "$tmp" "/apps/video/video" 0100755
rm -f "$tmp"
printf 'name=Video\nexec=video\n' > /tmp/video.ini
printf 'service\n'               > /tmp/video.caps
put /tmp/video.ini  "/apps/video/app.ini"          0100644
put /tmp/video.caps "/etc/aegis/caps.d/video"      0100644

# 2. The test clip, at the fixed autoplay path the player defaults to when it's
#    launched with no file argument.
put "$CLIP" "/usr/share/video/autoplay.mp4" 0100644

# 3. Autoplay vigil service (graphical, oneshot, gated on the `videoplay` token).
#    vigil execs an absolute run path directly (no shell, no args), so the player
#    is launched arg-less and picks up /usr/share/video/autoplay.mp4 itself —
#    the same proven launch pattern the soak's stresstest service uses.
sd=/etc/vigil/services/videoplay
printf 'videoplay'          > /tmp/v.cmdline
printf 'graphical'          > /tmp/v.mode
printf 'oneshot'            > /tmp/v.policy
printf '/apps/video/video'  > /tmp/v.run
printf 'root'               > /tmp/v.user
put /tmp/v.cmdline "$sd/cmdline" 0100644
put /tmp/v.mode    "$sd/mode"    0100644
put /tmp/v.policy  "$sd/policy"  0100644
put /tmp/v.run     "$sd/run"     0100644
put /tmp/v.user    "$sd/user"    0100644

# Truncate trailing zero blocks (kernel re-expands from the superblock).
python3 - "$OUT" <<'PYEOF'
import os, sys
p = sys.argv[1]; d = open(p,'rb').read(); i = len(d)
while i > 0 and d[i-1] == 0: i -= 1
os.truncate(p, max(4096, ((i+4095)//4096)*4096))
PYEOF
echo "[video-iso] rootfs -> $OUT ($(du -h "$OUT" | cut -f1))"

# Build the ISO (mode `videoplay`: graphical autologin + the service token).
D=build/video-isodir
rm -rf "$D"; mkdir -p "$D/boot/limine" "$D/EFI/BOOT"
cp build/aegis-stripped.elf "$D/boot/aegis.elf"
cp "$OUT" "$D/boot/rootfs.img"
cp build/esp-desktop.img "$D/boot/esp.img"
sh tools/gen-limine-conf.sh videoplay > "$D/boot/limine/limine.conf"
cp tools/limine/limine-bios.sys tools/limine/limine-bios-cd.bin tools/limine/limine-uefi-cd.bin "$D/boot/limine/"
cp tools/limine/BOOTX64.EFI tools/limine/BOOTIA32.EFI "$D/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label "$D" -o "$ISO" >/dev/null 2>&1
build/limine/limine bios-install "$ISO" >/dev/null 2>&1
echo "[video-iso] $ISO ($(du -h "$ISO" | cut -f1))"
