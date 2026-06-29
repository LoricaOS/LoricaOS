#!/bin/bash
# tools/build-curl.sh — configure and compile curl with BearSSL
set -e

REPO="$(git rev-parse --show-toplevel)"
BEARSSL_INSTALL="$REPO/build/bearssl-install"
CURL_SRC="$REPO/references/curl"
BUILD_DIR="$REPO/build/curl-build"
OUT="$REPO/build/curl"

# Fetch the curl source if it isn't vendored yet.
[ -f "$CURL_SRC/configure" ] || bash "$REPO/tools/fetch-curl.sh"

mkdir -p "$BUILD_DIR" "$OUT"

cd "$BUILD_DIR"

"$CURL_SRC/configure" \
  CC="musl-gcc" \
  CFLAGS="-O2 -fno-pie" \
  LDFLAGS="-static -no-pie -L$BEARSSL_INSTALL/lib -L$BEARSSL_INSTALL/lib64" \
  PKG_CONFIG="" \
  --srcdir="$CURL_SRC" \
  --prefix="$OUT/install" \
  --host=x86_64-linux-musl \
  --disable-shared \
  --enable-static \
  --without-openssl \
  --without-mbedtls \
  --without-wolfssl \
  --without-libpsl \
  --without-libidn2 \
  --without-nghttp2 \
  --without-nghttp3 \
  --without-zlib \
  --without-brotli \
  --without-zstd \
  --disable-ldap \
  --disable-ldaps \
  --disable-rtsp \
  --disable-pop3 \
  --disable-imap \
  --disable-smtp \
  --disable-telnet \
  --disable-dict \
  --disable-tftp \
  --disable-gopher \
  --disable-mqtt \
  --disable-threaded-resolver \
  --with-bearssl="$BEARSSL_INSTALL" \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt

# Do NOT force a -static tool link: curl's tool shares curlx_* symbols with
# libcurl that don't resolve in a plain static relink against this curl/bearssl,
# and libtool emits a dynamic tool anyway. A dynamic curl is fine on Aegis — the
# musl dynamic linker (/lib/ld-musl-x86_64.so.1) and libc.so are present, and
# BearSSL is baked into the static libcurl.a, so curl only needs libc at runtime.
make -j"$(nproc)" V=1 2>&1 | tail -5

if [ ! -f src/curl ]; then
    echo "[curl] ERROR: make did not produce src/curl" >&2
    exit 1
fi

cp src/curl "$OUT/curl"
strip "$OUT/curl"
echo "[curl] built: $OUT/curl ($(du -sh "$OUT/curl" | cut -f1)), $(file -b "$OUT/curl" | cut -d, -f1)"
