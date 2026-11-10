#!/bin/sh
# setup-cxx-toolchain.sh — provision the Aegis C++ cross toolchain.
#
# Aegis userland is C/musl with zero C++ historically. The browser port
# (Ladybird) needs a modern C++ stdlib. Rather than build musl-cross-make from
# source (an hour+), we use a prebuilt Bootlin x86-64 musl toolchain: GCC 15.1
# with static libstdc++ + libgcc_eh (exceptions/RTTI) built against musl. Its
# static binaries are x86_64-linux-musl ELFs that run unmodified on Aegis (the
# C++ runtime's syscall surface — arch_prctl/set_tid_address/writev/mmap/brk/
# ioctl/exit_group — is already implemented by the kernel).
#
# Idempotent: skips download if the selected prefix already resolves to a g++.
# Defaults to /opt. Set CXX_TOOLCHAIN_PREFIX and CXX_TOOLCHAIN_CACHE to absolute
# workspace-local paths for non-root, disposable builds.
set -e

PREFIX="${CXX_TOOLCHAIN_PREFIX:-/opt/aegis-cxx}"
CACHE="${CXX_TOOLCHAIN_CACHE:-/opt/xtc}"
GXX="$PREFIX/bin/x86_64-buildroot-linux-musl-g++"
TARBALL=x86-64--musl--bleeding-edge-2025.08-1.tar.xz
URL=https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/$TARBALL
EXTRACT_DIR="$CACHE/x86-64--musl--bleeding-edge-2025.08-1"

case "$PREFIX" in /*) ;; *) echo "[cxx-toolchain] prefix must be absolute" >&2; exit 1;; esac
case "$CACHE" in /*) ;; *) echo "[cxx-toolchain] cache must be absolute" >&2; exit 1;; esac

if [ -x "$GXX" ]; then
    echo "[cxx-toolchain] already present: $GXX"
    "$GXX" --version
    exit 0
fi
if [ -e "$PREFIX" ] && [ ! -L "$PREFIX" ]; then
    echo "[cxx-toolchain] refusing to replace existing directory: $PREFIX" >&2
    exit 1
fi

echo "[cxx-toolchain] provisioning Bootlin musl GCC 15.1 -> $PREFIX"
mkdir -p "$CACHE" "$(dirname "$PREFIX")"
cd "$CACHE"
if [ ! -d "$EXTRACT_DIR" ]; then
    curl -fSL --max-time 600 -o "$TARBALL" "$URL"
    tar xf "$TARBALL"
    rm -f "$TARBALL"
fi
ln -sfn "$EXTRACT_DIR" "$PREFIX"
echo "[cxx-toolchain] done: $GXX"
"$GXX" --version
