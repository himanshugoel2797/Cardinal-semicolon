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
        self.gdb_listener = None   # TCP listener for GDB (ch2 tunnel)
        self.gdb_client = None     # connected GDB socket, if any

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

    # ---- GDB tunnel (ch2 <-> TCP) ----
    def gdb_accept(self):
        try:
            conn, _ = self.gdb_listener.accept()
        except OSError:
            return
        if self.gdb_client is not None:
            try:
                self.gdb_client.close()
            except OSError:
                pass
        conn.setblocking(False)
        self.gdb_client = conn
        self.emit("    [gdb] debugger connected on the ch2 tunnel\n")

    def gdb_drop(self):
        if self.gdb_client is not None:
            try:
                self.gdb_client.close()
            except OSError:
                pass
            self.gdb_client = None
            self.emit("    [gdb] debugger disconnected\n")

    # GDB -> guest: frame the raw RSP bytes onto ch2 and write to the serial link.
    def gdb_from_client(self, data):
        if self.serial:
            try:
                self.serial.sendall(frame(CH_GDB, data))
            except OSError:
                pass

    # guest -> GDB: forward unframed ch2 payload to the connected debugger.
    def gdb_to_client(self, payload):
        if self.gdb_client is None:
            return
        try:
            self.gdb_client.sendall(payload)
        except OSError:
            self.gdb_drop()

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


# Adapter so a real serial tty looks like the unix socket the loop expects
# (fileno/recv/sendall/setblocking/close). Raw 8N1 at the requested baud.
class SerialTTY:
    def __init__(self, dev, baud):
        import termios
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        speed = getattr(termios, "B%d" % baud, termios.B115200)
        a[0] = 0  # iflag
        a[1] = 0  # oflag
        a[3] = 0  # lflag (raw: no echo/canonical/signals)
        a[2] = (a[2] & ~termios.CSIZE & ~termios.PARENB) | termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = speed  # ispeed
        a[5] = speed  # ospeed
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def fileno(self):
        return self.fd

    def setblocking(self, flag):
        pass  # always non-blocking (O_NONBLOCK)

    def recv(self, n):
        try:
            return os.read(self.fd, n)
        except (BlockingIOError, InterruptedError):
            return b""

    def sendall(self, b):
        mv = memoryview(b)
        while mv:
            try:
                k = os.write(self.fd, mv)
                mv = mv[k:]
            except (BlockingIOError, InterruptedError):
                continue

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


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
    # Which link the CSMUX mux rides under QEMU: COM1 (default) or an emulated FTDI
    # USB-serial adapter (`-device usb-serial`), exercising the real-hardware path.
    ap.add_argument("--link", choices=["com1", "ftdi"],
                    default=os.environ.get("LINK", "com1"))
    # Real-hardware mode: talk to an actual serial adapter instead of launching
    # QEMU (no QEMU monitor, so a hung death test cannot be force-reset -- the
    # overall timeout aborts the run). The guest must be booted with
    # "cardinal.test cardinal.harness"; with an FTDI adapter the mux auto-rides it.
    ap.add_argument("--serial-device", default=os.environ.get("SERIAL_DEVICE"),
                    help="real tty (e.g. /dev/ttyUSB0); skips QEMU")
    ap.add_argument("--baud", type=int, default=int(os.environ.get("BAUD", "115200")))
    ap.add_argument("--timeout", type=int, default=int(os.environ.get("TIMEOUT", "600")),
                    help="overall wall-clock budget in seconds")
    ap.add_argument("--death-timeout", type=int, default=int(os.environ.get("DEATH_TIMEOUT", "60")),
                    help="per-death-test budget before forcing a reset")
    args = ap.parse_args()

    h = Harness(args)
    qemu = None
    tmp = serial_srv = mon_srv = None

    if args.serial_device:
        # --- real hardware: talk to an actual serial adapter, no QEMU ---
        try:
            h.serial = SerialTTY(args.serial_device, args.baud)
        except OSError as e:
            print("harness: cannot open %s: %s" % (args.serial_device, e), file=sys.stderr)
            return 2
        print("harness: driving real serial %s @ %d (no QEMU; hang-reset disabled)"
              % (args.serial_device, args.baud), file=sys.stderr)
    else:
        # --- QEMU: mux over COM1 (default) or an emulated FTDI USB-serial ---
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
            "-chardev", "socket,id=mux,path=%s,server=off" % serial_path,
            "-chardev", "socket,id=mon0,path=%s,server=off" % mon_path,
            "-mon", "chardev=mon0,mode=readline",
            "-display", "none",
        ]
        if args.link == "ftdi":
            # The mux rides an emulated FTDI adapter (VID 0x0403); usb_serial binds
            # it and routes CSMUX onto it. COM1 carries only the pre-mux boot log.
            com1_log = os.path.join(tmp, "com1.log")
            qemu_cmd += [
                "-serial", "file:%s" % com1_log,
                "-device", "qemu-xhci,id=xhci",
                "-device", "usb-serial,chardev=mux,bus=xhci.0",
            ]
            print("harness: FTDI link; COM1 boot log -> %s" % com1_log, file=sys.stderr)
        else:
            # The mux rides COM1 directly.
            qemu_cmd += ["-serial", "chardev:mux"]

        print("harness: launching:", " ".join(qemu_cmd), file=sys.stderr)
        qemu = subprocess.Popen(qemu_cmd)

        try:
            serial_srv.settimeout(20)
            h.serial, _ = serial_srv.accept()
            mon_srv.settimeout(20)
            try:
                h.monitor, _ = mon_srv.accept()
            except socket.timeout:
                print("harness: monitor socket not connected (hang-reset disabled)", file=sys.stderr)
        except socket.timeout:
            print("harness: QEMU did not connect to the serial socket", file=sys.stderr)
            qemu.kill()
            return 2

    h.serial.setblocking(False)

    # GDB tunnel: a local TCP port bridged to ch2. GDB attaches with
    # `target remote :<port>`; on real hardware this is how a debugger reaches the
    # stub when the single serial link is shared with the log + control channels.
    try:
        h.gdb_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        h.gdb_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        h.gdb_listener.bind(("127.0.0.1", args.gdb_port))
        h.gdb_listener.listen(1)
        h.gdb_listener.setblocking(False)
        print("harness: GDB ch2 tunnel on tcp://127.0.0.1:%d "
              "(target remote :%d)" % (args.gdb_port, args.gdb_port), file=sys.stderr)
    except OSError as e:
        print("harness: GDB tunnel disabled (%s)" % e, file=sys.stderr)
        h.gdb_listener = None

    deframer = Deframer()
    ctrl_buf = bytearray()
    deadline = time.monotonic() + args.timeout

    rc = 1
    try:
        while not h.done:
            if time.monotonic() > deadline:
                h.emit("[SysTest] harness overall timeout (%ss) -- aborting\n" % args.timeout)
                break
            rset = [h.serial]
            if h.gdb_listener is not None:
                rset.append(h.gdb_listener)
            if h.gdb_client is not None:
                rset.append(h.gdb_client)
            r, _, _ = select.select(rset, [], [], 0.5)
            h.check_timeout()

            # GDB tunnel: accept a debugger / relay its bytes onto ch2.
            if h.gdb_listener in r:
                h.gdb_accept()
            if h.gdb_client is not None and h.gdb_client in r:
                try:
                    gdata = h.gdb_client.recv(4096)
                except (BlockingIOError, InterruptedError):
                    gdata = b""
                except OSError:
                    gdata = b""
                    h.gdb_drop()
                if gdata:
                    h.gdb_from_client(gdata)
                elif h.gdb_client is not None and h.gdb_client in r:
                    h.gdb_drop()  # EOF

            if h.serial not in r:
                if qemu is not None and qemu.poll() is not None:
                    # QEMU gone (final isa-debug-exit on ALLDONE, or a crash).
                    break
                continue
            try:
                data = h.serial.recv(4096)
            except (BlockingIOError, InterruptedError):
                continue
            except OSError:
                break  # serial device went away
            if not data:
                if qemu is not None and qemu.poll() is not None:
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
                        h.gdb_to_client(payload)  # forward to the attached debugger
    finally:
        if qemu is not None:
            try:
                qemu.terminate()
                qemu.wait(timeout=5)
            except Exception:
                qemu.kill()
        rc = h.finalize()
        for s in (h.serial, h.monitor, h.gdb_client, h.gdb_listener, serial_srv, mon_srv):
            try:
                if s:
                    s.close()
            except OSError:
                pass

    return rc


if __name__ == "__main__":
    sys.exit(main())
