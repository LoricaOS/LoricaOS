#!/bin/bash
# tools/build-tinyssh.sh — build TinySSH (tinysshd) as a static musl binary for Aegis.
#
# TinySSH's build runs compile-and-run feature detection (cryptoint/, randombytes),
# so it must be built with a HOST-native musl toolchain (/usr/bin/musl-gcc), not the
# Aegis cross/dynamic toolchain. The resulting fully-static binary is self-contained
# and runs on Aegis (kernel provides /dev/urandom, getrandom, /dev/ptmx, sockets).
set -e

REPO="${REPO:-$(git rev-parse --show-toplevel)}"
OUT="$REPO/build/tinyssh"
TINYSSH_COMMIT="6a60c2aac18da9035a7401316789d6f4df7e295f"
TINYSSH_SHA256="14cec5eaeab63a40322381f16ed4a9128e85419d6c60e7d0a2ffb81ef4226d87"
TINYSSH_URL="https://codeload.github.com/janmojzis/tinyssh/tar.gz/$TINYSSH_COMMIT"
ARCHIVE="$OUT/tinyssh-$TINYSSH_COMMIT.tar.gz"

# Source reproducibility only: strict Aegis SSH authentication/session launch is
# still blocked. See docs/tinyssh-strict-aegis.md before claiming SSH support.
# Never reuse a mutable/patched references checkout. Verify even cached archives
# and apply the compatibility patches to a fresh build-owned extraction.
mkdir -p "$OUT"
WORK="$(mktemp -d "$OUT/.source.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
INPUT="$ARCHIVE"
if [ ! -f "$ARCHIVE" ]; then
    INPUT="$WORK/source.tar.gz"
    echo "[tinyssh] fetching $TINYSSH_COMMIT ..."
    curl -fL --proto '=https' --tlsv1.2 "$TINYSSH_URL" -o "$INPUT"
fi
if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL="$(sha256sum "$INPUT")"
else
    ACTUAL="$(shasum -a 256 "$INPUT")"
fi
if [ "${ACTUAL%% *}" != "$TINYSSH_SHA256" ]; then
    echo "[tinyssh] source checksum mismatch; refusing to extract or build" >&2
    exit 1
fi
if [ "$INPUT" != "$ARCHIVE" ]; then
    mv "$INPUT" "$ARCHIVE"
fi
tar -xzf "$ARCHIVE" -C "$WORK"
SRC="$WORK/tinyssh-$TINYSSH_COMMIT"

# Aegis has no fchdir(2) (a dir fd can't carry a path — vfs_file_t is fixed-size),
# but getcwd/chdir work. Patch tinysshd's save-cwd / restore-cwd dance to use a
# path string instead of open_cwd()+fchdir(). Idempotent (guarded on fchdir).
MT="$SRC/main_tinysshd.c"
if [ -f "$MT" ] && grep -q 'fchdir(fdwd)' "$MT"; then
    echo "[tinyssh] patching fchdir -> getcwd/chdir (Aegis has no fchdir)"
    sed -i 's/^static int fdwd;/static int fdwd; static char fdwd_path[256];/' "$MT"
    sed -i 's/fdwd = open_cwd();/getcwd(fdwd_path, sizeof(fdwd_path)); fdwd = open_cwd();/' "$MT"
    sed -i 's/if (fchdir(fdwd) == -1)/if (chdir(fdwd_path) == -1)/' "$MT"
fi

# Aegis has no readlink on /proc/self/fd/N, so musl ttyname() returns NULL.
# channel_openpty/channel_forkpty use ttyname(slave) only as a sanity check —
# the slave (opened via /dev/pts/N, TIOCGPTN) is valid — so disable the checks.
# (openpty/login_tty are musl's and don't use ttyname; TIOCSCTTY is implemented.)
CF="$SRC/channel_forkpty.c"
if [ -f "$CF" ] && grep -q 'if (!ttyname(slave)) return -1;' "$CF"; then
    echo "[tinyssh] patching out ttyname() PTY sanity checks (no /proc/self/fd)"
    sed -i 's/if (!ttyname(\*aslave))/if (0 \&\& !ttyname(*aslave))/' "$CF"
    sed -i 's/if (!ttyname(slave)) return -1;/if (0 \&\& !ttyname(slave)) return -1;/' "$CF"
fi
# channel_droppriv's isatty(0) block uses ttyname(0) (fails) + chown/chmod of the
# tty device (unsupported on /dev/pts/N) and returns 0 -> _exit(111). The block
# only sets SSH_TTY + tty ownership, both non-essential — disable it.
CD="$SRC/channel_drop.c"
if [ -f "$CD" ] && grep -q 'if (isatty(0)) {' "$CD"; then
    echo "[tinyssh] disabling channel_droppriv isatty/ttyname/chown block"
    sed -i 's/if (isatty(0)) {/if (0 \&\& isatty(0)) {/' "$CD"
fi

mkdir -p "$OUT"
cd "$SRC"
make clean >/dev/null 2>&1 || true

# -Icryptoint: crypto_uint*.h are generated there. -static: self-contained binary.
make CC=musl-gcc \
     CPPFLAGS="-Icryptoint -I." \
     CFLAGS="-Os -D_GNU_SOURCE" \
     LDFLAGS="-static" \
     tinysshd tinysshd-makekey tinysshd-printkey

# makekey/printkey are argv[0]-dispatch symlinks in the source tree; ship real
# copies so the rootfs (which copies files, not symlinks) gets standalone binaries.
cp -f tinysshd           "$OUT/tinysshd"
cp -f tinysshd           "$OUT/tinysshd-makekey"
cp -f tinysshd           "$OUT/tinysshd-printkey"
echo "[tinyssh] built: $OUT/tinysshd ($(stat -c%s "$OUT/tinysshd") bytes)"
