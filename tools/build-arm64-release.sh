#!/bin/bash
# build-arm64-release.sh — assemble the aarch64 release artifact set, mirroring
# the x86 release for arm64. Produces, in $OUT (default ~/loricaos-<ver>-arm64):
#
#   aegis-arm64-<kver>.elf              the kernel release artifact (stripped)
#   loricaos-<ver>-arm64-server.iso     text/server UEFI ISO
#   loricaos-<ver>-arm64-desktop.iso    graphical UEFI ISO
#   SHA256SUMS                          checksums over the three above
#
# Versions come from the VERSION files (loricaos → ISO ver, aegis → kernel ver),
# so this stays reproducible and git-independent like the rest of the build.
# Runs on CT117; needs the mirrored component source + arm64 toolchain (same as
# build-arm64-{server,desktop}.sh).
set -euo pipefail
REPO="${REPO:-/root/loricaos}"
AEGIS="${AEGIS:-/root/aegis}"
export PATH="/usr/local/bin:$PATH"
VER="$(cat "$REPO/VERSION")"
KVER="$(cat "$AEGIS/VERSION")"
OUT="${OUT:-$HOME/loricaos-$VER-arm64}"
log(){ echo "[arm64-release] $*"; }

rm -rf "$OUT"; mkdir -p "$OUT"

# 1. Kernel: build the ISO tree (gives isodir/boot/aegis.elf the ISO scripts
#    copy) + the stripped release artifact.
log "== kernel v$KVER =="
( cd "$AEGIS" && make -f Makefile.arm64 iso dist >/dev/null )
cp "$AEGIS/build/arm64/dist/aegis-arm64-$KVER.elf" "$OUT/"

# 2. Server ISO (text). 3. Desktop ISO (graphical). The desktop build re-runs the
# server rootfs build internally, so the two ISOs never cross-contaminate.
# ponytail: that's a redundant server-rootfs build; fold it in only if release
# time actually hurts.
log "== server ISO =="
ISO="$OUT/loricaos-$VER-arm64-server.iso"  bash "$REPO/tools/build-arm64-server.sh"  >/dev/null
log "== desktop ISO =="
ISO="$OUT/loricaos-$VER-arm64-desktop.iso" bash "$REPO/tools/build-arm64-desktop.sh" >/dev/null

# 4. Checksums (filenames only, stable order).
( cd "$OUT" && sha256sum "aegis-arm64-$KVER.elf" \
      "loricaos-$VER-arm64-server.iso" "loricaos-$VER-arm64-desktop.iso" > SHA256SUMS )
log "release artifacts (loricaos v$VER / kernel v$KVER):"
ls -la "$OUT"
