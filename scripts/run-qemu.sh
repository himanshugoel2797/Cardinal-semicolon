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
#   SMP=2           number of guest CPUs (BSP + APs; each runs a Lisp scheduler)
#   MACHINE=q35     qemu machine     (q35 has PCIe + an ACPI MCFG table, which
#                                     the PCI registration code needs; the older
#                                     i440fx "pc" machine has no MCFG)
#   ACCEL=auto|kvm|tcg   acceleration (default auto: kvm if /dev/kvm is usable)
#   GPU=none|virtio|virtio-vga   add a display device for CoreDisplay to bind
#                                (virtio -> virtio-gpu-pci 1af4:1050, which the
#                                 VirtioGpu driver matches via devices.txt)
#   DISPLAY_MODE=none|gtk|sdl    qemu display backend (default none = headless)
#   SCREENSHOT=path  after booting, dump the guest screen to a PPM and exit
#   TIMEOUT=30      seconds before auto-killing the guest (0 = no timeout)
#   SENDKEY=a[,b..] after SENDKEY_DELAY s, inject these keys via the qemu monitor
#                   (exercises the PS/2 keyboard IRQ headlessly); serial -> stdout
#   SENDKEY_DELAY=12  seconds to wait for boot before injecting keys
#   EXTRA="..."     extra qemu args
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${ISO:-$REPO_ROOT/build/ISO/os.iso}"
MEM="${MEM:-512}"
SMP="${SMP:-2}"
MACHINE="${MACHINE:-q35}"
ACCEL="${ACCEL:-auto}"
GPU="${GPU:-none}"
DISPLAY_MODE="${DISPLAY_MODE:-none}"
SCREENSHOT="${SCREENSHOT:-}"
TIMEOUT="${TIMEOUT:-30}"
EXTRA="${EXTRA:-}"

gpu_args=()
vga_arg=(-vga std)
case "$GPU" in
  none) ;;
  virtio)      gpu_args=(-device virtio-gpu-pci) ;;                 # secondary; std VGA is primary
  virtio-vga)  gpu_args=(-device virtio-vga); vga_arg=(-vga none) ;; # virtio-gpu IS the primary display
  *) echo "error: unknown GPU=$GPU (none|virtio|virtio-vga)" >&2; exit 1 ;;
esac

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

common_args=(
  -machine "$MACHINE"
  "${accel_args[@]}"
  -m "$MEM" -smp "$SMP"
  -cdrom "$ISO" -boot d
  "${vga_arg[@]}"
  "${gpu_args[@]}"
  -no-reboot -no-shutdown
  -d guest_errors
)

# Screenshot mode: boot with a control monitor, wait, dump the screen, quit.
if [ -n "$SCREENSHOT" ]; then
  command -v python3 >/dev/null || { echo "error: python3 needed for SCREENSHOT" >&2; exit 1; }
  sock="$(mktemp -u /tmp/cardinal-qmon.XXXX.sock)"
  wait_s="${SCREENSHOT_DELAY:-12}"
  echo "[run-qemu] booting ${wait_s}s then dumping screen -> $SCREENSHOT"
  qemu-system-x86_64 "${common_args[@]}" \
    -serial file:/tmp/cardinal-qemu-serial.log \
    -display none \
    -monitor "unix:$sock,server,nowait" $EXTRA &
  qpid=$!
  python3 - "$sock" "$SCREENSHOT" "$wait_s" <<'PY'
import socket, sys, time
sock, out, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except OSError: time.sleep(0.2)
else:
    print("could not connect to qemu monitor"); sys.exit(1)
time.sleep(wait)
s.sendall(b"screendump " + out.encode() + b"\n"); time.sleep(1.0)
s.sendall(b"quit\n"); time.sleep(0.3); s.close()
PY
  wait "$qpid" 2>/dev/null || true
  echo "[run-qemu] serial log: /tmp/cardinal-qemu-serial.log"
  ls -l "$SCREENSHOT" 2>/dev/null
  exit 0
fi

# Keystroke-injection mode: boot headless with a control monitor, wait for boot,
# inject keys (firing the PS/2 keyboard IRQ), let it run to TIMEOUT, dump serial.
if [ -n "${SENDKEY:-}" ]; then
  command -v python3 >/dev/null || { echo "error: python3 needed for SENDKEY" >&2; exit 1; }
  sock="$(mktemp -u /tmp/cardinal-qmon.XXXX.sock)"
  trap 'rm -f "$sock"' EXIT
  serial="/tmp/cardinal-qemu-serial.log"
  delay="${SENDKEY_DELAY:-12}"
  : > "$serial"
  echo "[run-qemu] booting headless; will inject keys '$SENDKEY' after ${delay}s"
  # Use a (headless) VNC display rather than -display none: the input subsystem
  # needs a console to attach the PS/2 keyboard to, or monitor `sendkey` has
  # nowhere to route and the guest IRQ never fires. No VNC client need connect.
  # Auto-pick a free display (start at SENDKEY_VNC, scan upward) so a stale/busy
  # port doesn't make QEMU exit silently.
  timeout --foreground "${TIMEOUT:-30}" \
    qemu-system-x86_64 "${common_args[@]}" \
      -serial "file:$serial" \
      -vnc "127.0.0.1:${SENDKEY_VNC:-20},to=99" \
      -monitor "unix:$sock,server,nowait" $EXTRA &
  qpid=$!
  python3 - "$sock" "$delay" "$SENDKEY" <<'PY'
import socket, sys, time
sock, delay, keys = sys.argv[1], float(sys.argv[2]), sys.argv[3]
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except OSError: time.sleep(0.2)
else:
    print("could not connect to qemu monitor"); sys.exit(1)
time.sleep(delay)
for k in keys.split(','):
    s.sendall(b"sendkey " + k.encode() + b"\n"); time.sleep(0.3)
s.close()
PY
  wait "$qpid" 2>/dev/null || true
  echo "[run-qemu] ---- serial log ----"
  cat "$serial"
  exit 0
fi

set -x
exec timeout --foreground "${TIMEOUT:-30}" \
  qemu-system-x86_64 "${common_args[@]}" \
    -serial stdio \
    -display "$DISPLAY_MODE" \
    $EXTRA
