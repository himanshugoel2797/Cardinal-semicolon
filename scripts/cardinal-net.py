#!/usr/bin/env python3
# Copyright (c) 2026 Himanshu Goel
#
# This software is released under the MIT License.
# https://opensource.org/licenses/MIT
#
# Host-side companion for CoreNetDebug: a UDP echo "ping" and a reliable
# named-blob "upload" over the RDT protocol (servers/inc/CoreNetwork/rdt.h).
#
# The device is responder-driven: it only ACKs what it has, so this tool owns all
# retransmission. That makes it dramatically faster than the serial bridge while
# staying robust on a lossy link.
#
# Examples (QEMU slirp with hostfwd=udp::13370-:1337,hostfwd=udp::13380-:1338):
#   python3 scripts/cardinal-net.py ping   127.0.0.1 --port 13370
#   python3 scripts/cardinal-net.py upload 127.0.0.1 --port 13380 \
#           --name driver.celf --file build/.../driver.celf

import argparse
import os
import random
import socket
import struct
import sys
import time

# --- RDT wire protocol (must match servers/inc/CoreNetwork/rdt.h) -------------
RDT_MAGIC = b"CRDT"
RDT_TYPE_START = 1
RDT_TYPE_DATA = 2
RDT_TYPE_ACK = 3
RDT_FLAG_FIN = 1 << 0

# 4s magic, B type, B flags, H name_len, I xfer_id, I total_len, I offset,
# H chunk_len, H csum  -- multi-byte fields network (big-endian) order.
RDT_FMT = ">4sBBHIIIHH"
RDT_HDR_LEN = struct.calcsize(RDT_FMT)  # 24
assert RDT_HDR_LEN == 24

RDT_MAX_NAME = 64
DATA_CHUNK = 1024  # payload bytes per DATA datagram (fits a 1500-byte MTU)


def net_checksum16(data: bytes) -> int:
    """Internet checksum, byte-for-byte identical to checksum.h's net_checksum16
    (16-bit words assembled little-endian, one's-complement, folded, inverted)."""
    s = 0
    n = len(data)
    i = 0
    while i + 1 < n:
        s += data[i] | (data[i + 1] << 8)
        i += 2
    if i < n:
        s += data[i]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def rdt_pack(type_, flags, name_len, xfer_id, total_len, offset, chunk_len, trailing=b""):
    base = struct.pack(RDT_FMT, RDT_MAGIC, type_, flags, name_len,
                       xfer_id, total_len, offset, chunk_len, 0)
    # csum covers the header minus its own 2 trailing bytes, plus the trailing data.
    csum = net_checksum16(base[:RDT_HDR_LEN - 2] + trailing)
    hdr = struct.pack(RDT_FMT, RDT_MAGIC, type_, flags, name_len,
                      xfer_id, total_len, offset, chunk_len, csum)
    return hdr + trailing


def rdt_parse_ack(pkt: bytes):
    """Return (flags, xfer_id, offset) for a valid ACK, else None."""
    if len(pkt) < RDT_HDR_LEN:
        return None
    magic, type_, flags, _name_len, xfer_id, _total, offset, _chunk, csum = \
        struct.unpack(RDT_FMT, pkt[:RDT_HDR_LEN])
    if magic != RDT_MAGIC or type_ != RDT_TYPE_ACK:
        return None
    if net_checksum16(pkt[:RDT_HDR_LEN - 2] + pkt[RDT_HDR_LEN:]) != csum:
        return None
    return flags, xfer_id, offset


def cmd_ping(args):
    addr = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)

    sent = 0
    recv = 0
    rtts = []
    payload_fill = bytes((i & 0xFF) for i in range(max(0, args.size - 4)))
    for seq in range(args.count):
        pkt = struct.pack(">I", seq) + payload_fill
        t0 = time.time()
        sent += 1
        got = False
        for _ in range(args.retries):
            sock.sendto(pkt, addr)
            try:
                while True:
                    data, _ = sock.recvfrom(65535)
                    if len(data) >= 4 and struct.unpack(">I", data[:4])[0] == seq:
                        got = True
                        break
            except socket.timeout:
                pass
            if got:
                break
        if got:
            rtt = (time.time() - t0) * 1000.0
            rtts.append(rtt)
            recv += 1
            print(f"echo seq={seq} {len(pkt)}B rtt={rtt:.2f} ms")
        else:
            print(f"echo seq={seq} timeout")
        time.sleep(args.interval)

    loss = 100.0 * (sent - recv) / sent if sent else 0.0
    print(f"\n--- {args.host}:{args.port} echo statistics ---")
    print(f"{sent} sent, {recv} received, {loss:.0f}% loss")
    if rtts:
        print(f"rtt min/avg/max = {min(rtts):.2f}/{sum(rtts)/len(rtts):.2f}/{max(rtts):.2f} ms")
    return 0 if recv == sent else 1


def _await_ack(sock, want_xfer, want_offset):
    """Read ACKs until one for want_xfer with offset >= want_offset; return
    (offset, fin) or None on timeout."""
    while True:
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            return None
        parsed = rdt_parse_ack(data)
        if parsed is None:
            continue
        flags, xfer_id, offset = parsed
        if xfer_id == want_xfer and offset >= want_offset:
            return offset, bool(flags & RDT_FLAG_FIN)


def cmd_upload(args):
    name = args.name.encode("utf-8")
    if len(name) >= RDT_MAX_NAME:
        print(f"error: name too long (max {RDT_MAX_NAME - 1} bytes)", file=sys.stderr)
        return 2
    with open(args.file, "rb") as f:
        blob = f.read()
    total = len(blob)

    addr = (args.host, args.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)
    xfer_id = random.getrandbits(32)

    # FNV-1a, matching CoreNetDebug's on_upload digest, so the host can confirm
    # the device printed the same value.
    digest = 2166136261
    for b in blob:
        digest = ((digest ^ b) * 16777619) & 0xFFFFFFFF

    print(f"uploading '{args.name}' ({total} bytes) to {args.host}:{args.port} "
          f"xfer_id=0x{xfer_id:08x} digest=0x{digest:08x}")
    t0 = time.time()

    # START handshake.
    start = rdt_pack(RDT_TYPE_START, 0, len(name), xfer_id, total, 0, 0, name)
    acked = False
    for _ in range(args.retries):
        sock.sendto(start, addr)
        r = _await_ack(sock, xfer_id, 0)
        if r is not None:
            acked = True
            recd, fin = r
            break
    if not acked:
        print("error: no ACK for START (device unreachable or netdbg disabled)", file=sys.stderr)
        return 1

    # Stop-and-wait DATA loop. The device ACKs cumulative bytes received.
    retransmits = 0
    while recd < total:
        chunk = blob[recd:recd + DATA_CHUNK]
        pkt = rdt_pack(RDT_TYPE_DATA, 0, 0, xfer_id, total, recd, len(chunk), chunk)
        progressed = False
        for _ in range(args.retries):
            sock.sendto(pkt, addr)
            r = _await_ack(sock, xfer_id, recd + len(chunk))
            if r is not None:
                recd, fin = r
                progressed = True
                break
            retransmits += 1
        if not progressed:
            print(f"\nerror: transfer stalled at {recd}/{total} bytes", file=sys.stderr)
            return 1
        done = 100.0 * recd / total if total else 100.0
        print(f"\r  {recd}/{total} bytes ({done:.0f}%)", end="", flush=True)
    print()

    dt = time.time() - t0
    rate = (total / dt / 1024.0) if dt > 0 else 0.0
    print(f"done: {total} bytes in {dt:.3f} s = {rate:.1f} KiB/s "
          f"({retransmits} retransmits)")
    print(f"verify: device should log digest=0x{digest:08x}")
    return 0


def main():
    ap = argparse.ArgumentParser(description="CoreNetDebug host tool (UDP echo + RDT upload)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ping", help="UDP echo round-trip / latency")
    p.add_argument("host")
    p.add_argument("--port", type=int, default=1337)
    p.add_argument("--count", type=int, default=5)
    p.add_argument("--size", type=int, default=32, help="payload bytes")
    p.add_argument("--interval", type=float, default=0.2)
    p.add_argument("--timeout", type=float, default=1.0)
    p.add_argument("--retries", type=int, default=3)
    p.set_defaults(func=cmd_ping)

    p = sub.add_parser("upload", help="reliable named-blob upload (RDT)")
    p.add_argument("host")
    p.add_argument("--port", type=int, default=1338)
    p.add_argument("--name", required=True, help="blob name reported to the device")
    p.add_argument("--file", required=True, help="local file to send")
    p.add_argument("--timeout", type=float, default=1.0)
    p.add_argument("--retries", type=int, default=8)
    p.set_defaults(func=cmd_upload)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
