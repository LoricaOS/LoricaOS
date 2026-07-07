#!/usr/bin/env bash
# lantern-iso.sh — desktop ISO with Lantern (the Ladybird-engine browser)
# PRE-INSTALLED and auto-launched. A one-off demo artifact: it bypasses herald's
# on-device install of the ~279 MB package entirely (the download flakes on the
# Aegis TCP stack and the on-device ext2 extract is single-core-slow), by
# extracting the engine natively into the desktop rootfs and firing it with the
# same token-gated vigil service pattern tools/video-iso.sh uses.
#
#   HPKG=/root/lantern_1.0.0_x86_64.hpkg  bash tools/lantern-iso.sh
#
# Requires: build/rootfs-desktop.img (make desktop-iso once) + kernel + Limine.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

HPKG="${HPKG:?set HPKG to the lantern .hpkg}"
DESKTOP=build/rootfs-desktop.img
OUT=build/rootfs-lantern.img
ISO=build/loricaos-lantern.iso
DEBUGFS="${DEBUGFS:-debugfs}"
FS_KIB=409600   # 400 MiB logical — room for the ~279 MB engine

[ -f "$DESKTOP" ] || { echo "lantern-iso: need $DESKTOP (make desktop-iso)"; exit 1; }
[ -f "$HPKG" ]    || { echo "lantern-iso: need HPKG $HPKG"; exit 1; }

cp "$DESKTOP" "$OUT"
truncate -s "${FS_KIB}K" "$OUT"
e2fsck -fy "$OUT" >/dev/null 2>&1 || true
resize2fs "$OUT" "${FS_KIB}K" >/dev/null 2>&1 || { echo "lantern-iso: resize2fs failed"; exit 1; }

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

# 1. Extract the package payload preserving each file's mode (the engine's
#    binaries + .so's are 0755 in the tar; herald preserves that, and so must we
#    or the loader/ld reject them). Skip the manifest (not a payload file).
X="$(mktemp -d)"
tar xf "$HPKG" -C "$X"
n=0
while IFS= read -r f; do
    rel="${f#"$X"/}"
    [ "$rel" = manifest ] && continue
    mode="0100$(stat -c %a "$f")"
    put "$f" "/$rel" "$mode"
    n=$((n+1))
done < <(find "$X" -type f)
echo "[lantern-iso] wrote $n package files"
rm -rf "$X"

# 2. Cap policy: lantern and its RequestServer need NET_SOCKET (per the manifest;
#    the other engine processes talk AF_UNIX and need only the baseline).
printf 'service NET_SOCKET\n' > /tmp/l.caps
put /tmp/l.caps "/etc/aegis/caps.d/lantern"       0100644
put /tmp/l.caps "/etc/aegis/caps.d/RequestServer" 0100644

# 3. Auto-launch vigil service (graphical, oneshot, gated on the `lanterndemo`
#    cmdline token) — execs the launcher arg-less after the compositor is up.
sd=/etc/vigil/services/lanterndemo
printf 'lanterndemo'            > /tmp/l.cmdline
printf 'graphical'             > /tmp/l.mode
printf 'oneshot'              > /tmp/l.policy
printf '/apps/lantern/lantern' > /tmp/l.run
printf 'root'                 > /tmp/l.user
put /tmp/l.cmdline "$sd/cmdline" 0100644
put /tmp/l.mode    "$sd/mode"    0100644
put /tmp/l.policy  "$sd/policy"  0100644
put /tmp/l.run     "$sd/run"     0100644
put /tmp/l.user    "$sd/user"    0100644

# Truncate trailing zero blocks (kernel re-expands from the superblock).
python3 - "$OUT" <<'PYEOF'
import os, sys
p = sys.argv[1]; d = open(p,'rb').read(); i = len(d)
while i > 0 and d[i-1] == 0: i -= 1
os.truncate(p, max(4096, ((i+4095)//4096)*4096))
PYEOF
echo "[lantern-iso] rootfs -> $OUT ($(du -h "$OUT" | cut -f1))"

# 4. ISO: graphical autologin + the lanterndemo token.
D=build/lantern-isodir
rm -rf "$D"; mkdir -p "$D/boot/limine" "$D/EFI/BOOT"
cp build/aegis-stripped.elf "$D/boot/aegis.elf"
cp "$OUT" "$D/boot/rootfs.img"
cp build/esp-desktop.img "$D/boot/esp.img"
cat > "$D/boot/limine/limine.conf" <<CONF
timeout: 0

/LoricaOS (lantern)
    protocol: limine
    path: boot():/boot/aegis.elf
    module_path: boot():/boot/rootfs.img
    module_path: boot():/boot/esp.img
    cmdline: boot=graphical quiet bastion_autologin=live lanterndemo aegis_live=1
CONF
cp tools/limine/limine-bios.sys tools/limine/limine-bios-cd.bin tools/limine/limine-uefi-cd.bin "$D/boot/limine/"
cp tools/limine/BOOTX64.EFI tools/limine/BOOTIA32.EFI "$D/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label "$D" -o "$ISO" >/dev/null 2>&1
build/limine/limine bios-install "$ISO" >/dev/null 2>&1
echo "[lantern-iso] $ISO ($(du -h "$ISO" | cut -f1))"
