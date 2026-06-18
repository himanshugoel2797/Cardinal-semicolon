#!/usr/bin/env bash
# Build and run the host-side Lisp test harness. The Lisp core uses only the
# portable libc subset shared by the host and common/, so it compiles with the
# plain host clang -- the point of host-first iteration (notes/core/lisp-substrate.md).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LISP="$HERE/.."
OUT="$(mktemp -d)/test_lisp"
clang -std=c11 -Wall -Wextra -Werror -g -I"$LISP/inc" \
  "$LISP"/src/*.c "$HERE"/test_lisp.c -o "$OUT"
exec "$OUT"
