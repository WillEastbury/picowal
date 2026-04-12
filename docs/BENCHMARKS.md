# PicoWAL Benchmarks

Performance benchmarks for PicoWAL running on a Raspberry Pi Pico 2W.

## Test Environment

| Component | Specification |
|-----------|---------------|
| **MCU** | RP2350 Cortex-M33, 150MHz, 520KB SRAM |
| **Flash** | 4MB (system data: users, schemas, KV store) |
| **SD Card** | 16GB SDHC, SPI1 @ 25MHz (deferred init) |
| **WiFi** | CYW43439, 802.11n, 2.4GHz |
| **Network** | Static IP 192.168.222.223, single-hop LAN |
| **Firmware** | ~490KB text, ~374KB BSS |
| **Client** | Python `urllib` on Windows, same LAN |
| **Sessions** | 8 concurrent session slots |
| **TCP PCBs** | 8 (lwIP) |

## Latest Run — 2026-04-12

### Write Throughput

| Test | Cards | Time | Rate | Avg Latency |
|------|-------|------|------|-------------|
| **Sequential write** | 100 | 6.4s | **16 cards/sec** | 64.1 ms |

Sequential writes are bottlenecked by the HTTP request/response WiFi round-trip (~30ms) plus flash KV write time per card. The device was running on flash-only KV (SD not initialised), so write latency is dominated by WiFi overhead rather than storage.

### Read / Page Render Throughput

| Test | Requests | Avg Latency | P95 | Max |
|------|----------|-------------|-----|-----|
| **Pack list page** | 50 | **27.9ms** | 44.9ms | 46.2ms |
| **Status page** | 50 | **31.6ms** | 42.5ms | 47.2ms |

### Query Throughput

Queries run against pack 10 with 100 benchmark cards, flash-backed KV.

| Test | Queries | Avg Latency | P95 | Max |
|------|---------|-------------|-----|-----|
| **WHERE filter** (`value > 50`) | 30 | **33.9ms** | 47.5ms | 47.6ms |
| **SELECT \*** | 20 | **31.0ms** | 44.4ms | 44.4ms |

### Sustained Mixed Load

Single-threaded, 30-second continuous operation mixing writes, page renders, and queries:

| Metric | Value |
|--------|-------|
| **Total operations** | 786 |
| **Throughput** | **26.2 ops/sec** |
| **Writes** | 292 |
| **Reads** | 183 |
| **Queries** | 311 |
| **Errors** | 108 (12% error rate) |

The device sustains **26 mixed ops/sec** over 30 seconds without crashes or data corruption. Errors are HTTP-level timeouts under sustained fire — the single-threaded TCP poll loop occasionally can't drain responses fast enough.

### Latency & Jitter Summary

| Operation | Min | Avg | Median | P95 | Max | StdDev | Jitter (avg) |
|-----------|-----|-----|--------|-----|-----|--------|-------------|
| **Login** | 12.7ms | 24.0ms | 30.3ms | 35.3ms | 35.3ms | 9.1ms | 8.2ms |
| **Status page** | 20.2ms | 31.6ms | 32.0ms | 42.5ms | 47.2ms | 6.6ms | 8.2ms |
| **Sequential write** | 48.6ms | 64.1ms | 63.8ms | 80.2ms | 89.1ms | 8.2ms | 9.4ms |
| **Page read** | 13.0ms | 27.9ms | 30.8ms | 44.9ms | 46.2ms | 9.5ms | 9.1ms |
| **Query WHERE** | 18.2ms | 33.9ms | 32.3ms | 47.5ms | 47.6ms | 7.0ms | 8.8ms |
| **Query S:\*** | 17.3ms | 31.0ms | 31.6ms | 44.4ms | 44.4ms | 8.7ms | 8.9ms |
| **Keep-alive** | 20.1ms | 33.2ms | 34.0ms | 49.7ms | 49.7ms | 8.4ms | — |
| **Mixed load** | 12.1ms | 34.7ms | 31.0ms | 69.6ms | 121.1ms | 16.6ms | 15.7ms |

### Latency Breakdown (typical keep-alive request)

```
WiFi round-trip (baseline):          ~15-25ms
HTTP parsing + routing:              ~0.5ms
Flash KV read (per card):            ~0.1ms
Schema lookup + field decode:        ~0.3ms
HTML/response generation:            ~1-3ms
TCP transmission (2-8KB page):       ~2-4ms
────────────────────────────────────────────
Total:                               ~20-35ms
```

### Notes on This Run

- **Flash-only KV**: SD card was not initialised (deferred init after boot fix). All data operations hit flash KV, not SD-backed storage.
- **Single connection**: All tests used a single HTTP keep-alive connection to avoid TCP PCB exhaustion (8 PCBs available).
- **No batch writes tested**: The batch endpoint requires SD-backed storage which was unavailable.
- **Error rate improved**: 12% in mixed load vs 40% in earlier runs — the SD init fix eliminated boot-time SPI contention that was degrading the TCP stack.

## Historical Results (pre-SD-fix)

Earlier benchmarks with SD-backed storage active (3,000+ cards):

| Test | Rate | Avg Latency |
|------|------|-------------|
| Sequential write | 42 cards/sec | 23.8ms |
| Batch write ×32 | 1,396 cards/sec | 0.7ms/card |
| Page render | 54 pages/sec | 18ms |
| 4-thread reads | 129 pages/sec | 7.8ms |
| Query WHERE (3K cards) | 53 queries/sec | 19ms |
| Sustained mixed | 27 ops/sec | — |

## Hardware Limits

| Resource | Capacity | Current Usage |
|----------|----------|---------------|
| SRAM | 520 KB | 374 KB (72%) |
| Flash | 4 MB | 490 KB firmware |
| SD Card | 16 GB | Deferred init |
| Sessions | 8 | Concurrent logins |
| TCP PCBs | 8 | Concurrent connections |
| Max card size | 508 bytes | (512 - 4 byte key footer) |
| Max pack count | 1,024 | 10-bit ordinal |
| Max cards/pack | 4,194,303 | 22-bit card ID |

## Key Takeaways

1. **Use batch writes** — 33× faster than sequential. The `/batch` endpoint is the recommended write path for bulk operations.
2. **Queries are fast** — even on flash KV, WHERE filters complete in 34ms avg.
3. **Pages render in 28ms** — SSR HTML with schema-driven forms, lookup resolution, and pagination.
4. **Single-threaded is best** — The Pico's single-core TCP poll loop favours sequential request patterns. Use HTTP keep-alive and batch APIs.
5. **Keep-alive matters** — saves ~10ms per request by avoiding TCP handshake overhead.
6. **TCP PCB limit** — 8 PCBs means concurrent multi-thread writes will fail. Use single-connection batch patterns instead.
