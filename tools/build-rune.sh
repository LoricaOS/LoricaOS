#!/bin/bash
# build-rune.sh — Build rune text editor as a static x86-64 binary.
#
# Output: user/bin/rune (ET_EXEC, statically linked, musl)
#
# Must run on Linux with Rust + musl target installed.
# Install musl target: rustup target add x86_64-unknown-linux-musl

set -euo pipefail

RUNE_REPO="https://github.com/exec/rune.git"
RUNE_SRC="references/rune"
RUNE_BIN="user/bin/rune"

# Skip if already built
if [ -f "$RUNE_BIN" ]; then
    echo "[rune] binary already exists, skipping."
    exit 0
fi

# Clone or update. Check for Cargo.toml, not just directory existence —
# a cache restore might create references/rune/target/ without the source.
if [ -f "$RUNE_SRC/Cargo.toml" ] && [ -d "$RUNE_SRC/.git" ]; then
    echo "[rune] updating existing checkout..."
    git -C "$RUNE_SRC" pull --ff-only || true
else
    echo "[rune] cloning from $RUNE_REPO..."
    # Preserve target/ across clone for cache benefit
    if [ -d "$RUNE_SRC/target" ]; then
        mv "$RUNE_SRC/target" "${RUNE_SRC}.target.tmp"
        rm -rf "$RUNE_SRC"
        git clone "$RUNE_REPO" "$RUNE_SRC"
        mv "${RUNE_SRC}.target.tmp" "$RUNE_SRC/target"
    else
        rm -rf "$RUNE_SRC"
        git clone "$RUNE_REPO" "$RUNE_SRC"
    fi
fi

# Ensure musl target is installed
rustup target add x86_64-unknown-linux-musl 2>/dev/null || true

# Build as non-PIE static binary (ET_EXEC for Aegis ELF loader compatibility)
echo "[rune] building for x86_64-unknown-linux-musl (static, non-PIE)..."
cd "$RUNE_SRC"
# Size-optimized release: opt-level=z (size), fat LTO + 1 codegen unit (cross-
# crate dead-code elim), panic=abort (drops unwinding tables/landing pads —
# Aegis doesn't catch_unwind). Overrides the repo's [profile.release] via env so
# the external rune checkout needs no patching. Shrinks the static musl binary
# substantially (~5.9 MiB -> ~3 MiB) at a small runtime-speed cost — fine for an
# editor, and rune is the single biggest file in the rootfs.
CARGO_PROFILE_RELEASE_OPT_LEVEL=z \
CARGO_PROFILE_RELEASE_LTO=true \
CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \
CARGO_PROFILE_RELEASE_PANIC=abort \
RUSTFLAGS="-C target-feature=+crt-static -C relocation-model=static" \
    cargo build --release --target x86_64-unknown-linux-musl

cd - > /dev/null
cp "$RUNE_SRC/target/x86_64-unknown-linux-musl/release/rune" "$RUNE_BIN"
echo "[rune] built: $RUNE_BIN ($(du -h "$RUNE_BIN" | cut -f1))"
