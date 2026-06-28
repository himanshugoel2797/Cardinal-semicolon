#!/bin/sh
# Host build + run of the Wasm interpreter test harness. Fast iteration loop,
# independent of the kernel CMake. Uses the host clang with ASan+UBSan.
#
#   libs/wasm/test/build-and-run.sh
#
# wat2wasm (from wabt, installed in ./.devenv) compiles any *.wat fixtures next
# to the harness into *.wasm.h byte-array headers before building.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
LIB="$HERE/.."
REPO=$(cd "$LIB/../.." && pwd)
OUT="${TMPDIR:-/tmp}/cardinal-wasm-test"
mkdir -p "$OUT"

CC=${CC:-clang}
WAT2WASM="$REPO/.devenv/bin/wat2wasm"

# Compile .wat fixtures -> embeddable byte-array headers (xxd-style).
if [ -x "$WAT2WASM" ]; then
    for wat in "$HERE"/fixtures/*.wat; do
        [ -e "$wat" ] || continue
        base=$(basename "$wat" .wat)
        "$WAT2WASM" "$wat" -o "$OUT/$base.wasm"
        # emit:  static const unsigned char <base>_wasm[] = { ... };
        {
            printf 'static const unsigned char %s_wasm[] = {' "$base"
            od -An -v -tu1 "$OUT/$base.wasm" | tr -s ' ' '\n' | grep -v '^$' \
                | paste -sd, -
            printf '};\n'
        } > "$HERE/fixtures/$base.wasm.h"
    done
fi

CFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer"
$CC $CFLAGS -I"$LIB/inc" -I"$LIB/src" -I"$HERE/fixtures" \
    "$LIB"/src/*.c "$HERE"/*.c -o "$OUT/wasm_test"

"$OUT/wasm_test"
