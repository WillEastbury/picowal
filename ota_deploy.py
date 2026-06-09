#!/usr/bin/env python3
"""OTA firmware updater for PicoWAL appliance.

Usage: python ota_deploy.py [host] [bin_file] [username] [password]

Defaults:
  host:     192.168.222.223
  bin_file: build/pico2w_lcd.bin
  username: admin
  password: admin
"""

import sys
import struct
import urllib.request
import urllib.error
import http.cookiejar
import os
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.222.223"
BIN  = sys.argv[2] if len(sys.argv) > 2 else "build/pico2w_lcd.bin"
USER = sys.argv[3] if len(sys.argv) > 3 else "admin"
PASS = sys.argv[4] if len(sys.argv) > 4 else "admin"

BASE = f"http://{HOST}"
CHUNK_SIZE = 1024

def main():
    if not os.path.exists(BIN):
        print(f"ERROR: {BIN} not found")
        sys.exit(1)

    firmware = open(BIN, "rb").read()
    print(f"Firmware: {BIN} ({len(firmware)} bytes)")

    # Cookie jar for session
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

    # Login
    print(f"Logging in as {USER}...")
    login_body = bytearray(64)
    login_body[0] = len(USER)
    for i, c in enumerate(USER[:31]):
        login_body[1 + i] = ord(c)
    login_body[32] = len(PASS)
    for i, c in enumerate(PASS[:31]):
        login_body[33 + i] = ord(c)

    req = urllib.request.Request(f"{BASE}/login", data=bytes(login_body), method="POST")
    try:
        resp = opener.open(req, timeout=5)
        if resp.status != 200:
            print(f"Login failed: {resp.status}")
            sys.exit(1)
        card_id = struct.unpack("<I", resp.read(4))[0]
        print(f"Logged in (card {card_id})")
    except urllib.error.HTTPError as e:
        print(f"Login failed: {e.code}")
        sys.exit(1)

    # Begin OTA
    print("Starting OTA...")
    req = urllib.request.Request(f"{BASE}/update/begin", data=b"", method="POST")
    try:
        resp = opener.open(req, timeout=30)
        print(f"Begin: {resp.read().decode()}")
    except urllib.error.HTTPError as e:
        print(f"Begin failed: {e.code} {e.read().decode()}")
        sys.exit(1)

    # Send chunks
    offset = 0
    total = len(firmware)
    retries_left = 3
    while offset < total:
        end = min(offset + CHUNK_SIZE, total)
        chunk = firmware[offset:end]

        req = urllib.request.Request(
            f"{BASE}/update/chunk",
            data=chunk,
            method="POST",
            headers={"Content-Type": "application/octet-stream"},
        )
        try:
            resp = opener.open(req, timeout=30)
            resp.read()
            retries_left = 3  # reset on success
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            retries_left -= 1
            if retries_left <= 0:
                print(f"\nChunk failed at {offset} after retries: {e}")
                sys.exit(1)
            print(f"\n  Retry at {offset} ({e})...", end="")
            time.sleep(2)
            continue

        offset = end
        pct = 100 * offset // total
        bar = "#" * (pct // 2) + "-" * (50 - pct // 2)
        print(f"\r  [{bar}] {offset}/{total} ({pct}%)", end="", flush=True)

    print()

    # Commit
    print("Committing and rebooting...")
    req = urllib.request.Request(f"{BASE}/update/commit", data=b"", method="POST")
    try:
        resp = opener.open(req, timeout=10)
        print(f"Commit: {resp.read().decode()}")
    except Exception:
        print("Commit sent (connection may have closed on reboot)")

    # Wait for reboot
    print("Waiting for reboot...", end="", flush=True)
    for _ in range(60):
        time.sleep(1)
        print(".", end="", flush=True)
        try:
            resp = urllib.request.urlopen(f"{BASE}/status", timeout=2)
            if resp.status == 200:
                print(f"\nDevice is back up!")
                return
        except Exception:
            pass

    print("\nDevice did not respond after 60s — check manually")

if __name__ == "__main__":
    main()
