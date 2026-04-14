# Copilot Instructions — picowal

## Build

Requires the Pico SDK, ARM GCC toolchain, and CMake.

```sh
# Windows (PowerShell)
$env:PICO_SDK_PATH = 'C:\source\pico-sdk'
$env:Path = 'C:\arm-gnu-toolchain\bin;C:\Python313\Scripts;' + $env:Path
cmake -B build -G Ninja
cmake --build build
```

Output: `build/pico2w_lcd.uf2` (BOOTSEL) and `build/pico2w_lcd.bin` (OTA).

## Deployment

**BOOTSEL flash (reliable, always works):**
1. Hold BOOTSEL button while plugging USB
2. Copy `pico2w_lcd.uf2` to the `RP2350` drive (usually H: or G:)
3. Device auto-reboots

**OTA over WiFi (from running device):**
```sh
python ota_deploy.py 192.168.0.9
```
⚠️ After OTA commit, the device needs a **USB power cycle** (unplug/replug) — `watchdog_reboot` doesn't properly reset the CYW43 WiFi chip.

**Admin wipe (erases all data, re-seeds defaults):**
POST `/admin/wipe` (admin auth required). Needs power cycle after.

## Hardware

- **MCU**: Raspberry Pi Pico 2W (RP2350, Cortex-M33, 150MHz, 520KB SRAM)
- **Display**: Waveshare Pico-ResTouch-LCD-3.5 (ILI9488, SPI1)
- **SD Card**: 16GB SDHC on SPI1 (shared with LCD, CS on GP22)
- **WiFi**: CYW43439 (2.4GHz only, 802.11n)
- **Current network**: SSID `Bussy5G`, static IP `192.168.0.9/24`, GW `192.168.0.1`

## Architecture

**Dual-core split (RP2350):**

- **Core 0** — WiFi, lwIP, HTTP server (port 80), UDP WAL (port 8002), LCD heartbeat, poll loop
- **Core 1** — WAL engine: flash KV writes via multicore FIFO, background compaction

**Storage tiers:**

- `kv_flash` — flash KV store (packs 0-1: schemas + users)
- `kv_sd` — SD card KV with COW writes (packs 2+: user data)
- `kv_store.h` — unified routing layer (`KV_STORE_REDIRECT` macros)

**3-tier index:**

- Tier 1: SRAM (18K entries, O(log n), key+slot parallel arrays)
- Tier 2: Flash XIP (32K entries at 0xC0000, zero-copy binary search)
- Tier 3: SD keylist (boot-time loading only)
- Eviction: when SRAM full, evicts lowest key (still in flash tier)

**UDP WAL protocol (port 8002):**

- Session-based: HELLO/RESUME handshake with epoch replay protection
- Batch writes: up to 32 cards per batch, bitmap ACK
- Durability levels: FIRE_AND_FORGET (0x01), ACK_QUEUED (0x02), ACK_DURABLE (0x03), ACK_ALL_COMMITTED (0x04)
- Deferred queue: 16 SRAM slots, overflow to 96-slot raw datagram ring
- Poll drains one card per cycle to keep WiFi responsive

**OTA:**

- 600KB slot, SD staging, SRAM-resident SD reads + sector-0-last flash write
- `sd_read_block`/`sd_read_blocks` are `__no_inline_not_in_flash_func` for XIP safety

## SRAM budget (~520KB)

| Region | Size | Notes |
|--------|------|-------|
| KV index (keys) | 72KB | 18K entries × 4 bytes |
| KV index (slots) | 72KB | COW parallel array |
| WAL state | 67KB | 192 × 2KB slots |
| UDP raw ring | 48KB | 96 × 512 byte datagrams |
| UDP deferred queue | 69KB | 16 × 32 × 134 byte cards |
| lwIP pbufs + heap | 41KB | 20 TCP PCBs, 8 UDP PCBs |
| HTTP conn buffers | 24KB | |
| Other | ~27KB | Stack, caches, globals |
| **Free** | **~24KB** | Stack + heap headroom |

## Key constraints

- **No flash writes from Core 0** — use WAL FIFO to Core 1, or `kv_store_put` (which goes through SD directly for packs 2+)
- **No flash_range_erase in lwIP callbacks** — blocks WiFi chip, causes boot hang
- **SD SPI1 shared with LCD** — bit-bang bus wakeup required at SD init
- **SD init needs timeout guards** — SPI bus can get stuck, must not block boot
- **`__no_inline_not_in_flash_func`** — required for any function called during OTA flash write
- **BSS near limit (496/520KB)** — check `arm-none-eabi-size` before adding static buffers
- **Max card size**: 508 bytes (512 - 4 byte key footer)
- **lwIP TIME_WAIT**: reduced to 2s (TCP_MSL=1000ms) for fast PCB recycling

## Conventions

**C style:**

- `snake_case` for functions and variables, `UPPER_SNAKE_CASE` for macros and constants, `_t` suffix for typedefs.
- Standard `#ifndef`/`#define` header guards.
- No heap allocation — all storage is static/global or stack buffers. Do not introduce `malloc`/`free`.
- `__attribute__((packed))` on wire/flash structs.
- Error handling: `bool` return for internal APIs, lwIP `err_t` for network, HTTP status codes for web routes.

**Multicore safety:**

- Never access flash directly from Core 0 — all flash ops go through the WAL FIFO to Core 1.
- Use `wal_dmb()` before flipping the ready flag on shared slots.
- UDP writes bypass the FIFO — use `kv_store_put` directly (Core 0, cooperative).

**Web server assets:**

- GUI HTML/CSS/JS are embedded as `static const char *` arrays in `web_server.c`.

## Test scripts

- `bench_run.py [host]` — HTTP benchmark (login, writes, reads, queries, mixed load)
- `udp_client.py [host]` — UDP protocol test (HELLO, batch write, read, fire-and-forget)
- `stress_test.py [host] [card_count]` — 5 UDP writers + 1 HTTP query thread
- `tcp_test.py [host]` — Raw TCP WAL protocol test (port 8001, not currently enabled)
- `ota_deploy.py [host]` — OTA firmware update via HTTP

## MCP — Playwright

A Playwright MCP server is configured in `.github/copilot-mcp.yml` for testing the `/gui` and metadata editor web interfaces.

**Do not run Playwright on this device if the local environment is ARM/Linux (e.g. Termux on a phone).** Chromium will OOM or get SIGTERM. Only use Playwright on remote Copilot cloud agents or x86 CI runners where memory is not constrained.
