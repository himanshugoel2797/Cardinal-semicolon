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
#   NIC=none|virtio-net|rtl8139  attach a NIC on slirp: virtio-net-pci (1af4:1041)
#                                or the legacy Realtek rtl8139 (10ec:8139)
#   NET_PCAP=path                with NIC set, dump the link to a pcap file
#   DISK=path        attach an ICH9 AHCI controller (8086:2922) + a raw-image SATA
#                    drive on port 0 (drives the Lisp ahci block-device driver)
#   AUDIO=none|hda   attach an Intel HD Audio controller (ich9-intel-hda, 8086:293e)
#                    + an hda-output line-out codec, with a `wav` audiodev that
#                    captures everything the codec plays to AUDIO_WAV. Drives the
#                    Lisp hdaudio driver; a non-silent WAV is the playback proof.
#   AUDIO_WAV=path   where AUDIO=hda writes the captured audio (default
#                    /tmp/cardinal-audio.wav)
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

# Optional NIC. NIC=virtio-net attaches a virtio-net-pci (1af4:1041) on a user-mode
# (slirp) netdev; NET_PCAP=path also captures the link to a pcap for inspection.
# HOSTFWD="tcp::5555-:7" forwards a host port to a guest port over slirp (repeatable
# with commas), so a host tool (e.g. `nc localhost 5555`) can reach a guest TCP
# service -- the way to drive the TCP echo (port 7) from outside.
nic_args=()
netdev_user="user,id=n0"
if [ -n "${HOSTFWD:-}" ]; then
  IFS=',' read -ra _fwds <<< "$HOSTFWD"
  for f in "${_fwds[@]}"; do netdev_user+=",hostfwd=$f"; done
fi
case "${NIC:-none}" in
  none) ;;
  virtio-net)
    # disable-legacy=on forces a MODERN virtio device (device id 1af4:1041, the
    # PCI-capability layout); the transitional default would enumerate as the
    # legacy 1af4:1000 with an I/O-port BAR.
    nic_args=(-netdev "$netdev_user" -device "virtio-net-pci,disable-legacy=on,netdev=n0")
    [ -n "${NET_PCAP:-}" ] && nic_args+=(-object "filter-dump,id=d0,netdev=n0,file=$NET_PCAP")
    ;;
  rtl8139)
    # The Realtek RTL8139 (10ec:8139) -- a legacy INTx, 32-bit-DMA NIC -- on the
    # same slirp netdev. Drives the Lisp rtl8139 driver (no virtio-net present, so
    # init's NIC gating falls through to it).
    nic_args=(-netdev "$netdev_user" -device "rtl8139,netdev=n0")
    [ -n "${NET_PCAP:-}" ] && nic_args+=(-object "filter-dump,id=d0,netdev=n0,file=$NET_PCAP")
    ;;
  *) echo "error: unknown NIC=$NIC (none|virtio-net|rtl8139)" >&2; exit 1 ;;
esac

# Optional SECOND NIC on a distinct slirp subnet (10.0.3.0/24), to exercise the
# multi-homed/routing path: NIC2=virtio-net|rtl8139 attaches another NIC of that
# type so init enumerates and brings up both, and each interface DHCPs on its own
# subnet. Requires NIC set (the first NIC stays on the default 10.0.2.0/24).
case "${NIC2:-none}" in
  none) ;;
  virtio-net) nic_args+=(-netdev "user,id=n1,net=10.0.3.0/24,dhcpstart=10.0.3.15"
                         -device "virtio-net-pci,disable-legacy=on,netdev=n1") ;;
  rtl8139)    nic_args+=(-netdev "user,id=n1,net=10.0.3.0/24,dhcpstart=10.0.3.15"
                         -device "rtl8139,netdev=n1") ;;
  *) echo "error: unknown NIC2=$NIC2 (none|virtio-net|rtl8139)" >&2; exit 1 ;;
esac

# Optional AHCI disk. DISK=path attaches an ICH9 AHCI controller (8086:2922, which
# the Lisp ahci driver matches) with a raw-image SATA drive on port 0, exercising
# the block-device read/write path. Mirrors the NIC case structure.
disk_args=()
if [ -n "${DISK:-}" ]; then
  [ -f "$DISK" ] || { echo "error: DISK image not found: $DISK" >&2; exit 1; }
  disk_args=(-device ich9-ahci,id=ahci0 \
             -drive id=disk,file="$DISK",format=raw,if=none \
             -device ide-hd,drive=disk,bus=ahci0.0)
fi

# Optional USB. USB=<controller>-<device> attaches a host controller the Lisp USB
# stack binds (uhci -> piix3-usb-uhci 8086:7020; xhci -> qemu-xhci 1b36:000d) plus
# a device the class drivers claim: kbd (usb-kbd, HID), storage (usb-storage on a
# small raw disk), hub (usb-hub with a kbd behind it). Drives coreusb + the HCI +
# class drivers end to end.
usb_args=()
case "${USB:-none}" in
  none) ;;
  uhci-kbd)     usb_args=(-device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0) ;;
  xhci-kbd)     usb_args=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0) ;;
  uhci-hub)     usb_args=(-device piix3-usb-uhci,id=uhci
                          -device usb-hub,bus=uhci.0,port=1
                          -device usb-kbd,bus=uhci.0,port=1.1) ;;
  xhci-storage|uhci-storage)
    ctl="qemu-xhci"; bus="xhci"
    [ "${USB}" = uhci-storage ] && { ctl="piix3-usb-uhci"; bus="uhci"; }
    usbdisk="${USB_DISK:-/tmp/cardinal-usb-disk.img}"
    [ -f "$usbdisk" ] || { command -v qemu-img >/dev/null && qemu-img create -f raw "$usbdisk" 8M >/dev/null; }
    usb_args=(-device "$ctl,id=$bus"
              -drive id=usbdisk,file="$usbdisk",format=raw,if=none
              -device "usb-storage,drive=usbdisk,bus=$bus.0") ;;
  *) echo "error: unknown USB=$USB (none|uhci-kbd|xhci-kbd|uhci-hub|uhci-storage|xhci-storage)" >&2; exit 1 ;;
esac

# Optional HD Audio. AUDIO=hda attaches an ich9-intel-hda controller (8086:293e,
# the first ID the Lisp hdaudio driver tries) plus an hda-output line-out codec,
# routed to a `wav` audiodev that records the played audio to AUDIO_WAV. With the
# driver's bring-up tone, the captured WAV is non-silent -- the end-to-end proof
# that a guest stream reaches a host sink.
audio_args=()
case "${AUDIO:-none}" in
  none) ;;
  hda)
    AUDIO_WAV="${AUDIO_WAV:-/tmp/cardinal-audio.wav}"
    audio_args=(-audiodev "wav,id=snd0,path=$AUDIO_WAV"
                -device ich9-intel-hda,id=hda0
                -device hda-output,bus=hda0.0,audiodev=snd0)
    echo "[run-qemu] HD Audio capture -> $AUDIO_WAV"
    ;;
  *) echo "error: unknown AUDIO=$AUDIO (none|hda)" >&2; exit 1 ;;
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
  "${nic_args[@]}"
  "${disk_args[@]}"
  "${usb_args[@]}"
  "${audio_args[@]}"
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
