# PicoCluster — Pinout Diagrams (Tree Topology, Half-Duplex)

Single-wire half-duplex Manchester links. Head fans out for compute,
storage fans in for data, cross-connects between storage nodes.

## Architecture

```
    ┌────────────────────────────────────────────────────┐
    │                    HEAD NODE                        │
    │  PIO switch: 12 half-duplex ports @ 50 Mbps each   │
    │  W5500: network ingress/egress                     │
    └─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬───────────────┘
      │  │  │  │  │  │  │  │  │  │  │  │   (1 wire each)
      ▼  ▼  ▼  ▼  ▼  ▼  ▼  ▼  ▼  ▼  ▼  ▼
     W1 W2 W3 W4 W5 W6 W7 W8 W9 W10 W11 W12
      │  │  │  │  │  │  │  │  │   │   │   │  (1 wire each)
      └──┴──┴──┘  └──┴──┴──┘  └───┴───┴───┘
          │            │              │
          ▼            ▼              ▼
    ┌─────────┐  ┌─────────┐  ┌─────────┐
    │STORAGE 1│──│STORAGE 2│──│STORAGE 3│   (cross-connects)
    │  SD+PIO │  │  SD+PIO │  │  SD+PIO │
    └─────────┘──└─────────┘──└─────────┘
```

**Data flow:**
- **Head → Worker**: "Execute card X with data Y" (fan-out, command path)
- **Worker → Storage**: "Give me card X" (fan-in, data path)
- **Storage → Worker**: "Here's the bytecode" (reply on same wire)
- **Worker → Head**: "Here's the result" (reply on same wire)
- **Storage ↔ Storage**: Cross-connect for card replication

## Pin Map

| GPIO | Worker (2 pins) | Head (12+6 pins) | Storage (6+4 pins) |
|------|-----------------|-------------------|---------------------|
| 0 | **Head link** | Port 0 (Worker 1) | Worker port 0 |
| 1 | **Storage link** | Port 1 (Worker 2) | Worker port 1 |
| 2 | — | Port 2 (Worker 3) | Worker port 2 |
| 3 | — | Port 3 (Worker 4) | Worker port 3 |
| 4 | — | Port 4 (Worker 5) | **Xconn peer A** |
| 5 | — | Port 5 (Worker 6) | **Xconn peer B** |
| 6 | — | Port 6 (Worker 7) | — |
| 7 | — | Port 7 (Worker 8) | — |
| 8 | — | Port 8 (Worker 9) | — |
| 9 | — | Port 9 (Worker 10) | — |
| 10 | — | Port 10 (Worker 11) | — |
| 11 | — | Port 11 (Worker 12) | — |
| 12 | — | — | SD MISO |
| 13 | — | — | SD CS |
| 14 | — | — | SD SCK |
| 15 | — | — | SD MOSI |
| 16 | — | W5500 MISO | — |
| 17 | — | W5500 CS | — |
| 18 | — | W5500 SCK | — |
| 19 | — | W5500 MOSI | — |
| 20 | — | W5500 RST | — |
| 21 | — | W5500 INT | — |

---

## 1. Worker Node (2 GPIOs — minimal!)

```
                    ┌──────────────────────┐
                    │     Pico2 (RP2350)   │
                    │                      │
    Head link   ───┤ GP0                   │
    Storage link───┤ GP1                   │
                    │ GP2-GP28: FREE       │
                    ├──────────────────────┤
               GND ─┤ GND            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V
                    └──────────────────────┘

    PIO:   2 SMs used (half-duplex), 10 SMs FREE
    GPIOs: 2 used, 26 FREE
    Wires: 2 signal + shared GND = 3 physical
```

---

## 2. Storage Node (6 GPIOs + 4 SPI)

```
                    ┌──────────────────────┐
                    │     Pico2 (RP2350)   │
                    │                      │
    Worker 0    ───┤ GP0            GP15  ├─── SD MOSI
    Worker 1    ───┤ GP1            GP14  ├─── SD SCK
    Worker 2    ───┤ GP2            GP13  ├─── SD CS
    Worker 3    ───┤ GP3            GP12  ├─── SD MISO
    Xconn A     ───┤ GP4                  │
    Xconn B     ───┤ GP5                  │
                    ├──────────────────────┤
               GND ─┤ GND            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V
                    └──────────────────────┘

    PIO:   6 SMs (4 worker + 2 xconn), 6 FREE
    SPI1:  SD card @ 25 MHz (hardware SPI, no PIO)
    Wires: 6 signal + 4 SPI + GND
```

---

## 3. Head Node (12 GPIOs + 6 SPI)

```
                    ┌──────────────────────────────────┐
                    │        Pico2W (RP2350)           │
                    │     12-PORT PIO SWITCH           │
                    │                                  │
    Worker 1  ─────┤ GP0                      GP21  ├── W5500 INT
    Worker 2  ─────┤ GP1                      GP20  ├── W5500 RST
    Worker 3  ─────┤ GP2                      GP19  ├── W5500 MOSI
    Worker 4  ─────┤ GP3                      GP18  ├── W5500 SCK
    Worker 5  ─────┤ GP4                      GP17  ├── W5500 CS
    Worker 6  ─────┤ GP5                      GP16  ├── W5500 MISO
    Worker 7  ─────┤ GP6                             │
    Worker 8  ─────┤ GP7                             │
    Worker 9  ─────┤ GP8                             │
    Worker 10 ─────┤ GP9                             │
    Worker 11 ─────┤ GP10                            │
    Worker 12 ─────┤ GP11                            │
                    ├──────────────────────────────────┤
               GND ─┤ GND                      GND   ├─ GND
              VBUS ─┤ VBUS                     VSYS  ├─ 5V
                    └──────────────────────────────────┘

    PIO: ALL 12 SMs used (3 blocks × 4 = 12 half-duplex ports)
    SPI0: W5500 Ethernet @ 40 MHz
    WiFi: CYW43 onboard (backup/management)
```

---

## Wire Count Summary

| Connection | Wires | Count | Total |
|-----------|-------|-------|-------|
| Head → each worker | 1 signal | ×12 | 12 |
| Worker → storage | 1 signal | ×12 | 12 |
| Storage ↔ storage | 1 signal each | ×3 pairs | 3 |
| **Total signal wires** | | | **27** |
| Shared GND bus | 1 | | 1 |
| **Grand total physical** | | | **28 wires** |

Each worker node: **3 physical connections** (head wire, storage wire, GND)

---

## Signal Specifications

| Parameter | Value |
|-----------|-------|
| Encoding | Manchester half-duplex |
| Bit rate | 50 Mbps per link |
| Protocol | Master-initiated (send → turnaround → reply) |
| Turnaround gap | 2 µs |
| Logic levels | 3.3V CMOS |
| System clock | 300 MHz |
| Max wire length | ~20 cm |
| Aggregate BW (head) | 12 × 50 = 600 Mbps |
| W5500 SPI throughput | ~40 Mbps (actual bottleneck) |
