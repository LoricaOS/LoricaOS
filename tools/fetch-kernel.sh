#!/usr/bin/env bash
# Fetch the Aegis kernel image LoricaOS is built against.
#
# An explicit LOCAL_KERNEL + LOCAL_KERNEL_SHA256 pair selects an unpublished
# x86-64 artifact instead. No local-mode failure falls back to a release.
# Otherwise resolution order (verified against tools/kernel.sha256):
#   1. local cache  vendor/aegis[-arm64]-<version>.elf   (committed or pre-populated)
#   2. download     <release URL>/v<version>/aegis[-arm64].elf
#
# The kernel is a versioned ARTIFACT — LoricaOS does not build it. OS and kernel
# versions move independently; this is the single point that couples them.
#
# The 3rd arg (or $ARCH) selects the arch: x86_64 (default) → aegis.elf,
# arm64/aarch64 → aegis-arm64.elf. Each arch is a separate release artifact.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VER="${1:?usage: fetch-kernel.sh <version> <dest-path> [arch]}"
DEST="${2:?usage: fetch-kernel.sh <version> <dest-path> [arch]}"
ARCH="${3:-${ARCH:-x86_64}}"

case "$ARCH" in
    arm64|aarch64) SUFFIX="-arm64" ;;
    microvm) SUFFIX="-microvm" ;;   # PVH/virtio-mmio kernel for Firecracker et al.
    x86_64|amd64|"") SUFFIX="" ;;
    *) echo "[fetch-kernel] ERROR: unknown arch '$ARCH'" >&2; exit 1 ;;
esac

LOCAL_MODE=0
if [ "${LOCAL_KERNEL+x}" = x ] || [ "${LOCAL_KERNEL_SHA256+x}" = x ]; then
    LOCAL_MODE=1
    if [ -z "${LOCAL_KERNEL:-}" ] || [ -z "${LOCAL_KERNEL_SHA256:-}" ]; then
        echo "[fetch-kernel] ERROR: LOCAL_KERNEL and LOCAL_KERNEL_SHA256 must both be supplied and nonempty" >&2
        exit 1
    fi
    case "$ARCH" in
        x86_64|amd64|"") ;;
        *) echo "[fetch-kernel] ERROR: local kernels support only the normal x86_64 target" >&2; exit 1 ;;
    esac
    if [[ "$LOCAL_KERNEL" != /* ]] || [ ! -f "$LOCAL_KERNEL" ]; then
        echo "[fetch-kernel] ERROR: LOCAL_KERNEL must name an existing absolute file path" >&2
        exit 1
    fi
    if [[ ! "$LOCAL_KERNEL_SHA256" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "[fetch-kernel] ERROR: LOCAL_KERNEL_SHA256 must be a trusted 64-digit SHA-256" >&2
        exit 1
    fi
    SOURCE="$LOCAL_KERNEL"
    EXPECTED="$(printf '%s' "$LOCAL_KERNEL_SHA256" | tr 'A-F' 'a-f')"
else
    CACHE="vendor/aegis${SUFFIX}-${VER}.elf"
    URL="https://github.com/LoricaOS/Aegis/releases/download/v${VER}/aegis${SUFFIX}.elf"
    LOCK=tools/kernel.sha256
    ASSET="aegis${SUFFIX}.elf"
    EXPECTED="$(awk -v ver="$VER" -v asset="$ASSET" \
        '$1 == ver && $2 == asset { print $3; exit }' "$LOCK")"

    if [ -z "$EXPECTED" ]; then
        echo "[fetch-kernel] ERROR: no checksum for v$VER $ASSET in $LOCK" >&2
        exit 1
    fi

    mkdir -p vendor "$(dirname "$DEST")"

    if [ -f "$CACHE" ]; then
        echo "[fetch-kernel] using cached kernel $CACHE (v$VER)"
    else
        echo "[fetch-kernel] downloading Aegis kernel v$VER"
        echo "[fetch-kernel]   $URL"
        if ! curl -fsSL "$URL" -o "$CACHE.tmp"; then
            echo "[fetch-kernel] ERROR: could not fetch kernel v$VER" >&2
            echo "[fetch-kernel] (place it at $CACHE manually to build offline)" >&2
            rm -f "$CACHE.tmp"
            exit 1
        fi
        mv "$CACHE.tmp" "$CACHE"
    fi

    SOURCE="$CACHE"
fi

# Verify the bytes we will actually install, not a source that could change
# between verification and copying. Keep the temporary file beside DEST so the
# final rename is atomic. Failed verification leaves the old destination intact.
mkdir -p "$(dirname "$DEST")"
STAGED="$(mktemp "${DEST}.tmp.XXXXXX")"
PROVENANCE=""
trap 'rm -f "$STAGED"; [ -z "$PROVENANCE" ] || rm -f "$PROVENANCE"' EXIT
cp "$SOURCE" "$STAGED"
FOUND="$(sha256sum "$STAGED" | awk '{print $1}')"
if [ "$FOUND" != "$EXPECTED" ]; then
    echo "[fetch-kernel] ERROR: checksum mismatch for $SOURCE" >&2
    echo "[fetch-kernel] expected $EXPECTED" >&2
    echo "[fetch-kernel] found    $FOUND" >&2
    exit 1
fi

if [ "$LOCAL_MODE" = 1 ]; then
    # ELF64, little-endian, current ELF version, EM_X86_64 (62). This is a
    # practical architecture sanity check, not a full ELF loader/boot validator.
    IDENT="$(od -An -tx1 -N7 "$STAGED" | tr -d ' \n')"
    MACHINE="$(od -An -tx1 -j18 -N2 "$STAGED" | tr -d ' \n')"
    if [ "$IDENT" != 7f454c46020101 ] || [ "$MACHINE" != 3e00 ] || \
       [ "$(wc -c < "$STAGED")" -lt 64 ]; then
        echo "[fetch-kernel] ERROR: local kernel must be a little-endian x86_64 ELF64 image" >&2
        exit 1
    fi
    PROVENANCE="$(mktemp "$(dirname "$DEST")/.kernel-local.tmp.XXXXXX")"
    printf 'kind=local-unpublished\nsource=%s\nsha256=%s\n' "$SOURCE" "$EXPECTED" > "$PROVENANCE"
    mv -f "$STAGED" "$DEST"
    rm -f "$(dirname "$DEST")/.kernel-version"
    mv -f "$PROVENANCE" "$(dirname "$DEST")/.kernel-local"
    echo "[fetch-kernel] verified LOCAL UNPUBLISHED kernel ($EXPECTED) -> $DEST"
else
    mv -f "$STAGED" "$DEST"
    echo "$VER" > "$(dirname "$DEST")/.kernel-version"
    rm -f "$(dirname "$DEST")/.kernel-local"
    echo "[fetch-kernel] verified $ASSET v$VER -> $DEST"
fi
