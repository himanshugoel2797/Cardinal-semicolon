#!/usr/bin/env bash
# Build and run the host-side Lisp test harnesses. The Lisp core uses only the
# portable libc subset shared by the host and common/, so it compiles with the
# plain host clang -- the point of host-first iteration (notes/core/lisp-substrate.md).
# Each test_*.c is its own main(), built and run as a separate binary.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LISP="$HERE/.."
TMP="$(mktemp -d)"
status=0
for t in "$HERE"/test_*.c; do
  name="$(basename "$t" .c)"
  clang -std=c11 -Wall -Wextra -Werror -g -I"$LISP/inc" \
    "$LISP"/src/*.c "$t" -o "$TMP/$name"
  "$TMP/$name" || status=1
  echo
done
exit "$status"
