#!/bin/bash
# build-arm64-server.sh — assemble a real aarch64 LoricaOS *server* rootfs + ISO.
#
# Phase 1 of the arm64 port: a full static-musl base userland (vigil/login/stsh
# + admin tools) plus the LoricaOS coreutils, laid into a real ext2 rootfs the
# kernel mounts, booting to an interactive login shell where `ls`, `cat`, `ps`
# etc. work — not just the embedded-blob smoke test.
#
# Everything is STATIC musl (no dynamic linker on arm64 yet), which is why the
# manifest's dynamic .elf/coreutils-.hpkg pipeline (x86) is rebuilt from source
# here instead of reused.
#
# Runs on the CT117 build host (aarch64-linux-gnu toolchain + xorriso + mke2fs).
# Inputs:
#   $REPO            = this loricaos checkout            (default /root/loricaos)
#   $COREUTILS       = coreutils source checkout         (default /root/coreutils)
#   $AEGIS           = built aegis kernel tree           (default /root/aegis)
# Outputs:
#   build/blobs-arm64/*.bin           the static aarch64 binaries
#   build/arm64-server/rootfs.ext2    the assembled root filesystem
#   ~/loricaos-arm64-server.iso       the bootable UEFI ISO
set -euo pipefail

REPO="${REPO:-/root/loricaos}"
COREUTILS="${COREUTILS:-/root/coreutils}"
AEGIS="${AEGIS:-/root/aegis}"
SYSROOT="$REPO/build/musl-arm64"
CC="$SYSROOT/bin/aarch64-musl-gcc"
BLOBS="$REPO/build/blobs-arm64"
STAGE="$REPO/build/arm64-server/rootfs"
OUT_EXT2="$REPO/build/arm64-server/rootfs.ext2"
ISO="${ISO:-$HOME/loricaos-arm64-server.iso}"
STRIP="aarch64-linux-gnu-strip"

log() { echo "[arm64-server] $*"; }

cd "$REPO"

# 1. musl sysroot + vigil/login/stsh (reuses the existing builder).
log "== sysroot + core blobs =="
bash tools/build-arm64-userland.sh >/dev/null

# 2. Extra base tools — single-file, libc-only. (installer needs libinstall,
#    herald needs bearssl, sshd/httpd/dhcp/ip need net testing — deferred.)
log "== base admin tools =="
# NOTE: 'shell' (user/bin/shell → /bin/sh) is built separately as sh.bin so it
# does NOT clobber stsh's shell.bin blob (both would otherwise be shell.bin).
SIMPLE_TOOLS=(vigictl hostname reboot shutdown aegisctl chronos ip dhcp httpd)
# Tools that need libinstall.a (+ its header) — built after it below.
INSTALL_TOOLS=(adminpw useradd)
for t in "${SIMPLE_TOOLS[@]}"; do
    src="$REPO/user/bin/$t/main.c"
    [ -f "$src" ] || { log "SKIP $t (no main.c)"; continue; }
    if "$CC" -O2 -Wall -o "$BLOBS/$t.bin" "$src" 2>"/tmp/arm64-$t.err"; then
        "$STRIP" -s "$BLOBS/$t.bin"; log "built $t"
    else
        log "SKIP $t (compile failed; see /tmp/arm64-$t.err)"
    fi
done
# /bin/sh — the small POSIX-ish shell (distinct blob from stsh).
if "$CC" -O2 -Wall -o "$BLOBS/sh.bin" "$REPO/user/bin/shell/main.c" 2>/tmp/arm64-sh.err; then
    "$STRIP" -s "$BLOBS/sh.bin"; log "built sh"
else
    log "SKIP sh (compile failed)"
fi

# libinstall.a + tools that use it (adminpw, useradd). crypt() is in musl's libc.
LI_DIR="$REPO/user/lib/libinstall"
LI_LIB="$BLOBS/libinstall.a"
li_objs=""
for f in config copy credentials gpt install_elevate run; do
    if "$CC" -c -O2 -I"$LI_DIR" -o "/tmp/li-$f.o" "$LI_DIR/$f.c" 2>"/tmp/arm64-li-$f.err"; then
        li_objs="$li_objs /tmp/li-$f.o"
    else
        log "libinstall: $f.c failed (see /tmp/arm64-li-$f.err)"
    fi
done
if [ -n "$li_objs" ]; then
    aarch64-linux-gnu-ar rcs "$LI_LIB" $li_objs && log "built libinstall.a"
    for t in "${INSTALL_TOOLS[@]}"; do
        if "$CC" -O2 -Wall -I"$LI_DIR" -o "$BLOBS/$t.bin" "$REPO/user/bin/$t/main.c" "$LI_LIB" 2>"/tmp/arm64-$t.err"; then
            "$STRIP" -s "$BLOBS/$t.bin"; log "built $t"
        else
            log "SKIP $t (see /tmp/arm64-$t.err)"
        fi
    done
fi

# 3. coreutils — cross-compile every util under $COREUTILS/src/<name>/ with the
#    aarch64 musl wrapper (the Makefile's per-dir loop, but arch-retargeted).
log "== coreutils =="
CU_OUT="$BLOBS/coreutils"
rm -rf "$CU_OUT"; mkdir -p "$CU_OUT"
cu_n=0; cu_fail=""
for d in "$COREUTILS"/src/*/; do
    n="$(basename "$d")"
    if "$CC" -O2 -Wall -o "$CU_OUT/$n" "$d"*.c 2>"/tmp/arm64-cu-$n.err"; then
        "$STRIP" -s "$CU_OUT/$n"; cu_n=$((cu_n+1))
    else
        cu_fail="$cu_fail $n"
    fi
done
log "coreutils built: $cu_n  failed:${cu_fail:- none}"

# 4. Assemble the rootfs staging tree.
log "== rootfs staging =="
rm -rf "$STAGE"; mkdir -p "$STAGE"/{bin,etc,home,dev,proc,tmp,run}
# 4a. /etc skeleton from the repo (passwd/shadow/group, caps.d, vigil services).
cp -a "$REPO/rootfs/etc/." "$STAGE/etc/"
cp -a "$REPO/rootfs/home/." "$STAGE/home/" 2>/dev/null || true
# 4b. Prune dev/test vigil services — keep only getty (→ login) so boot is clean.
if [ -d "$STAGE/etc/vigil/services" ]; then
    for s in "$STAGE/etc/vigil/services"/*/; do
        case "$(basename "$s")" in
            getty) ;;                       # keep
            *) rm -rf "$s" ;;               # drop dltest/smpstress/chronos/...
        esac
    done
fi
# 4c. Base binaries → /bin.
cp "$BLOBS/vigil.bin"  "$STAGE/bin/vigil"
cp "$BLOBS/login.bin"  "$STAGE/bin/login"
cp "$BLOBS/shell.bin"  "$STAGE/bin/stsh"   # 'shell' blob == stsh (interactive)
for t in "${SIMPLE_TOOLS[@]}" "${INSTALL_TOOLS[@]}"; do
    [ -f "$BLOBS/$t.bin" ] && cp "$BLOBS/$t.bin" "$STAGE/bin/$t"
done
[ -f "$BLOBS/sh.bin" ] && cp "$BLOBS/sh.bin" "$STAGE/bin/sh"
# 4d. coreutils → /bin.
cp "$CU_OUT"/* "$STAGE/bin/" 2>/dev/null || true
# 4e. home dir for the live user.
mkdir -p "$STAGE/home/live"
chmod 0755 "$STAGE/bin/"* 2>/dev/null || true
log "rootfs has $(ls "$STAGE/bin" | wc -l | tr -d ' ') binaries in /bin"

# 5. ext2 image (sized to content + slack). Loaded whole as a Limine module
#    (ramdisk root), matching the arm64 kernel's boot path.
log "== ext2 image =="
rm -f "$OUT_EXT2"
/sbin/mke2fs -q -t ext2 -b 4096 -d "$STAGE" -L aegis-arm64 "$OUT_EXT2" 64M
log "rootfs.ext2: $(stat -c%s "$OUT_EXT2") bytes"

# 6. UEFI ISO: kernel + rootfs-as-module, booted via Limine (aarch64).
log "== UEFI ISO =="
ISODIR="$REPO/build/arm64-server/isodir"
rm -rf "$ISODIR"; mkdir -p "$ISODIR/boot/limine" "$ISODIR/EFI/BOOT"
cp "$AEGIS/build/arm64/isodir/boot/aegis.elf" "$ISODIR/boot/aegis.elf"
cp "$OUT_EXT2" "$ISODIR/boot/rootfs.img"
printf 'timeout: 0\n\n/LoricaOS arm64 (server)\n    protocol: limine\n    path: boot():/boot/aegis.elf\n    module_path: boot():/boot/rootfs.img\n    cmdline: boot=text\n' \
    > "$ISODIR/boot/limine/limine.conf"
LIMINE="$AEGIS/tools/limine"
cp "$LIMINE/limine-uefi-cd.bin" "$ISODIR/boot/limine/"
cp "$LIMINE/BOOTAA64.EFI" "$ISODIR/EFI/BOOT/"
xorriso -as mkisofs -R -r -J \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISODIR" -o "$ISO" >/tmp/arm64-iso.log 2>&1 \
    || { log "xorriso failed"; tail -20 /tmp/arm64-iso.log; exit 1; }
log "ISO: $ISO ($(stat -c%s "$ISO" 2>/dev/null || echo ?) bytes)"
log "done."
