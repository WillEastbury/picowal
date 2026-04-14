#!/usr/bin/env python3
"""Stress test: 1M cards across 10 packs via 5 UDP connections + 1 HTTP query thread.

Usage: python stress_test.py [host]
"""

import sys, os, time, struct, threading, statistics, socket
import urllib.request, urllib.error, http.cookiejar

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.9"
UDP_PORT = 8002
HTTP_PORT = 80

TOTAL_CARDS = int(sys.argv[2]) if len(sys.argv) > 2 else 10_000
NUM_PACKS = 10
PACKS = list(range(10, 20))  # packs 10-19
NUM_UDP_CONNS = 5
BATCH_SIZE = 32
CARDS_PER_CONN = TOTAL_CARDS // NUM_UDP_CONNS  # 200K each

# ── UDP client (inline, no import) ────────────────────────────

def wr16(v): return struct.pack("<H", v)
def wr32(v): return struct.pack("<I", v)
def wr64(v): return struct.pack("<Q", v)
def rd16(b, o=0): return struct.unpack_from("<H", b, o)[0]
def rd32(b, o=0): return struct.unpack_from("<I", b, o)[0]
def rd64(b, o=0): return struct.unpack_from("<Q", b, o)[0]

HDR_SIZE = 15

def build_card(name, value):
    buf = struct.pack("<HH", 0xCA7D, 1)
    nb = name.encode()[:30]
    buf += bytes([0, len(nb)]) + nb
    buf += bytes([1, 4]) + struct.pack("<I", value & 0xFFFFFFFF)
    return buf


class UdpConn:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(5.0)
        self.session_id = 0
        self.epoch = 0
        self.seq = 0

    def _send(self, msg_type, payload=b""):
        self.seq += 1
        hdr = wr64(self.session_id) + wr16(self.epoch) + wr32(self.seq) + bytes([msg_type])
        self.sock.sendto(hdr + payload, (self.host, self.port))

    def _recv(self, timeout=5.0):
        self.sock.settimeout(timeout)
        data, _ = self.sock.recvfrom(1500)
        if len(data) < HDR_SIZE:
            return None, None
        return data[14], data[15:]

    def hello(self):
        self._send(0x10, os.urandom(16))
        msg, payload = self._recv()
        if msg == 0x11:
            self.session_id = rd64(payload, 0)
            self.epoch = rd32(payload, 24)
            return True
        return False

    def batch_fire_and_forget(self, cards):
        """cards: list of (pack, card_id, payload_bytes)"""
        batch_seq = self.seq & 0xFFFF
        body = wr16(batch_seq) + bytes([len(cards), 0x01])  # FIRE_AND_FORGET
        for pack, card_id, payload in cards:
            body += wr16(pack) + wr32(card_id) + wr16(len(payload)) + payload
        self._send(0x20, body)

    def batch_ack_queued(self, cards):
        """Returns bitmap or None on timeout."""
        batch_seq = self.seq & 0xFFFF
        body = wr16(batch_seq) + bytes([len(cards), 0x02])  # ACK_QUEUED
        for pack, card_id, payload in cards:
            body += wr16(pack) + wr32(card_id) + wr16(len(payload)) + payload
        self._send(0x20, body)
        try:
            msg, payload = self._recv()
            if msg in (0x21, 0x22):
                return rd32(payload, 3)
        except socket.timeout:
            pass
        return None

    def batch_ack_durable(self, cards):
        """Returns committed bitmap or None on timeout."""
        batch_seq = self.seq & 0xFFFF
        body = wr16(batch_seq) + bytes([len(cards), 0x03])  # ACK_DURABLE
        for pack, card_id, payload in cards:
            body += wr16(pack) + wr32(card_id) + wr16(len(payload)) + payload
        self._send(0x20, body)
        try:
            msg, payload = self._recv(timeout=10.0)
            if msg in (0x21, 0x22):
                return rd32(payload, 3)
        except socket.timeout:
            pass
        return None

    def close(self):
        self.sock.close()


# ── UDP writer thread ────────────────────────────────────────

class WriterThread(threading.Thread):
    def __init__(self, thread_id, host, start_card, count, packs):
        super().__init__(daemon=True)
        self.thread_id = thread_id
        self.host = host
        self.start_card = start_card
        self.count = count
        self.packs = packs
        self.written = 0
        self.acked = 0
        self.committed = 0
        self.errors = 0
        self.elapsed = 0
        self.done = False

    def run(self):
        conn = UdpConn(self.host, UDP_PORT)
        try:
            if not conn.hello():
                print(f"  [W{self.thread_id}] HELLO failed")
                self.done = True
                return
        except Exception as e:
            print(f"  [W{self.thread_id}] HELLO error: {e}")
            self.done = True
            return

        t0 = time.perf_counter()
        card_id = self.start_card
        batch = []
        batches_since_commit = 0

        for i in range(self.count):
            pack = self.packs[i % len(self.packs)]
            card = build_card(f"s{self.thread_id}_{i}", i)
            batch.append((pack, card_id, card))
            card_id += 1

            if len(batch) >= BATCH_SIZE:
                batches_since_commit += 1

                # Every 10th batch: ACK_DURABLE (hard commit)
                # Otherwise: ACK_QUEUED (fast, paced)
                if batches_since_commit >= 10:
                    bitmap = conn.batch_ack_durable(batch)
                    batches_since_commit = 0
                    if bitmap is not None:
                        bits = bin(bitmap).count('1')
                        self.committed += bits
                        self.written += len(batch)
                    else:
                        self.errors += 1
                else:
                    bitmap = conn.batch_ack_queued(batch)
                    if bitmap is not None:
                        bits = bin(bitmap).count('1')
                        self.acked += bits
                        self.written += len(batch)
                    else:
                        self.errors += 1

                batch = []

        # Final batch: always hard commit
        if batch:
            bitmap = conn.batch_ack_durable(batch)
            if bitmap is not None:
                self.committed += bin(bitmap).count('1')
                self.written += len(batch)
            else:
                self.errors += 1

        self.elapsed = time.perf_counter() - t0
        conn.close()
        self.done = True


# ── HTTP query thread ────────────────────────────────────────

class QueryThread(threading.Thread):
    def __init__(self, host, packs, stop_event):
        super().__init__(daemon=True)
        self.host = host
        self.packs = packs
        self.stop_event = stop_event
        self.queries = 0
        self.errors = 0
        self.times = []

    def run(self):
        jar = http.cookiejar.CookieJar()
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

        # Login
        body = bytearray(64)
        u = b"admin"; p = b"admin"
        body[0] = len(u); body[1:1+len(u)] = u
        body[32] = len(p); body[33:33+len(p)] = p
        try:
            req = urllib.request.Request(f"http://{self.host}/login", data=bytes(body),
                                         method="POST")
            req.add_header("Content-Type", "application/octet-stream")
            opener.open(req, timeout=10)
        except Exception as e:
            print(f"  [Q] Login failed: {e}")
            return

        while not self.stop_event.is_set():
            pack = self.packs[self.queries % len(self.packs)]
            query_str = f"S:* F:{pack}"
            try:
                t0 = time.perf_counter()
                req = urllib.request.Request(f"http://{self.host}/query",
                                             data=query_str.encode(), method="POST")
                req.add_header("Content-Type", "text/plain")
                resp = opener.open(req, timeout=10)
                resp.read()
                dt = (time.perf_counter() - t0) * 1000
                self.times.append(dt)
                self.queries += 1
            except Exception:
                self.errors += 1
            time.sleep(0.05)  # ~20 queries/sec target


# ── Main ─────────────────────────────────────────────────────

def main():
    print("=" * 65)
    print("  PicoWAL Stress Test")
    print(f"  Target: {HOST}")
    print(f"  {TOTAL_CARDS:,} cards across {NUM_PACKS} packs")
    print(f"  {NUM_UDP_CONNS} UDP writers + 1 HTTP query thread")
    print("=" * 65)

    # Start query thread
    stop_queries = threading.Event()
    qt = QueryThread(HOST, PACKS, stop_queries)
    qt.start()
    print(f"\n[+] Query thread started")

    # Start writer threads
    writers = []
    for i in range(NUM_UDP_CONNS):
        start = i * CARDS_PER_CONN
        w = WriterThread(i, HOST, start, CARDS_PER_CONN, PACKS)
        writers.append(w)
        w.start()
        print(f"[+] Writer {i} started: cards {start:,}–{start+CARDS_PER_CONN-1:,}")

    # Monitor progress
    t_start = time.perf_counter()
    last_report = t_start
    while True:
        alive = [w for w in writers if not w.done]
        if not alive:
            break

        now = time.perf_counter()
        if now - last_report >= 5.0:
            total_written = sum(w.written for w in writers)
            elapsed = now - t_start
            rate = total_written / elapsed if elapsed > 0 else 0
            pct = total_written * 100 / TOTAL_CARDS
            print(f"  [{elapsed:.0f}s] {total_written:,}/{TOTAL_CARDS:,} ({pct:.1f}%) "
                  f"= {rate:,.0f} cards/sec  queries={qt.queries} q_errors={qt.errors}")
            last_report = now

        time.sleep(1)

    # Stop query thread
    stop_queries.set()
    qt.join(timeout=5)

    t_total = time.perf_counter() - t_start

    # Results
    total_written = sum(w.written for w in writers)
    total_acked = sum(w.acked for w in writers)
    total_committed = sum(w.committed for w in writers)
    total_errors = sum(w.errors for w in writers)
    write_rate = total_written / t_total if t_total > 0 else 0

    print("\n" + "=" * 65)
    print("  RESULTS")
    print("=" * 65)
    print(f"  Total time:      {t_total:.1f}s")
    print(f"  Cards sent:      {total_written:,}")
    print(f"  Cards ACK'd:     {total_acked:,}")
    print(f"  Cards committed: {total_committed:,}")
    print(f"  Write errors:    {total_errors:,}")
    print(f"  Write rate:      {write_rate:,.0f} cards/sec (paced)")
    print()

    for w in writers:
        wr = w.written / w.elapsed if w.elapsed > 0 else 0
        print(f"  Writer {w.thread_id}: {w.written:,} sent, {w.acked:,} ack'd, "
              f"{w.committed:,} committed, {w.errors} errors in {w.elapsed:.1f}s = {wr:,.0f}/sec")

    print()
    print(f"  HTTP queries:    {qt.queries}")
    print(f"  Query errors:    {qt.errors}")
    if qt.times:
        p = sorted(qt.times)
        n = len(p)
        print(f"  Query latency:   avg={statistics.mean(p):.0f}ms  "
              f"med={p[n//2]:.0f}ms  p95={p[int(n*0.95)]:.0f}ms  "
              f"max={p[-1]:.0f}ms")
    print()


if __name__ == "__main__":
    main()
