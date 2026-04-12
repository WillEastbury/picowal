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

## Project structure

```
├── CMakeLists.txt
├── ota_deploy.py          — OTA deployment script
├── src/
│   ├── main.c             — Boot sequence, core launch
│   ├── net_core.c         — WiFi, poll loop, LCD dashboard
│   ├── kv_flash.c/.h      — Flash-backed KV store
│   ├── kv_sd.c/.h         — SD-backed KV store
│   ├── kv_store.h         — Unified routing (flash vs SD)
│   ├── storage.c/.h       — Packed compressed storage engine
│   ├── field_index.c/.h   — O(1) hash-based field value index
│   ├── query.c/.h         — Query parser, optimizer, executor
│   ├── user_auth.c/.h     — Auth, RBAC, schema seeding
│   ├── hs_config.h        — Heatshrink compression config
│   ├── mbedtls_config.h   — Minimal mbedtls (SHA-256 only)
│   └── httpd/
│       ├── web_server.c   — All HTTP routes, SSR, OTA
│       └── web_server.h
├── drivers/
│   ├── lcd/ili9488.c      — ILI9488 LCD driver
│   ├── touch/xpt2046.c    — XPT2046 touch driver
│   └── sd/sd_card.c/.h    — SD card SPI driver
├── lib/heatshrink/        — Compression library
└── docs/
    └── FPGA_DESIGN.md     — FPGA KV engine design doc
```

## Build

Requires: Pico SDK, ARM GCC toolchain, CMake, Ninja.

```powershell
$env:PICO_SDK_PATH = "C:\source\pico-sdk"
cmake -B build -G Ninja
cmake --build build
```

Output: `build/pico2w_lcd.uf2` (BOOTSEL) and `build/pico2w_lcd.bin` (OTA).

## License

MIT
