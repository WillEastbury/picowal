# PicoWAL — Write-Ahead Log Appliance on Raspberry Pi Pico 2W

A dual-core WAL (Write-Ahead Log) appliance running on the RP2350 with WiFi networking and a 3.5" LCD status display.

## Architecture

```
┌─────────────────────────────┐     ┌─────────────────────────────┐
│         CORE 0              │     │         CORE 1              │
│                             │     │                             │
│  WiFi + TCP receive         │     │  WAL engine + compactor     │
│  Zero-copy pbuf parsing     │────▶│  Process APPEND / READ      │
│  Request ring writer        │     │  Background compaction      │
│  Response drain + TCP send  │◀────│  Response ring writer       │
│                             │     │                             │
│  DMA-accelerated transfers  │ FIFO│  Scans WAL index, merges    │
│  pbuf_ref() zero-copy path  │(rev)│  duplicate keys, reclaims   │
└─────────────────────────────┘     └─────────────────────────────┘
              │                                   │
              └───────────┬───────────────────────┘
                          │
                   192 × 2KB SRAM
                   Buffer Pool (384KB)
                   + 256 Request/Response Ring
```

### Core 0 — Network + Buffer Manager
- Connects to WiFi (Pico W CYW43)
- Establishes TCP connection to a configurable host
- Parses framed WAL requests from the TCP stream
- **Zero-copy path**: when value data is contiguous in a pbuf, passes the pointer directly to Core 1 via `pbuf_ref()` — no copy on Core 0 at all
- **DMA fallback**: when data spans multiple pbufs, DMA copies directly to buffer slot (CPU-free)
- Drains reverse FIFO for Core 1 responses, sends TCP acks

### Core 1 — WAL Engine + Compactor
- Pops request IDs from forward FIFO
- Executes `APPEND_DELTA` (register delta in WAL index) or `READ_DELTASET` (collect + compact deltas for a key into one response slot)
- Pushes response ID back via reverse FIFO
- **Background compaction**: when FIFO is empty, incrementally scans the WAL for duplicate keys, keeps only the latest delta, frees old slots

### Memory Ownership Protocol
Shared memory between cores uses a strict ownership fence:

| `ready` state | Owner | Rule |
|---------------|-------|------|
| `REQ_EMPTY (0)` | Core 0 | Core 1 must not touch |
| `REQ_PENDING (1)` | Core 1 | Core 0 must not touch |
| `REQ_DONE (2)` | Core 0 | Core 1 must not touch |

Every ownership transition is preceded by a hardware `DMB` (data memory barrier) instruction.

## Hardware

- **Raspberry Pi Pico 2W** (RP2350, dual Cortex-M33, 520KB SRAM, WiFi)
- **Waveshare Pico-ResTouch-LCD-3.5** (480×320, ILI9488 SPI, XPT2046 touch)

## Memory Layout

| Region | Size | Usage |
|--------|------|-------|
| Buffer pool | 384 KB | 192 × 2KB delta payload slots |
| Request/response rings | ~6 KB | 256 entries each |
| WAL index | ~2 KB | 192 entry metadata |
| lwIP + WiFi stack | ~50 KB | pbuf pool, TCP state, CYW43 |
| Code + stack | ~70 KB | .text + .bss + stacks |
| **Total** | ~512 KB | of 520 KB available |

## Wire Protocol

TCP stream, little-endian. Each request is a single byte opcode followed by its payload.

### Requests (host → pico)

| Op | Code | Payload | Description |
|----|------|---------|-------------|
| NOOP | `0x00` | *(none)* | Keepalive |
| APPEND | `0x01` | `key_hash:u32` `value_len:u16` `delta_op:u8` `value:bytes` | Append a delta |
| READ | `0x02` | `key_hash:u32` | Read compacted deltaset for key |

`delta_op`: `0x00` = SET, `0x01` = DELETE (tombstone)

### Responses (pico → host)

| Op | Code | Payload | Description |
|----|------|---------|-------------|
| NOOP_ACK | `0x80` | *(none)* | |
| APPEND_ACK | `0x81` | `seq:u32` | Assigned sequence number |
| READ_RESP | `0x82` | `count:u32` `total_len:u16` `data:bytes` | Compacted delta payloads |
| ERROR | `0xFF` | `code:u8` | `0x01`=full, `0x02`=too big, `0x03`=protocol |

## Configuration

Edit `src/net_core.h`:

```c
#define WIFI_SSID       "Puddles-Mesh"
#define WIFI_PASSWORD   "Whatever1"
#define WAL_HOST        "192.168.1.100"
#define WAL_PORT        8001
```

## Build

Requires: Pico SDK 2.1.1, ARM GCC 13.3+, CMake, Ninja, Visual Studio (for picotool host build).

```powershell
$env:PICO_SDK_PATH = "C:\source\pico-sdk"
$env:PATH = "C:\arm-gnu-toolchain\bin;" + $env:PATH

cmake -B build -G Ninja
ninja -C build
```

Output: `build/pico2w_lcd.uf2`

## Flash

1. Hold **BOOTSEL** on the Pico 2W while plugging in USB
2. Copy `build/pico2w_lcd.uf2` to the `RPI-RP2` drive

## Project Structure

```
├── CMakeLists.txt
├── src/
│   ├── main.c              # Entry point, multicore launch
│   ├── wal_defs.h          # Buffer pool, request/response rings, FIFO protocol
│   ├── wal_fence.h         # Hardware DMB memory barrier
│   ├── wal_dma.h/c         # DMA-accelerated memory-to-memory copies
│   ├── net_core.h/c        # Core 0: WiFi, TCP, zero-copy receive, dispatch
│   ├── wal_engine.h/c      # Core 1: WAL index, APPEND/READ, compaction
│   └── lwipopts.h          # lwIP configuration
├── drivers/
│   ├── lcd/ili9488.h/c     # ILI9488 SPI display driver
│   └── touch/xpt2046.h/c   # XPT2046 touch controller
├── flash.bat
└── README.md
```

## License

MIT
