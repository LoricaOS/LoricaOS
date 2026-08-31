#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
cc -std=c99 -O2 -Wall -Wextra -Werror \
  -Ibuild/bearssl-install/include -Iuser/lib/libinstall \
  tools/cryptroot-crypto-test.c user/lib/libinstall/cryptroot_crypto.c \
  -Lbuild/bearssl-install/lib -lbearssl -o build/cryptroot-crypto-test
build/cryptroot-crypto-test
