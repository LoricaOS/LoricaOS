#!/bin/bash
# tools/fetch-tcc.sh — fetch the TinyCC (tcc) source into references/tinycc/.
#
# tcc is a small, self-contained C compiler + assembler + linker. LoricaOS
# builds it static-musl (tools/build-tcc.sh) as the first on-device C toolchain
# — the keystone of self-hosting (a compiler that runs on Aegis and emits
# working Aegis binaries). See the self-hosting roadmap.
#
# We track the mob (development) branch at a pinned commit: the last tagged
# release (0.9.27, 2017) predates the R_X86_64 TLS-relocation support that
# linking against modern musl's libc.a requires; the mob branch handles it and
# builds cleanly against musl with --config-musl.
set -e
REPO="$(git rev-parse --show-toplevel)"
PIN="${TCC_COMMIT:-a338258}"
SRC="$REPO/references/tinycc"

if [ -f "$SRC/configure" ] && git -C "$SRC" rev-parse --verify "$PIN^{commit}" >/dev/null 2>&1; then
    git -C "$SRC" checkout -q "$PIN" 2>/dev/null || true
    echo "[tcc] source present at references/tinycc/ ($PIN) — skip"
    exit 0
fi

mkdir -p "$REPO/references"
rm -rf "$SRC"
git clone https://github.com/TinyCC/tinycc.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
echo "[tcc] source cloned to references/tinycc/ (pinned $PIN)"
