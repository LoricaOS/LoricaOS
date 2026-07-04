#!/bin/bash
# tools/build-make.sh — build GNU make static-musl for on-device use.
#
# CRITICAL: build it to spawn children with plain fork()+exec, NOT posix_spawn
# and NOT vfork. Aegis's clone() rejects the CLONE_VM|CLONE_VFORK flag combo
# musl's posix_spawn uses (exec fails ENOSYS), and its vfork+exec path also
# fails to run the child (exec fails 127). Forcing ac_cv_func_posix_spawn=no and
# ac_cv_func_vfork=no makes make use fork(), which works on Aegis exactly as it
# does for every other program. (A cleaner long-term fix is to teach the kernel
# the posix_spawn/vfork clone flags — see the self-hosting roadmap.)
set -e
REPO="${REPO:-$(git rev-parse --show-toplevel)}"
CC="${CC:-musl-gcc}"
SRC="$REPO/references/make"
OUT="$REPO/build/make-install"

[ -f "$SRC/configure" ] || bash "$REPO/tools/fetch-make.sh"

cd "$SRC"
CC="$CC" CFLAGS="-O2 -static" LDFLAGS="-static" ./configure \
    --host=x86_64-linux-musl \
    --disable-nls \
    ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no \
    ac_cv_func_vfork=no ac_cv_func_vfork_works=no ac_cv_func_fork_works=yes

make
rm -rf "$OUT"; mkdir -p "$OUT"
cp make "$OUT/make"
echo "[make] built -> $OUT/make"
"$OUT/make" --version | head -1
