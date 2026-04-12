#!/usr/bin/env python3
"""Raw TCP WAL protocol tester — connects to port 8001, authenticates,
sends APPEND/READ ops and measures latency vs HTTP."""

import socket, struct, time, statistics

HOST = "192.168.222.223"
PORT = 8001

# PSK must match AUTH_PSK in net_core.h
PSK = bytes([
    0x50, 0x69, 0x63, 0x6F, 0x57, 0x41, 0x4C, 0x5F,
    0x41, 0x75, 0x74, 0x68, 0x4B, 0x65, 0x79, 0x32,
    0x30, 0x32, 0x36, 0x5F, 0x53, 0x65, 0x63, 0x72,
    0x65, 0x74, 0x50, 0x53, 0x4B, 0x21, 0x21, 0x21,
])

# Wire opcodes
WIRE_AUTH_CHALLENGE = 0xA0
WIRE_AUTH_RESPONSE  = 0xA1
WIRE_AUTH_OK        = 0xA2
WIRE_AUTH_FAIL      = 0xA3
WIRE_OP_NOOP        = 0x00
WIRE_OP_APPEND      = 0x01
WIRE_OP_READ        = 0x02
WIRE_ACK_NOOP       = 0x80
WIRE_ACK_APPEND     = 0x81
WIRE_ACK_READ       = 0x82
WIRE_ERROR          = 0xFF

AUTH_NONCE_LEN    = 16
AUTH_RESPONSE_LEN = 32


def crc32(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)))
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def compute_hmac(nonce, key):
    out = bytearray(AUTH_RESPONSE_LEN)
    for rnd in range(8):
        mix = bytearray(64)
        for i in range(32):
            mix[i] = (key[i] if i < len(key) else 0) ^ ((rnd * 37) & 0xFF)
        for i in range(32):
            mix[32 + i] = out[i]
        h = crc32(mix)
        h ^= crc32(nonce)
        h ^= crc32(key)
        h = (h * 2654435761 + rnd) & 0xFFFFFFFF
        out[rnd * 4 + 0] = (h >> 0) & 0xFF
        out[rnd * 4 + 1] = (h >> 8) & 0xFF
        out[rnd * 4 + 2] = (h >> 16) & 0xFF
        out[rnd * 4 + 3] = (h >> 24) & 0xFF
    return bytes(out)


def recv_exact(sock, n, timeout=5):
    sock.settimeout(timeout)
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Connection closed")
        data += chunk
    return data


def connect_and_auth():
    """Connect, receive challenge, compute HMAC, authenticate."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    t0 = time.perf_counter()
    sock.connect((HOST, PORT))
    t_connect = (time.perf_counter() - t0) * 1000

    # Receive challenge: [0xA0][16 bytes nonce]
    challenge = recv_exact(sock, 1 + AUTH_NONCE_LEN)
    if challenge[0] != WIRE_AUTH_CHALLENGE:
        raise ValueError(f"Expected CHALLENGE 0xA0, got 0x{challenge[0]:02x}")
    nonce = challenge[1:]

    # Compute and send HMAC response
    hmac = compute_hmac(nonce, PSK)
    t0 = time.perf_counter()
    sock.sendall(bytes([WIRE_AUTH_RESPONSE]) + hmac)

    # Receive auth result
    result = recv_exact(sock, 1)
    t_auth = (time.perf_counter() - t0) * 1000

    if result[0] == WIRE_AUTH_OK:
        return sock, t_connect, t_auth
    elif result[0] == WIRE_AUTH_FAIL:
        sock.close()
        raise ValueError("Auth failed — PSK mismatch")
    else:
        sock.close()
        raise ValueError(f"Unexpected auth response: 0x{result[0]:02x}")


def send_noop(sock):
    """Send NOOP, receive ACK — measures raw round-trip."""
    t0 = time.perf_counter()
    sock.sendall(bytes([WIRE_OP_NOOP]))
    resp = recv_exact(sock, 1)
    dt = (time.perf_counter() - t0) * 1000
    if resp[0] != WIRE_ACK_NOOP:
        raise ValueError(f"Expected NOOP ACK 0x80, got 0x{resp[0]:02x}")
    return dt


def send_append(sock, key_hash, value, delta_op=0):
    """Send APPEND: [0x01][key:4][len:2][delta:1][data:len]"""
    data = value if isinstance(value, bytes) else value.encode()
    hdr = struct.pack("<BIhB", WIRE_OP_APPEND, key_hash, len(data), delta_op)
    t0 = time.perf_counter()
    sock.sendall(hdr + data)
    resp = recv_exact(sock, 1)
    dt = (time.perf_counter() - t0) * 1000
    if resp[0] == WIRE_ERROR:
        err = recv_exact(sock, 1)
        raise ValueError(f"APPEND error: 0x{err[0]:02x}")
    if resp[0] != WIRE_ACK_APPEND:
        raise ValueError(f"Expected APPEND ACK 0x81, got 0x{resp[0]:02x}")
    return dt


def send_read(sock, key_hash):
    """Send READ: [0x02][key:4], receive [0x82][len:2][data] or error."""
    hdr = struct.pack("<BI", WIRE_OP_READ, key_hash)
    t0 = time.perf_counter()
    sock.sendall(hdr)
    resp = recv_exact(sock, 1)
    dt_hdr = (time.perf_counter() - t0) * 1000

    if resp[0] == WIRE_ERROR:
        err = recv_exact(sock, 1)
        return dt_hdr, None, err[0]
    if resp[0] != WIRE_ACK_READ:
        raise ValueError(f"Expected READ ACK 0x82, got 0x{resp[0]:02x}")

    length_bytes = recv_exact(sock, 2)
    length = struct.unpack("<H", length_bytes)[0]
    data = recv_exact(sock, length) if length > 0 else b""
    dt = (time.perf_counter() - t0) * 1000
    return dt, data, 0


def percentiles(data):
    if not data:
        return {}
    s = sorted(data)
    n = len(s)
    return {
        "min": s[0], "avg": statistics.mean(s), "med": s[n//2],
        "p95": s[int(n*0.95)] if n >= 20 else s[-1],
        "max": s[-1],
        "std": statistics.stdev(s) if n > 1 else 0,
    }


def fmt(p):
    return f"min={p['min']:.1f} avg={p['avg']:.1f} med={p['med']:.1f} p95={p['p95']:.1f} max={p['max']:.1f}ms"


def main():
    print("=" * 60)
    print("  PicoWAL Raw TCP Protocol Test")
    print(f"  Target: {HOST}:{PORT}")
    print("=" * 60)

    # 1. Connect + Auth
    print("\n[1] Connect + Auth...")
    try:
        sock, t_conn, t_auth = connect_and_auth()
        print(f"    Connected in {t_conn:.1f}ms, auth in {t_auth:.1f}ms")
    except Exception as e:
        print(f"    FAILED: {e}")
        return

    # 2. NOOP round-trips
    print("\n[2] NOOP round-trips (50)...")
    noop_times = []
    for _ in range(50):
        try:
            dt = send_noop(sock)
            noop_times.append(dt)
        except Exception as e:
            print(f"    NOOP error: {e}")
            break
    if noop_times:
        p = percentiles(noop_times)
        print(f"    {fmt(p)}")
    else:
        print("    No successful NOOPs")

    # 3. APPEND round-trips
    print("\n[3] APPEND writes (100)...")
    append_times = []
    errors = 0
    for i in range(100):
        try:
            key = 0xBE000000 | i
            dt = send_append(sock, key, f"bench_tcp_{i:04d}_padding_data")
            append_times.append(dt)
        except Exception as e:
            errors += 1
            if errors >= 5:
                print(f"    Too many errors, stopping: {e}")
                break
    if append_times:
        p = percentiles(append_times)
        rate = len(append_times) / (sum(append_times) / 1000) if sum(append_times) > 0 else 0
        print(f"    {len(append_times)} ok, {errors} errors = {rate:.0f} ops/sec")
        print(f"    {fmt(p)}")

    # 4. READ round-trips
    print("\n[4] READ (50)...")
    read_times = []
    for i in range(50):
        try:
            key = 0xBE000000 | i
            dt, data, err = send_read(sock, key)
            if err:
                pass  # key not found is ok
            read_times.append(dt)
        except Exception as e:
            print(f"    READ error: {e}")
            break
    if read_times:
        p = percentiles(read_times)
        print(f"    {fmt(p)}")

    # 5. Connection overhead comparison
    print("\n[5] Reconnect overhead (5 attempts)...")
    sock.close()
    reconn_times = []
    for _ in range(5):
        try:
            t0 = time.perf_counter()
            s, tc, ta = connect_and_auth()
            total = (time.perf_counter() - t0) * 1000
            reconn_times.append(total)
            s.close()
        except Exception as e:
            print(f"    Reconnect error: {e}")
    if reconn_times:
        p = percentiles(reconn_times)
        print(f"    {fmt(p)}")

    print("\n" + "=" * 60)
    print("  SUMMARY")
    print("=" * 60)
    if noop_times:
        print(f"  NOOP RTT:     {statistics.mean(noop_times):.1f}ms avg")
    if append_times:
        print(f"  APPEND RTT:   {statistics.mean(append_times):.1f}ms avg ({len(append_times)/(sum(append_times)/1000):.0f} ops/sec)")
    if read_times:
        print(f"  READ RTT:     {statistics.mean(read_times):.1f}ms avg")
    if reconn_times:
        print(f"  Reconnect:    {statistics.mean(reconn_times):.1f}ms avg")
    print(f"  (Compare: HTTP keep-alive ~28ms avg from bench_run.py)")
    print()


if __name__ == "__main__":
    main()
