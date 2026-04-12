#!/usr/bin/env python3
"""UDP WAL protocol client — connects to port 8002, creates session,
sends batch writes and reads, measures latency vs HTTP."""

import socket, struct, time, statistics, sys, os

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.222.223"
PORT = 8002
HDR_SIZE = 15  # session_id(8) + epoch(2) + seq(4) + msg_type(1)

# Message types
UMSG_HELLO          = 0x10
UMSG_HELLO_OK       = 0x11
UMSG_RESUME         = 0x12
UMSG_RESUME_OK      = 0x13
UMSG_RESUME_FAIL    = 0x14
UMSG_BATCH_WRITE    = 0x20
UMSG_BATCH_QUEUED   = 0x21
UMSG_BATCH_COMMITTED = 0x22
UMSG_READ           = 0x30
UMSG_DATA           = 0x31
UMSG_NOT_FOUND      = 0x32

# Durability
UDUR_FIRE_AND_FORGET  = 0x01
UDUR_ACK_QUEUED       = 0x02
UDUR_ACK_DURABLE      = 0x03
UDUR_ACK_ALL_COMMITTED = 0x04

def wr16(v): return struct.pack("<H", v)
def wr32(v): return struct.pack("<I", v)
def wr64(v): return struct.pack("<Q", v)
def rd16(b, o=0): return struct.unpack_from("<H", b, o)[0]
def rd32(b, o=0): return struct.unpack_from("<I", b, o)[0]
def rd64(b, o=0): return struct.unpack_from("<Q", b, o)[0]


class UdpWalClient:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(3.0)
        self.session_id = 0
        self.epoch = 0
        self.seq = 0

    def _send(self, msg_type, payload=b""):
        self.seq += 1
        hdr = wr64(self.session_id) + wr16(self.epoch) + wr32(self.seq) + bytes([msg_type])
        self.sock.sendto(hdr + payload, (self.host, self.port))

    def _recv(self, timeout=3.0):
        self.sock.settimeout(timeout)
        data, addr = self.sock.recvfrom(1500)
        if len(data) < HDR_SIZE:
            return None, None, None
        sid = rd64(data, 0)
        epoch = rd16(data, 8)
        seq = rd32(data, 10)
        msg_type = data[14]
        payload = data[15:]
        return msg_type, payload, sid

    def hello(self):
        client_random = os.urandom(16)
        self._send(UMSG_HELLO, client_random)
        msg, payload, sid = self._recv()
        if msg != UMSG_HELLO_OK:
            raise ValueError(f"Expected HELLO_OK, got 0x{msg:02x}" if msg else "No response")
        self.session_id = rd64(payload, 0)
        self.epoch = rd32(payload, 24)
        return self.session_id

    def resume(self):
        self._send(UMSG_RESUME)
        msg, payload, sid = self._recv()
        if msg == UMSG_RESUME_OK:
            self.epoch = rd32(payload, 0)
            return True
        return False

    def batch_write(self, cards, durability=UDUR_ACK_QUEUED):
        """cards: list of (pack, card_id, payload_bytes)"""
        batch_seq = self.seq & 0xFFFF
        body = wr16(batch_seq) + bytes([len(cards), durability])
        for pack, card_id, payload in cards:
            body += wr16(pack) + wr32(card_id) + wr16(len(payload)) + payload
        self._send(UMSG_BATCH_WRITE, body)

        if durability == UDUR_FIRE_AND_FORGET:
            return batch_seq, 0xFFFFFFFF, None

        # Wait for QUEUED ACK
        msg, payload, _ = self._recv()
        if msg != UMSG_BATCH_QUEUED:
            return batch_seq, 0, f"Expected QUEUED, got 0x{msg:02x}" if msg else "timeout"
        q_bitmap = rd32(payload, 3)

        if durability < UDUR_ACK_DURABLE:
            return batch_seq, q_bitmap, None

        # Wait for COMMITTED ACK
        msg2, payload2, _ = self._recv(timeout=10.0)
        if msg2 != UMSG_BATCH_COMMITTED:
            return batch_seq, q_bitmap, f"No COMMITTED (got 0x{msg2:02x})" if msg2 else "timeout"
        c_bitmap = rd32(payload2, 3)
        return batch_seq, c_bitmap, None

    def read(self, pack, card_id):
        body = wr16(pack) + wr32(card_id)
        self._send(UMSG_READ, body)
        msg, payload, _ = self._recv()
        if msg == UMSG_DATA:
            dlen = rd16(payload, 6)
            return payload[8:8+dlen]
        elif msg == UMSG_NOT_FOUND:
            return None
        else:
            raise ValueError(f"Unexpected read response: 0x{msg:02x}" if msg else "timeout")

    def close(self):
        self.sock.close()


def build_card(name, value):
    """Build a minimal card: magic + version + 2 fields."""
    buf = struct.pack("<HH", 0xCA7D, 1)  # magic + version
    # Field 0 (ord 0, type 0x09=utf8): name
    nb = name.encode()
    buf += bytes([0, len(nb)]) + nb
    # Field 1 (ord 1, type 0x03=uint32): value
    buf += bytes([1, 4]) + struct.pack("<I", value)
    return buf


def percentiles(data):
    if not data: return {}
    s = sorted(data)
    n = len(s)
    return {"min": s[0], "avg": statistics.mean(s), "med": s[n//2],
            "p95": s[int(n*0.95)] if n>=20 else s[-1], "max": s[-1],
            "std": statistics.stdev(s) if n>1 else 0}

def fmt(p):
    return f"min={p['min']:.1f} avg={p['avg']:.1f} med={p['med']:.1f} p95={p['p95']:.1f} max={p['max']:.1f}ms"


def main():
    print("=" * 60)
    print("  PicoWAL UDP Protocol Test")
    print(f"  Target: {HOST}:{PORT}")
    print("=" * 60)

    c = UdpWalClient(HOST, PORT)

    # 1. HELLO
    print("\n[1] Session HELLO...")
    t0 = time.perf_counter()
    try:
        sid = c.hello()
        dt = (time.perf_counter() - t0) * 1000
        print(f"    Session: {sid:016x}  ({dt:.1f}ms)")
    except Exception as e:
        print(f"    FAILED: {e}")
        return

    # 2. Single writes with ACK_QUEUED
    print("\n[2] Single-card writes ×100 (ACK_QUEUED)...")
    times = []
    for i in range(100):
        card = build_card(f"udp_{i}", i * 7)
        t0 = time.perf_counter()
        seq, bitmap, err = c.batch_write([(10, i, card)], UDUR_ACK_QUEUED)
        dt = (time.perf_counter() - t0) * 1000
        if err:
            print(f"    Error at {i}: {err}")
            break
        times.append(dt)
    if times:
        p = percentiles(times)
        rate = len(times) / (sum(times)/1000) if sum(times)>0 else 0
        print(f"    {len(times)} ok = {rate:.0f} writes/sec")
        print(f"    {fmt(p)}")

    # 3. Batch writes (32 cards per batch)
    print("\n[3] Batch ×32 writes ×10 (ACK_QUEUED)...")
    batch_times = []
    for b in range(10):
        cards = [(10, 1000+b*32+i, build_card(f"batch_{b}_{i}", b*32+i)) for i in range(32)]
        t0 = time.perf_counter()
        seq, bitmap, err = c.batch_write(cards, UDUR_ACK_QUEUED)
        dt = (time.perf_counter() - t0) * 1000
        if err:
            print(f"    Batch {b} error: {err}")
            break
        if bitmap != 0xFFFFFFFF:
            print(f"    Batch {b}: partial bitmap 0x{bitmap:08x}")
        batch_times.append(dt)
    if batch_times:
        p = percentiles(batch_times)
        cards_per_sec = (len(batch_times)*32) / (sum(batch_times)/1000)
        print(f"    {len(batch_times)} batches = {cards_per_sec:.0f} cards/sec")
        print(f"    {fmt(p)}")

    # 4. Batch with ACK_DURABLE (2-phase)
    print("\n[4] Batch ×8 with ACK_DURABLE (queued + committed)...")
    dur_times = []
    for b in range(5):
        cards = [(10, 2000+b*8+i, build_card(f"dur_{b}_{i}", b*100+i)) for i in range(8)]
        t0 = time.perf_counter()
        seq, bitmap, err = c.batch_write(cards, UDUR_ACK_DURABLE)
        dt = (time.perf_counter() - t0) * 1000
        if err:
            print(f"    Batch {b}: {err}")
        dur_times.append(dt)
    if dur_times:
        p = percentiles(dur_times)
        print(f"    {fmt(p)}")

    # 5. Reads
    print("\n[5] Reads ×50...")
    read_times = []
    for i in range(50):
        t0 = time.perf_counter()
        try:
            data = c.read(10, i)
            dt = (time.perf_counter() - t0) * 1000
            read_times.append(dt)
        except Exception as e:
            pass
    if read_times:
        p = percentiles(read_times)
        print(f"    {len(read_times)} ok  {fmt(p)}")
    else:
        print("    All reads failed")

    # 6. Fire-and-forget throughput
    print("\n[6] Fire-and-forget ×200 (no ACK)...")
    t0 = time.perf_counter()
    for i in range(200):
        card = build_card(f"ff_{i}", i)
        c.batch_write([(10, 3000+i, card)], UDUR_FIRE_AND_FORGET)
    dt = (time.perf_counter() - t0) * 1000
    rate = 200 / (dt/1000)
    print(f"    200 cards in {dt:.0f}ms = {rate:.0f} cards/sec (send-side)")

    # Summary
    print("\n" + "=" * 60)
    print("  SUMMARY")
    print("=" * 60)
    if times:
        print(f"  Single write (ACK_QUEUED):  {statistics.mean(times):.1f}ms avg ({len(times)/(sum(times)/1000):.0f}/sec)")
    if batch_times:
        print(f"  Batch ×32 (ACK_QUEUED):     {statistics.mean(batch_times):.1f}ms avg ({len(batch_times)*32/(sum(batch_times)/1000):.0f} cards/sec)")
    if dur_times:
        print(f"  Batch ×8 (ACK_DURABLE):     {statistics.mean(dur_times):.1f}ms avg")
    if read_times:
        print(f"  Read:                       {statistics.mean(read_times):.1f}ms avg")
    print(f"  Fire-and-forget:            {rate:.0f} cards/sec (send-side)")
    print(f"  (Compare: HTTP single write ~31ms, HTTP keep-alive ~28ms)")
    print()

    c.close()

if __name__ == "__main__":
    main()
