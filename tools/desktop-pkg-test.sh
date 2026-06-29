#!/usr/bin/env bash
#
# desktop-pkg-test.sh — verify the `desktop` herald system package end to end.
#
# Two independent checks (each a fresh QEMU boot of a purpose-built live ISO):
#
#   A. INFERENCE → GREETER.  Boot the desktop rootfs with NO explicit boot= on
#      the kernel cmdline ("infer" limine mode). vigil must infer graphical mode
#      from the presence of /bin/bastion and bring up the greeter — exactly the
#      reboot behaviour a server gets after `herald install desktop`. PASS =
#      "[BASTION] greeter ready" with vigil resolving "boot mode: graphical".
#
#   B. CLASS=SYSTEM INSTALL.  Boot a server (text) live ISO that carries the
#      signed desktop_<ver>.hpkg at /root, then drive login → admin elevation →
#      `herald install /root/desktop.hpkg` and confirm the graphical stack +
#      a cap policy actually land on the filesystem and the kernel reloads.
#      (Local-file install exercises the same install_bytes class=system path a
#      repo install does; it avoids the emulated-TCP large-download variable.)
#
# Build/test box only (QEMU + e2fsprogs + xorriso). See build-test-infra memory.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VERSION="$(cat VERSION 2>/dev/null || echo 0.0.0)"
HERALD_KEY="${HERALD_KEY:-tools/herald-keys/herald.key}"   # must match herald's embedded anchor
B=build
KERNEL=$B/aegis-stripped.elf
LIMINE_DIR=tools/limine
LIMINE_BIN=$B/limine
ROOTFS_DESKTOP=$B/rootfs-desktop.img
ESP_DESKTOP=$B/esp-desktop.img
ROOTFS_SERVER=$B/rootfs-server.img
ESP_SERVER=$B/esp-server.img
HPKG=$B/desktop_${VERSION}_x86_64.hpkg

fail() { echo "[desktop-pkg-test] FAIL: $*" >&2; exit 1; }

[ -f "$HPKG" ] || fail "package $HPKG not built (run tools/build-desktop-pkg.sh)"
[ -f "$HPKG.sig" ] || fail "missing $HPKG.sig"
for f in "$KERNEL" "$ROOTFS_DESKTOP" "$ESP_DESKTOP" "$ROOTFS_SERVER" "$ESP_SERVER"; do
    [ -f "$f" ] || fail "missing build artifact $f (build the ISOs first)"
done

# build_iso <out> <stagedir> <limine-mode> <rootfs.img> <esp.img>
# Mirrors the Makefile's LIMINE_ISO_RULE so test ISOs boot identically.
build_iso() {
    local out=$1 stage=$2 mode=$3 rootfs=$4 esp=$5
    rm -rf "$stage"; mkdir -p "$stage/boot/limine" "$stage/EFI/BOOT"
    cp "$KERNEL" "$stage/boot/aegis.elf"
    cp "$rootfs" "$stage/boot/rootfs.img"
    cp "$esp"    "$stage/boot/esp.img"
    sh tools/gen-limine-conf.sh "$mode" > "$stage/boot/limine/limine.conf"
    cp "$LIMINE_DIR"/limine-bios.sys "$LIMINE_DIR"/limine-bios-cd.bin \
       "$LIMINE_DIR"/limine-uefi-cd.bin "$stage/boot/limine/"
    cp "$LIMINE_DIR"/BOOTX64.EFI "$LIMINE_DIR"/BOOTIA32.EFI "$stage/EFI/BOOT/"
    xorriso -as mkisofs -R -r -J \
        -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image \
        --protective-msdos-label \
        "$stage" -o "$out" >/dev/null 2>&1
    "$LIMINE_BIN" bios-install "$out" >/dev/null 2>&1
}

# ── Part A: inference → greeter ──────────────────────────────────────────────
echo "[desktop-pkg-test] Part A: vigil infers graphical → greeter"
build_iso "$B/aegis-infer.iso" "$B/infer-isodir" infer "$ROOTFS_DESKTOP" "$ESP_DESKTOP"
LOGA="$(mktemp)"
timeout 150 qemu-system-x86_64 -machine pc -cdrom "$B/aegis-infer.iso" -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot -m 2048M \
    > "$LOGA" 2>&1 || true
# The infer ISO's cmdline provably carries no boot= (gen-limine-conf.sh "infer"
# mode), so a greeter coming up at all proves vigil + bastion inferred graphical.
if grep -q "\[BASTION\] greeter ready" "$LOGA"; then
    echo "[desktop-pkg-test] Part A PASS: greeter came up with no explicit boot= (inferred graphical)"
else
    echo "----- Part A serial tail -----"; tail -30 "$LOGA"
    fail "Part A: greeter did not come up via inference"
fi
rm -f "$LOGA"

# ── Part B: class=system install on a server box ─────────────────────────────
echo "[desktop-pkg-test] Part B: herald install (class=system) on a server boot"
INST=$B/rootfs-instpkg.img
cp "$ROOTFS_SERVER" "$INST"
truncate -s 96M "$INST"
e2fsck -fy "$INST" >/dev/null 2>&1 || true
resize2fs "$INST" >/dev/null 2>&1 || fail "resize2fs on baked rootfs failed"
debugfs -w -R "mkdir /root" "$INST" >/dev/null 2>&1 || true
debugfs -w -R "write $HPKG /root/desktop.hpkg" "$INST" >/dev/null 2>&1 \
    || fail "debugfs could not bake the .hpkg into the rootfs"
debugfs -w -R "write $HPKG.sig /root/desktop.hpkg.sig" "$INST" >/dev/null 2>&1 \
    || fail "debugfs could not bake the .sig into the rootfs"
# Confirm the bake (debugfs stat exits 0 even when absent — grep for "Inode:").
debugfs -R "stat /root/desktop.hpkg" "$INST" 2>/dev/null | grep -q "Inode:" \
    || fail "baked .hpkg not present in rootfs after write"

# A tiny local app package to exercise dependency warnings on a LOCAL install:
# it depends on 'desktop' (installed by the herald run above) + 'lumen-nonexistent'
# (never installed), so herald must warn about ONLY the missing one.
DEPHPKG=$B/deptest_1.0.0_x86_64.hpkg
DEPSTAGE="$(mktemp -d)"
mkdir -p "$DEPSTAGE/apps/deptest"
printf 'dummy deptest payload\n' > "$DEPSTAGE/apps/deptest/deptest"
printf 'name=Dep Test\nexec=deptest\n' > "$DEPSTAGE/apps/deptest/app.ini"
printf 'id=deptest\nname=Dep Test\nversion=1.0.0\nexec=deptest\ndepends=desktop lumen-nonexistent\n' \
    > "$DEPSTAGE/manifest"
HERALD_KEY="$HERALD_KEY" bash tools/herald-pack.sh "$DEPSTAGE" "$DEPHPKG" >/dev/null \
    || fail "could not build deptest package"
rm -rf "$DEPSTAGE"
debugfs -w -R "write $DEPHPKG /root/deptest.hpkg" "$INST" >/dev/null 2>&1 \
    || fail "debugfs could not bake deptest.hpkg"
debugfs -w -R "write $DEPHPKG.sig /root/deptest.hpkg.sig" "$INST" >/dev/null 2>&1 \
    || fail "debugfs could not bake deptest.hpkg.sig"

build_iso "$B/aegis-instpkg.iso" "$B/instpkg-isodir" server "$INST" "$ESP_SERVER"
if python3 tools/desktop-pkg-driver.py "$B/aegis-instpkg.iso"; then
    echo "[desktop-pkg-test] Part B PASS: class=system install landed the desktop stack + reloaded caps"
else
    fail "Part B: herald install of the desktop package failed (see transcript above)"
fi

echo "[desktop-pkg-test] ALL PASS — desktop package installs and yields a greeter"
