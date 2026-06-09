# PicoWAL FPGA Storage Engine — Design Document

## Overview

A **pure-hardware networked KV storage appliance** built on a Lattice iCE40 HX8K FPGA.
No CPU. No firmware. Three chips. REST API with authentication, RBAC, and a web UI.

```
 RJ45 ──► W5500 ──SPI──► iCE40 HX8K ──SPI──► SD Card
          £3              Alchitry Cu           £5
          100Mbps         3,640 LUTs            16GB
          TCP/UDP/IP      47% utilised          8M cards
```

---

## 1. Bill of Materials

| Component | Part | Package | Role | Price |
|-----------|------|---------|------|-------|
| FPGA | Lattice iCE40 HX8K | Alchitry Cu dev board | Application fabric | — |
| Ethernet | WIZnet W5500 | Module with headers | 10/100 Mbps, hardwired TCP/UDP/IP | ~£3 |
| Storage | MicroSD card | Breakout board | Bulk KV storage | ~£5 (16GB) |
| Production storage | Kingston KTM4GH1AHI01 | 153-FBGA (eMMC 5.1, 4GB) | Soldered production alternative | ~$22 |

Optional Wi-Fi bridge (not in data path):
- Raspberry Pi Zero 2W running a UDP relay (~10 lines of Python)

---

## 2. FPGA Resource Budget (iCE40 HX8K)

### Available Resources

| Resource | Total |
|----------|-------|
| Logic Cells (LUT + FF) | 7,680 |
| RAM4K Blocks | 32 (128Kbit = 16KB) |
| PLLs | 2 |
| Max I/O Pins | 206 |

### Utilisation

| Block | LUTs | BRAM | Notes |
|-------|------|------|-------|
| W5500 SPI driver (SB_SPI hard IP) | 100 | 1 RAM4K | SB_SPI primitive + register/buffer FSM (0 LUTs for SPI itself) |
| HTTP method parser | 120 | 0 | GET/PUT/DELETE/POST detection |
| URL parser | 200 | 0 | `/0/{pack}/{card}`, `/0/{pack}?start&limit`, `/_mget` |
| Query string parser | 80 | 0 | `start=N&limit=N` decimal decode |
| Header scanner | 250 | 0 | Authorization + Content-Length + Cookie extract |
| Base64 decoder | 200 | 0 | 6-bit lookup, 4→3 byte conversion |
| Cookie/hex decoder | 80 | 0 | `Cookie: sid=` parse + hex → 32 bytes |
| Auth validator (SipHash-2-4) | 350 | 0 | Password hash + compare + token gen |
| RBAC checker | 35 | 0 | Sequential ordinal list scan |
| RLS (Pack 1 row-level check) | 30 | 0 | Card == session.user_card compare |
| Field redaction (Pack 1) | 20 | 0 | Skip ord 1,2 in response builder |
| Password change endpoint | 60 | 0 | /_passwd routing + old pass verify |
| Body reader | 80 | 0 | PUT payload into delta buffer |
| Command jump table | 140 | 0 | Op dispatch by method + URL pattern |
| Login/logout handler | 100 | 0 | JSON body parse + session create/expire |
| Session scan FSM | 60 | 0 | Pack 3 token match (reuses SD read) |
| SD SPI controller | 440 | 0 | Init FSM + CMD17/CMD24/CMD18/CMD25 |
| KV_LIST scan FSM | 150 | 0 | Sequential magic check + stream |
| KV_MGET multi-read FSM | 100 | 0 | Body parse + random reads + stream |
| Merge engine | 375 | 4 RAM4K | OLD_BUF + NEW_BUF (2KB each) |
| Response builder | 100 | 1 RAM4K | HTTP status + headers + body |
| Static UI page (KnockoutJS SPA) | 120 | 4 RAM4K | ~2.2KB HTML+JS served from BRAM |
| First-boot defaults | 90 | 1 RAM4K | Admin user + sysconfig template |
| Clock / PLL / reset | 50 | 0 | 100MHz → system clock |
| Tick counter (1Hz) | 30 | 0 | 100MHz ÷ 100M = 1s tick for session expiry |
| Boot epoch register | 10 | 0 | Increment on boot, compare on auth |
| **TOTAL** | **3,640** | **11 RAM4K** | |
| **SPARE** | **4,040 (53%)** | **21 RAM4K (66%)** | |

### Pin Utilisation

| Bus | Pins | Signals |
|-----|------|---------|
| W5500 SPI (SB_SPI hard) | 4 | CS, SCK, MOSI, MISO |
| W5500 control | 2 | /INT, /RST |
| SD Card SPI (soft master) | 4 | CS, SCK, MOSI, MISO |
| **Total** | **10** | of 206 available (5%) |

### Hard IP Utilisation

| Primitive | Used for | Instances |
|-----------|----------|-----------|
| **SB_SPI** | W5500 Ethernet (8MHz, Mode 0) | 1 of 1 |
| SB_I2C | — (available for future) | 0 of 1 |
| **SB_PLL40_CORE** | 100MHz → 50MHz system clock | 1 of 2 |

Note: SD card uses soft SPI master (25MHz needed, SB_SPI max is 8MHz).
The SB_SPI saves ~250 LUTs vs a soft SPI implementation for the W5500.

---

## 3. Data Model

### Hierarchy

```
 Pack (10-bit ordinal: 0..1023)
   └── Card (22-bit ordinal: 0..4,194,303)
         └── Fields (5-bit ordinal: 0..31)
```

### Key Packing (32-bit)

```
 ┌──────────────┬──────────────────────────────┐
 │  PACK: 10    │         CARD: 22             │
 │  0..1023     │       0..4,194,303           │
 └──────────────┴──────────────────────────────┘
 bit 31      bit 22  bit 21                 bit 0
```

### Field Format

```
 ┌─────────────────┬──────────────┬──────────────────┐
 │ ORDINAL BYTE    │ LENGTH BYTE  │ DATA             │
 ├───┬───┬───┬─────┼──────────────┤                  │
 │F2 │F1 │F0 │ o:5 │   len: u8    │ len bytes        │
 └───┴───┴───┴─────┴──────────────┴──────────────────┘
  3 flag bits        full byte (0..255)
  in ordinal byte
  
 Ordinal: 5-bit (0..31 fields per card)
 Length:  8-bit (0..255 bytes per field)
 Flags:  3 bits reserved (F0: deleted, F1: array, F2: immutable)
```

### Hard Constraints

| Constraint | Value | Hardware benefit |
|------------|-------|-----------------|
| Max card payload | 2,048 bytes | 11-bit counters, fixed 4 SD blocks |
| Max field data | 255 bytes | 8-bit length, u8 |
| Max fields per card | 32 | u5 ordinal, single 32-bit bitmap register |
| Max packs | 1,024 | 10-bit ordinal |
| Max cards per pack | 4,194,304 | 22-bit ordinal |

---

## 4. SD Card Layout

### Direct Block Addressing (no index, no allocation)

```
 SD block address = key × 4

 key = (pack << 22) | card
 Each card occupies exactly 4 × 512B = 2,048 bytes
 No fragmentation. No free list. No garbage collection.
```

### Card On-Disk Format

```
 Offset  Size  Content
 0x000   2     Magic: 0xCA7D (valid card) or 0x0000/0xFFFF (empty)
 0x002   2     Version: uint16 (incremented on each write)
 0x004   N     Field 0: [ord:1][len:1][data:len]
               Field 1: [ord:1][len:1][data:len]
               ...
 0x004+N       Zero padding to 2,048 bytes
```

### Capacity

| SD Card Size | Max Cards | Key Space Used |
|-------------|-----------|---------------|
| 128MB | 65,536 | 16-bit |
| 1GB | 524,288 | 19-bit |
| 16GB | 8,388,608 | 23-bit |
| 32GB (SDHC max) | 16,777,216 | 24-bit |

---

## 5. Reserved Packs

### Pack 0 — Metadata (Schemas)

Stores schema definitions for all packs.

| Card | Content |
|------|---------|
| 0 | Pack 0 schema (name: "metadata", field definitions) |
| 1 | Pack 1 schema (name: "users", field definitions) |
| 2 | Pack 2 schema (name: "sysconfig", field definitions) |
| N | Pack N schema |

### Pack 1 — Users

Stores user accounts with authentication and RBAC data.

| Card | Content |
|------|---------|
| 0 | Admin user (factory default) |
| 1+ | Additional users |

#### User Card Field Layout

```
 ord │ len  │ field            │ type
 ────┼──────┼──────────────────┼──────────────────────────
  0  │ ≤33  │ username         │ ascii [len:u8][up to 32 chars]
  1  │ ≤33  │ pswdhash         │ blob [len:u8][32 bytes SipHash-2-4 output]
  2  │ ≤17  │ salt             │ blob [len:u8][16 bytes random]
  3  │   1  │ flags            │ uint8 bit0=disabled, bit1-7=reserved
  4  │   1  │ failedattempts   │ uint8 (0..255)
  5  │ ≤33  │ read_packs       │ array_u16 [len:u8][uint16...] (0xFFFF=all)
  6  │ ≤33  │ write_packs      │ array_u16 [len:u8][uint16...] (0xFFFF=all)
  7  │ ≤33  │ delete_packs     │ array_u16 [len:u8][uint16...] (0xFFFF=all)
 ────┴──────┴──────────────────┴──────────────────────────

 Total: ~184 bytes per user (worst case)
 Max users per 16GB SD: 4,194,304
```

#### Flags Byte

```
 ┌───┬───┬───┬───┬───┬───┬───┬──────────┐
 │ 7 │ 6 │ 5 │ 4 │ 3 │ 2 │ 1 │    0     │
 │ reserved                      │disabled │
 └───┴───┴───┴───┴───┴───┴───┴──────────┘
```

#### ACL Lists

```
 Each list is an array of uint16 pack ordinals.
 0xFFFF = wildcard sentinel (access ALL packs — superadmin).
 Empty list (len=0) = no access for that operation.

 RBAC check logic:
   for each uint16 in acl list:
     if entry == 0xFFFF → ALLOW (wildcard, match everything)
     if entry == pack_ordinal → ALLOW
   end of list → 403 FORBIDDEN

 Example — admin (full access):
   read_packs:   [0xFFFF]
   write_packs:  [0xFFFF]
   delete_packs: [0xFFFF]

 Example — read-only IoT sensor:
   read_packs:   [3, 4, 5]
   write_packs:  [3]
   delete_packs: []

 Example — app user (read all, write some, delete nothing):
   read_packs:   [0xFFFF]
   write_packs:  [3, 7, 12]
   delete_packs: []
```

### Pack 2 — System Configuration

| Card | Content |
|------|---------|
| 0 | Network config (hostname, IP, subnet, gateway, MAC) |
| 1 | Device info (version, serial, etc.) |
| 2+ | Spare |

### Pack 3 — Sessions

Stores active login sessions. Card ordinal = session ID.

| Card | Content |
|------|---------|
| 0+ | One card per active session |

#### Session Card Field Layout

```
 ord │ len  │ field            │ type
 ────┼──────┼──────────────────┼─────────────────────────
  0  │ ≤33  │ session_token    │ blob [len:u8][32 bytes random]
  1  │  4   │ user_card        │ uint32 (card ordinal in Pack 1)
  2  │  4   │ created_tick     │ uint32 (FPGA ticks since boot)
  3  │  4   │ expires_tick     │ uint32 (created + session_duration)
  4  │  4   │ last_active_tick │ uint32 (updated every request)
  5  │  1   │ flags            │ uint8 (bit0=expired, bit1=revoked)
  6  │  4   │ boot_epoch       │ uint32 (must match current_epoch)
 ────┴──────┴──────────────────┴─────────────────────────
 Total: ~56 bytes per session
```

#### Boot Epoch

```
 POWER ON:
   1. Read Pack 2 / Card 0, boot_epoch field
   2. Increment boot_epoch
   3. Write back to SD (delta merge)
   4. Store in FPGA register: current_epoch

 Any session with boot_epoch != current_epoch is instantly invalid.
 All sessions expire on reboot with zero scanning.
```

#### Tick Counter

```
 FPGA 1Hz tick counter:
   PLL 100MHz ÷ 100,000,000 = 1 second tick
   32-bit register wraps after ~136 years
   Used for: session expiry, idle timeout, last_active tracking
```

#### Session Auth Flow

```
 LOGIN (POST /login):
   1. Parse body: username + password
   2. Validate credentials (Pack 1 auth)
   3. Generate 32-byte session token (hardware counter + SipHash)
   4. Create Pack 3 card:
        session_token = generated token
        user_card = matched user card ordinal
        created_tick = current tick
        expires_tick = current tick + SESSION_DURATION
        last_active_tick = current tick
        flags = 0x00
        boot_epoch = current_epoch
   5. Response body: [user_card:u32 LE]
   6. Response header: Set-Cookie: sid=<64 hex chars>; Path=/; HttpOnly

 COOKIE AUTH (every subsequent request):
   1. Parse Cookie header: extract sid=<hex>
   2. Hex decode → 32-byte token
   3. Scan Pack 3 cards for matching session_token
   4. Validate:
        a. flags bit0 or bit1 set → 401 (expired/revoked)
        b. boot_epoch != current_epoch → 401 (stale, post-reboot)
        c. tick_counter > expires_tick → set flags bit0, 401 (absolute expiry)
        d. tick_counter - last_active_tick > IDLE_TIMEOUT → set flags bit0, 401 (idle)
   5. Update last_active_tick = tick_counter (delta merge)
   6. Load user_card from Pack 1 → RBAC check → proceed

 LOGOUT (POST /logout):
   1. Find session card by cookie token
   2. Set flags bit0=1 (expired) via delta merge
   3. Response: Set-Cookie: sid=; Max-Age=0
```

### Packs 4..1023 — Application Data

User-defined. Schema stored in Pack 0, Card N (matching pack ordinal).

---

## 5c. Security Rules

### Field Redaction (FPGA-enforced)

```
 When serving any GET on Pack 1 (users):
   Field ordinal 1 (pswdhash): ALWAYS stripped from response
   Field ordinal 2 (salt):     ALWAYS stripped from response
   Even for admin. Even for own card. These are WRITE-ONLY from HTTP.
   Only the FPGA's internal auth FSM reads these fields.
```

FPGA cost: ~20 LUTs (two ordinal compares + pack check in response builder).

### Row-Level Security on Pack 1 (users)

```
 Pack 0 (metadata):  Public read. Admin write.
 Pack 1 (users):     RLS enforced:
   Request to GET/PUT/DELETE /0/1/{card}:
     Admin (acl contains 0xFFFF)           → ALLOW any card
     Normal user, card == session.user_card → ALLOW own card only
     Normal user, card != session.user_card → 403 FORBIDDEN
   FPGA internal (login auth flow)          → full access (no HTTP)
 Pack 2 (sysconfig): Admin only.
 Pack 3 (sessions):  FPGA internal only. Never exposed via HTTP.
 Packs 4+:           Per ACL lists in user card.
```

FPGA cost: ~30 LUTs (one additional compare: requested card vs session user_card).

### Password Change (dedicated endpoint)

```
 POST /0/1/{card}/_passwd
 Body: [old_pass_len:u8][old_pass][new_pass_len:u8][new_pass]

 FPGA:
   1. Verify caller is card owner or admin
   2. Read current salt + pswdhash from SD (internal, not via HTTP)
   3. SipHash(old_pass + salt) → compare with stored hash
   4. If mismatch → 401
   5. Generate new random salt (hardware counter based)
   6. SipHash(new_pass + new_salt) → new hash
   7. Delta merge: write ord 1 (pswdhash) + ord 2 (salt) to SD
   8. Response: 200 OK
```

Client never handles hashes. FPGA does all crypto internally.
FPGA cost: ~60 LUTs (endpoint routing + old password verify).

### Login Response

```
 POST /login
 Request body:  [username_len:u8][username bytes][password_len:u8][password bytes]
 Response body: [user_card:u32 LE]
 Response header: Set-Cookie: sid=<64 hex chars>; Path=/; HttpOnly

 Client then:
   1. Store user_card ordinal locally
   2. GET /0/1/{user_card} → own profile (pswdhash/salt redacted)
   3. Read ACL lists from own profile → build navigation
   4. GET /0/0/{pack} for each readable pack → schema + name
```

User-defined. Schema stored in Pack 0, Card N (matching pack ordinal).

---

## 5b. Metadata Card Format (Pack 0)

Each card in Pack 0 describes one pack. Card ordinal = pack ordinal it describes.

```
 Pack 0 / Card 0 = schema for Pack 0 (metadata — self-describing)
 Pack 0 / Card 1 = schema for Pack 1 (users)
 Pack 0 / Card 2 = schema for Pack 2 (sysconfig)
 Pack 0 / Card N = schema for Pack N
```

#### Metadata Card Field Layout

```
 ord │ len    │ field            │ type
 ────┼────────┼──────────────────┼─────────────────────────
  0  │ ≤32   │ pack_name        │ ascii (length-prefixed)
  1  │ 1      │ field_count      │ uint8
  2  │ ≤96   │ field_defs       │ field_def[] (3 bytes × max 32)
  3  │ 1      │ flags            │ bit0=system, bit1=readonly, bit2=hidden
  4  │ ≤32   │ description      │ ascii (length-prefixed)
  5  │ ≤255  │ field_names      │ null-separated ASCII strings
 ────┴────────┴──────────────────┴─────────────────────────
 Max total: ~301 bytes per metadata card
```

#### Field Definition Format (inside field_defs, ordinal 2)

```
 3 bytes per field definition:
 ┌──────────┬──────────┬──────────┐
 │ ord: u5  │ type: u8 │maxlen:u8 │
 │+ 3 flags │          │          │
 └──────────┴──────────┴──────────┘
```

#### Field Type Codes

```
 FIXED LENGTH (size known from type, no prefix):
 0x01  uint8     1 byte
 0x02  uint16    2 bytes LE
 0x03  uint32    4 bytes LE
 0x04  int8      1 byte
 0x05  int16     2 bytes LE
 0x06  int32     4 bytes LE
 0x07  bool      1 byte (0x00=false, 0x01=true)
 0x0A  date      4 bytes: YYYY(u16) MM(u8) DD(u8)
 0x0B  time      3 bytes: HH(u8) MM(u8) SS(u8)
 0x0C  datetime  7 bytes: date + time
 0x0D  ipv4      4 bytes
 0x0E  mac       6 bytes
 0x0F  enum      1 byte (index)

 LENGTH-PREFIXED (first byte = data length, then data):
 0x08  ascii     [len:u8][data] 7-bit ASCII string
 0x09  utf8      [len:u8][data] UTF-8 string
 0x10  array_u16 [len:u8][data] array of uint16 (len/2 entries)
 0x11  blob      [len:u8][data] raw byte array
```

#### Concrete Example — Pack 1 (Users) Metadata

```
 Pack 0 / Card 1:

 field 0 (pack_name):    [0x05] "users"
 field 1 (field_count):  8
 field 2 (field_defs):
   [0, 0x08, 32]    ord=0, type=ascii,     maxlen=32   (username)
   [1, 0x11, 32]    ord=1, type=blob,      maxlen=32   (pswdhash)
   [2, 0x11, 16]    ord=2, type=blob,      maxlen=16   (salt)
   [3, 0x01,  1]    ord=3, type=uint8,     maxlen=1    (flags)
   [4, 0x01,  1]    ord=4, type=uint8,     maxlen=1    (failedattempts)
   [5, 0x10, 32]    ord=5, type=array_u16, maxlen=32   (read_packs)
   [6, 0x10, 32]    ord=6, type=array_u16, maxlen=32   (write_packs)
   [7, 0x10, 32]    ord=7, type=array_u16, maxlen=32   (delete_packs)
 field 3 (flags):        0x01 (system pack)
 field 4 (description):  [0x16] "User accounts and RBAC"
 field 5 (field_names):  "username\0pswdhash\0salt\0flags\0failedattempts\0read_packs\0write_packs\0delete_packs\0"
```

#### UI Auto-Rendering from Metadata

The browser reads metadata from `/0/0/{pack}` and data from `/0/{pack}/{card}`:

```
 1. Fetch schema: GET /0/0/1  → field names, types, maxlens
 2. Fetch data:   GET /0/1/0  → admin user card
 3. For each field_def:
      type → input widget (text, number, checkbox, date, password)
      maxlen → input maxlength
      name → form label
      blob/pswdhash → render as hidden/password
 4. PUT /0/1/0 with delta fields on Save
```

No FPGA cost — metadata is just data on SD, consumed by the JS client.

---

## 6. HTTP REST API

### Endpoints

| Method | URL | Auth | Action |
|--------|-----|------|--------|
| GET | `/` | None | Serve KnockoutJS SPA from BRAM |
| GET | `/status` | None | Live hardware counters (JSON) |
| **POST** | **`/login`** | **None (credentials in body)** | **Validate + create session + Set-Cookie + return user_card:u32** |
| **POST** | **`/logout`** | **Cookie** | **Expire session + clear cookie** |
| GET | `/0/{pack}/{card}` | Cookie or Basic | Read single card (Pack 1: redacts ord 1,2) |
| GET | `/0/{pack}?start=N&limit=N` | Cookie or Basic | List all valid cards in pack (paginated) |
| POST | `/0/{pack}/_mget` | Cookie or Basic | Multi-get specific cards by ordinal list |
| PUT | `/0/{pack}/{card}` | Cookie or Basic | Write/merge card (body = delta fields) |
| DELETE | `/0/{pack}/{card}` | Cookie or Basic | Delete card |
| **POST** | **`/0/1/{card}/_passwd`** | **Cookie** | **Change password (FPGA hashes internally)** |

### Authentication

Two auth methods supported (cookie checked first, then Basic):

1. **Cookie auth (browser/SPA):** `Cookie: sid=<64 hex chars>` — session token lookup in Pack 3
2. **Basic auth (API/CLI):** `Authorization: Basic base64(username:password)` — direct Pack 1 lookup

### Auth Flow

```
 1. Parse Authorization header
 2. Base64 decode → username + password
 3. Sequential scan Pack 1 cards:
      compare username field
 4. On match:
      a. Check flags bit0 (disabled → 403)
      b. Read salt
      c. SipHash-2-4(password + salt)
      d. Compare with stored pswdhash
 5. On hash match:
      a. Reset failedattempts to 0 (delta merge)
      b. Read ACL list for HTTP method
      c. Sequential scan for pack ordinal or 0xFFFF wildcard
      d. Match → 200 (proceed) / No match → 403
 6. On hash mismatch:
      a. Increment failedattempts (delta merge)
      b. If failedattempts >= 5 → set flags bit0 = 1 (auto-lockout)
      c. 401 Unauthorised
```

---

## 7. KV Operations (Jump Table)

```
 Op byte (from HTTP method + URL pattern) → direct dispatch:

 GET  /0/{pack}/{card}          → KV_READ:    SD read → HTTP response body
 GET  /0/{pack}?start=N&limit=N → KV_LIST:    sequential scan → stream results
 POST /0/{pack}/_mget           → KV_MGET:    multi-read by ordinal list
 PUT  /0/{pack}/{card}          → KV_WRITE:   SD read old → merge deltas → SD write new
 DELETE /0/{pack}/{card}        → KV_DELETE:   SD write zeroed magic bytes → HTTP 204
```

### KV_LIST — Paginated Pack Scan

```
 GET /0/{pack}?start=0&limit=100

 1. start_key = (pack << 22) | start_card
 2. count = 0
 3. For card = start_card to start_card + scan_range:
      SD read 2 bytes at (key × 4) → magic check
      if magic == 0xCA7D:
        SD read full 2KB → stream to W5500 TX:
          [card_ordinal:u32][payload_len:u16][payload]
        count++
        if count >= limit → stop
 4. Send end sentinel [0xFFFFFFFF]
 5. HTTP 200 with streamed body
```

### KV_MGET — Multi-Get by Ordinal List

```
 POST /0/{pack}/_mget
 Body: [card_ordinal:u32][card_ordinal:u32]...

 1. Parse body as list of u32 card ordinals
 2. For each ordinal:
      key = (pack << 22) | card_ordinal
      SD read block at key × 4
      if magic == 0xCA7D:
        stream: [card_ordinal:u32][payload_len:u16][payload]
      else:
        skip (card not found, not an error)
 3. Send end sentinel [0xFFFFFFFF]
 4. HTTP 200 with streamed body
```

### KV_WRITE — Delta Merge

```
 1. Compute SD block address: key × 4
 2. SD multi-block read (CMD18) → OLD_BUF (2KB BRAM)
 3. Load incoming delta ordinals into 32-bit bitmap register
 4. Scan OLD_BUF field by field:
      if bitmap[ordinal] set → copy delta version to NEW_BUF
      else → copy old field to NEW_BUF
 5. Append any delta fields with new ordinals
 6. Increment version
 7. SD multi-block write (CMD25) ← NEW_BUF
 8. HTTP 200 response
```

### KV_READ

```
 1. Compute SD block address: key × 4
 2. SD multi-block read → buffer
 3. Check magic (0xCA7D) → if not, HTTP 404
 4. Build HTTP response with card payload
```

### KV_DELETE

```
 1. Compute SD block address: key × 4
 2. SD write: zero magic bytes (0x0000) at block offset 0
 3. HTTP 204 No Content
```

---

## 8. First Boot Sequence

```
 POWER ON
    │
    ▼
 PLL lock → system clock stable
    │
    ▼
 W5500 init (MAC address, IP config, open sockets)
    │
    ▼
 SD card init (CMD0 → CMD8 → ACMD41 → CMD58)
    │
    ▼
 Read Pack 1 / Card 0 (admin user)
    SD block = (1 << 22) × 4 = block 16,777,216
    │
    ├── magic == 0xCA7D → Normal boot, admin exists
    │
    └── magic ≠ 0xCA7D → FIRST BOOT
         │
         ▼
       Write factory default admin card from BRAM template:
         username:       [0x05] "admin"
         pswdhash:       [0x20] SipHash("admin" + zero_salt)
         salt:           [0x10] 0x00 × 16
         flags:          0x00 (enabled)
         failedattempts: 0x00
         read_packs:     [0x02] [0xFF, 0xFF]  (0xFFFF = all)
         write_packs:    [0x02] [0xFF, 0xFF]  (0xFFFF = all)
         delete_packs:   [0x02] [0xFF, 0xFF]  (0xFFFF = all)
         │
         ▼
       Write factory default sysconfig (Pack 2 / Card 0):
         hostname:  "picowal"
         ip:        192.168.1.100
         subnet:    255.255.255.0
         gateway:   192.168.1.1
         │
         ▼
       Normal boot continues
```

---

## 9. Web UI (KnockoutJS SPA)

Two files served from the appliance (or CDN). Bootstrap + KnockoutJS loaded from CDN.

### Files

| File | Size | Served from | Purpose |
|------|------|-------------|---------|
| `index.html` | ~3KB | BRAM (6 RAM4K) or CDN | SPA shell: login modal, card browser, field editor |
| `picowal.js` | ~4KB | BRAM (8 RAM4K) or CDN | Binary codec, KO binding, all API calls |
| Bootstrap 5 | ~25KB | CDN | CSS styling |
| KnockoutJS 3 | ~25KB | CDN | MVVM data binding |

### Features

- **Modal login form** → POST /login → session cookie set by FPGA
- **Auto 401 intercept** → session expired → redirect to login
- **Pack/Card browser** → LIST with pagination, click to open
- **Field editor** → auto-generated from metadata schema (Pack 0)
- **Dirty tracking** → per-field, highlighted with border
- **Save** → only dirty fields sent as binary delta PUT
- **New card** → loads schema, marks all fields dirty
- **Delete** → with confirm dialog

### Data Flow (zero JSON)

```
 Load card:
   1. GET /0/0/{pack}  → binary metadata → parseCard() → field defs + names
   2. GET /0/{pack}/{card} → binary card → parseCard() → decode values by type
   3. Build KO observables with dirty tracking per field
   4. KO auto-renders form: input type from field type, label from name

 Save card:
   1. Filter dirty props → parseFromInput() per type
   2. encodeValue() per type → binary field bytes
   3. buildDelta() → sort by ordinal → 2KB binary buffer
   4. PUT /0/{pack}/{card} with raw ArrayBuffer body
```

```html
<!DOCTYPE html><html><head>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5/dist/css/bootstrap.min.css" rel="stylesheet">
<script src="https://cdn.jsdelivr.net/npm/knockout@3/build/output/knockout-latest.js"></script>
</head><body class="bg-dark text-light p-3">
<div id="app">
<!-- Login -->
<div data-bind="visible:!loggedIn()">
 <h3>PicoWAL Login</h3>
 <div class="mb-2" style="max-width:300px">
  <input class="form-control mb-1" placeholder="Username" data-bind="value:user">
  <input class="form-control mb-1" type="password" placeholder="Password" data-bind="value:pass">
  <button class="btn btn-primary w-100" data-bind="click:login">Login</button>
  <div class="text-danger mt-1" data-bind="text:err"></div>
 </div>
</div>
<!-- Main -->
<div data-bind="visible:loggedIn">
 <h3>PicoWAL <small data-bind="text:user"></small>
  <button class="btn btn-sm btn-outline-light" data-bind="click:logout">Logout</button></h3>
 <div id="s" class="alert alert-info" data-bind="text:status"></div>
 <div class="input-group mb-2" style="max-width:500px">
  <input class="form-control" placeholder="pack" data-bind="value:pack">
  <input class="form-control" placeholder="card" data-bind="value:card">
  <button class="btn btn-primary" data-bind="click:get">GET</button>
  <button class="btn btn-warning" data-bind="click:list">LIST</button>
  <button class="btn btn-danger" data-bind="click:del">DEL</button>
 </div>
 <table class="table table-dark table-sm" data-bind="visible:fields().length">
  <thead><tr><th>Ord</th><th>Field</th><th>Value</th></tr></thead>
  <tbody data-bind="foreach:fields">
   <tr><td data-bind="text:ord"></td><td data-bind="text:name"></td>
   <td><input class="form-control form-control-sm bg-dark text-light"
    data-bind="value:val"></td></tr>
  </tbody>
 </table>
 <button class="btn btn-success" data-bind="visible:fields().length,click:save">Save</button>
 <h5 class="mt-3" data-bind="visible:items().length">Results</h5>
 <table class="table table-dark table-sm" data-bind="visible:items().length">
  <thead><tr><th>Card</th><th>Fields</th></tr></thead>
  <tbody data-bind="foreach:items">
   <tr><td data-bind="text:id"></td><td data-bind="text:summary"></td></tr>
  </tbody>
 </table>
</div>
</div>
<script>
function VM(){
 var s=this;
 s.user=ko.observable('');s.pass=ko.observable('');
 s.loggedIn=ko.observable(false);s.err=ko.observable('');
 s.status=ko.observable('');s.pack=ko.observable('');
 s.card=ko.observable('');s.fields=ko.observableArray();
 s.items=ko.observableArray();
 s.api=function(m,u,b){return fetch(u,{method:m,body:b,
  credentials:'same-origin'}).then(function(r){
  if(r.status==401){s.loggedIn(false);s.err('Session expired');}
  return r;})};
 s.login=function(){s.api('POST','/login',
  JSON.stringify({u:s.user(),p:s.pass()}))
  .then(function(r){if(r.ok){s.loggedIn(true);s.err('');
  s.api('GET','/status').then(function(r){return r.text()})
  .then(function(t){s.status(t)})}
  else s.err('Login failed')})};
 s.logout=function(){s.api('POST','/logout').then(function(){
  s.loggedIn(false);s.fields([]);s.items([])})};
 s.get=function(){s.api('GET','/0/'+s.pack()+'/'+s.card())
  .then(function(r){return r.json()}).then(function(d){
  s.fields(d.fields||[]);s.items([])})};
 s.list=function(){s.api('GET','/0/'+s.pack()+'?start=0&limit=50')
  .then(function(r){return r.json()}).then(function(d){
  s.items(d.items||[]);s.fields([])})};
 s.del=function(){s.api('DELETE','/0/'+s.pack()+'/'+s.card())
  .then(function(){s.fields([]);s.get()})};
 s.save=function(){var b=s.fields().map(function(f){
  return{ord:f.ord,val:f.val}});
  s.api('PUT','/0/'+s.pack()+'/'+s.card(),JSON.stringify(b))}
}
ko.applyBindings(new VM());
</script></body></html>
```

---

## 10. FPGA Module Hierarchy

```
 top.v
 ├── pll.v                    PLL: 100MHz → system clock
 ├── reset_sync.v             Reset synchroniser
 │
 ├── w5500_spi.v              SB_SPI hard IP wrapper for W5500
 │   └── (uses iCE40 SB_SPI primitive — zero LUT SPI engine)
 │
 ├── w5500_ctrl.v             W5500 init, socket management
 │
 ├── http_parser.v            HTTP request decode FSM
 │   ├── method_parse.v       GET/PUT/DELETE/POST detection
 │   ├── url_parse.v          Pack/Card ordinal extraction + /_mget + /login + /logout
 │   ├── query_parse.v        ?start=N&limit=N decoder
 │   ├── header_scan.v        Header field matching (Auth + Cookie + Content-Length)
 │   ├── base64_decode.v      Authorization payload decode
 │   └── cookie_parse.v       Cookie: sid= extract + hex decode
 │
 ├── auth.v                   Authentication + RBAC + Sessions
 │   ├── siphash.v            SipHash-2-4 core (password hash + token gen)
 │   ├── user_scan.v          Pack 1 sequential card scan
 │   ├── session_scan.v       Pack 3 session token lookup
 │   ├── session_create.v     Generate token + write Pack 3 card
 │   └── rbac_check.v         ACL list scan + wildcard
 │
 ├── kv_engine.v              Jump table + operation dispatch
 │   ├── kv_read.v            SD read → response
 │   ├── kv_list.v            Sequential pack scan → streamed response
 │   ├── kv_mget.v            Multi-key read → streamed response
 │   ├── kv_write.v           SD read → merge → SD write
 │   │   └── merge_engine.v   Field-level delta merge
 │   └── kv_delete.v          Tombstone write
 │
 ├── sd_spi.v                 SD card SPI controller
 │   ├── sd_init.v            Power-up + CMD0/CMD8/ACMD41/CMD58
 │   ├── sd_read.v            CMD17/CMD18 read path
 │   ├── sd_write.v           CMD24/CMD25 write path
 │   └── crc.v                CRC7 (commands) + CRC16 (data)
 │
 ├── response_builder.v       HTTP response assembly
 ├── ui_rom.v                 Static HTML page (BRAM)
 ├── status_counters.v        Hardware counter registers
 └── first_boot.v             Default card templates (BRAM)
```

---

## 11. Build Toolchain

```
 Source:    Verilog
 Synth:    Yosys (open source)
 PnR:      nextpnr-ice40 (open source)
 Pack:     icepack
 Program:  Alchitry Loader or iceprog

 Build:    make → .bin bitstream
 Flash:    make program
```

---

## 12. Pin Constraints (Alchitry Cu .pcf)

```
 # W5500 SPI
 set_io w5500_cs    <pin>
 set_io w5500_sck   <pin>
 set_io w5500_mosi  <pin>
 set_io w5500_miso  <pin>
 set_io w5500_int   <pin>
 set_io w5500_rst   <pin>

 # SD Card SPI
 set_io sd_cs       <pin>
 set_io sd_sck      <pin>
 set_io sd_mosi     <pin>
 set_io sd_miso     <pin>

 # System
 set_io clk_100mhz  P7     # Alchitry Cu 100MHz oscillator
 set_io rst_n       P8     # Alchitry Cu reset button
```

Pin assignments to be finalised based on Alchitry Br breakout bank wiring.

---

## 13. Future Expansion (spare resources)

With 53% LUTs and 66% BRAM spare:

| Feature | LUTs | BRAM |
|---------|------|------|
| Second SD card (RAID-1 mirror) | ~200 | 0 |
| Hardware CRC32 per record | ~100 | 0 |
| Write-back cache (hot cards in BRAM) | ~300 | 8 RAM4K |
| Bloom filter (fast key existence) | ~200 | 4 RAM4K |
| Hardware AES-128 encryption at rest | ~1,500 | 2 RAM4K |
| UDP socket support (fast path) | ~200 | 1 RAM4K |
| Wi-Fi bridge (Pico 2W parallel port) | ~500 | 2 RAM4K |
| Additional W5500 (second Ethernet) | ~350 | 1 RAM4K |

---

## 14. Future Target: ECP5-5G (GbE on-die)

The design is portable to Lattice ECP5-5G-25 for a fully on-die GbE appliance:

```
 RJ45 ──► 88E1512 ──SGMII──► ECP5-5G-25 ──QSPI──► W25Q256 (32MB)
          GbE PHY              24K LUTs              on-board flash
```

| Resource | ECP5-5G-25 | Usage |
|----------|-----------|-------|
| LUTs | 24,000 | ~8,620 (36%) — full TCP/IP/HTTP on die |
| BRAM | 126KB | KV index + packet buffers |
| SERDES | 2× 3.125Gbps | SGMII for native GbE |
| External chips | GbE PHY + QSPI flash only | No W5500, no SD card |
| Dev board | Lattice Versa ECP5 (~£100) — has GbE PHY + RJ45 on board |
| Custom BOM | ~£20 |

Same Yosys + nextpnr open-source toolchain. Same Verilog codebase — just add MAC/IP/TCP modules and retarget.
