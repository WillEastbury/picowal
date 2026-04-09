# Copilot Instructions — picowal

## Build

Requires the Pico SDK, ARM GCC toolchain, and CMake.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk   # or $env:PICO_SDK_PATH on Windows
cmake -B build
cmake --build build
```

Output: `build/pico2w_lcd.uf2`. Flash by copying to the Pico BOOTSEL drive, or run `flash.bat` on Windows.

There are no tests or linters in this project.

## Architecture

**Dual-core split (RP2350):**

- **Core 0** — WiFi, lwIP networking, HTTP server (port 80), legacy WAL TCP listener (port 8001), LCD status display.
- **Core 1** — WAL engine loop: reads requests from a multicore FIFO, processes `APPEND`/`READ`/`KV_GET`/`KV_PUT` ops against flash, sends responses back via reverse FIFO, and runs background compaction when idle.

Cores communicate through a shared-memory ring of 32 × 2048-byte slots with a `volatile` ready flag and an ARM `dmb` fence (`wal_fence.h`) before ownership handoff. There is no mutex — ownership is transferred via the ready flag.

**Storage stack:**

- `kv_flash` — append-only flash KV store with 4 KB pages, in-RAM sorted index, record-level compression, and background dead-page reclaim.
- `metadata_dict` — metadata layer on top of `kv_flash` for type and field dictionaries.
- `key_store` — persists the PSK in the last flash sector.
- `wal_engine` — Core 1 flash worker that serializes all flash access through the FIFO.
- `wal_dma` — DMA-assisted data movement for payloads.

**HTTP server (`src/httpd/web_server.c`):**

- Raw lwIP TCP callbacks, connection pool of 2, 4 KB request buffer per connection.
- GUI assets (HTML, CSS, JS) are embedded as C string literal arrays — not served from a filesystem.
- All responses include CORS and keep-alive headers.
- Authenticated routes require `Authorization: PSK <64-hex>` header.

**Client (`client/PicoWal.Client/`):**

- .NET 10 library — async TCP client for the WAL protocol (port 8001).
- Uses `TcpClient`, `SemaphoreSlim` for connection serialization, HMAC/CRC auth.

## Conventions

**C style:**

- `snake_case` for functions and variables, `UPPER_SNAKE_CASE` for macros and constants, `_t` suffix for typedefs.
- Standard `#ifndef`/`#define` header guards.
- No heap allocation — all storage is static/global or stack buffers. Do not introduce `malloc`/`free`.
- Fixed-size buffers with `_Static_assert` to verify sizes at compile time.
- `__attribute__((packed))` on wire/flash structs.
- Error handling: `bool` return for internal APIs, lwIP `err_t` for network code, HTTP status codes for web routes. Early-return on error.

**Multicore safety:**

- Never access flash directly from Core 0 — all flash ops go through the WAL FIFO to Core 1.
- Use `wal_dmb()` before flipping the ready flag on shared slots.

**Web server assets:**

- GUI HTML/CSS/JS are stored as `static const char *` arrays in `web_server.c`. When modifying GUI content, update these embedded strings.

**Drivers:**

- LCD (`ili9488.c`) and touch (`xpt2046.c`) are CMake INTERFACE libraries under `drivers/`. The main target links them and inherits their sources/includes automatically.

## MCP — Playwright

A Playwright MCP server is configured in `.github/copilot-mcp.yml` for testing the `/gui` and metadata editor web interfaces.

**Do not run Playwright on this device if the local environment is ARM/Linux (e.g. Termux on a phone).** Chromium will OOM or get SIGTERM. Only use Playwright on remote Copilot cloud agents or x86 CI runners where memory is not constrained.
