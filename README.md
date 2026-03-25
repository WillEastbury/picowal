# Pico 2W Storage Appliance

HTTP-first flash-backed record storage for a Raspberry Pi Pico 2W with a Waveshare Pico-ResTouch-LCD-3.5.

The current firmware exposes a small authenticated HTTP API, an on-device metadata dictionary, and a browser GUI that can read, write, decode, and seed binary records using that metadata.

## Current appliance behavior

- Wi-Fi STA mode on the Pico 2W
- HTTP service on port `80`
- Minimal LCD status panel refreshed every `10s`
- PSK-based authentication for record and metadata writes
- Flash-backed KV record store with record-level compression for larger payloads
- On-device metadata dictionaries for record types and fields
- Browser GUI at `/gui`

The old raw WAL TCP listener on port `8001` is no longer used.

## HTTP endpoints

### Appliance

- `GET /`
  - Returns plain-text appliance stats
- `GET /gui`
  - Serves the browser GUI shell
- `GET /gui.css`
- `GET /gui_codec.js`
- `GET /gui_app.js`
- `GET /key`
  - Returns the current PSK as hex
  - Restricted to clients on the same subnet

### Records

- `GET /0/{type}/{id}`
  - Reads a record body
- `POST /0/{type}/{id}`
  - Writes a record body

These routes require:

```http
Authorization: PSK <64-hex-char-key>
```

`type` is `0..1023` and `id` is `0..4194303`.

### Metadata

- `GET /meta/types`
- `GET /meta/types/{ordinal}`
- `GET /meta/types/by-name/{name}`
- `POST /meta/types/{ordinal}`
  - Body: type name

- `GET /meta/fields`
- `GET /meta/fields/{ordinal}`
- `GET /meta/fields/by-name/{name}`
- `POST /meta/fields/{ordinal}`
  - Body: `NAME|TYPE|MAXLEN`

Metadata writes also require the PSK header.

## Metadata field types

Supported field types:

- `bool`
- `char`
- `char[]`
- `byte`
- `byte[]`
- `uint8`
- `int8`
- `int16`
- `int32`
- `uint16`
- `uint32`
- `isodate`
- `isotime`
- `isodatetime`
- `utf8`
- `latin1`

Notes:

- `char[]` remains supported for compatibility.
- `utf-8` and `latin-1` are also accepted as aliases when creating field definitions.

## Quick start

### 1. Get the PSK

From a machine on the same subnet:

```text
GET http://<pico-ip>/key
```

Copy the returned 64-character hex key.

### 2. Create one type and a few fields

Example:

- record type ordinal `1` = `device`
- field ordinal `1` = `enabled|bool|1`
- field ordinal `2` = `name|utf8|32`
- field ordinal `3` = `title_latin|latin1|32`
- field ordinal `4` = `count|uint16|2`
- field ordinal `5` = `when|isodatetime|20`

Example HTTP requests:

```text
POST /meta/types/1
Authorization: PSK <key>

device
```

```text
POST /meta/fields/1
Authorization: PSK <key>

enabled|bool|1
```

```text
POST /meta/fields/2
Authorization: PSK <key>

name|utf8|32
```

```text
POST /meta/fields/3
Authorization: PSK <key>

title_latin|latin1|32
```

```text
POST /meta/fields/4
Authorization: PSK <key>

count|uint16|2
```

```text
POST /meta/fields/5
Authorization: PSK <key>

when|isodatetime|20
```

### 3. Open the GUI

Open:

```text
http://<pico-ip>/gui
```

Then:

- paste the PSK into the `PSK` box
- set `TYPE` to `1`
- set `ID` to `1`
- click `LOAD METADATA`

### 4. Save your first binary record

Put this JSON into `VALUE`:

```json
{
  "enabled": true,
  "name": "cafe-01",
  "title_latin": "Müller",
  "count": 42,
  "when": "2026-03-25T16:00:00Z"
}
```

Then click `SAVE`.

The GUI will:

- resolve field names to ordinals
- encode fixed-width values inline
- encode variable-width values into the heap
- write the binary record to `POST /0/1/1`

### 5. Load it back

Click `LOAD`.

The GUI will:

- fetch the raw record bytes from `GET /0/1/1`
- decode the ordinal table and heap
- look up field metadata by ordinal
- repopulate the editor as JSON

### 6. Seed many records

After metadata is loaded, use the seed controls in `/gui`:

- `RECORDS`
- `START ID`
- `BATCH SIZE`
- `SEED FROM METADATA`

This generates metadata-aware binary records and writes them through the same `/0/{type}/{id}` API.

## Browser GUI

Open `http://<pico-ip>/gui`.

The GUI supports:

- manual load/save of records
- metadata loading from `/meta/types` and `/meta/fields`
- metadata-aware binary encode/decode
- bulk record seeding from metadata
- split HTML/CSS/JS assets for smaller downloads on-device

### GUI save/load format

The GUI stores records as a metadata-aware binary format.

Layout:

- record header
  - magic
  - field count
  - heap length
- field table
  - field ordinal
  - field type
  - flags
  - length
  - inline value or heap offset
- heap
  - variable-length payload bytes only

Design goals:

- ordinal-based lookup instead of field names in stored records
- fixed-width values stay inline in the field table
- variable-width values use `offset + length` into the heap
- only fields present in the JSON editor are emitted

The GUI decodes records back into JSON using the field metadata dictionary.

### JSON editor shape

In `/gui`, the `VALUE` box is a JSON object keyed by field name. Example:

```json
{
  "enabled": true,
  "title": "Hello",
  "count": 42,
  "when": "2026-03-25T16:00:00Z"
}
```

On save:

- field names are resolved to field ordinals from metadata
- values are encoded according to metadata type and max length

On load:

- the binary record is decoded using field ordinals and metadata
- the JSON editor is repopulated with typed values

### Bulk seeding

The GUI can generate up to `5000` records from metadata.

Seeding behavior:

- cycles across metadata types
- uses ascending record IDs from the chosen start ID
- generates sample values by field type
- writes records through `POST /0/{type}/{id}`

## Storage model

- Flash-backed append-only record store
- 4 KB pages
- Multiple records can share a page
- In-memory sorted key index
- Record-level compression on larger saved values
- Background reclaim of dead pages
- Opportunistic prewarming of the next append page to flatten write latency

Current storage stats shown on the LCD and `/` include:

- record count
- LCD: boot state, `IP:port`, record count, space state, free bytes, used pages
- HTTP `/`: used bytes
- free bytes
- usage percentage
- dead pages

`USED BYTES` currently reflects allocated flash page bytes, not exact occupied payload bytes.

## Hardware

- Raspberry Pi Pico 2W
- Waveshare Pico-ResTouch-LCD-3.5
  - ILI9488 LCD
  - XPT2046 touch

## Build

Requires:

- Pico SDK
- ARM GCC toolchain
- CMake

Example on Windows:

```powershell
$env:PICO_SDK_PATH = "C:\source\pico-sdk"
cmake -B build
cmake --build build
```

Output:

- `build\pico2w_lcd.uf2`

## Flash

1. Hold **BOOTSEL** while connecting the Pico
2. Copy `build\pico2w_lcd.uf2` to the `RPI-RP2` drive

Or use:

```powershell
.\flash.bat
```

## Project structure

```text
├── CMakeLists.txt
├── flash.bat
├── src/
│   ├── main.c
│   ├── net_core.c
│   ├── kv_flash.c
│   ├── metadata_dict.c
│   ├── metadata_dict.h
│   ├── wal_defs.h
│   ├── wal_engine.c
│   └── httpd/
│       ├── web_server.c
│       └── web_server.h
├── drivers/
│   ├── lcd/
│   │   └── ili9488.c
│   └── touch/
│       └── xpt2046.c
└── README.md
```

## License

MIT
