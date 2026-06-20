#!/usr/bin/env bash
# Build and run the host-side shader-tier test harnesses. Like libs/lisp/test,
# the shader core uses only the portable libc subset shared by host and common/,
# so it compiles with plain host clang -- host-first iteration
# (notes/core/lisp-shaders.md). Each test_*.c is its own main(), built and run as
# a separate binary against ALL of libs/lisp_shader/src + libs/lisp/src.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADER="$HERE/.."
LISP="$SHADER/../lisp"
TMP="$(mktemp -d)"
status=0
for t in "$HERE"/test_*.c; do
  name="$(basename "$t" .c)"
  clang -std=c11 -Wall -Wextra -Werror -g \
    -I"$SHADER/inc" -I"$SHADER/src" -I"$LISP/inc" \
    "$SHADER"/src/*.c "$LISP"/src/*.c "$t" -o "$TMP/$name" -lm
  # Pass the test dir (harness convention; current tests ignore the arg).
  "$TMP/$name" "$HERE" || status=1
  echo
done
exit "$status"
