#!/usr/bin/env python3
"""PicoWAL comprehensive benchmark — single-connection, keep-alive.

Covers: login, status, sequential writes, batch writes, page reads,
query WHERE/aggregate, latency percentiles, jitter, and sustained mixed load.

Mindful of 8 TCP PCB limit — uses a single persistent connection.
"""

import sys, time, struct, statistics, urllib.request, urllib.error
import http.cookiejar, json, random, string

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.222.223"
BASE = f"http://{HOST}"
USER = "admin"
PASS = "admin"

# ── helpers ──────────────────────────────────────────────────
jar = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(
    urllib.request.HTTPCookieProcessor(jar),
    urllib.request.HTTPHandler,
)

def req(path, data=None, method=None, ct=None, timeout=10):
    url = f"{BASE}{path}"
    r = urllib.request.Request(url, data=data, method=method)
    if ct:
        r.add_header("Content-Type", ct)
    return opener.open(r, timeout=timeout)

def login():
    body = bytearray(64)
    u = USER.encode(); p = PASS.encode()
    body[0] = len(u); body[1:1+len(u)] = u
    body[32] = len(p); body[33:33+len(p)] = p
    r = req("/login", bytes(body), "POST", "application/octet-stream")
    return r.read()

def timed(fn, *a, **kw):
    t0 = time.perf_counter()
    result = fn(*a, **kw)
    dt = (time.perf_counter() - t0) * 1000
    return dt, result

def build_card(pack, card, fields):
    """Build a binary card: magic + version + fields."""
    buf = bytearray()
    buf += struct.pack("<HH", 0xCA7D, 1)  # magic + version
    for ordinal, ftype, value in fields:
        ord_byte = ordinal & 0x1F
        if isinstance(value, str):
            vb = value.encode("utf-8")
        elif isinstance(value, int):
            if ftype in (0x01,):
                vb = struct.pack("<B", value & 0xFF)
            elif ftype in (0x02,):
                vb = struct.pack("<H", value & 0xFFFF)
            elif ftype in (0x03,):
                vb = struct.pack("<I", value & 0xFFFFFFFF)
            else:
                vb = struct.pack("<I", value & 0xFFFFFFFF)
        elif isinstance(value, bytes):
            vb = value
        else:
            vb = str(value).encode()
        buf += bytes([ord_byte, len(vb)]) + vb
    return bytes(buf)

def percentiles(data):
    if not data:
        return {}
    s = sorted(data)
    n = len(s)
    return {
        "min": s[0],
        "avg": statistics.mean(s),
        "med": s[n//2],
        "p95": s[int(n*0.95)] if n >= 20 else s[-1],
        "p99": s[int(n*0.99)] if n >= 100 else s[-1],
        "max": s[-1],
        "std": statistics.stdev(s) if n > 1 else 0,
    }

def jitter(data):
    if len(data) < 2:
        return 0, 0
    diffs = [abs(data[i] - data[i-1]) for i in range(1, len(data))]
    return statistics.mean(diffs), max(diffs)

def fmt_stats(p, unit="ms"):
    return (f"min={p['min']:.1f} avg={p['avg']:.1f} med={p['med']:.1f} "
            f"p95={p['p95']:.1f} max={p['max']:.1f} std={p['std']:.1f}{unit}")

# ── benchmark pack: use pack 10 to avoid collisions ─────────
BENCH_PACK = 10

def write_card(card_id, value_str):
    card = build_card(BENCH_PACK, card_id, [
        (0, 0x09, f"bench_{card_id}"),
        (1, 0x03, card_id),
        (2, 0x09, value_str),
    ])
    r = req(f"/0/{BENCH_PACK}/{card_id}", card, "PUT", "application/octet-stream")
    return r.read()

def read_page(pack, start=0, limit=20):
    r = req(f"/w/{pack}?start={start}&limit={limit}")
    return r.read()

def query(q):
    r = req(f"/query?q={urllib.request.quote(q)}")
    return r.read()

# ── main ─────────────────────────────────────────────────────
def main():
    results = {}
    print("=" * 60)
    print("  PicoWAL Benchmark Run")
    print(f"  Target: {BASE}")
    print("=" * 60)

    # 1. Login
    print("\n[1] Login...")
    login_times = []
    for i in range(10):
        dt, _ = timed(login)
        login_times.append(dt)
    p = percentiles(login_times)
    j_avg, j_max = jitter(login_times)
    print(f"    Login: {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
    results["login"] = {"stats": p, "jitter_avg": j_avg, "jitter_max": j_max}

    # 2. Status page
    print("\n[2] Status page...")
    status_times = []
    for i in range(50):
        dt, body = timed(lambda: req("/status").read())
        status_times.append(dt)
    p = percentiles(status_times)
    j_avg, j_max = jitter(status_times)
    print(f"    Status: {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
    results["status"] = {"stats": p, "jitter_avg": j_avg, "jitter_max": j_max}

    # 3. Sequential writes (100 cards)
    print("\n[3] Sequential writes (100 cards)...")
    write_times = []
    errors = 0
    for i in range(100):
        val = ''.join(random.choices(string.ascii_lowercase, k=20))
        try:
            dt, _ = timed(write_card, i, val)
            write_times.append(dt)
        except Exception as e:
            errors += 1
    p = percentiles(write_times)
    j_avg, j_max = jitter(write_times)
    total_t = sum(write_times) / 1000
    rate = len(write_times) / total_t if total_t > 0 else 0
    print(f"    Writes: {len(write_times)} ok, {errors} errors in {total_t:.1f}s = {rate:.0f} cards/sec")
    print(f"    Latency: {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
    results["seq_write"] = {"stats": p, "ok": len(write_times), "errors": errors, "rate": rate,
                            "jitter_avg": j_avg, "jitter_max": j_max}

    # 4. Page reads (pack listing)
    print("\n[4] Page reads (50 requests)...")
    read_times = []
    for i in range(50):
        try:
            dt, _ = timed(read_page, BENCH_PACK, 0, 20)
            read_times.append(dt)
        except Exception:
            pass
    if read_times:
        p = percentiles(read_times)
        j_avg, j_max = jitter(read_times)
        print(f"    Reads: {len(read_times)} ok  {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
        results["page_read"] = {"stats": p, "jitter_avg": j_avg, "jitter_max": j_max}
    else:
        print("    Reads: all 50 failed")

    # 5. Query — WHERE filter
    print("\n[5] Query WHERE filter (30 queries)...")
    qw_times = []
    for i in range(30):
        try:
            dt, body = timed(query, f"S:name,value F:{BENCH_PACK} W:value>50")
            qw_times.append(dt)
        except Exception:
            pass
    if qw_times:
        p = percentiles(qw_times)
        j_avg, j_max = jitter(qw_times)
        print(f"    WHERE: {len(qw_times)} ok  {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
        results["query_where"] = {"stats": p, "jitter_avg": j_avg, "jitter_max": j_max}
    else:
        print("    WHERE: all failed (pack may not support query fields)")

    # 6. Query — SELECT *
    print("\n[6] Query SELECT * (20 queries)...")
    qs_times = []
    for i in range(20):
        try:
            dt, body = timed(query, f"S:* F:{BENCH_PACK}")
            qs_times.append(dt)
        except Exception:
            pass
    if qs_times:
        p = percentiles(qs_times)
        j_avg, j_max = jitter(qs_times)
        print(f"    S:*: {len(qs_times)} ok  {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
        results["query_star"] = {"stats": p, "jitter_avg": j_avg, "jitter_max": j_max}
    else:
        print("    S:*: all failed")

    # 7. Connection overhead — fresh vs keep-alive
    print("\n[7] Connection overhead...")
    ka_times = []
    for i in range(20):
        dt, _ = timed(lambda: req("/").read())
        ka_times.append(dt)
    p_ka = percentiles(ka_times)
    print(f"    Keep-alive: {fmt_stats(p_ka)}")
    results["keepalive"] = {"stats": p_ka}

    # 8. Sustained mixed load (30 seconds)
    print("\n[8] Sustained mixed load (30s)...")
    ops = {"write": 0, "read": 0, "query": 0, "error": 0}
    mixed_times = []
    t_end = time.perf_counter() + 30
    while time.perf_counter() < t_end:
        op = random.choice(["write", "read", "query"])
        try:
            if op == "write":
                cid = random.randint(200, 999)
                val = ''.join(random.choices(string.ascii_lowercase, k=15))
                dt, _ = timed(write_card, cid, val)
            elif op == "read":
                dt, _ = timed(read_page, BENCH_PACK, random.randint(0, 50), 20)
            else:
                dt, _ = timed(query, f"S:* F:{BENCH_PACK}")
            mixed_times.append(dt)
            ops[op] += 1
        except Exception:
            ops["error"] += 1
    total_ops = ops["write"] + ops["read"] + ops["query"]
    throughput = total_ops / 30
    p = percentiles(mixed_times) if mixed_times else {}
    j_avg, j_max = jitter(mixed_times) if len(mixed_times) > 1 else (0, 0)
    print(f"    Total: {total_ops} ops, {ops['error']} errors = {throughput:.1f} ops/sec")
    print(f"    Breakdown: writes={ops['write']} reads={ops['read']} queries={ops['query']}")
    if p:
        print(f"    Latency: {fmt_stats(p)}  jitter avg={j_avg:.1f} max={j_max:.1f}")
    results["mixed"] = {"ops": ops, "throughput": throughput,
                        "stats": p, "jitter_avg": j_avg, "jitter_max": j_max}

    # ── Summary ──────────────────────────────────────────────
    print("\n" + "=" * 60)
    print("  BENCHMARK COMPLETE")
    print("=" * 60)
    for name, r in results.items():
        s = r.get("stats", {})
        if s:
            print(f"  {name:16s}  avg={s.get('avg',0):.1f}ms  p95={s.get('p95',0):.1f}ms  max={s.get('max',0):.1f}ms")
    print()

    return results

if __name__ == "__main__":
    main()
