#!/bin/sh
set -eu
out="$(mktemp -d "${TMPDIR:-/tmp}/vigil-launch.XXXXXX")"
trap 'rm -rf "$out"' EXIT HUP INT TERM
${CC:-cc} -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -DENOCAP=EPERM tools/vigil-launch-test.c -o "$out/test"
"$out/test"
