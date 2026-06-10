#!/usr/bin/env bash
#
# Boot the built Cardinal; ISO in QEMU, headless, with the kernel's COM1 debug
# output on the terminal. Intended for quick smoke tests on a dev box / WSL2
# (the cmake `run` target needs qemu-virgil + GTK/virgl and is not used here).
#
#   ./scripts/build.sh                 # produce build/
#   cmake --build build --target image # produce build/ISO/os.iso
#   ./scripts/run-qemu.sh              # boot it
#
# Env overrides:
#   ISO=path        ISO to boot      (default build/ISO/os.iso)
#   MEM=512         guest RAM (MiB)
#   MACHINE=q35     qemu machine     (q35 has PCIe + an ACPI MCFG table, which
#                                     the PCI registration code needs; the older
#                                     i440fx "pc" machine has no MCFG)
#   ACCEL=auto|kvm|tcg   acceleration (default auto: kvm if /dev/kvm is usable)
#   TIMEOUT=30      seconds before auto-killing the guest (0 = no timeout)
#   EXTRA="..."     extra qemu args
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${ISO:-$REPO_ROOT/build/ISO/os.iso}"
MEM="${MEM:-512}"
MACHINE="${MACHINE:-q35}"
ACCEL="${ACCEL:-auto}"
TIMEOUT="${TIMEOUT:-30}"
EXTRA="${EXTRA:-}"

[ -f "$ISO" ] || { echo "error: ISO not found: $ISO (run the 'image' target first)" >&2; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "error: qemu-system-x86_64 not installed" >&2; exit 1; }

# Pick acceleration.
accel_args=()
if [ "$ACCEL" = kvm ] || { [ "$ACCEL" = auto ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; }; then
  accel_args=(-accel kvm -cpu host)
  echo "[run-qemu] using KVM acceleration"
else
  accel_args=(-accel tcg -cpu qemu64,+sse2)
  [ "$ACCEL" = auto ] && [ -e /dev/kvm ] && echo "[run-qemu] /dev/kvm not accessible (need 'usermod -aG kvm \$USER' + re-login); falling back to TCG"
  echo "[run-qemu] using TCG (software) emulation"
fi

set -x
exec timeout --foreground "${TIMEOUT:-30}" \
  qemu-system-x86_64 \
    -machine "$MACHINE" \
    "${accel_args[@]}" \
    -m "$MEM" -smp 2 \
    -cdrom "$ISO" -boot d \
    -vga std \
    -serial stdio \
    -display none \
    -no-reboot -no-shutdown \
    -d guest_errors \
    $EXTRA
