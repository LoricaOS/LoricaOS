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
MUSL_SRC="references/musl-${MUSL_VER}"
DESTDIR="$(pwd)/build/musl-dynamic"

# Skip if already built AND correct architecture
if [ -f "${DESTDIR}/usr/lib/libc.so" ]; then
    if file "${DESTDIR}/usr/lib/libc.so" | grep -q "x86-64"; then
        echo "[build-musl] libc.so already exists (x86-64), skipping."
        exit 0
    else
        echo "[build-musl] WARNING: libc.so exists but is NOT x86-64 — rebuilding!"
        rm -rf "${DESTDIR}"
    fi
fi

# Download if absent
if [ ! -d "${MUSL_SRC}" ]; then
    mkdir -p references
    if [ ! -f "${MUSL_TAR}" ]; then
        echo "[build-musl] Downloading musl ${MUSL_VER}..."
        curl -L -o "${MUSL_TAR}" "${MUSL_URL}"
    fi
    echo "[build-musl] Extracting..."
    tar -xzf "${MUSL_TAR}" -C references/
fi

# Aegis fix (#66): musl's res_msend.c mtime() uses CLOCK_REALTIME for its DNS
# retry/timeout math (unsigned millisecond deltas). Aegis CLOCK_REALTIME carries an
# NTP/RTC-settable epoch offset that can jump BACKWARD (clock sync / RTC seed),
# underflowing those deltas and stalling/flaking the resolver. CLOCK_MONOTONIC
# (raw PIT ticks — never jumps; kernel sys_clock_gettime clk_id=1) is the correct
# source for an interval timer. mtime() is the only CLOCK_REALTIME user in this file.
# Applied every build and idempotent (no-op once patched) so it also catches an
# already-extracted source tree persisted under references/ (which `git clean
# -fdx --exclude=references` keeps across nuclear rebuilds).
RES_MSEND="${MUSL_SRC}/src/network/res_msend.c"
if [ -f "${RES_MSEND}" ] && grep -q 'CLOCK_REALTIME' "${RES_MSEND}"; then
    echo "[build-musl] Patching res_msend.c CLOCK_REALTIME -> CLOCK_MONOTONIC (Aegis #66)..."
    sed -i 's/CLOCK_REALTIME/CLOCK_MONOTONIC/g' "${RES_MSEND}"
fi

# Configure — nuke stale config.mak if it targets the wrong architecture
cd "${MUSL_SRC}"
if [ -f config.mak ]; then
    if ! grep -q 'ARCH = x86_64' config.mak; then
        echo "[build-musl] WARNING: config.mak targets wrong arch — reconfiguring!"
        rm -f config.mak
        rm -rf obj lib
    fi
fi
if [ ! -f config.mak ]; then
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
fi

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

echo "[build-musl] Done. libc.so at ${DESTDIR}/usr/lib/libc.so"
