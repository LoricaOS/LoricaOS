#!/usr/bin/env bash
# Host-only, offline regression tests; never modify the real build/cache/pins.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
trap 'echo "local-kernel: failed at line $LINENO" >&2' ERR
unset LOCAL_KERNEL LOCAL_KERNEL_SHA256
mkdir -p "$TMP/repo/tools" "$TMP/repo/vendor" "$TMP/bin" "$TMP/inputs"
cp "$ROOT/tools/fetch-kernel.sh" "$TMP/repo/tools/"
cp "$ROOT/Makefile" "$ROOT/VERSION" "$TMP/repo/"
printf 'test-release\n' > "$TMP/repo/KERNEL_VERSION"

fail() { echo "local-kernel: FAIL: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
expect_fail() {
    if "$@" > "$TMP/failure.log" 2>&1; then
        fail "unexpected success: $*"
    fi
    # A rejected selection must neither replace a prior image nor its metadata.
    cmp "$TMP/previous.elf" build/aegis-stripped.elf
    cmp "$TMP/previous.provenance" build/.kernel-local
    [ ! -e build/.kernel-version ]
}

# A small synthetic ELF64/x86-64 header; no compiler or host arch dependency.
{
    printf '\177ELF\002\001\001'
    dd if=/dev/zero bs=1 count=9 2>/dev/null
    printf '\002\000\076\000\001\000\000\000'
    dd if=/dev/zero bs=1 count=40 2>/dev/null
    printf 'local one\n'
} > "$TMP/inputs/input one's.elf"
FIRST="$TMP/inputs/input one's.elf"
SECOND="$TMP/inputs/second.elf"
cp "$FIRST" "$SECOND"
printf 'local two\n' >> "$SECOND"
H1="$(hash "$FIRST")"
H2="$(hash "$SECOND")"
printf 'locked release fixture\n' > "$TMP/release.elf"
RH="$(hash "$TMP/release.elf")"
printf 'test-release aegis.elf %s\n' "$RH" > "$TMP/repo/tools/kernel.sha256"

# Stub the network. Validate the release URL as well as the downloaded bytes.
cat > "$TMP/bin/curl" <<'EOF'
#!/usr/bin/env bash
set -eu
printf '%s\n' "$*" >> "$TEST_CURL_LOG"
[ "$#" = 4 ] && [ "$1" = -fsSL ] && [ "$3" = -o ]
[ "$2" = https://github.com/LoricaOS/Aegis/releases/download/vtest-release/aegis.elf ]
cp "$TEST_RELEASE" "$4"
EOF
# Simulate bytes changing during the copy, after the source hash was supplied.
# This catches verification of the source instead of the staged destination.
export TEST_REAL_CP="$(command -v cp)"
cat > "$TMP/bin/cp" <<'EOF'
#!/usr/bin/env bash
set -eu
"$TEST_REAL_CP" "$@"
if [ "${TEST_TAMPER_COPY:-}" = 1 ]; then
    case "${@: -1}" in
        build/aegis-stripped.elf.tmp.*) printf 'corrupted copy\n' >> "${@: -1}" ;;
    esac
fi
EOF
chmod +x "$TMP/bin/curl" "$TMP/bin/cp"
export PATH="$TMP/bin:$PATH"
export TEST_CURL_LOG="$TMP/curl.log" TEST_RELEASE="$TMP/release.elf"
: > "$TEST_CURL_LOG"
cd "$TMP/repo"

# Use the actual Makefile kernel rule, plus a focused downstream consumer.
cat >> Makefile <<'EOF'

build/consumer: build/aegis-stripped.elf
	cp "$<" "$@"
EOF
select_local() {
    make --no-print-directory build/consumer \
        "LOCAL_KERNEL=$1" "LOCAL_KERNEL_SHA256=$2"
    cmp "$1" build/aegis-stripped.elf
    cmp "$1" build/consumer
    grep -Fx 'kind=local-unpublished' build/.kernel-local
    grep -Fx "source=$1" build/.kernel-local
    grep -Fx "sha256=$(hash "$1")" build/.kernel-local
    [ ! -e build/.kernel-version ]
}
select_local "$FIRST" "$H1"
cp build/aegis-stripped.elf "$TMP/previous.elf"
cp build/.kernel-local "$TMP/previous.provenance"

# Both missing-pair directions, empty values, bad format, missing/relative input.
expect_fail make build/consumer "LOCAL_KERNEL=$FIRST"
expect_fail make build/consumer "LOCAL_KERNEL_SHA256=$H1"
expect_fail make build/consumer LOCAL_KERNEL= LOCAL_KERNEL_SHA256=
expect_fail make build/consumer "LOCAL_KERNEL=$FIRST" LOCAL_KERNEL_SHA256=
expect_fail make build/consumer LOCAL_KERNEL= "LOCAL_KERNEL_SHA256=$H1"
expect_fail make build/consumer "LOCAL_KERNEL=$FIRST" LOCAL_KERNEL_SHA256=invalid
expect_fail make build/consumer "LOCAL_KERNEL=$TMP/absent.elf" "LOCAL_KERNEL_SHA256=$H1"
expect_fail make build/consumer LOCAL_KERNEL=relative.elf "LOCAL_KERNEL_SHA256=$H1"
expect_fail make build/consumer "LOCAL_KERNEL=$FIRST" "LOCAL_KERNEL_SHA256=$H2"
cp "$FIRST" "$TMP/inputs/tampered.elf"
printf 'tampered\n' >> "$TMP/inputs/tampered.elf"
expect_fail make build/consumer "LOCAL_KERNEL=$TMP/inputs/tampered.elf" "LOCAL_KERNEL_SHA256=$H1"
expect_fail env TEST_TAMPER_COPY=1 make build/consumer "LOCAL_KERNEL=$FIRST" "LOCAL_KERNEL_SHA256=$H1"

# ELF sanity: reject non-ELF, truncated headers, and correct-hash wrong machines.
printf 'not an ELF\n' > "$TMP/inputs/not-elf"
expect_fail make build/consumer "LOCAL_KERNEL=$TMP/inputs/not-elf" \
    "LOCAL_KERNEL_SHA256=$(hash "$TMP/inputs/not-elf")"
printf '\177ELF\002\001\001' > "$TMP/inputs/truncated.elf"
expect_fail make build/consumer "LOCAL_KERNEL=$TMP/inputs/truncated.elf" \
    "LOCAL_KERNEL_SHA256=$(hash "$TMP/inputs/truncated.elf")"
cp "$FIRST" "$TMP/inputs/arm64.elf"
printf '\267\000' | dd of="$TMP/inputs/arm64.elf" bs=1 seek=18 conv=notrunc 2>/dev/null
expect_fail make build/consumer "LOCAL_KERNEL=$TMP/inputs/arm64.elf" \
    "LOCAL_KERNEL_SHA256=$(hash "$TMP/inputs/arm64.elf")"
expect_fail env LOCAL_KERNEL="$FIRST" LOCAL_KERNEL_SHA256="$H1" \
    bash tools/fetch-kernel.sh test-release build/aegis-stripped.elf arm64
[ ! -s "$TEST_CURL_LOG" ] || fail "local selection contacted the network"

# An existing/tampered destination and older-mtime alternate input cannot bypass
# verification. The downstream consumer must receive the newly selected bytes.
printf 'tampered destination\n' > build/aegis-stripped.elf
select_local "$FIRST" "$H1"
touch -t 200001010000 "$SECOND"
select_local "$SECOND" "$H2"
select_local "$FIRST" "$(printf '%s' "$H1" | tr 'a-f' 'A-F')"

# Removing the overrides must restore the locked release, not reuse local bytes.
make --no-print-directory build/consumer
cmp "$TEST_RELEASE" build/aegis-stripped.elf
cmp "$TEST_RELEASE" build/consumer
grep -Fx test-release build/.kernel-version
[ ! -e build/.kernel-local ]
[ "$(wc -l < "$TEST_CURL_LOG")" -eq 1 ]

# Also test release -> local -> cached release, with an existing destination.
select_local "$SECOND" "$H2"
make --no-print-directory build/consumer
cmp "$TEST_RELEASE" build/consumer
[ ! -e build/.kernel-local ]
[ "$(wc -l < "$TEST_CURL_LOG")" -eq 1 ]

# Release failures must not accept an old local destination, nor fetch an
# unpinned version. Use a local selection to reuse the failure-preservation check.
select_local "$FIRST" "$H1"
cp build/aegis-stripped.elf "$TMP/previous.elf"
cp build/.kernel-local "$TMP/previous.provenance"
expect_fail bash tools/fetch-kernel.sh unpinned build/aegis-stripped.elf
[ "$(wc -l < "$TEST_CURL_LOG")" -eq 1 ]
printf 'tampered cache\n' > vendor/aegis-test-release.elf
expect_fail make build/consumer
[ "$(wc -l < "$TEST_CURL_LOG")" -eq 1 ]
rm vendor/aegis-test-release.elf
printf 'tampered download\n' > "$TEST_RELEASE"
expect_fail make build/consumer
[ "$(wc -l < "$TEST_CURL_LOG")" -eq 2 ]
if find build -name '*.tmp.*' | grep -q .; then
    fail "staging files leaked"
fi

echo "local-kernel: PASS (verified staging, fail-closed inputs, ELF sanity, Make switching, locked release)"
