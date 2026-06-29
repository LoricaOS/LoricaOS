#!/bin/bash
# tools/fetch-curl.sh — fetch the curl source tarball into references/curl/.
# curl is built statically against BearSSL by tools/build-curl.sh and shipped
# as /bin/curl (used by herald's online repository client). Release tarballs
# ship a pre-generated ./configure, so no autotools regen is needed.
set -e
REPO="$(git rev-parse --show-toplevel)"
VER="${CURL_VERSION:-8.5.0}"
URL="https://curl.se/download/curl-${VER}.tar.gz"
TMP="/tmp/curl-${VER}.tar.gz"

if [ -f "$REPO/references/curl/configure" ]; then
    echo "[curl] source already present at references/curl/ — skip"
    exit 0
fi

wget -O "$TMP" "$URL"
mkdir -p "$REPO/references"
cd "$REPO/references"
tar xzf "$TMP"
rm -rf curl
mv "curl-${VER}" curl
echo "[curl] source extracted to references/curl/ (${VER})"
