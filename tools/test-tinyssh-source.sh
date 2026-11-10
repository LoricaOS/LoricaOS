#!/usr/bin/env bash
# Host-only source acquisition test; no toolchain, network, or SSH session.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -ne 1 ]; then
    echo "usage: bash tools/test-tinyssh-source.sh PINNED_SOURCE_ARCHIVE" >&2
    exit 2
fi
FIXTURE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
mkdir -p "$ROOT/build"
TMP="$(mktemp -d "$ROOT/build/tinyssh-source-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/bin" "$TMP/repo/references/tinyssh"
cp "$FIXTURE" "$TMP/fixture.tar.gz"
export FIXTURE="$TMP/fixture.tar.gz"
export REPO="$TMP/repo" TEST_LOG="$TMP/log"
export PATH="$TMP/bin:$PATH"
COMMIT=6a60c2aac18da9035a7401316789d6f4df7e295f
ARCHIVE="$REPO/build/tinyssh/tinyssh-$COMMIT.tar.gz"
echo "old unpinned source must not be used" > "$REPO/references/tinyssh/main_tinysshd.c"

cat > "$TMP/bin/curl" <<'EOF'
#!/bin/sh
set -eu
test "$#" = 7
test "$1" = -fL
test "$2" = --proto
test "$3" = '=https'
test "$4" = --tlsv1.2
test "$5" = https://codeload.github.com/janmojzis/tinyssh/tar.gz/6a60c2aac18da9035a7401316789d6f4df7e295f
test "$6" = -o
echo download >> "$TEST_LOG"
cp "$FIXTURE" "$7"
EOF
cat > "$TMP/bin/make" <<'EOF'
#!/bin/sh
set -eu
test "$(basename "$PWD")" = tinyssh-6a60c2aac18da9035a7401316789d6f4df7e295f
test "$1" != clean || exit 0
# Existing patches still apply to the real pinned source.
grep -Fq 'if (chdir(fdwd_path) == -1)' main_tinysshd.c
grep -Fq 'if (0 && !ttyname(*aslave))' channel_forkpty.c
grep -Fq 'if (0 && !ttyname(slave)) return -1;' channel_forkpty.c
grep -Fq 'if (0 && isatty(0)) {' channel_drop.c
# Detect accidental source reuse on the next invocation.
test ! -e already-built
touch already-built
printf 'mock executable\n' > tinysshd
echo build >> "$TEST_LOG"
EOF
chmod +x "$TMP/bin/curl" "$TMP/bin/make"

# The production builder targets Linux. Keep its existing GNU sed/stat calls
# unchanged, translating only their CLI spelling for this host-only macOS test.
if [ "$(uname -s)" = Darwin ]; then
    cat > "$TMP/bin/sed" <<'EOF'
#!/bin/sh
test "$1" != -i || { shift; exec /usr/bin/sed -i '' "$@"; }
exec /usr/bin/sed "$@"
EOF
    cat > "$TMP/bin/stat" <<'EOF'
#!/bin/sh
test "$1" != -c%s || { shift; exec /usr/bin/stat -f%z "$@"; }
exec /usr/bin/stat "$@"
EOF
    chmod +x "$TMP/bin/sed" "$TMP/bin/stat"
fi

bash "$ROOT/tools/build-tinyssh.sh"
test "$(cat "$TEST_LOG")" = "$(printf 'download\nbuild')"
test "$(cat "$REPO/build/tinyssh/tinysshd")" = "mock executable"
cmp "$REPO/build/tinyssh/tinysshd" "$REPO/build/tinyssh/tinysshd-makekey"
cmp "$REPO/build/tinyssh/tinysshd" "$REPO/build/tinyssh/tinysshd-printkey"

# Cache is verified, but patched/build source is never reused.
bash "$ROOT/tools/build-tinyssh.sh"
test "$(cat "$TEST_LOG")" = "$(printf 'download\nbuild\nbuild')"
test "$(cat "$REPO/references/tinyssh/main_tinysshd.c")" = "old unpinned source must not be used"
test -z "$(find "$REPO/build/tinyssh" -name '.source.*' -print)"

# A modified cached archive must not reach extraction/build.
printf 'corrupt cache\n' > "$ARCHIVE"
if bash "$ROOT/tools/build-tinyssh.sh" > "$TMP/rejected" 2>&1; then
    echo "FAIL: corrupt cached archive accepted" >&2
    exit 1
fi
grep -q 'source checksum mismatch' "$TMP/rejected"
test "$(cat "$TEST_LOG")" = "$(printf 'download\nbuild\nbuild')"
test -z "$(find "$REPO/build/tinyssh" -name '.source.*' -print)"

# Likewise for a corrupt download: do not publish it as the cached archive.
rm "$ARCHIVE"
printf 'corrupt download\n' > "$FIXTURE"
if bash "$ROOT/tools/build-tinyssh.sh" > "$TMP/rejected" 2>&1; then
    echo "FAIL: corrupt downloaded archive accepted" >&2
    exit 1
fi
grep -q 'source checksum mismatch' "$TMP/rejected"
test ! -e "$ARCHIVE"
test "$(cat "$TEST_LOG")" = "$(printf 'download\nbuild\nbuild\ndownload')"
test -z "$(find "$REPO/build/tinyssh" -name '.source.*' -print)"
echo "tinyssh source: PASS (pin, checksum, fresh patches, cache/download rejection)"
