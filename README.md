# PicoWAL — Micro-Database Appliance

A networked micro-database running on a **Raspberry Pi Pico 2W** with a Waveshare Pico-ResTouch-LCD-3.5 display and 16GB SD card.

Serves a full SSR web UI with user auth, schema management, a query language with joins and aggregates, master-child forms, batch writes, and OTA firmware updates — all from a $6 microcontroller.

📊 **[Benchmark Results →](docs/BENCHMARKS.md)** | 🗺️ **[FPGA Design →](docs/FPGA_DESIGN.md)**

## What it does

- **HTTP database server** on port 80 over WiFi
- **16GB SD card** for user data (6.6 million card slots)
- **4MB flash** for system data (users, schemas)
- **Query engine** with `S:` / `F:` / `W:` syntax, cross-pack joins, aggregates
- **Cost-based optimizer** with cardinality estimates and predicate reordering
- **Server-side rendered** web UI — no client frameworks, ~2KB of JS total
- **Master-child forms** with inline editable grid
- **OTA firmware updates** via SD staging (no BOOTSEL needed)
- **LCD dashboard** showing flash/SD stats, uptime, IP address

## Quick start

### 1. Build & flash

```powershell
$env:PICO_SDK_PATH = "C:\source\pico-sdk"
cmake -B build -G Ninja
cmake --build build
# Hold BOOTSEL, plug USB, copy build/pico2w_lcd.uf2 to RPI-RP2 drive
```

### 2. Connect

Device boots to `192.168.222.223:80` (static IP). Log in with `admin` / `admin`.

### 3. Create a pack

Admin → Schema → New Pack → set name, module, ordinal → add fields.

### 4. Query

```
S:name,price,currencies.code
F:products,currencies
W:price|>|1000
```

## Architecture

```
┌─────────────────────────────────────────────┐
│  Browser                                     │
│  ┌─────────┐ ┌──────────┐ ┌──────────────┐  │
│  │ SSR HTML │ │ app.js   │ │ Grid Editor  │  │
│  │ (cached) │ │ (2KB,24h)│ │ (batch save) │  │
│  └────┬─────┘ └────┬─────┘ └──────┬───────┘  │
└───────┼─────────────┼──────────────┼──────────┘
        │ HTTP/1.1    │              │
┌───────┴─────────────┴──────────────┴──────────┐
│  Pico 2W (RP2350 Cortex-M33, 520KB SRAM)     │
│                                                │
│  ┌──────────────────────────────────────────┐  │
│  │ web_server.c — SSR pages, routes, OTA    │  │
│  │ query.c — parser, optimizer, executor    │  │
│  │ user_auth.c — sessions, RBAC, SHA-256    │  │
│  └────────────────┬─────────────────────────┘  │
│                   │                            │
│  ┌────────────────┴─────────────────────────┐  │
│  │ kv_store.h — unified routing layer       │  │
│  │   Pack 0-1 → kv_flash (system data)      │  │
│  │   Pack 2+  → kv_sd (user data on SD)     │  │
│  └──────┬─────────────────┬─────────────────┘  │
│         │                 │                    │
│  ┌──────┴──────┐  ┌──────┴──────────────────┐  │
│  │ 4MB Flash   │  │ 16GB SD Card (SPI1)     │  │
│  │ kv_flash.c  │  │ kv_sd.c + OTA staging   │  │
│  │ 4KB sectors │  │ 2KB cards, bitmap alloc  │  │
│  └─────────────┘  └─────────────────────────┘  │
│                                                │
│  ┌──────────────────────────────────────────┐  │
│  │ LCD Dashboard (ILI9488, SPI1 shared)     │  │
│  │ Flash/SD stats, IP, uptime, OTA status   │  │
│  └──────────────────────────────────────────┘  │
└────────────────────────────────────────────────┘
```

## Data model

### Packs and cards

Data is organized into **packs** (like tables) containing **cards** (like rows). Each card is a binary blob with a 4-byte magic header (`0xCA7D`) followed by ordinal-tagged fields.

| Pack | Purpose | Storage |
|------|---------|---------|
| 0 | Schema definitions | Flash |
| 1 | Users & auth | Flash |
| 2 | Days (reference) | SD |
| 3 | Countries (reference) | SD |
| 4 | Currencies (reference) | SD |
| 5+ | User-defined | SD |

### Schema card format (Pack 0)

Each pack's schema is stored as a card in Pack 0:

| Ordinal | Field | Description |
|---------|-------|-------------|
| 0 | Pack name | Length-prefixed UTF-8 |
| 1 | Field count | uint8 |
| 2 | Field definitions | 3 bytes each: ord, type, maxlen |
| 3 | Flags | bit 0: public-read, bits 1-3: cardinality bucket |
| 4 | Module | Groups packs into nav dropdowns |
| 5 | Field names | Null-separated ASCII |
| 6 | Children | Array of child pack ordinals (1:many) |

### Field types

| Code | Name | Friendly | Size |
|------|------|----------|------|
| 0x01 | uint8 | Small number (0-255) | 1 |
| 0x02 | uint16 | Number (0-65k) | 2 |
| 0x03 | uint32 | Large number | 4 |
| 0x04 | int8 | Small number (+/-) | 1 |
| 0x05 | int16 | Number (+/-) | 2 |
| 0x06 | int32 | Large number (+/-) | 4 |
| 0x07 | bool | Yes / No | 1 |
| 0x08 | ascii | Text | len-prefixed |
| 0x09 | utf8 | Text | len-prefixed |
| 0x0A | date | Date | len-prefixed |
| 0x0B | time | Time | len-prefixed |
| 0x0C | datetime | Date & Time | len-prefixed |
| 0x10 | array_u16 | Number list | count-prefixed |
| 0x11 | blob | Binary data | raw bytes |
| 0x12 | lookup | Link | uint32 card ID |

### Relationships

- **Many→1 (lookup)**: A field of type `0x12` stores a card ID from another pack. The `maxlen` byte specifies the target pack ordinal.
- **1→Many (children)**: Schema ord 6 lists child pack ordinals. The child pack must have a lookup field pointing back to the parent.

## Query language

```
S:field1,field2              — Select fields
S:*                          — Select all
S:pack.field                 — Select from joined pack
S:SUM|field                  — Aggregate (SUM, AVG, MIN, MAX, COUNT)
F:pack1,pack2                — From (comma-separated, first is primary)
W:field|op|value             — Where (multiple = AND)
W:pack.field|op|value        — Where on joined pack
```

**Operators:** `==`, `!=`, `>`, `<`, `>=`, `<=`, `IN`, `NI`

**Response:** Pipe-delimited rows, CRLF line endings, `X-Pack` and `X-Count` HTTP headers.

### Query optimizer

- **Cardinality cache**: 3-bit log10 buckets in SRAM, lazy-flushed to schema flags
- **Fanout stats**: Tracks avg children per lookup value
- **Predicate reordering**: Sorts WHERE clauses by estimated selectivity (most selective first)
- **Short-circuit AND**: First failing predicate skips the row

### Examples

```
S:name,sku,price,currencies.code
F:products,currencies
W:price|>|1000
```

```
S:SUM|total,customers.name
F:orders,customers
```

## Web UI

### Pages

| Route | Description |
|-------|-------------|
| `/` | Home — pack cards with counts |
| `/pack/{n}` | Card list — multi-column, paginated, search bar |
| `/pack/{n}/{card}` | Card editor — breadcrumbs, prev/next, master-child grid |
| `/status` | Appliance stats |
| `/query` | Query form + results |
| `/admin` | User management |
| `/admin/meta` | Schema browser |
| `/admin/meta/{n}` | Schema editor — fields, module, children |
| `/admin/log` | Debug log ring buffer |
| `/update` | OTA firmware update |

### Features

- **Sans-serif UI** with warm dark palette
- **Pretty field labels**: `in_stock` → "In Stock"
- **Friendly type hints**: "Text" not "utf8", "Yes / No" not "bool"
- **Module grouping**: Packs grouped into nav dropdowns (Reference ▾, Sales ▾, Admin ▾)
- **Pagination**: 10 cards per page with prev/next
- **Lookup dropdowns**: ≤16 cards → `<select>`, >16 → search input
- **Master-child grids**: Inline editable detail tables with Save All (batch write)
- **Cache headers**: `app.js` cached 24h, favicon 204 cached 7d

## Batch API

```
POST /batch
Content-Type: application/octet-stream
```

Binary format: `0xBA 0x7C | count(u16) | [pack(u16) card(u32) len(u16) data]...`

Validates all entries + RBAC first, then writes all cards. Max 32 per batch.

## OTA firmware update

Firmware uploads to SD card staging area (512KB, blocks 1-1024), then flashes to slot A on commit. No XIP contention — SD reads via SPI are independent of flash.

```
POST /update/begin    → prepare SD staging
POST /update/chunk    → write 1KB chunks to SD
POST /update/commit   → SD → SRAM → flash, reboot
```

Or use the deploy script:

```powershell
python ota_deploy.py 192.168.222.223 build/pico2w_lcd.bin
```

## Hardware

- **Raspberry Pi Pico 2W** — RP2350 Cortex-M33, 520KB SRAM, 4MB flash
- **Waveshare Pico-ResTouch-LCD-3.5** — ILI9488 LCD + XPT2046 touch
- **16GB SDHC** — shared SPI1 bus (GP10=SCK, GP11=MOSI, GP12=MISO, GP22=CS)
- **Static IP**: 192.168.222.223/16, gateway 192.168.0.1

## Source files — complete reference

### Boot & orchestration

| File | Lines | Purpose |
|------|-------|---------|
| `src/main.c` | 60 | Entry point. Inits LCD, touch, SD, flash KV, SD KV, DMA. Launches Core 1 (`wal_engine_run`), then enters Core 0 network loop (`net_core_run`). |
| `src/wal_defs.h` | 145 | Shared WAL structures: `wal_state_t`, request/response rings, slot pool (32 × 512B), FIFO helpers, `fifo_push_timeout()`. Owned by both cores. |
| `src/wal_fence.h` | 9 | `wal_dmb()` — memory barrier for cross-core visibility. |

### Core 0 — network & UI

| File | Lines | Purpose |
|------|-------|---------|
| `src/net_core.c` | 659 | Core 0 main loop: WiFi connect (5× retry with backoff), static IP, HTTP init, UDP WAL init, hardware watchdog (8s), poll loop (cyw43 + TCP drain + UDP + LCD + SD flush). Also contains the raw TCP WAL server (port 8001) with HMAC-SHA256 challenge/response auth. Core 1 heartbeat stall detection. |
| `src/net_core.h` | 51 | WiFi credentials, PSK, static IP config, WAL port constants. |
| `src/httpd/web_server.c` | 3464 | **Largest file.** Full SSR HTTP server: connection pool (6 conns), request parser, route dispatcher, HTML templating, CSS generation, app.js serving, cookie auth + PSK fallback, RBAC enforcement. Routes: card CRUD, batch writes, query UI, schema editor, user admin, OTA upload, admin wipe/reboot, debug log, notes. |
| `src/httpd/web_server.h` | 16 | Exports: `web_server_init()`, `web_server_recent_activity()`, `web_log()`, cardinality helpers. |
| `src/udp_wal.c` | 491 | UDP WAL protocol server (port 8002). Session management (8 sessions, epoch-based), HELLO/RESUME handshake, batch write (up to 16 cards), single-card read, 4 durability levels, bitmap ACK. Optional ChaCha20-Poly1305 encryption. Deferred queue (16 slots) + raw ring (48 overflow) for backpressure. |
| `src/udp_wal.h` | 65 | Protocol constants, message types, durability flags. |

### Core 1 — WAL engine

| File | Lines | Purpose |
|------|-------|---------|
| `src/wal_engine.c` | 373 | Core 1 consumer: drains request ring via multicore FIFO, dispatches APPEND/READ/KV_GET/KV_PUT/DELETE/RANGE/RECORD_COUNT ops to `kv_flash`. Runs background compaction when idle. OTA halt support (`__wfe()` spin). |
| `src/wal_engine.h` | 6 | Exports: `wal_engine_run()`. |
| `src/wal_dma.c` | 60 | DMA copy helper using hardware DMA channel. `wal_dma_copy()` for fast bulk memory moves. |
| `src/wal_dma.h` | 25 | DMA init/copy/wait/busy API. |

### Storage — KV stores

| File | Lines | Purpose |
|------|-------|---------|
| `src/kv_flash.c` | 853 | **Flash-backed KV store** for packs 0–1 (schemas + users). Page-based storage with V2 headers (sequence + CRC), mutation group commits, sorted SRAM index (binary search), compaction, deadlog recovery. Interrupt-safe flash writes. |
| `src/kv_flash.h` | 101 | Full KV API: `kv_init/put/get/get_copy/delete/exists/range/stats/compact_step/wipe`. |
| `src/kv_sd.c` | 623 | **SD-backed KV store** for packs 2+. Copy-on-write (COW) slot allocation with bitmap, 3-tier index (SRAM → Flash XIP → SD keylist), sorted merge range queries. Flash index at 0xC0000 with write-order hardening. |
| `src/kv_sd.h` | 112 | SD KV API: `kvsd_init/put/get/get_copy/delete/exists/range/flush/dirty/stats/ready`. Defines FIDX region, KVSD_INDEX_MAX (18000). |
| `src/kv_store.h` | 74 | **Unified routing layer.** Inline functions route by pack ordinal: packs 0–1 → `kv_flash`, packs 2+ → `kv_sd` (when ready, else flash fallback). Used by web_server, udp_wal, query engine. |
| `src/storage.c` | 603 | Experimental packed/compressed storage engine with heatshrink. Superblock, block allocator, pack summaries, log writes. Currently compiled but not active in HTTP routes. |
| `src/storage.h` | 93 | Storage engine API with compression support. |
| `src/field_index.c` | 203 | Hash-based field-value index for O(1) lookups on SD. 4096-bucket hash table, chained entries in SD blocks. Used by `storage.c`. |
| `src/field_index.h` | 48 | Field index API: `fidx_init/insert/search/search_prefix/remove_card`. |

### Metadata & schema

| File | Lines | Purpose |
|------|-------|---------|
| `src/metadata_dict.c` | 231 | Schema catalog: caches pack definitions from Pack 0, provides type/field/schema lookups. Reload from flash on boot. |
| `src/metadata_dict.h` | 60 | Type/field/schema structs, catalog API. |

### Auth & security

| File | Lines | Purpose |
|------|-------|---------|
| `src/user_auth.c` | 736 | User management: session table (4 slots), login/logout, SHA-256 password hashing (salted), RBAC (readPacks/writePacks/deletePacks per user), admin detection, password change, user CRUD. Seeds default admin + reference data schemas on first boot. |
| `src/user_auth.h` | 107 | Auth API: `user_auth_init/login/logout/check/can_read/can_write/can_delete/is_admin/create_user/change_password/seed_schema`. |
| `src/crypto.c` | 299 | **Standalone crypto** (no mbedTLS for ciphers): ChaCha20 stream cipher, Poly1305 MAC, AEAD encrypt/decrypt, HKDF-SHA256 key derivation. Used by UDP WAL encryption. |
| `src/crypto.h` | 36 | Crypto primitives API. |
| `src/key_store.c` | 66 | PSK flash persistence (load/generate/format). Not currently in build target. |
| `src/key_store.h` | 12 | PSK store API. |

### Query engine

| File | Lines | Purpose |
|------|-------|---------|
| `src/query.c` | 820 | Full query engine: `S:/F:/W:` parser, cost-based optimizer (cardinality estimates, predicate reordering, fanout stats), executor with cross-pack joins, aggregates (SUM/AVG/MIN/MAX/COUNT), pipe-delimited output. |
| `src/query.h` | 87 | Query structs, parse/execute API. |

### Configuration headers

| File | Lines | Purpose |
|------|-------|---------|
| `src/lwipopts.h` | 44 | lwIP tuning: 20 TCP PCBs, 8 UDP PCBs, 2s TIME_WAIT, 41KB heap, keepalive on. |
| `src/mbedtls_config.h` | 10 | Minimal mbedTLS: SHA-256 only (for password hashing + HKDF). |
| `src/hs_config.h` | 10 | Heatshrink compression: window=8, lookahead=4. |

### Hardware drivers

| File | Lines | Purpose |
|------|-------|---------|
| `drivers/lcd/ili9488.c` | 212 | ILI9488 480×320 LCD driver over SPI0. Init sequence, pixel/rect/char/string drawing, backlight PWM. Shares bus with nothing (dedicated SPI0). |
| `drivers/lcd/ili9488.h` | 33 | LCD API + color constants. |
| `drivers/sd/sd_card.c` | 276 | SD card SPI driver on SPI1. Bit-bang bus wakeup (required after LCD init), CMD0/CMD8/ACMD41 with timeout, single/multi block read/write. SPI transfer has 50ms timeout guard. All read+write functions `__no_inline_not_in_flash_func` for OTA safety. |
| `drivers/sd/sd_card.h` | 35 | SD API: `sd_init/read_block/read_blocks/write_block/write_blocks/get_info/get_debug`. Pin definitions. |
| `drivers/touch/xpt2046.c` | 46 | XPT2046 resistive touch over SPI0 (shared with LCD). 3-sample averaging. |
| `drivers/touch/xpt2046.h` | 16 | Touch API: `touch_init/read`. |

### Libraries

| Directory | Purpose |
|-----------|---------|
| `lib/heatshrink/` | LZSS compression library (encoder + decoder). Used by `storage.c`. |

### Python tools

| File | Lines | Purpose |
|------|-------|---------|
| `ota_deploy.py` | 132 | OTA firmware uploader — login, begin, chunk (1KB), commit, verify. |
| `bench_run.py` | 270 | HTTP benchmark — login, writes, reads, queries, mixed load. |
| `udp_client.py` | 296 | UDP WAL protocol client — HELLO, batch write, single read, encryption. |
| `stress_test.py` | 335 | Mixed load stress test — 5 UDP writers + 1 HTTP query thread. |
| `tcp_test.py` | 273 | Raw TCP WAL protocol tester — HMAC auth, NOOP/APPEND/READ. |

---

## Data flow

### HTTP request → KV store → response

```
Browser ──HTTP──► lwIP TCP ──► web_server.c route dispatcher
                                    │
                         ┌──────────┴──────────┐
                         │ Cookie/PSK auth      │
                         │ RBAC check           │
                         └──────────┬──────────┘
                                    │
                              kv_store.h router
                              ┌─────┴─────┐
                       Pack 0-1│           │Pack 2+
                         kv_flash.c    kv_sd.c
                              │           │
                         4MB Flash    16GB SD Card
```

### UDP WAL → KV store

```
UDP datagram ──► udp_wal.c recv callback
                      │
              ┌───────┴────────┐
              │ Session lookup  │
              │ Decrypt (opt)   │
              │ Validate batch  │
              └───────┬────────┘
                      │
                 Deferred queue (16 slots)
                 Raw ring overflow (48 slots)
                      │
                 udp_wal_poll() ── one card per cycle ──► kv_store.h
```

### TCP WAL → flash KV (Core 1)

```
TCP connect ──► net_core.c ──► HMAC challenge/response
                                    │
                            wal_state_t request ring
                                    │
                            ──FIFO signal──►
                                    │
                            Core 1: wal_engine.c
                                    │
                              kv_flash.c (packs 0-1 only)
```

### OTA firmware update

```
POST /update/begin  ──► Clear SD staging area (blocks 1-1024)
POST /update/chunk  ──► Write 1KB chunks to SD
POST /update/commit ──► Halt Core 1 ──► Copy SD → SRAM → Flash
                        (sector-0-last write order)
                        ──► watchdog_reboot()
```

### Boot sequence

```
main.c
  │
  ├── lcd_init() + touch_init()      (SPI0)
  ├── sd_init()                       (SPI1, bit-bang wakeup)
  ├── kv_init()                       (flash scan + recovery)
  ├── kvsd_init()                     (SD superblock + keylist)
  ├── metadata_reload_cache()         (schema catalog)
  ├── user_auth_init()                (seed admin if first boot)
  ├── wal_dma_init()                  (DMA channel)
  ├── multicore_launch_core1()        (wal_engine_run)
  └── net_core_run()                  (WiFi + poll loop, never returns)
```

---

## Memory layout

### Flash (4MB)

| Region | Offset | Size | Purpose |
|--------|--------|------|---------|
| Firmware | 0x000000 | ~504KB | Application code + read-only data |
| Flash index (FIDX) | 0x0C0000 | 64KB | SD key index cache (write-order hardened) |
| KV region | 0x100000+ | ~3MB | Flash KV pages (V2 headers, mutation groups) |

### SRAM (520KB total, ~348KB BSS)

| Component | Size | Notes |
|-----------|------|-------|
| g_index[] (18K keys) | 72KB | Sorted SD key index |
| g_slots[] (18K slots) | 72KB | Parallel slot array |
| WAL slot pool (32 × 512B) | 16KB | Request/response ring |
| UDP deferred queue | ~35KB | 16 batches × 16 cards × 134B |
| UDP raw ring | 24KB | 48 overflow datagram slots |
| lwIP heap + pbufs | 41KB | Network buffers |
| HTTP conn buffers | 24KB | 6 connections × 4KB |
| OTA chunk buffer | 16KB | SD → flash staging |
| Stack + heap | ~60KB | Both cores |
| **Free** | **~172KB** | Available for future use |

### SD card (16GB, raw block access)

| Region | Blocks | Purpose |
|--------|--------|---------|
| Superblock | 0 | Magic, version, counts, keylist pointer |
| OTA staging | 1–1024 | 512KB firmware staging area |
| Bitmap | 1025–1056 | 32 blocks × 4096 bits = 131K slot tracker |
| Keylist | 1057–1312 | 256 blocks × 64 entries = 16K key+slot pairs |
| Data slots | 1313+ | 4 blocks per card (2KB), COW allocation |

---

## Interaction map

```
main.c ──────────► net_core.c (Core 0)
    │                  │
    │                  ├── web_server.c ──► kv_store.h ──► kv_flash.c / kv_sd.c
    │                  │       │                               │          │
    │                  │       ├── query.c ◄── metadata_dict.c │          │
    │                  │       ├── user_auth.c                 │          │
    │                  │       └── storage.c ◄── field_index.c │          │
    │                  │                                       │          │
    │                  ├── udp_wal.c ──► crypto.c              │          │
    │                  │       └──────► kv_store.h ────────────┘          │
    │                  │                                                  │
    │                  └── ili9488.c (LCD dashboard)                      │
    │                                                                     │
    └────────────► wal_engine.c (Core 1) ──► kv_flash.c                  │
                        │                                                 │
                        └── wal_dma.c                                    │
                                                                         │
                   sd_card.c ◄───────────────────────────────────────────┘
```

## Build

Requires: Pico SDK, ARM GCC toolchain, CMake, Ninja.

```powershell
$env:PICO_SDK_PATH = "C:\source\pico-sdk"
cmake -B build -G Ninja
cmake --build build
```

Output: `build/pico2w_lcd.uf2` (BOOTSEL) and `build/pico2w_lcd.bin` (OTA).

**Build stats:** ~504KB text, ~348KB BSS, 14 source files + 3 drivers + heatshrink lib.

## License

MIT
