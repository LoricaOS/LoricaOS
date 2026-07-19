#!/bin/bash
# build-arm64-desktop.sh — arm64 graphical desktop ISO (bastion greeter + lumen
# compositor + citadel-dock), on top of the arm64 server base.
#
# Builds the glyph toolkit + lumen/bastion/citadel-dock for aarch64 (static
# musl), overlays them + the desktop assets/services on the server rootfs, and
# packs a Limine UEFI ISO that boots graphical (boot=graphical → vigil runs the
# bastion greeter on the Limine framebuffer). Boot with the two virtio input
# devices:  -device virtio-keyboard-pci -device virtio-mouse-pci
#
# Runs on CT117. Needs the GUI source at /root/{glyph,lumen,bastion,citadel-dock}
# (mirror from the Mac) + the built arm64 kernel.
set -euo pipefail
REPO="${REPO:-/root/loricaos}"
AEGIS="${AEGIS:-/root/aegis}"
GLYPH="${GLYPH:-/root/glyph}"
W="$REPO/build/musl-arm64/bin/aarch64-musl-gcc"
STRIP=aarch64-linux-gnu-strip
STAGE="$REPO/build/arm64-server/rootfs"    # reuse the server staging tree
OUT_EXT2="$REPO/build/arm64-desktop/rootfs.ext2"
ISO="${ISO:-$HOME/loricaos-arm64-desktop.iso}"
log(){ echo "[arm64-desktop] $*"; }

# 1. Server base — builds the staging tree + all base/coreutils binaries.
log "== server base =="
bash "$REPO/tools/build-arm64-server.sh" >/dev/null

# 2. glyph toolkit (glyph HEAD == 1.1.0; lumen/bastion GLYPH_VERSION pins are
#    stale 1.0.0, so build the toolkit locally and compile the components
#    directly against it — bypassing each Makefile's fetch-glyph.sh).
log "== glyph toolkit =="
# `make clean` first: the tree may hold x86 objects from build-x86-hpkgs.sh
# (which cleans for the same reason in reverse). Without it the archives keep
# the other arch's objects and every component fails to link with
# "skipping incompatible libglyph.a".
( cd "$GLYPH" && make clean >/dev/null 2>&1; make MUSL_CC="$W" AR=aarch64-linux-gnu-ar >/dev/null 2>&1 )
for app in lumen bastion citadel-dock lumen-shell; do
    rm -rf "/root/$app/toolkit"; mkdir -p "/root/$app/toolkit/lib" "/root/$app/toolkit/include"
    cp "$GLYPH"/libglyph.a "$GLYPH"/libcitadel.a "$GLYPH"/libaudio.a "$GLYPH"/libauth.a \
       "/root/$app/toolkit/lib/"
    cp "$GLYPH"/lib/glyph/*.h "$GLYPH"/lib/citadel/*.h "$GLYPH"/lib/audio/*.h \
       "$GLYPH"/lib/libauth/*.h "/root/$app/toolkit/include/"
done
GUILIBS="-Ltoolkit/lib -lcitadel -laudio -lauth -lglyph"
log "== lumen / bastion / citadel-dock / lumen-shell =="
( cd /root/lumen        && "$W" -O2 -Wall -Itoolkit/include -o lumen.elf   src/*.c $GUILIBS )
( cd /root/bastion      && "$W" -O2 -Wall -Itoolkit/include -o bastion.elf src/*.c $GUILIBS )
( cd /root/citadel-dock && "$W" -O2 -Wall -Itoolkit/include -o dock.elf \
      $(ls /root/citadel-dock/src/*.c 2>/dev/null || ls /root/citadel-dock/*.c) $GUILIBS )
( cd /root/lumen-shell  && "$W" -O2 -Wall -Itoolkit/include -o shell.elf   src/*.c $GUILIBS )

# 2b. Build every app (same toolkit). /apps apps → /apps/<id>/<id> + app.ini +
#     icon; the applications menu is a /bin binary. Each app repo lives at
#     /root/<repo> (mirror from the Mac).
stage_toolkit() {   # <repo-dir>
    rm -rf "$1/toolkit"; mkdir -p "$1/toolkit/lib" "$1/toolkit/include"
    cp "$GLYPH"/libglyph.a "$GLYPH"/libcitadel.a "$GLYPH"/libaudio.a "$GLYPH"/libauth.a "$1/toolkit/lib/"
    cp "$GLYPH"/lib/glyph/*.h "$GLYPH"/lib/citadel/*.h "$GLYPH"/lib/audio/*.h "$GLYPH"/lib/libauth/*.h "$1/toolkit/include/"
}
# Every non-core component, installed EXACTLY as herald installs it on x86: the
# built binary at the path its own pack.sh declares (DESTBIN) + the pkg/ tree
# overlaid verbatim (app.ini, icon, caps.d, vigil services). GUI apps land at
# apps/<id>/<id>; lumen-dbg is a bin/ probe service, the menu is bin/applications
# — driving off each pack.sh keeps all three shapes correct without special-
# casing. This set MUST equal tools/components.list minus the four core
# components above; games/extras (2048/snake/minesweeper/feeds) are herald-only
# on x86, so they are NOT baked here. Kept 1:1 by CI (tools/check-desktop-parity.sh,
# which parses COMPONENTS below).
COMPONENTS="lumen-settings lumen-terminal lumen-calculator lumen-editor \
    lumen-filemanager lumen-run lumen-tunes lumen-calendar lumen-imageviewer \
    lumen-sysmon lumen-netman lumen-video lumen-dbg lumen-applications-menu"
comp_ok=0
for c in $COMPONENTS; do
    R="/root/$c"; [ -d "$R" ] || { log "SKIP $c (not mirrored)"; continue; }
    stage_toolkit "$R"
    inc=""; libs="$GUILIBS"
    # lumen-video also links a trimmed static FFmpeg. Build it once for arm64
    # (cross), reusing the loricaos ffmpeg source tree so there's no network fetch.
    if [ "$c" = lumen-video ]; then
        FFI="$R/build/ffmpeg-install-arm64"
        if [ ! -f "$FFI/lib/libavcodec.a" ]; then
            log "building arm64 FFmpeg for lumen-video (slow, one-time)..."
            [ -f "$R/references/ffmpeg/configure" ] || { mkdir -p "$R/references"; ln -sfn "$REPO/references/ffmpeg" "$R/references/ffmpeg"; }
            if ! ( cd "$R" && CC="$W" ARCH=aarch64 SUFFIX=-arm64 REPO="$R" CROSS_PREFIX=aarch64-linux-gnu- \
                   bash tools/build-ffmpeg.sh ) >"/tmp/app-$c-ffmpeg.log" 2>&1; then
                log "SKIP $c (FFmpeg build failed; /tmp/app-$c-ffmpeg.log)"; continue
            fi
        fi
        inc="-I$FFI/include"
        # No -lpthread/-lm: musl folds both into libc; adding them here pulls in
        # the glibc aarch64 libm.a and segfaults the cross linker.
        libs="-L$FFI/lib -lavformat -lavcodec -lswscale -lswresample -lavutil $GUILIBS"
    fi
    if ! ( cd "$R" && "$W" -O2 -Wall -Itoolkit/include $inc -o component.elf src/*.c $libs ) 2>"/tmp/app-$c.err"; then
        log "SKIP $c (build failed; /tmp/app-$c.err)"; continue
    fi
    dest=$(grep -m1 '^DESTBIN=' "$R/tools/pack.sh" | sed 's/^DESTBIN=//; s/[[:space:]#].*//')
    [ -n "$dest" ] || { log "SKIP $c (no DESTBIN in pack.sh)"; continue; }
    mkdir -p "$STAGE/$(dirname "$dest")"
    "$STRIP" -o "$STAGE/$dest" "$R/component.elf" 2>/dev/null || cp "$R/component.elf" "$STAGE/$dest"
    chmod 0755 "$STAGE/$dest"
    [ -d "$R/pkg" ] && cp -R "$R/pkg/." "$STAGE/"   # app.ini / icon / caps.d / vigil, verbatim
    comp_ok=$((comp_ok+1))
done
log "components built + installed: $comp_ok"

# 3. Overlay the desktop onto the server staging.
log "== overlay desktop =="
install_bin(){ cp "$1" "$STAGE/bin/$2"; "$STRIP" -s "$STAGE/bin/$2"; chmod 0755 "$STAGE/bin/$2"; }
install_bin /root/lumen/lumen.elf          lumen
install_bin /root/bastion/bastion.elf      bastion
install_bin /root/citadel-dock/dock.elf    citadel-dock
install_bin /root/lumen-shell/shell.elf    lumen-shell
# assets → /usr/share (wallpaper/logo/claude) + /usr/share/fonts (ttf)
mkdir -p "$STAGE/usr/share/fonts"
cp /root/lumen/assets/*.ttf                 "$STAGE/usr/share/fonts/" 2>/dev/null || true
cp /root/lumen/assets/wallpaper.* /root/lumen/assets/logo.raw \
   /root/lumen/assets/claude.raw            "$STAGE/usr/share/"       2>/dev/null || true
# capability policies (from each component's pkg/)
cp /root/lumen/pkg/caps.d/lumen                          "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
cp /root/bastion/pkg/etc/aegis/caps.d/bastion            "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
cp /root/citadel-dock/pkg/etc/aegis/caps.d/citadel-dock  "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
cp /root/lumen-shell/pkg/etc/aegis/caps.d/lumen-shell    "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
# services: bastion greeter (graphical) + dock + top-bar shell. vigil in
# graphical mode runs the bastion service instead of getty.
for svc in bastion citadel-dock lumen-shell; do
    src=$(ls -d /root/$svc/pkg/etc/vigil/services/* 2>/dev/null | head -1)
    [ -n "$src" ] && { mkdir -p "$STAGE/etc/vigil/services/$(basename "$src")"; \
        cp "$src"/* "$STAGE/etc/vigil/services/$(basename "$src")/"; }
done
log "rootfs has $(ls "$STAGE/bin" | wc -l | tr -d ' ') binaries; GUI + assets overlaid"

# 4. ext2 image (bigger — GUI + assets) as a Limine module.
log "== ext2 image =="
mkdir -p "$(dirname "$OUT_EXT2")"; rm -f "$OUT_EXT2"
# Size is an override, not a constant: the Pi 5 TFTP netboot path carries this
# image as an initramfs, and TFTP's 16-bit block counter puts a ceiling on it
# (62M is what the netboot loop has been running). ISO builds can use the
# roomier default.
ROOTFS_SIZE="${ROOTFS_SIZE:-128M}"
/sbin/mke2fs -q -t ext2 -b 4096 -d "$STAGE" -L aegis-arm64 "$OUT_EXT2" "$ROOTFS_SIZE"
# Drop the trailing free blocks. The fs is a single block group with 4 KiB
# blocks, so it has no backup superblocks in the tail and everything past the
# last used block is zero; the kernel's ramdisk_init reads the real block count
# out of the superblock and zero-extends back to full size in RAM (see
# kernel/drivers/ramdisk.c). The x86 ISO has always shipped truncated — this is
# the same trick, and it matters far more here because the Pi 5 netboot sends
# every one of these bytes over TFTP on each boot (62M -> ~24M).
python3 - "$OUT_EXT2" <<'TRUNC'
import os, sys
p = sys.argv[1]
size = os.path.getsize(p)
with open(p, "rb") as f:
    off, last, CH = size, 0, 1 << 20
    while off > 0:
        rd = min(CH, off); off -= rd
        f.seek(off); blk = f.read(rd)
        nz = len(blk.rstrip(b"\x00"))
        if nz:
            last = off + nz
            break
new = ((last + 4095) // 4096) * 4096
if new < size:
    os.truncate(p, new)
    print("[arm64-desktop] truncated %d -> %d bytes (kernel re-extends at boot)"
          % (size, new))
TRUNC
log "rootfs.ext2: $(stat -c%s "$OUT_EXT2") bytes"

# 5. UEFI ISO, cmdline boot=graphical.
log "== UEFI ISO =="
ISODIR="$REPO/build/arm64-desktop/isodir"; rm -rf "$ISODIR"
mkdir -p "$ISODIR/boot/limine" "$ISODIR/EFI/BOOT"
cp "$AEGIS/build/arm64/isodir/boot/aegis.elf" "$ISODIR/boot/aegis.elf"
cp "$OUT_EXT2" "$ISODIR/boot/rootfs.img"
printf 'timeout: 0\n\n/LoricaOS arm64 (desktop)\n    protocol: limine\n    path: boot():/boot/aegis.elf\n    module_path: boot():/boot/rootfs.img\n    cmdline: boot=graphical\n' \
    > "$ISODIR/boot/limine/limine.conf"
LIMINE="$AEGIS/tools/limine"
cp "$LIMINE/limine-uefi-cd.bin" "$ISODIR/boot/limine/"
cp "$LIMINE/BOOTAA64.EFI" "$ISODIR/EFI/BOOT/"
xorriso -as mkisofs -R -r -J --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISODIR" -o "$ISO" >/tmp/arm64-desktop-iso.log 2>&1 \
    || { log "xorriso failed"; tail -20 /tmp/arm64-desktop-iso.log; exit 1; }
log "ISO: $ISO ($(stat -c%s "$ISO") bytes)"
log "done. boot with: -device virtio-keyboard-pci -device virtio-mouse-pci"
