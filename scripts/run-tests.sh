#!/usr/bin/env bash
#
# Build and run the host-side unit tests (tests/) with the HOST compiler.
# These cover the freestanding-but-host-compilable leaf code (crypto, the
# Internet-checksum helper, ...) without needing the cross toolchain or QEMU.
#
#   ./scripts/run-tests.sh
#
# Env knobs: GEN (default Ninja), BUILD_TYPE (default Debug).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEN="${GEN:-Ninja}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

cmake -S "$ROOT/tests" -B "$ROOT/tests_build" -G "$GEN" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$ROOT/tests_build"
ctest --test-dir "$ROOT/tests_build" --output-on-failure
