#!/bin/bash
# build-musl.sh — Build musl as a shared library for dynamic linking.
#
# Output:
#   build/musl-dynamic/lib/libc.so                — shared library + interpreter
#   build/musl-dynamic/lib/ld-musl-x86_64.so.1    — symlink to libc.so
#   build/musl-dynamic/usr/bin/musl-gcc            — gcc wrapper for dynamic linking
#   build/musl-dynamic/usr/lib/musl-gcc.specs      — gcc specs file
#
# Must run on Linux (musl builds for the host architecture).

set -euo pipefail

MUSL_VER=1.2.5
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
MUSL_TAR="references/musl-${MUSL_VER}.tar.gz"
MUSL_SRC="build/musl-aegis-source/musl-${MUSL_VER}"
DESTDIR="$(pwd)/build/musl-dynamic"
PATCH_HELPER="tools/patch-musl-aegis.py"
# Hash the full recipe, including the exact-source patch helper. A libc built
# before this migration (no stamp), or with any different patch, must rebuild.
RECIPE_ID="$(python3 - "$0" "$PATCH_HELPER" <<'PY'
import hashlib
from pathlib import Path
import sys
h = hashlib.sha256()
for name in sys.argv[1:]:
    h.update(Path(name).read_bytes())
    h.update(b'\0')
print(h.hexdigest())
PY
)"
STAMP="${DESTDIR}/.aegis-recipe"

# Skip only a successful build of this exact recipe AND correct architecture.
if [ -f "${DESTDIR}/usr/lib/libc.so" ]; then
    if file "${DESTDIR}/usr/lib/libc.so" | grep -q "x86-64" &&
       [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$RECIPE_ID" ]; then
        echo "[build-musl] libc.so already exists (x86-64, matching Aegis recipe), skipping."
        exit 0
    fi
    echo "[build-musl] Architecture or Aegis recipe changed — rebuilding!"
fi

# Download if absent. Never trust an existing extracted tree or unchecked tar.
mkdir -p references
if [ ! -f "${MUSL_TAR}" ]; then
    echo "[build-musl] Downloading musl ${MUSL_VER}..."
    curl -fL -o "${MUSL_TAR}.tmp" "${MUSL_URL}"
    mv "${MUSL_TAR}.tmp" "${MUSL_TAR}"
fi
python3 - "$MUSL_TAR" "$PATCH_HELPER" <<'PY'
import hashlib
from pathlib import Path
import runpy
import sys
expected = runpy.run_path(sys.argv[2])["TARBALL_SHA256"]
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest() != expected:
    sys.exit("[build-musl] musl 1.2.5 tarball SHA256 mismatch")
PY

# Re-extract on recipe changes: stale objects/configuration and old patch output
# must not survive. Leave any references/musl-* tree alone (not a build input).
rm -rf build/musl-aegis-source
mkdir -p build/musl-aegis-source
tar -xzf "${MUSL_TAR}" -C build/musl-aegis-source/
python3 "$PATCH_HELPER" "${MUSL_SRC}"
# Keep the old installation if source verification fails, but remove it before
# installing so stale libraries cannot survive. No stamp until every step passes.
rm -rf "${DESTDIR}"

# Configure the fresh, patched source.
cd "${MUSL_SRC}"
echo "[build-musl] Configuring..."
# -march=x86-64: pin the baseline x86-64 ISA (SSE2 only). Without it the
# build inherits the host gcc's default -march, so building on a modern box
# (e.g. a Zen4 workstation with -march=native) bakes AVX/AVX-512 into libc —
# binaries that then #UD on any older or emulated target, notably the
# microVM's baseline CPU. Pinning here makes the userland portable
# regardless of build host. Raise to x86-64-v2/v3 only if every target CPU
# (including the microVM's) is guaranteed to support it.
./configure \
    --prefix=/usr \
    --syslibdir=/lib \
    --enable-shared \
    CFLAGS="-O2 -fno-pie -march=x86-64"

# Build
echo "[build-musl] Building..."
make -j"$(nproc)"

# Install to DESTDIR
echo "[build-musl] Installing to ${DESTDIR}..."
make install DESTDIR="${DESTDIR}"

# Fix up paths: specs file and wrapper reference /usr/{lib,include} but we
# installed to $DESTDIR/usr/{lib,include}. Patch them to use absolute DESTDIR paths.
echo "[build-musl] Fixing up specs and wrapper paths..."
SPECS="${DESTDIR}/usr/lib/musl-gcc.specs"
# Idempotent: only rewrite the pristine /usr paths once. Re-running the sed on an
# already-patched specs doubles the DESTDIR prefix (…/musl-dynamic/…/musl-dynamic/
# usr/include) and breaks the header search path, so guard on the patched path.
if ! grep -q "${DESTDIR}/usr/lib" "$SPECS"; then
    sed -i "s|/usr/lib|${DESTDIR}/usr/lib|g; s|/usr/include|${DESTDIR}/usr/include|g" "$SPECS"
fi
# Also fix the dynamic linker path in specs to point to DESTDIR for HOST linking
# (the -dynamic-linker /lib/ld-musl-x86_64.so.1 stays as /lib/ — that's the RUNTIME path
# inside the guest kernel, not the host path)

# Fix the wrapper to point to our specs file
WRAPPER="${DESTDIR}/usr/bin/musl-gcc"
# -march=x86-64 pins the baseline ISA for everything compiled through the
# wrapper too (not just libc), so user binaries stay portable on any build host.
# It precedes "$@", so an explicit -march on a caller's command line still wins.
cat > "$WRAPPER" << WEOF
#!/bin/sh
exec "\${REALGCC:-gcc}" -march=x86-64 "\$@" -specs "${SPECS}"
WEOF
chmod +x "$WRAPPER"

printf '%s\n' "$RECIPE_ID" > "$STAMP"
echo "[build-musl] Done. libc.so at ${DESTDIR}/usr/lib/libc.so"
