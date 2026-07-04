#!/usr/bin/env bash
# self-host-iso.sh — build a server ISO that, at boot, uses make + tcc ON AEGIS
# to build a real multi-file, libc-using C program, then runs it. The full
# "you can build software on this OS" proof of self-hosting.
#
# Assembles the rootfs from a directory tree: debugfs rdump of the server rootfs
# + an overlay of the on-device toolchain (tcc + make + musl libc/crt/headers +
# the freshly-built stsh) + the test project, then mke2fs -d. Run on CT117.
#
# Needs: make server-iso, tools/build-tcc.sh (build/tcc-install), tools/
# build-make.sh (build/make-install), and build/musl-dynamic (the app musl).
set -eu
REPO="$(git rev-parse --show-toplevel)"; cd "$REPO"

TCC="$REPO/build/tcc-install/usr/local"
MAKE="$REPO/build/make-install/make"
MUSL="$REPO/build/musl-dynamic/usr"
STSH="$REPO/user/bin/stsh/stsh.elf"
ROOTDIR=/tmp/aegisroot
OUT=build/rootfs-selfhost.img
ISO=build/loricaos-selfhost.iso
SIZE_KIB=196608   # 192 MiB

[ -f build/rootfs-server.img ] || { echo "need build/rootfs-server.img (make server-iso)"; exit 1; }
[ -x "$TCC/bin/tcc" ] || { echo "need $TCC/bin/tcc (bash tools/build-tcc.sh)"; exit 1; }
[ -x "$MAKE" ] || { echo "need $MAKE (bash tools/build-make.sh)"; exit 1; }

# 1. Extract the server rootfs to a directory.
rm -rf "$ROOTDIR"; mkdir -p "$ROOTDIR"
debugfs -R "rdump / $ROOTDIR" build/rootfs-server.img >/dev/null 2>&1

# 2. Overlay the on-device toolchain (+ the freshly-built stsh).
install -Dm755 "$TCC/bin/tcc" "$ROOTDIR/bin/tcc"
mkdir -p "$ROOTDIR/usr/local/lib"
cp -r "$TCC/lib/tcc" "$ROOTDIR/usr/local/lib/tcc"
install -Dm755 "$MAKE" "$ROOTDIR/bin/make"
[ -f "$STSH" ] && install -Dm755 "$STSH" "$ROOTDIR/bin/stsh"

# 3. Overlay musl. tcc's baked-in crt path is the multiarch dir
#    /usr/lib/x86_64-linux-gnu; its library search covers that + /usr/lib, and
#    its sysinclude covers /usr/include. Stage crt + static libc in both libdirs.
mkdir -p "$ROOTDIR/usr/lib/x86_64-linux-gnu" "$ROOTDIR/usr/include"
cp "$MUSL"/lib/*.a "$MUSL"/lib/*.o "$ROOTDIR/usr/lib/x86_64-linux-gnu/" 2>/dev/null || true
cp "$MUSL"/lib/*.a "$MUSL"/lib/*.o "$ROOTDIR/usr/lib/" 2>/dev/null || true
cp -r "$MUSL"/include/* "$ROOTDIR/usr/include/"

# 4. A real multi-file, libc-using test project + a build script.
mkdir -p "$ROOTDIR/selfhost"
cat > "$ROOTDIR/selfhost/util.c" <<'EOF'
int util_sum(int n){ int s=0; for(int i=1;i<=n;i++) s+=i; return s; }
EOF
cat > "$ROOTDIR/selfhost/main.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
extern int util_sum(int);
int main(void){
    int *p = malloc(sizeof(int)*4);          /* exercise the heap/libc */
    p[0] = util_sum(10);
    fprintf(stderr, "SELFHOST_OK make+tcc built this on Aegis: sum=%d\n", p[0]);
    free(p);
    return 0;
}
EOF
cat > "$ROOTDIR/selfhost/Makefile" <<'EOF'
CC = tcc
hello: main.o util.o
	$(CC) -static main.o util.o -o hello
main.o: main.c
	$(CC) -c main.c -o main.o
util.o: util.c
	$(CC) -c util.c -o util.o
EOF
cat > "$ROOTDIR/selfhost/build.sh" <<'EOF'
cd /selfhost
export PATH=/bin:/usr/bin:/usr/local/bin
echo "[SELFHOST] make + tcc building a real libc program on Aegis" >&2
make >&2
/selfhost/hello
EOF

# 5. Boot-run vigil service, gated on the `selfhost` cmdline token. A non-slash
#    run string is executed via sh -c, so `stsh <script>` runs the test.
sd="$ROOTDIR/etc/vigil/services/selfhost"; mkdir -p "$sd"
printf 'selfhost'                > "$sd/cmdline"
printf 'root'                    > "$sd/user"
printf 'stsh /selfhost/build.sh' > "$sd/run"
printf 'oneshot'                 > "$sd/policy"

# 6. Build the ext2 image from the directory, then wrap the ISO.
rm -f "$OUT"
mke2fs -q -F -t ext2 -b 1024 -d "$ROOTDIR" "$OUT" "${SIZE_KIB}" 2>&1 | tail -1 || true
python3 - "$OUT" <<'PYEOF'
import os,sys
p=sys.argv[1]; d=open(p,'rb').read(); i=len(d)
while i>0 and d[i-1]==0: i-=1
os.truncate(p, max(4096, ((i+4095)//4096)*4096))
PYEOF
echo "[selfhost] rootfs -> $OUT ($(du -h "$OUT"|cut -f1))"

D=build/selfhost-isodir
rm -rf "$D"; mkdir -p "$D/boot/limine" "$D/EFI/BOOT"
cp build/aegis-stripped.elf "$D/boot/aegis.elf"
cp "$OUT" "$D/boot/rootfs.img"
cp build/esp-server.img "$D/boot/esp.img"
sh tools/gen-limine-conf.sh ffsmoke | sed 's/ffsmoke/selfhost/g' > "$D/boot/limine/limine.conf"
cp tools/limine/limine-bios.sys tools/limine/limine-bios-cd.bin tools/limine/limine-uefi-cd.bin "$D/boot/limine/"
cp tools/limine/BOOTX64.EFI tools/limine/BOOTIA32.EFI "$D/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label "$D" -o "$ISO" >/dev/null 2>&1
build/limine/limine bios-install "$ISO" >/dev/null 2>&1 || true
echo "[selfhost] $ISO ($(du -h "$ISO"|cut -f1))"
