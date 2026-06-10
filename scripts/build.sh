#!/usr/bin/env bash
#
# Build Cardinal; end to end.
#
#   1. Build the host-side tools (sign_exec) into utils_build/ with the HOST
#      compiler (these run on the build machine, not the target).
#   2. Configure + build the kernel, modules, servers and drivers for the
#      x86_64-elf target with clang/lld into build/.
#
# Run inside the dev environment:
#   source ./scripts/devenv/activate.sh
#   ./scripts/build.sh
#
# Useful overrides:
#   BUILD_TYPE=Debug|Release   (default Debug)
#   GEN="Unix Makefiles"       (default Ninja)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
GEN="${GEN:-Ninja}"

log() { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }

# --- 1. host tools ---------------------------------------------------------
log "Configuring host tools (utils -> utils_build/)"
cmake -S utils -B utils_build -G "$GEN" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build utils_build
test -x utils_build/sign_exec/sign_exec || {
  echo "error: sign_exec was not produced" >&2; exit 1; }

# --- 2. target build -------------------------------------------------------
log "Configuring target build (clang --target=x86_64-elf -> build/)"
cmake -S . -B build -G "$GEN" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_ASM_COMPILER=clang

log "Building target"
cmake --build build

log "Done. Artifacts in build/ (kernel/kernel.bin, *.celf modules)."
