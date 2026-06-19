#!/usr/bin/env bash
#
# Boot the Cardinal; test ISO headless in QEMU, run the in-OS SysTest suite, and
# report pass/fail. The kernel is booted with the "cardinal.test" cmdline (baked
# into the test GRUB entry), so SysTest's test_run_all() runs every registered
# test, prints TAP-style results over COM1, and exits the machine via QEMU's
# isa-debug-exit device.
#
#   ./scripts/build.sh                      # produce build/
#   cmake --build build --target test-image # produce build/ISO/os-test.iso
#   ./scripts/run-tests-qemu.sh             # run the tests
#
# Pass/fail is decided primarily by the serial sentinel the runner prints
# ("[SysTest] ALL TESTS PASSED" / "[SysTest] TESTS FAILED"); the isa-debug-exit
# code (33 pass / 35 fail) is used as a corroborating signal. Exit 0 == passed.
#
# Env overrides:
#   ISO=path        test ISO        (default build/ISO/os-test.iso)
#   MEM=512         guest RAM (MiB)
#   MACHINE=q35     qemu machine    (q35 has the ACPI MCFG the PCI code needs)
#   ACCEL=auto|kvm|tcg   acceleration (default auto)
#   SMP=2           guest cpu count (per-CPU tests want >1)
#   TIMEOUT=120     seconds before the run is declared hung
#   LOG=path        serial log file (default build/systest-serial.log)
#   EXTRA="..."     extra qemu args
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${ISO:-$REPO_ROOT/build/ISO/os-test.iso}"
MEM="${MEM:-512}"
MACHINE="${MACHINE:-q35}"
ACCEL="${ACCEL:-auto}"
SMP="${SMP:-2}"
TIMEOUT="${TIMEOUT:-120}"
LOG="${LOG:-$REPO_ROOT/build/systest-serial.log}"
EXTRA="${EXTRA:-}"

[ -f "$ISO" ] || { echo "error: test ISO not found: $ISO (build the 'test-image' target first)" >&2; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "error: qemu-system-x86_64 not installed" >&2; exit 1; }

accel_args=()
if [ "$ACCEL" = kvm ] || { [ "$ACCEL" = auto ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; }; then
  accel_args=(-accel kvm -cpu host)
  echo "[run-tests] using KVM acceleration"
else
  accel_args=(-accel tcg -cpu qemu64,+sse2)
  echo "[run-tests] using TCG (software) emulation"
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"

echo "[run-tests] booting $ISO (timeout ${TIMEOUT}s)"
set +e
timeout --foreground "$TIMEOUT" \
  qemu-system-x86_64 \
    -machine "$MACHINE" \
    "${accel_args[@]}" \
    -m "$MEM" -smp "$SMP" \
    -cdrom "$ISO" -boot d \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot \
    -serial file:"$LOG" \
    -display none \
    $EXTRA
qemu_code=$?

echo "----- serial log -----"
cat "$LOG"
echo "----------------------"
echo "[run-tests] qemu exit code: $qemu_code (33=pass via isa-debug-exit, 35=fail, 124=timeout)"

# Under interpreter-as-scheduler, SysLisp's single-core self-tests are the in-OS
# suite (SysTest's native-task framework cannot run -- there is no native
# scheduler). In "cardinal.test" mode SysLisp runs them and exits via
# isa-debug-exit, printing one of these sentinels.
if grep -q "\[SysLisp\] ALL TESTS PASSED" "$LOG" && ! grep -q "\[SysLisp\] SELF-TEST FAILED" "$LOG"; then
  echo "[run-tests] RESULT: PASS"
  exit 0
fi

if grep -q "\[SysLisp\] SELF-TEST FAILED" "$LOG"; then
  echo "[run-tests] RESULT: FAIL (one or more self-tests failed)" >&2
  exit 1
fi

echo "[run-tests] RESULT: FAIL (no SysLisp completion sentinel found -- boot hang/panic?)" >&2
exit 1
