#!/usr/bin/env bash
# Build the Cardinal Lisp simulator (sim/README.md). Like the test harness, the
# Lisp core compiles with the plain host clang -- it uses only the portable libc
# subset. The X11 backend is compiled in when the X11 headers are present
# (pkg-config x11); otherwise the binary still builds with the offscreen backend.
#
#   ./build.sh            # build -> libs/lisp/sim/sim
#   ./build.sh --run      # build, then run offscreen with sim/demo-script.txt
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LISP="$HERE/.."
ROOT="$(cd "$LISP/../.." && pwd)"
LISPDIR="$ROOT/lisp"
OUT="$HERE/sim"

CC="${CC:-clang}"
CFLAGS=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -g -O1 -I"$LISP/inc" -I"$HERE"
        -DSIM_SRC_DIR="\"$HERE\"" -DLISP_ROOT_DIR="\"$LISPDIR\"")
LDLIBS=()

# Optional X11 backend.
if pkg-config --exists x11 2>/dev/null; then
  CFLAGS+=(-DHAVE_X11 $(pkg-config --cflags x11))
  LDLIBS+=($(pkg-config --libs x11))
  echo "[sim build] X11 backend: enabled"
else
  echo "[sim build] X11 backend: disabled (no x11 headers); offscreen only"
fi

echo "[sim build] compiling..."
# shellcheck disable=SC2068
$CC ${CFLAGS[@]} \
  "$LISP"/src/*.c \
  "$HERE"/sim_main.c "$HERE"/host_prims.c \
  "$HERE"/backend_x11.c "$HERE"/backend_offscreen.c "$HERE"/keymap.c \
  -lm ${LDLIBS[@]+"${LDLIBS[@]}"} -o "$OUT"
echo "[sim build] -> $OUT"

if [[ "${1:-}" == "--run" ]]; then
  echo "[sim build] running offscreen with demo-script.txt..."
  SIM_BACKEND=offscreen SIM_SCRIPT="$HERE/demo-script.txt" \
    SIM_PPM="$HERE/sim-frame.ppm" "$OUT"
  echo "[sim build] wrote $HERE/sim-frame.ppm"
fi
