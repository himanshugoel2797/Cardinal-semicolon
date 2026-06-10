#!/usr/bin/env bash
#
# Developer environment bootstrap for Cardinal;.
#
# Creates/updates the repo-local conda env at ./.devenv, which provides the
# entire toolchain: clang/clang++ (cross compiler via --target=x86_64-elf),
# lld, llvm-tools, nasm, qemu, xorriso, cmake, astyle.
#
# Re-running is safe and incremental (the env is updated in place).
#
# After this completes, activate a build session with:
#   source ./scripts/devenv/activate.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENV_PREFIX="$REPO_ROOT/.devenv"

log() { printf '\033[1;32m[setup]\033[0m %s\n' "$*"; }

if ! command -v conda >/dev/null 2>&1; then
  echo "error: conda not found on PATH. Install miniforge/miniconda first." >&2
  exit 1
fi
CONDA_BASE="$(conda info --base)"
# shellcheck disable=SC1091
source "$CONDA_BASE/etc/profile.d/conda.sh"

if [ -d "$ENV_PREFIX" ]; then
  log "Updating existing conda env at $ENV_PREFIX"
  conda env update --prefix "$ENV_PREFIX" --file "$REPO_ROOT/scripts/devenv/environment.yml" --prune
else
  log "Creating conda env at $ENV_PREFIX"
  conda env create --prefix "$ENV_PREFIX" --file "$REPO_ROOT/scripts/devenv/environment.yml"
fi

conda activate "$ENV_PREFIX"
log "Environment ready:"
log "  clang: $(clang --version | head -1)"
log "  lld:   $(ld.lld --version | head -1)"
log "  cmake: $(cmake --version | head -1)"
log ""
log "Start a build session with:  source ./scripts/devenv/activate.sh"
