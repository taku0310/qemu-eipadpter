#!/usr/bin/env python3
"""
eip_originator.py - minimal EtherNet/IP originator (scanner) for testing the
adapter's Class 1 implicit I/O.

It performs:
  1. TCP RegisterSession
  2. SendRRData(Forward Open)  -> opens a Class 1 connection
  3. cyclic UDP/2222 exchange  -> sends O->T, receives T->O
  4. SendRRData(Forward Close) on exit

For a single-host self test the originator uses a distinct loopback address
(default 127.0.0.2) so its UDP/2222 socket does not collide with the adapter's
wildcard bind.  The adapter then sends T->O to that address.

Usage:
    python3 eip_originator.py [--adapter 127.0.0.1] [--local 127.0.0.2]
                              [--seconds 3] [--rpi-ms 50]
                              [--ot-size 32] [--to-size 32]
                              [--no-ot-run-idle]
"""
import argparse
import socket
import struct
import sys
import time

ENCAP_REGISTER     = 0x0065
ENCAP_SEND_RR_DATA = 0x006F

IO_PORT = 2222


def encap(command, session, payload=b"", context=b"\x00" * 8):
    hdr = struct.pack("<HHII8sI", command, len(payload), session, 0, context, 0)
    return hdr + payload


def recv_encap(sock):
    hdr = b""
    while len(hdr) < 24:
        chunk = sock.recv(24 - len(hdr))
        if not chunk:
            raise ConnectionError("closed during header")
        hdr += chunk
    command, length, session, status = struct.unpack("<HHII", hdr[:12])
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise ConnectionError("closed during body")
        body += chunk
    return command, session, status, body


def register_session(sock):
    sock.sendall(encap(ENCAP_REGISTER, 0, struct.pack("<HH", 1, 0)))
    _, session, status, _ = recv_encap(sock)
    if status != 0:
        raise RuntimeError("RegisterSession failed status=0x%x" % status)
    return session


def conn_params(size, point_to_point=True):
    p = size & 0x1FF
    p |= (0x02 if point_to_point else 0x01) << 13  # connection type
    return p


def forward_open(sock, session, ot_cid, ot_rpi_us, to_rpi_us,
                 ot_size, to_size, serial, vid, osn,
                 cfg_inst, out_inst, in_inst):
    # CIP Message Router request: service 0x54 to Connection Manager (class 6, inst 1)
    mr = bytes([0x54, 0x02, 0x20, 0x06, 0x24, 0x01])
    fo = b""
    fo += bytes([0x06, 0x9A])                 # priority/tick, timeout ticks
    fo += struct.pack("<I", ot_cid)           # O->T connection id (we pick)
    fo += struct.pack("<I", 0)                # T->O connection id (target picks)
    fo += struct.pack("<H", serial)           # connection serial number
    fo += struct.pack("<H", vid)              # originator vendor id
    fo += struct.pack("<I", osn)              # originator serial number
    fo += bytes([0x00, 0x00, 0x00, 0x00])     # timeout mult + 3 reserved
    fo += struct.pack("<I", ot_rpi_us)        # O->T RPI
    fo += struct.pack("<H", conn_params(ot_size))
    fo += struct.pack("<I", to_rpi_us)        # T->O RPI
    fo += struct.pack("<H", conn_params(to_size))
    fo += bytes([0x01])                       # transport type/trigger: class 1, cyclic
    # connection path: Assembly(4) / config / O->T / T->O
    path = bytes([0x20, 0x04, 0x24, cfg_inst, 0x2C, out_inst, 0x2C, in_inst])
    fo += bytes([len(path) // 2]) + path
    mr += fo

    # CPF: null address + unconnected data
    cpf = struct.pack("<I", 0) + struct.pack("<H", 0)        # iface handle + timeout
    cpf += struct.pack("<H", 2)                              # item count
    cpf += struct.pack("<HH", 0x0000, 0)                     # null address item
    cpf += struct.pack("<HH", 0x00B2, len(mr)) + mr          # unconnected data item

    sock.sendall(encap(ENCAP_SEND_RR_DATA, session, cpf))
    _, _, status, body = recv_encap(sock)
    if status != 0:
        raise RuntimeError("SendRRData(ForwardOpen) encap status=0x%x" % status)

    # parse CPF -> MR response
    off = 6
    (count,) = struct.unpack_from("<H", body, off); off += 2
    mr_resp = None
    for _ in range(count):
        t, l = struct.unpack_from("<HH", body, off); off += 4
        if t == 0x00B2:
            mr_resp = body[off:off + l]
        off += l
    if mr_resp is None:
        raise RuntimeError("no MR response in ForwardOpen reply")
    svc, _, gstatus = mr_resp[0], mr_resp[1], mr_resp[2]
    if gstatus != 0:
        raise RuntimeError("ForwardOpen CIP status=0x%x" % gstatus)
    ot_id, to_id = struct.unpack_from("<II", mr_resp, 4)
    return ot_id, to_id


def forward_close(sock, session, serial, vid, osn, cfg_inst, out_inst, in_inst):
    mr = bytes([0x4E, 0x02, 0x20, 0x06, 0x24, 0x01])
    mr += bytes([0x06, 0x9A])
    mr += struct.pack("<H", serial)
    mr += struct.pack("<H", vid)
    mr += struct.pack("<I", osn)
    path = bytes([0x20, 0x04, 0x24, cfg_inst, 0x2C, out_inst, 0x2C, in_inst])
    mr += bytes([len(path) // 2, 0x00]) + path   # path size + reserved
    cpf = struct.pack("<I", 0) + struct.pack("<H", 0)
    cpf += struct.pack("<H", 2)
    cpf += struct.pack("<HH", 0x0000, 0)
    cpf += struct.pack("<HH", 0x00B2, len(mr)) + mr
    sock.sendall(encap(ENCAP_SEND_RR_DATA, session, cpf))
    try:
        recv_encap(sock)
    except Exception:
        pass


def build_io_packet(conn_id, seq32, seq16, payload, run_idle):
    pkt = struct.pack("<H", 2)                       # item count
    pkt += struct.pack("<HH", 0x8002, 8)             # sequenced address
    pkt += struct.pack("<II", conn_id, seq32)
    data = struct.pack("<H", seq16)                  # class-1 sequence count
    if run_idle:
        data += struct.pack("<I", 1)                 # run/idle header: run
    data += payload
    pkt += struct.pack("<HH", 0x00B1, len(data)) + data
    return pkt


def parse_io_packet(pkt):
    (count,) = struct.unpack_from("<H", pkt, 0)
    off = 2
    conn_id = None
    data = None
    for _ in range(count):
        t, l = struct.unpack_from("<HH", pkt, off); off += 4
        if t == 0x8002:
            conn_id, _seq = struct.unpack_from("<II", pkt, off)
        elif t == 0x00B1:
            data = pkt[off:off + l]
        off += l
    return conn_id, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", default="127.0.0.1")
    ap.add_argument("--local", default="127.0.0.2",
                    help="local IP for UDP I/O (distinct from adapter on one host)")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--rpi-ms", type=int, default=50)
    ap.add_argument("--ot-size", type=int, default=32)
    ap.add_argument("--to-size", type=int, default=32)
    ap.add_argument("--no-ot-run-idle", action="store_true",
                    help="do NOT include 32-bit run/idle header in O->T")
    ap.add_argument("--cfg-inst", type=int, default=0x97)
    ap.add_argument("--out-inst", type=int, default=0x96)
    ap.add_argument("--in-inst", type=int, default=0x64)
    args = ap.parse_args()

    rpi_us = args.rpi_ms * 1000
    serial, vid, osn = 0x0001, 0x00FF, 0x12345678
    ot_cid = 0x12340001
    run_idle = not args.no_ot_run_idle

    # TCP session
    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp.bind((args.local, 0))
    tcp.connect((args.adapter, 44818))
    session = register_session(tcp)
    print("RegisterSession OK, handle=0x%08x" % session)

    ot_id, to_id = forward_open(
        tcp, session, ot_cid, rpi_us, rpi_us, args.ot_size, args.to_size,
        serial, vid, osn, args.cfg_inst, args.out_inst, args.in_inst)
    print("ForwardOpen OK: O->T id=0x%08x  T->O id=0x%08x" % (ot_id, to_id))

    # UDP I/O socket bound to our distinct local address
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp.bind((args.local, IO_PORT))
    udp.setblocking(False)

    seq32 = seq16 = 0
    out_counter = 0
    rx_count = 0
    last_to = None
    start = time.monotonic()
    next_send = start

    print("Exchanging Class 1 I/O for %.1fs (RPI %d ms)..." % (args.seconds, args.rpi_ms))
    while time.monotonic() - start < args.seconds:
        now = time.monotonic()
        if now >= next_send:
            seq32 += 1
            seq16 = (seq16 + 1) & 0xFFFF
            out_counter = (out_counter + 1) & 0xFFFFFFFF
            payload = struct.pack("<I", out_counter) + b"\xAA" * (args.ot_size - 4)
            pkt = build_io_packet(ot_id, seq32, seq16, payload[:args.ot_size], run_idle)
            udp.sendto(pkt, (args.adapter, IO_PORT))
            next_send += rpi_us / 1e6

        try:
            data, _ = udp.recvfrom(2048)
            cid, body = parse_io_packet(data)
            if body is not None:
                rx_count += 1
                last_to = body
        except BlockingIOError:
            time.sleep(0.001)

    print("Received %d T->O packets" % rx_count)
    if last_to is not None:
        # body = 16-bit seq count [+ run/idle] + payload; adapter T->O is modeless
        produced = last_to[2:]
        hb = struct.unpack_from("<I", produced, 0)[0] if len(produced) >= 4 else 0
        print("Last T->O heartbeat counter = %d" % hb)
        # produced[4:] is the looped-back O->T image: <O->T counter><0xAA...>
        looped = produced[4:4 + args.ot_size]
        looped_ctr = struct.unpack_from("<I", looped, 0)[0] if len(looped) >= 4 else 0
        loop = looped[:8].hex()
        print("Last T->O loopback bytes[0:8] = %s" % loop)
        # off-by-one vs out_counter is normal (last T->O produced just before our
        # final O->T was consumed); accept a small lag and verify the 0xAA fill.
        ctr_ok = abs(out_counter - looped_ctr) <= 2
        fill_ok = all(b == 0xAA for b in looped[4:])
        print("Loopback OK: %s (embedded O->T counter=%d, ours=%d)"
              % ("YES" if (ctr_ok and fill_ok) else "no", looped_ctr, out_counter))

    forward_close(tcp, session, serial, vid, osn,
                  args.cfg_inst, args.out_inst, args.in_inst)
    print("ForwardClose sent. Done.")
    udp.close()
    tcp.close()
    return 0 if rx_count > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
