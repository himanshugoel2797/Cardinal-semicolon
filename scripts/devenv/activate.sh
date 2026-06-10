#!/usr/bin/env bash
# Activate a Cardinal; build session (the conda env holds the whole toolchain).
#   source ./scripts/devenv/activate.sh
#
# (Must be sourced, not executed, so the activation persists in your shell.)

_cardinal_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/../.." && pwd)"

if command -v conda >/dev/null 2>&1; then
  source "$(conda info --base)/etc/profile.d/conda.sh"
  conda activate "$_cardinal_root/.devenv"
else
  echo "warning: conda not found; skipping env activation" >&2
fi

if command -v clang >/dev/null 2>&1; then
  echo "Cardinal; build env ready:"
  echo "  clang: $(clang --version | head -1)"
  echo "  cmake: $(cmake --version | head -1)"
  echo
  echo "Configure an out-of-source build with:"
  echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
else
  echo "warning: clang not found; run ./scripts/devenv/setup.sh" >&2
fi

unset _cardinal_root
