#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# SysTest death-test harness (LOCAL runs only -- never used in web CI).
#
# Launches a single persistent QEMU (NO -no-reboot) booting the harness ISO
# (kernel cmdline "cardinal.test cardinal.harness"), demultiplexes the CSMUX
# stream coming over one serial link, and drives the death-test control protocol:
#
#   - ch0 (LOG)  : human-readable debug log -> stdout + the log file
#   - ch1 (CTRL) : the death-test control FSM (handshake + cursor across reboots)
#   - ch2 (GDB)  : bridged to a local TCP port so GDB can attach (optional)
#
# Death-test flow: each boot the guest sends HELLO; we reply OLEH with the death
# cursor. The guest runs the death test at the cursor, which kills the kernel and
# reboots (0xCF9); on the next HELLO we have already recorded that death and we
# advance the cursor. A death that fails to kill the kernel ("SURVIVED") or hangs
# (no outcome before a per-death timeout -> we force a QEMU system_reset) is a
# failure. When the cursor passes the last death test the guest sends ALLDONE and
# exits via isa-debug-exit. We then emit the aggregate
# "[SysTest] ALL TESTS PASSED" / "TESTS FAILED" sentinel into the log file.
#
# Frame format (mirror of modules/SysDebug/src/csmux.c):
#   0x7E | chan(1) | len(2 LE) | payload | crc16(2 LE) | 0x7E   (body byte-stuffed)

import argparse
import os
import select
import socket
import subprocess
import sys
import tempfile
import time

CH_LOG = 0
CH_CTRL = 1
CH_GDB = 2

SOF = 0x7E
ESC = 0x7D
ESC_XOR = 0x20


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
    """Feed raw bytes; yields ('raw', bytes) for inter-frame bytes and
    ('frame', chan, payload) for complete, CRC-valid frames."""

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
        return ("frame", chan, bytes(self.buf[3 : 3 + ln]))


def parse_kv(line, key):
    i = line.find(key)
    if i < 0:
        return None
    i += len(key)
    j = i
    if j < len(line) and line[j] == "-":
        j += 1
    while j < len(line) and line[j].isdigit():
        j += 1
    try:
        return int(line[i:j])
    except ValueError:
        return None


class Harness:
    def __init__(self, args):
        self.args = args
        self.cursor = 0
        self.death_count = None
        self.normal_fails = None
        self.begun = None          # cursor of the death test in progress
        self.begun_expect = None
        self.begun_deadline = None
        self.verdicts = {}         # cursor -> (ok: bool, detail: str)
        self.reboots = 0
        self.done = False
        self.log = open(args.log, "wb")
        self.serial = None         # connected serial socket
        self.monitor = None        # connected monitor socket

    # ---- logging ----
    def emit(self, text):
        data = text.encode() if isinstance(text, str) else text
        self.log.write(data)
        self.log.flush()
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    # ---- control channel ----
    def send_ctrl(self, msg):
        if self.serial:
            self.serial.sendall(frame(CH_CTRL, (msg + "\n").encode()))

    def monitor_cmd(self, cmd):
        if self.monitor:
            try:
                self.monitor.sendall((cmd + "\n").encode())
            except OSError:
                pass

    def handle_ctrl_line(self, line):
        line = line.strip()
        if not line:
            return
        self.emit("    [ctrl<] " + line + "\n")
        if line.startswith("HELLO"):
            k = parse_kv(line, "deaths=")
            if k is not None:
                self.death_count = k
            # A HELLO while a death was outstanding == it rebooted (died) without
            # us having seen DIED. Resolve as a (vector-unconfirmed) pass.
            if self.begun is not None:
                self._record(self.begun, self.begun_expect is None or self.begun_expect < 0,
                             "died (vector unconfirmed via reboot)")
                self._advance(self.begun)
            self.reboots += 1
            self.send_ctrl("OLEH cursor=%d" % self.cursor)
        elif line.startswith("NORMALDONE"):
            self.normal_fails = parse_kv(line, "fails=")
        elif line.startswith("BEGIN"):
            c = parse_kv(line, "cursor=")
            self.begun = c
            self.begun_expect = parse_kv(line, "expect=")
            self.begun_deadline = time.monotonic() + self.args.death_timeout
        elif line.startswith("DIED"):
            c = parse_kv(line, "cursor=")
            vec = parse_kv(line, "vec=")
            exp = self.begun_expect
            ok = (exp is None or exp < 0 or vec == exp)
            detail = "died vec=%s" % vec if ok else "died vec=%s, expected %s" % (vec, exp)
            self._record(c, ok, detail)
            self._advance(c)
        elif line.startswith("SURVIVED"):
            c = parse_kv(line, "cursor=")
            self._record(c, False, "survived (kernel did not die)")
            self._advance(c)
        elif line.startswith("ALLDONE"):
            self.done = True

    def _record(self, cursor, ok, detail):
        if cursor in self.verdicts:
            return
        self.verdicts[cursor] = (ok, detail)
        self.emit("    [death %d] %s -- %s\n" % (cursor, "PASS" if ok else "FAIL", detail))

    def _advance(self, cursor):
        if cursor is not None and cursor >= self.cursor:
            self.cursor = cursor + 1
        self.begun = None
        self.begun_expect = None
        self.begun_deadline = None

    def check_timeout(self):
        if self.begun is not None and self.begun_deadline and time.monotonic() > self.begun_deadline:
            c = self.begun
            self._record(c, False, "hung (no outcome within %ss)" % self.args.death_timeout)
            self._advance(c)
            self.emit("    [death %d] forcing QEMU system_reset (hung)\n" % c)
            self.monitor_cmd("system_reset")

    # ---- finalize ----
    def finalize(self):
        # No handshake ever completed -> the guest never reached the harness
        # phase (very early crash, or no harness ISO). That is a failure, not a
        # vacuous pass.
        if self.death_count is None:
            self.emit("[SysTest] no harness handshake (HELLO never received)\n")
            self.emit("[SysTest] TESTS FAILED\n")
            self.log.close()
            return 1
        passed = (self.normal_fails == 0)
        for c in range(self.death_count or 0):
            ok, _ = self.verdicts.get(c, (False, "never ran"))
            if not ok:
                passed = False
        n_death_pass = sum(1 for v in self.verdicts.values() if v[0])
        self.emit("[SysTest] death tests: %d/%d passed; normal-phase fails=%s\n"
                  % (n_death_pass, self.death_count or 0, self.normal_fails))
        self.emit("[SysTest] ALL TESTS PASSED\n" if passed else "[SysTest] TESTS FAILED\n")
        self.log.close()
        return 0 if passed else 1


def main():
    ap = argparse.ArgumentParser(description="SysTest death-test harness (local only)")
    ap.add_argument("--iso", default="build/ISO/os-harness.iso")
    ap.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    ap.add_argument("--machine", default=os.environ.get("MACHINE", "q35"))
    ap.add_argument("--accel", default=os.environ.get("ACCEL", "tcg"))
    ap.add_argument("--mem", default=os.environ.get("MEM", "512"))
    ap.add_argument("--smp", default=os.environ.get("SMP", "2"))
    ap.add_argument("--log", default=os.environ.get("LOG", "build/systest-serial.log"))
    ap.add_argument("--gdb-port", type=int, default=int(os.environ.get("GDB_PORT", "1234")))
    ap.add_argument("--timeout", type=int, default=int(os.environ.get("TIMEOUT", "600")),
                    help="overall wall-clock budget in seconds")
    ap.add_argument("--death-timeout", type=int, default=int(os.environ.get("DEATH_TIMEOUT", "60")),
                    help="per-death-test budget before forcing a reset")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print("harness: ISO not found: %s (build the 'harness-image' target)" % args.iso, file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="systest-harness-")
    serial_path = os.path.join(tmp, "serial.sock")
    mon_path = os.path.join(tmp, "mon.sock")

    # Harness listens; QEMU connects as client (server=off) -> no startup race.
    serial_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    serial_srv.bind(serial_path)
    serial_srv.listen(1)
    mon_srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon_srv.bind(mon_path)
    mon_srv.listen(1)

    accel_args = ["-accel", args.accel] if args.accel else []
    qemu_cmd = [
        args.qemu,
        "-machine", args.machine,
        *accel_args,
        "-m", args.mem, "-smp", args.smp,
        "-cdrom", args.iso, "-boot", "d",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        # NOTE: deliberately NO -no-reboot, so the guest's 0xCF9 write reboots.
        "-chardev", "socket,id=cs0,path=%s,server=off" % serial_path,
        "-serial", "chardev:cs0",
        "-chardev", "socket,id=mon0,path=%s,server=off" % mon_path,
        "-mon", "chardev=mon0,mode=readline",
        "-display", "none",
    ]
    print("harness: launching:", " ".join(qemu_cmd), file=sys.stderr)
    qemu = subprocess.Popen(qemu_cmd)

    h = Harness(args)
    try:
        serial_srv.settimeout(15)
        h.serial, _ = serial_srv.accept()
        mon_srv.settimeout(15)
        try:
            h.monitor, _ = mon_srv.accept()
        except socket.timeout:
            print("harness: monitor socket not connected (hang-reset disabled)", file=sys.stderr)
    except socket.timeout:
        print("harness: QEMU did not connect to the serial socket", file=sys.stderr)
        qemu.kill()
        return 2

    h.serial.setblocking(False)
    deframer = Deframer()
    ctrl_buf = bytearray()
    deadline = time.monotonic() + args.timeout

    rc = 1
    try:
        while not h.done:
            if time.monotonic() > deadline:
                h.emit("[SysTest] harness overall timeout (%ss) -- aborting\n" % args.timeout)
                break
            if qemu.poll() is not None and not h.done:
                # QEMU exited (isa-debug-exit on ALLDONE, or crash). Drain handled
                # below; if we already saw ALLDONE this is the normal end.
                pass
            r, _, _ = select.select([h.serial], [], [], 0.5)
            h.check_timeout()
            if not r:
                if qemu.poll() is not None:
                    # No data and QEMU gone: give the FSM a moment, then stop.
                    if h.done or h.cursor >= (h.death_count or 0) and h.death_count is not None:
                        break
                    # Could be the final isa-debug-exit; stop if nothing pending.
                    break
                continue
            try:
                data = h.serial.recv(4096)
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                if qemu.poll() is not None:
                    break
                continue
            for ev in deframer.feed(data):
                if ev[0] == "raw":
                    h.emit(ev[1])  # GRUB / pre-CSMUX boot text
                else:
                    _, chan, payload = ev
                    if chan == CH_LOG:
                        h.emit(payload)
                    elif chan == CH_CTRL:
                        ctrl_buf.extend(payload)
                        while b"\n" in ctrl_buf:
                            line, _, rest = ctrl_buf.partition(b"\n")
                            ctrl_buf[:] = rest
                            h.handle_ctrl_line(line.decode(errors="replace"))
                    elif chan == CH_GDB:
                        pass  # (GDB TCP relay omitted in this minimal driver)
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=5)
        except Exception:
            qemu.kill()
        rc = h.finalize()
        for s in (h.serial, h.monitor, serial_srv, mon_srv):
            try:
                if s:
                    s.close()
            except OSError:
                pass

    return rc


if __name__ == "__main__":
    sys.exit(main())
