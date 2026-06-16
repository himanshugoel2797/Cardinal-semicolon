#!/usr/bin/env bash
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Local death-test runner. Builds the harness ISO (if needed) and drives it with
# the Python harness. NOT used in web CI -- death tests are intentionally
# local-only because the persistent-reboot run is expensive under TCG.
#
# Env knobs (all optional): ACCEL (default tcg), MACHINE (q35), MEM (512),
# SMP (2), TIMEOUT (overall, 600s), DEATH_TIMEOUT (per-test, 60s),
# LOG (build/systest-serial.log), ISO (build/ISO/os-harness.iso).
set -euo pipefail

cd "$(dirname "$0")/.."

ISO="${ISO:-build/ISO/os-harness.iso}"

if [ ! -f "$ISO" ]; then
  echo "[run-deathtests] building harness ISO ($ISO)..."
  cmake --build build --target harness-image
fi

exec python3 scripts/systest-harness.py \
  --iso "$ISO" \
  --accel "${ACCEL:-tcg}" \
  --machine "${MACHINE:-q35}" \
  --mem "${MEM:-512}" \
  --smp "${SMP:-2}" \
  --timeout "${TIMEOUT:-600}" \
  --death-timeout "${DEATH_TIMEOUT:-60}" \
  --log "${LOG:-build/systest-serial.log}"
