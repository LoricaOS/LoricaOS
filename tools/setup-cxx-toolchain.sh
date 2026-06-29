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
# Idempotent: skips download if /opt/aegis-cxx already resolves to a g++.
# Installs OUTSIDE the repo (/opt) so `git clean -fdx` / `make clean` never
# nuke it — it's a system toolchain like x86_64-elf-gcc, not a build artifact.
set -e

PREFIX=/opt/aegis-cxx
GXX="$PREFIX/bin/x86_64-buildroot-linux-musl-g++"
TARBALL=x86-64--musl--bleeding-edge-2025.08-1.tar.xz
URL=https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/$TARBALL
EXTRACT_DIR=/opt/xtc/x86-64--musl--bleeding-edge-2025.08-1

if [ -x "$GXX" ]; then
    echo "[cxx-toolchain] already present: $("$GXX" --version | head -1)"
    exit 0
fi

echo "[cxx-toolchain] provisioning Bootlin musl GCC 15.1 -> $PREFIX"
mkdir -p /opt/xtc
cd /opt/xtc
if [ ! -d "$EXTRACT_DIR" ]; then
    curl -fSL --max-time 600 -o "$TARBALL" "$URL"
    tar xf "$TARBALL"
    rm -f "$TARBALL"
fi
ln -sfn "$EXTRACT_DIR" "$PREFIX"
echo "[cxx-toolchain] done: $("$GXX" --version | head -1)"
