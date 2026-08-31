#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/repo/tools" "$TMP/repo/vendor"
cp tools/fetch-kernel.sh tools/kernel.sha256 "$TMP/repo/tools/"
cp /bin/sh "$TMP/repo/vendor/aegis-1.5.1.elf"
(cd "$TMP/repo" && ! bash tools/fetch-kernel.sh 1.5.1 out.elf >/dev/null 2>&1)
cp /bin/sh "$TMP/repo/vendor/aegis-1.0.0.elf"
(cd "$TMP/repo" && ! bash tools/fetch-kernel.sh 1.0.0 out.elf >/dev/null 2>&1)
echo "fetch-kernel: PASS (tampered and unpinned artifacts rejected)"
