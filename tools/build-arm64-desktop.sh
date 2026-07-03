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
( cd "$GLYPH" && make MUSL_CC="$W" AR=aarch64-linux-gnu-ar >/dev/null 2>&1 )
for app in lumen bastion citadel-dock; do
    rm -rf "/root/$app/toolkit"; mkdir -p "/root/$app/toolkit/lib" "/root/$app/toolkit/include"
    cp "$GLYPH"/libglyph.a "$GLYPH"/libcitadel.a "$GLYPH"/libaudio.a "$GLYPH"/libauth.a \
       "/root/$app/toolkit/lib/"
    cp "$GLYPH"/lib/glyph/*.h "$GLYPH"/lib/citadel/*.h "$GLYPH"/lib/audio/*.h \
       "$GLYPH"/lib/libauth/*.h "/root/$app/toolkit/include/"
done
GUILIBS="-Ltoolkit/lib -lcitadel -laudio -lauth -lglyph"
log "== lumen / bastion / citadel-dock =="
( cd /root/lumen        && "$W" -O2 -Wall -Itoolkit/include -o lumen.elf   src/*.c $GUILIBS )
( cd /root/bastion      && "$W" -O2 -Wall -Itoolkit/include -o bastion.elf src/*.c $GUILIBS )
( cd /root/citadel-dock && "$W" -O2 -Wall -Itoolkit/include -o dock.elf \
      $(ls /root/citadel-dock/src/*.c 2>/dev/null || ls /root/citadel-dock/*.c) $GUILIBS )

# 2b. Build every app (same toolkit). /apps apps → /apps/<id>/<id> + app.ini +
#     icon; the applications menu is a /bin binary. Each app repo lives at
#     /root/<repo> (mirror from the Mac).
stage_toolkit() {   # <repo-dir>
    rm -rf "$1/toolkit"; mkdir -p "$1/toolkit/lib" "$1/toolkit/include"
    cp "$GLYPH"/libglyph.a "$GLYPH"/libcitadel.a "$GLYPH"/libaudio.a "$GLYPH"/libauth.a "$1/toolkit/lib/"
    cp "$GLYPH"/lib/glyph/*.h "$GLYPH"/lib/citadel/*.h "$GLYPH"/lib/audio/*.h "$GLYPH"/lib/libauth/*.h "$1/toolkit/include/"
}
APP_REPOS="lumen-terminal lumen-calculator lumen-editor lumen-filemanager \
    lumen-settings lumen-sysmon lumen-netman lumen-calendar lumen-imageviewer \
    lumen-tunes lumen-run lumen-feeds lumen-2048 lumen-snake lumen-minesweeper"
app_ok=0
for app in $APP_REPOS; do
    R="/root/$app"; [ -d "$R" ] || { log "SKIP $app (not mirrored)"; continue; }
    stage_toolkit "$R"
    if ! ( cd "$R" && "$W" -O2 -Wall -Itoolkit/include -o component.elf src/*.c $GUILIBS ) 2>"/tmp/app-$app.err"; then
        log "SKIP $app (build failed; /tmp/app-$app.err)"; continue
    fi
    appdir=$(ls -d "$R"/pkg/apps/*/ 2>/dev/null | head -1); id=$(basename "$appdir")
    mkdir -p "$STAGE/apps/$id"
    cp "$R/component.elf" "$STAGE/apps/$id/$id"; "$STRIP" -s "$STAGE/apps/$id/$id"; chmod 0755 "$STAGE/apps/$id/$id"
    cp "$appdir/app.ini"  "$STAGE/apps/$id/" 2>/dev/null || true
    cp "$appdir/icon.png" "$STAGE/apps/$id/" 2>/dev/null || true
    cp "$R/pkg/etc/aegis/caps.d/$id" "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
    app_ok=$((app_ok+1))
done
# applications menu → /bin/applications
if [ -d /root/lumen-applications-menu ]; then
    stage_toolkit /root/lumen-applications-menu
    if ( cd /root/lumen-applications-menu && "$W" -O2 -Wall -Itoolkit/include -o component.elf src/*.c $GUILIBS ) 2>/tmp/app-applications.err; then
        cp /root/lumen-applications-menu/component.elf "$STAGE/bin/applications"
        "$STRIP" -s "$STAGE/bin/applications"; chmod 0755 "$STAGE/bin/applications"
        cp /root/lumen-applications-menu/pkg/etc/aegis/caps.d/applications "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
        app_ok=$((app_ok+1))
    else
        log "SKIP applications-menu (build failed)"
    fi
fi
log "apps built + installed: $app_ok"

# 3. Overlay the desktop onto the server staging.
log "== overlay desktop =="
install_bin(){ cp "$1" "$STAGE/bin/$2"; "$STRIP" -s "$STAGE/bin/$2"; chmod 0755 "$STAGE/bin/$2"; }
install_bin /root/lumen/lumen.elf          lumen
install_bin /root/bastion/bastion.elf      bastion
install_bin /root/citadel-dock/dock.elf    citadel-dock
# assets → /usr/share (wallpaper/logo/claude) + /usr/share/fonts (ttf)
mkdir -p "$STAGE/usr/share/fonts"
cp /root/lumen/assets/*.ttf                 "$STAGE/usr/share/fonts/" 2>/dev/null || true
cp /root/lumen/assets/wallpaper.* /root/lumen/assets/logo.raw \
   /root/lumen/assets/claude.raw            "$STAGE/usr/share/"       2>/dev/null || true
# capability policies (from each component's pkg/)
cp /root/lumen/pkg/caps.d/lumen                          "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
cp /root/bastion/pkg/etc/aegis/caps.d/bastion            "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
cp /root/citadel-dock/pkg/etc/aegis/caps.d/citadel-dock  "$STAGE/etc/aegis/caps.d/" 2>/dev/null || true
# services: bastion greeter (graphical) + dock. vigil in graphical mode runs
# the bastion service instead of getty.
for svc in bastion citadel-dock; do
    src=$(ls -d /root/$svc/pkg/etc/vigil/services/* 2>/dev/null | head -1)
    [ -n "$src" ] && { mkdir -p "$STAGE/etc/vigil/services/$(basename "$src")"; \
        cp "$src"/* "$STAGE/etc/vigil/services/$(basename "$src")/"; }
done
log "rootfs has $(ls "$STAGE/bin" | wc -l | tr -d ' ') binaries; GUI + assets overlaid"

# 4. ext2 image (bigger — GUI + assets) as a Limine module.
log "== ext2 image =="
mkdir -p "$(dirname "$OUT_EXT2")"; rm -f "$OUT_EXT2"
/sbin/mke2fs -q -t ext2 -b 4096 -d "$STAGE" -L aegis-arm64 "$OUT_EXT2" 128M
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
