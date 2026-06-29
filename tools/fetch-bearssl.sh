#!/bin/bash
# tools/fetch-bearssl.sh — fetch BearSSL 0.6 source tarball
set -e
REPO="$(git rev-parse --show-toplevel)"
URL="https://bearssl.org/bearssl-0.6.tar.gz"
TMP="/tmp/bearssl-0.6.tar.gz"

if [ -f "$REPO/references/bearssl-0.6/inc/bearssl.h" ]; then
    echo "[bearssl] already present at references/bearssl-0.6/ — skip"
    exit 0
fi

wget -O "$TMP" "$URL"
mkdir -p "$REPO/references"
cd "$REPO/references"
tar xzf "$TMP"
echo "[bearssl] extracted to references/bearssl-0.6/"
