#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# AtomicPi serial test harness over the wireless bridge (a D1 mini on
# /dev/ttyUSB0, 115200 8N1; see /home/hgoel1/serial_bridge/). Power control
# escapes understood by the bridge:
#     \x1d 0  -> power OFF (held)      \x1d 1 -> power ON
#     \x1d p  -> fixed off/on cycle    \x1d \x1d -> literal 0x1d
# The rig acks with "[power: off]" / "[power: on]".
#
# Subcommands:
#   off / on                   hold power off / restore it (confirm ack)
#   reset [--off-ms N]         power off, hold N ms, power on (confirm ack)
#   monitor [--seconds N]      just listen and print readable text
#   boot [--off-ms N ...]      power-cycle with an N-ms off hold, AUTO-RETRY until
#                              GRUB output, then capture the whole boot. Holding
#                              off longer (bigger --off-ms) is the knob for the
#                              every-other-boot failure (incomplete discharge).
#
# Needs root for /dev/ttyUSB0 and pyserial:
#   sudo $(which python3) scripts/atomicpi-serial.py boot
import argparse, re, sys, time
from collections import Counter

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: python3 -m pip install pyserial")

OFF = b"\x1d0"
ON = b"\x1d1"
ACK_OFF = b"[power: off]"
ACK_ON = b"[power: on]"
PRINTABLE = re.compile(rb"[\x20-\x7e]{16,}")  # >=16 printable chars = real text
FLOOD_BYTES = (0x00, 0x1C, 0xE0, 0xFC)        # wrong-baud signature (legacy diag)
# Bridge diagnostic beacons injected into the stream by the rig firmware; pure
# noise for OS-boot analysis, so strip them everywhere we save/print.
BEACON = re.compile(rb"\n?\[\[rig[^\]]*\]\]\n?|<rig-tcp[^>]*>\n?")


def strip_beacons(b):
    return BEACON.sub(b"", bytes(b))


class BeaconFilter:
    """Streaming beacon stripper: filters as bytes arrive, holding back a small
    tail so a beacon split across two reads is still caught."""
    def __init__(self, f):
        self.f = f
        self.pend = b""

    def write(self, d):
        self.pend += bytes(d)
        # Keep the last chunk in case it's a partial beacon; flush the rest clean.
        if len(self.pend) > 256:
            head, self.pend = self.pend[:-128], self.pend[-128:]
            self.f.write(strip_beacons(head))

    def flush(self):
        self.f.write(strip_beacons(self.pend))
        self.pend = b""


def open_port(args):
    return serial.Serial(args.port, args.baud, timeout=0.5)


def send_wait(s, cmd, ack, timeout=4.0, retries=3):
    """Send a power escape and wait (bounded) for its ack, resending on no-ack.
    The rig always re-acks (even if already in that state), so a resend after a
    lost frame -- e.g. one missed while the rig was mid-WiFi-reassociation -- is
    still confirmable. Returns (ok, buf)."""
    buf = bytearray()
    for _ in range(retries):
        s.write(cmd)
        s.flush()
        end = time.time() + timeout
        while time.time() < end:
            d = s.read(4096)
            if d:
                buf += d
                if ack in bytes(buf):
                    return True, buf
    return ack in bytes(buf), buf


def power_cycle(s, off_ms, label=""):
    """Off, hold off_ms, on -- each step ack-confirmed. Returns True if both acked."""
    s.reset_input_buffer()
    ok_off, _ = send_wait(s, OFF, ACK_OFF)
    print(f"{label}power OFF: {'ack' if ok_off else 'NO ACK'}", flush=True)
    time.sleep(off_ms / 1000.0)
    ok_on, _ = send_wait(s, ON, ACK_ON)
    print(f"{label}power ON ({off_ms}ms off): {'ack' if ok_on else 'NO ACK'}", flush=True)
    return ok_off and ok_on


def report(buf, out):
    buf = strip_beacons(buf)          # beacons filtered out of the saved file + analysis
    open(out, "wb").write(buf)
    print(f"\nsaved {len(buf)}B -> {out}")
    print("\n=== readable ASCII runs (>=4 chars) ===")
    for r in re.findall(rb"[\x20-\x7e\r\n]{4,}", bytes(buf))[:80]:
        print(repr(r.decode("latin1")))
    nonprint = bytes(b for b in buf if not (32 <= b < 127 or b in (10, 13)))
    if nonprint:
        flood = sum(Counter(nonprint)[x] for x in FLOOD_BYTES)
        print(f"\nnon-printable: {len(nonprint)}B; "
              f"{{00,1C,E0,FC}} = {100*flood/len(nonprint):.0f}% "
              f"(high % => wrong-baud flood, not real output)")
    print("\n=== markers ===")
    for m in (b"[SPCR]", b"space=", b"[Kernel]", b"Load module",
              b"servicescript", b"PANIC", b"ALL TESTS"):
        print(f"  {m.decode():16} {'FOUND' if m in bytes(buf) else 'no'}")


def cmd_boot(args):
    """Single power-cycle, then stream the boot to --out LIVE (so it can be
    tailed mid-capture) with the bridge beacons filtered out. No auto-retries:
    the board boots reliably now, and a retry would power-cycle away the very
    state we want to look at (e.g. a panic/debug prompt)."""
    s = open_port(args)
    power_cycle(s, args.off_ms)
    s.reset_input_buffer()
    # Unbuffered, beacon-filtering live writer to --out.
    f = open(args.out, "wb", buffering=0)
    fw = BeaconFilter(f)
    buf = bytearray()

    print(f"waiting up to {args.grub_timeout}s for boot output...", flush=True)
    deadline = time.time() + args.grub_timeout
    while time.time() < deadline:
        d = s.read(4096)
        if d:
            buf += d
            fw.write(d)
            if PRINTABLE.search(bytes(buf)):
                print("boot output seen; capturing...", flush=True)
                break
    else:
        print(f"no boot output within {args.grub_timeout}s ({len(buf)}B)", flush=True)

    end = time.time() + args.boot_window
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            fw.write(d)
    fw.flush()
    f.close()
    s.close()
    # report() re-writes --out with the cleaned full buffer + prints analysis.
    report(buf, args.out)


def cmd_probe(args):
    """Drive the in-kernel debug shell over the bridge, fully autonomously:
    power-cycle, select the GRUB "debug shell" entry via its serial hotkey 'd',
    wait for the shell prompt, then send each ';'-separated command and capture
    its reply. Lets the bring-up sequence be explored without any card swap:
      probe --cmds 'r 0xe0100000;r 0xe0100008;d 0xe0100000'"""
    s = open_port(args)
    if not args.no_cycle:
        power_cycle(s, args.off_ms)
    s.reset_input_buffer()

    # 1. Wait for the GRUB menu (countdown widened to ~15s), then boot the debug
    #    entry via its hotkey 'd'. A single ASCII char is reliable over serial,
    #    where arrow-key escape sequences are not. Keep tapping 'd' until the
    #    kernel starts (GRUB consumed it and is booting).
    print("waiting for GRUB menu...", flush=True)
    buf = bytearray()
    deadline = time.time() + args.grub_timeout
    menu = False
    while time.time() < deadline:
        d = s.read(256)
        if d:
            buf += d
            cb = strip_beacons(buf)
            if b"Cardinal" in cb or b"GNU GRUB" in cb or b"automatically" in cb:
                menu = True
                break
    if not menu:
        print("GRUB menu not detected; dump follows", flush=True)
        report(buf, args.out)
        s.close()
        return
    print("menu seen; pressing hotkey 'd'...", flush=True)
    s.timeout = 0.2
    kdeadline = time.time() + args.grub_timeout
    kernel = False
    KM = (b"KERNEL DEBUG", b"[Kernel]", b"Loaded at")
    last = 0
    while time.time() < kdeadline:
        if time.time() - last > 0.3:
            s.write(b"d"); s.flush(); last = time.time()
        d = s.read(256)
        if d:
            buf += d
            if any(m in strip_beacons(buf) for m in KM):
                kernel = True
                break
    if not kernel:
        print("kernel never started; dump follows", flush=True)
        report(buf, args.out)
        s.close()
        return

    # 2. Wait for the UNIQUE kernel banner "Entering debug shell" (the GRUB menu
    #    entry name also contains "debug shell", so don't match on that). If it
    #    never appears, the default entry booted -> report instead of probing it.
    print("kernel booting; waiting for 'Entering debug shell'...", flush=True)
    s.timeout = 0.5
    deadline = time.time() + args.boot_timeout
    ok = False
    while time.time() < deadline:
        d = s.read(4096)
        if d:
            buf += d
            if b"Entering debug shell" in strip_beacons(buf):
                ok = True
                break
    if not ok:
        print("debug banner not seen (default entry likely booted); dump follows",
              flush=True)
        report(buf, args.out)
        s.close()
        return
    # Drain any stray buffered 'd' presses before issuing real commands.
    time.sleep(0.5)
    s.write(b"\r"); s.flush()
    time.sleep(0.7)
    s.reset_input_buffer()

    # 3. Run each command, reading until the '>' prompt returns.
    cmds = [c.strip() for c in args.cmds.split(";") if c.strip()]
    transcript = bytearray()
    for c in cmds:
        s.write(c.encode() + b"\r")
        s.flush()
        end = time.time() + args.cmd_timeout
        reply = bytearray()
        while time.time() < end:
            d = s.read(4096)
            if d:
                reply += d
                if strip_beacons(reply).rstrip().endswith(b">"):
                    break
            elif reply:
                break
        clean = strip_beacons(reply).decode("latin1")
        print(f"$ {c}\n{clean}", flush=True)
        transcript += b"$ " + c.encode() + b"\n" + strip_beacons(reply)
    open(args.out, "wb").write(transcript)
    print(f"\nsaved transcript -> {args.out}", flush=True)
    s.close()


def cmd_monitor(args):
    s = open_port(args)
    buf = bytearray()
    end = time.time() + args.seconds
    print(f"listening {args.seconds}s on {args.port} @ {args.baud}...", flush=True)
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
    s.close()
    report(buf, args.out)


def cmd_reset(args):
    s = open_port(args)
    ok = power_cycle(s, args.off_ms)
    s.close()
    sys.exit(0 if ok else 1)


def cmd_off(args):
    s = open_port(args)
    ok, _ = send_wait(s, OFF, ACK_OFF)
    s.close()
    print("power OFF: ack" if ok else "NO ACK")
    sys.exit(0 if ok else 1)


def cmd_on(args):
    s = open_port(args)
    ok, _ = send_wait(s, ON, ACK_ON)
    s.close()
    print("power ON: ack" if ok else "NO ACK")
    sys.exit(0 if ok else 1)


def main():
    p = argparse.ArgumentParser(description="AtomicPi serial test harness")
    p.add_argument("--port", default="/dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--out", default="/tmp/atomicpi_boot.bin")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("boot", help="power-cycle once, stream boot live (no retries)")
    b.add_argument("--off-ms", type=int, default=8000, help="power-off hold (ms)")
    b.add_argument("--grub-timeout", type=int, default=75,
                   help="how long to wait for first boot output")
    b.add_argument("--boot-window", type=int, default=75,
                   help="how long to capture after boot output appears")
    b.set_defaults(func=cmd_boot)

    pr = sub.add_parser("probe", help="drive the in-kernel hwprobe console")
    pr.add_argument("--cmds", required=True,
                    help="';'-separated hwprobe commands, e.g. 'r 0xe0100000;d 0xe0100000'")
    pr.add_argument("--off-ms", type=int, default=8000)
    pr.add_argument("--grub-timeout", type=int, default=75,
                    help="window to press the hotkey through POST+GRUB")
    pr.add_argument("--boot-timeout", type=int, default=60,
                    help="wait for the debug-shell banner after the kernel starts")
    pr.add_argument("--cmd-timeout", type=float, default=4.0)
    pr.add_argument("--no-cycle", action="store_true",
                    help="don't power-cycle first (board already at prompt)")
    pr.set_defaults(func=cmd_probe)

    m = sub.add_parser("monitor", help="just listen")
    m.add_argument("--seconds", type=int, default=120)
    m.set_defaults(func=cmd_monitor)

    r = sub.add_parser("reset", help="off, hold, on (confirm acks)")
    r.add_argument("--off-ms", type=int, default=5000)
    r.set_defaults(func=cmd_reset)

    o = sub.add_parser("off", help="hold power off")
    o.set_defaults(func=cmd_off)
    n = sub.add_parser("on", help="restore power")
    n.set_defaults(func=cmd_on)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
