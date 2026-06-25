#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Host-side terminal for the Cardinal; interactive serial Lisp REPL.
#
# The kernel, booted with "cardinal.repl", switches its one serial link into
# framed CSMUX -- the debug log rides channel 0 and the REPL rides channel 2 -- so
# a raw terminal sees byte-stuffed garbage. This tool demultiplexes that stream:
# it prints the log + REPL output to your terminal and frames whatever you type
# onto channel 2 (the REPL's input). Type Lisp, press Enter; e.g. (+ 1 2) -> 3.
#
# Two transports:
#   - QEMU (default): launches qemu booting build/ISO/os-repl.iso, the mux over a
#     unix-socket COM1 (or an emulated FTDI with --link ftdi).
#   - Real hardware: --serial-device /dev/ttyUSB0 talks to an actual adapter.
#
# Two modes:
#   - interactive (default): relay your terminal <-> the REPL.
#   - --exec "<lisp>" (repeatable) / --script FILE: wait for the REPL-ready
#     banner, send each line, print the output, exit after it goes quiet. Used to
#     drive the REPL non-interactively (tests).
#
# Frame format mirrors modules/SysDebug/src/csmux.c:
#   0x7E | chan(1) | len(2 LE) | payload | crc16-ccitt(2 LE) | 0x7E  (byte-stuffed)

import argparse
import os
import select
import shlex
import socket
import subprocess
import sys
import tempfile
import time

CH_LOG = 0
CH_CTRL = 1
CH_REPL = 2

CSMUX_MAX_PAYLOAD = 1024
SOF = 0x7E
ESC = 0x7D
ESC_XOR = 0x20

READY_BANNER = b"serial REPL ready"  # printed by start-repl once it is parked


def crc16_ccitt(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return crc


def frame(chan, payload):
    body = bytes([chan, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
    crc = crc16_ccitt(body)
    body += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    out = bytearray([SOF])
    for b in body:
        if b in (SOF, ESC):
            out.append(ESC)
            out.append(b ^ ESC_XOR)
        else:
            out.append(b)
    out.append(SOF)
    return bytes(out)


class Deframer:
    """Feed raw bytes; yields ('raw', bytes) for inter-frame bytes (the pre-CSMUX
    boot log) and ('frame', chan, payload) for complete, CRC-valid frames."""

    def __init__(self):
        self.inframe = False
        self.escape = False
        self.buf = bytearray()
        self.raw = bytearray()

    def feed(self, data):
        events = []
        for b in data:
            if b == SOF:
                if self.raw:
                    events.append(("raw", bytes(self.raw)))
                    self.raw = bytearray()
                if self.inframe and len(self.buf) >= 5:
                    ev = self._finish()
                    if ev:
                        events.append(ev)
                self.inframe = True
                self.escape = False
                self.buf = bytearray()
                continue
            if not self.inframe:
                self.raw.append(b)
                continue
            if b == ESC:
                self.escape = True
                continue
            if self.escape:
                b ^= ESC_XOR
                self.escape = False
            self.buf.append(b)
        # Stream any inter-frame (raw) bytes as they arrive -- the pre-CSMUX boot
        # log has no frame boundary to flush on, so emit it per feed() call.
        if self.raw:
            events.append(("raw", bytes(self.raw)))
            self.raw = bytearray()
        return events

    def _finish(self):
        n = len(self.buf)
        if n < 5:
            return None
        chan = self.buf[0]
        ln = self.buf[1] | (self.buf[2] << 8)
        if ln != n - 5:
            return None
        if crc16_ccitt(self.buf[: 3 + ln]) != (self.buf[3 + ln] | (self.buf[3 + ln + 1] << 8)):
            return None
        return ("frame", chan, bytes(self.buf[3: 3 + ln]))


# Adapter so a real serial tty looks like the unix socket the loop expects.
class SerialTTY:
    def __init__(self, dev, baud):
        import termios
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        speed = getattr(termios, "B%d" % baud, termios.B115200)
        a[0] = 0
        a[1] = 0
        a[3] = 0
        a[2] = (a[2] & ~termios.CSIZE & ~termios.PARENB & ~termios.CSTOPB) | termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = speed
        a[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def fileno(self):
        return self.fd

    def setblocking(self, flag):
        pass

    def recv(self, n):
        try:
            return os.read(self.fd, n)
        except (BlockingIOError, InterruptedError):
            return b""

    def sendall(self, b):
        mv = memoryview(b)
        while mv:
            try:
                mv = mv[os.write(self.fd, mv):]
            except (BlockingIOError, InterruptedError):
                continue

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


def out(data):
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def connect_qemu(args):
    if not os.path.exists(args.iso):
        sys.stderr.write("csmux-repl: ISO not found: %s "
                         "(build the 'repl-image' target)\n" % args.iso)
        sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="csmux-repl-")
    serial_path = os.path.join(tmp, "serial.sock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(serial_path)
    srv.listen(1)
    accel = ["-accel", args.accel] if args.accel else []
    cmd = [args.qemu, "-machine", args.machine, *accel, "-m", args.mem, "-smp",
           args.smp, "-cdrom", args.iso, "-boot", "d", "-no-reboot", "-display",
           "none", "-chardev", "socket,id=mux,path=%s,server=off" % serial_path]
    if args.link == "ftdi":
        com1_log = os.path.join(tmp, "com1.log")
        cmd += ["-serial", "file:%s" % com1_log,
                "-device", "qemu-xhci,id=xhci",
                "-device", "usb-serial,chardev=mux,bus=xhci.0"]
        sys.stderr.write("csmux-repl: FTDI link; pre-mux boot log -> %s\n" % com1_log)
    else:
        cmd += ["-serial", "chardev:mux"]
    # QEMU_EXTRA: extra qemu args (shell-split), e.g. attaching an HD Audio
    # controller + a wav audiodev to drive/capture the REPL (play-tone) command.
    extra = os.environ.get("QEMU_EXTRA")
    if extra:
        cmd += shlex.split(extra)
    sys.stderr.write("csmux-repl: launching: %s\n" % " ".join(cmd))
    qemu = subprocess.Popen(cmd)
    srv.settimeout(20)
    try:
        ser, _ = srv.accept()
    except socket.timeout:
        sys.stderr.write("csmux-repl: QEMU did not connect to the serial socket\n")
        qemu.kill()
        sys.exit(2)
    return ser, qemu


def main():
    ap = argparse.ArgumentParser(description="Cardinal; serial Lisp REPL terminal")
    ap.add_argument("--iso", default=os.environ.get("ISO", "build/ISO/os-repl.iso"))
    ap.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    ap.add_argument("--machine", default=os.environ.get("MACHINE", "q35"))
    ap.add_argument("--accel", default=os.environ.get("ACCEL", "kvm"))
    ap.add_argument("--mem", default=os.environ.get("MEM", "512"))
    ap.add_argument("--smp", default=os.environ.get("SMP", "2"))
    ap.add_argument("--link", choices=["com1", "ftdi"],
                    default=os.environ.get("LINK", "com1"))
    ap.add_argument("--serial-device", default=os.environ.get("SERIAL_DEVICE"),
                    help="real tty (e.g. /dev/ttyUSB0); skips QEMU")
    ap.add_argument("--baud", type=int, default=int(os.environ.get("BAUD", "115200")))
    ap.add_argument("--exec", action="append", default=[], metavar="LISP",
                    help="send this line once the REPL is ready (repeatable)")
    ap.add_argument("--script", help="file of REPL input lines to send when ready")
    ap.add_argument("--timeout", type=int, default=int(os.environ.get("TIMEOUT", "60")),
                    help="overall budget in non-interactive mode (s)")
    ap.add_argument("--idle", type=float, default=2.0,
                    help="quit --exec mode after this many seconds with no output")
    args = ap.parse_args()

    sends = list(args.exec)
    if args.script:
        with open(args.script) as f:
            sends += [ln.rstrip("\n") for ln in f if ln.strip()]
    scripted = bool(sends)

    if args.serial_device:
        ser = SerialTTY(args.serial_device, args.baud)
        qemu = None
        sys.stderr.write("csmux-repl: on real serial %s @ %d\n"
                         % (args.serial_device, args.baud))
    else:
        ser, qemu = connect_qemu(args)
    ser.setblocking(False)

    deframer = Deframer()
    ready = False
    pending = b"".join((s + "\n").encode() for s in sends)
    last_out = time.time()
    deadline = time.time() + args.timeout
    stdin_fd = sys.stdin.fileno()
    inputs = [ser] if scripted else [ser, stdin_fd]

    if not scripted:
        out(b"[csmux-repl] connected; type Lisp at the REPL, Ctrl-C to quit.\n")

    try:
        while True:
            if qemu is not None and qemu.poll() is not None:
                break
            if scripted and time.time() > deadline:
                sys.stderr.write("csmux-repl: timeout\n")
                break
            if scripted and ready and not pending and time.time() - last_out > args.idle:
                break  # REPL went quiet after our input -> done
            r, _, _ = select.select(inputs, [], [], 0.3)
            if ser in r:
                data = ser.recv(4096)
                if data:
                    for ev in deframer.feed(data):
                        if ev[0] == "raw":
                            out(ev[1])
                        else:
                            _, chan, payload = ev
                            out(payload)  # log (ch0), ctrl (ch1), and REPL (ch2)
                        last_out = time.time()
                        if not ready and READY_BANNER in (ev[1] if ev[0] == "raw" else ev[2]):
                            ready = True
            if not scripted and stdin_fd in r:
                line = os.read(stdin_fd, 4096)
                if not line:
                    break  # Ctrl-D
                ser.sendall(frame(CH_REPL, line))
            # In scripted mode, send queued input once the REPL announces ready.
            if scripted and ready and pending:
                ser.sendall(frame(CH_REPL, pending))
                pending = b""
                last_out = time.time()
    except KeyboardInterrupt:
        pass
    finally:
        if qemu is not None:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
