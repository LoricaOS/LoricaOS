#!/bin/bash
# tools/digitalocean-setup.sh — provision a Debian 13 (trixie) build box for Aegis
#
# Usage:
#   Paste as DigitalOcean user-data, or run manually on a fresh Debian 13 droplet:
#     curl -sSf <url> | bash
#     # or
#     bash tools/digitalocean-setup.sh
#
# Assumes: root, Debian 13 (trixie), x86_64
set -euo pipefail

echo "[aegis-setup] starting..."

# ── apt packages ─────────────────────────────────────────────────────────────
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    git make nasm gcc g++ \
    xorriso mtools dosfstools \
    qemu-system-x86 \
    musl-tools \
    gdisk e2fsprogs e2tools \
    rsync wget \
    python3-pil \
    autoconf automake libtool

echo "[aegis-setup] apt packages installed"

# ── cross-compiler symlinks ──────────────────────────────────────────────────
# Aegis Makefile expects x86_64-elf-gcc; Debian ships x86_64-linux-gnu-gcc.
# Functionally equivalent with -ffreestanding -nostdlib.
for tool in gcc ld objcopy objdump nm; do
    ln -sf "/usr/bin/x86_64-linux-gnu-$tool" "/usr/local/bin/x86_64-elf-$tool"
done
echo "[aegis-setup] x86_64-elf-* symlinks created"

# ── rust (nightly + no_std target) ────��──────────────────────────────────────
if ! command -v rustup &>/dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain nightly
fi
. "$HOME/.cargo/env"
rustup default nightly
rustup component add rust-src
rustup target add x86_64-unknown-none
echo "[aegis-setup] rust nightly + x86_64-unknown-none ready"

# ── project directory ────────────────────────────────────────────────────────
mkdir -p ~/Developer/aegis
git config --global --add safe.directory ~/Developer/aegis

# ── fetch bearssl source ──────────────────────────────────���──────────────────
BEARSSL_DIR=~/Developer/aegis/references/bearssl-0.6
if [ ! -f "$BEARSSL_DIR/inc/bearssl.h" ]; then
    mkdir -p ~/Developer/aegis/references
    wget -qO /tmp/bearssl-0.6.tar.gz https://bearssl.org/bearssl-0.6.tar.gz
    tar xzf /tmp/bearssl-0.6.tar.gz -C ~/Developer/aegis/references/
    rm -f /tmp/bearssl-0.6.tar.gz
    echo "[aegis-setup] bearssl 0.6 fetched"
fi

# ── fetch curl source ────────────────────────────────────────────────────────
CURL_DIR=~/Developer/aegis/references/curl
if [ ! -f "$CURL_DIR/configure" ]; then
    mkdir -p ~/Developer/aegis/references
    wget -qO /tmp/curl.tar.gz https://curl.se/download/curl-8.12.1.tar.gz
    tar xzf /tmp/curl.tar.gz -C ~/Developer/aegis/references/
    mv ~/Developer/aegis/references/curl-8.12.1 "$CURL_DIR"
    rm -f /tmp/curl.tar.gz
    echo "[aegis-setup] curl 8.12.1 fetched"
fi

echo "[aegis-setup] done — rsync the repo to ~/Developer/aegis and run: make iso"
