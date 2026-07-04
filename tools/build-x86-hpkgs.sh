#!/bin/bash
# build-x86-hpkgs.sh — build + sign the x86_64 .hpkg for every LoricaOS
# component (the x86 twin of build-arm64-hpkgs.sh), so both arches ship the same
# 1.1.1 versions. Builds the glyph x86 toolkit locally and compiles each
# component against it (bypassing fetch-glyph, exactly like the arm64 build),
# then runs each repo's pack.sh with ARCH=x86_64. Optionally chancery-adds them.
#
#   OUT=<dir>            where the .hpkg land (default ~/x86-hpkgs)
#   HERALD_KEY=<key>     signing key for the .hpkg detached sigs (repo install
#                        doesn't use them; local install does)
#   CHANCERY_REPO=<dir>  if set, `chancery add` each here + publish
# Runs on CT117 (x86 musl toolchain + mirrored component source).
set -euo pipefail
REPO="${REPO:-/root/loricaos}"
GLYPH="${GLYPH:-/root/glyph}"
export PATH="/usr/local/bin:$PATH"
CC="${CC:-$REPO/build/musl-dynamic/usr/bin/musl-gcc}"
AR="${AR:-ar}"
STRIP="${STRIP:-strip}"
KEY="${HERALD_KEY:-$REPO/build/herald-keys/herald-dev.key}"
OUT="${OUT:-$HOME/x86-hpkgs}"
CHANCERY_REPO="${CHANCERY_REPO:-}"
CH="${CH:-/root/chancery/target/release/chancery}"
# -fno-pie -no-pie: the glyph static libs are non-PIC; x86 musl-gcc defaults to
# PIE, which rejects their R_X86_64_32 relocs (matches the component Makefiles).
CFLAGS="-O2 -fno-pie -no-pie -Wall"
GUILIBS="-Ltoolkit/lib -lcitadel -laudio -lauth -lglyph"
APPS="lumen-terminal lumen-calculator lumen-editor lumen-filemanager \
    lumen-settings lumen-sysmon lumen-netman lumen-calendar lumen-imageviewer \
    lumen-tunes lumen-run lumen-feeds lumen-2048 lumen-snake lumen-minesweeper \
    lumen-applications-menu"
log(){ echo "[x86-hpkgs] $*"; }

stage_toolkit(){   # <repo-dir>
    rm -rf "$1/toolkit"; mkdir -p "$1/toolkit/lib" "$1/toolkit/include"
    cp "$GLYPH"/libglyph.a "$GLYPH"/libcitadel.a "$GLYPH"/libaudio.a "$GLYPH"/libauth.a "$1/toolkit/lib/"
    cp "$GLYPH"/lib/glyph/*.h "$GLYPH"/lib/citadel/*.h "$GLYPH"/lib/audio/*.h "$GLYPH"/lib/libauth/*.h "$1/toolkit/include/"
}

# 1. glyph x86 toolkit (rebuild — the tree may hold arm64 objects from a desktop build).
log "== glyph x86 toolkit =="
( cd "$GLYPH" && make clean >/dev/null 2>&1 || true; make MUSL_CC="$CC" AR="$AR" >/dev/null 2>&1 )

# 2. Compile each component against it. lumen/bastion/dock have their own .elf
#    names + bespoke pack.sh; the apps use component.elf.
log "== compile components (x86) =="
( cd /root/lumen   && stage_toolkit . && "$CC" $CFLAGS -Itoolkit/include -o lumen.elf   src/*.c $GUILIBS )
( cd /root/bastion && stage_toolkit . && "$CC" $CFLAGS -Itoolkit/include -o bastion.elf src/*.c $GUILIBS && cp bastion.elf component.elf )
( cd /root/citadel-dock && stage_toolkit . && "$CC" $CFLAGS -Itoolkit/include -o dock.elf $(ls /root/citadel-dock/src/*.c 2>/dev/null || ls /root/citadel-dock/*.c) $GUILIBS )
for a in $APPS; do
    [ -d "/root/$a" ] || { log "SKIP $a (not mirrored)"; continue; }
    ( cd "/root/$a" && stage_toolkit . && "$CC" $CFLAGS -Itoolkit/include -o component.elf src/*.c $GUILIBS ) \
        || { log "SKIP $a (compile failed)"; continue; }
done

# 3. Pack each as x86_64 (ARCH default → <id>.hpkg, no suffix).
rm -rf "$OUT"; mkdir -p "$OUT"; n=0
for r in lumen bastion citadel-dock $APPS; do
    R="/root/$r"; [ -d "$R" ] || continue
    if ( cd "$R" && ARCH=x86_64 STRIP="$STRIP" HERALD_KEY="$KEY" sh tools/pack.sh ) >/tmp/x86pack-$r.log 2>&1; then
        for f in "$R/$r.hpkg"; do [ -f "$f" ] && { cp "$f" "$OUT/"; [ -f "$f.sig" ] && cp "$f.sig" "$OUT/"; n=$((n+1)); }; done
    else log "SKIP $r (pack failed; /tmp/x86pack-$r.log)"; fi
done
log "x86 .hpkg built: $n"; ls "$OUT"/*.hpkg 2>/dev/null | sed 's|.*/||' | sort

# 4. Optionally publish into a Chancery repo.
if [ -n "$CHANCERY_REPO" ]; then
    log "== chancery add → $CHANCERY_REPO =="
    ( cd "$CHANCERY_REPO"; for f in "$OUT"/*.hpkg; do "$CH" add "$f" --suite stable --component main >/dev/null; done; "$CH" publish )
fi
