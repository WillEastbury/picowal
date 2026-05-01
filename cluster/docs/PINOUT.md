# PicoCluster — Pinout Diagrams (Star/Snowflake Topology)

PIO on the head nodes acts as a **switching fabric**. Workers connect
point-to-point to their head with just 2 wires. No relay, no forwarding
on worker nodes.

## Architecture Overview

```
                 ┌─────────────────────┐
    Ethernet ────┤      HEAD 1         ├──── Interlink ────┐
    (W5500)      │  PIO Switch Fabric  │                   │
                 │  6 ports × 20 Mbps  │                   │
                 └─┬──┬──┬──┬──┬──┬───┘                   │
                   │  │  │  │  │  │                        │
                   ▼  ▼  ▼  ▼  ▼  ▼                       │
                  W1 W2 W3 S1 W4 W5                        │
                                                           │
                 ┌─────────────────────┐                   │
    Ethernet ────┤      HEAD 2         ├───────────────────┘
    (W5500)      │  PIO Switch Fabric  │
                 │  6 ports × 20 Mbps  │
                 └─┬──┬──┬──┬──┬──┬───┘
                   │  │  │  │  │  │
                   ▼  ▼  ▼  ▼  ▼  ▼
                  W6 W7 W8 S2 S3 W9
```

## Pin Map Summary

| GPIO | Worker | Storage | Head |
|------|--------|---------|------|
| 0 | Link TX | Link TX | Port 0 TX |
| 1 | Link RX | Link RX | Port 0 RX |
| 2 | — | — | Port 1 TX |
| 3 | — | — | Port 1 RX |
| 4 | — | — | Port 2 TX |
| 5 | — | — | Port 2 RX |
| 6 | — | — | Port 3 TX |
| 7 | — | — | Port 3 RX |
| 8 | — | — | Port 4 TX |
| 9 | — | — | Port 4 RX |
| 10 | — | — | Port 5 TX |
| 11 | — | — | Port 5 RX |
| 12 | — | SD MISO | — |
| 13 | — | SD CS | — |
| 14 | — | SD SCK | — |
| 15 | — | SD MOSI | — |
| 16 | — | — | W5500 MISO |
| 17 | — | — | W5500 CS |
| 18 | — | — | W5500 SCK |
| 19 | — | — | W5500 MOSI |
| 20 | — | — | W5500 RST |
| 21 | — | — | W5500 INT |
| 26 | — | — | Interlink TX |
| 27 | — | — | Interlink RX |

---

## 1. Worker Node (Pico2 — 2 wires only!)

```
                    ┌──────────────────────┐
                    │     Pico2 (RP2350)   │
                    │                      │
       Link TX  ───┤ GP0            GP28  ├─
       Link RX  ───┤ GP1            GP27  ├─
                    │ GP2            GP26  ├─
                    │ GP3            RUN   ├─
                    │ GP4            GP22  ├─
                    │ GP5            GP21  ├─
                    │ GP6            GP20  ├─
                    │ GP7            GP19  ├─
                    ├──────────────────────┤
               GND ─┤ GND            GP18  ├─
                    │                GP17  ├─
                    │                GP16  ├─
                    │                GP15  ├─
                    │                GP14  ├─
                    │                GP13  ├─
                    │                GP12  ├─
                    │                GP11  ├─
                    │                GP10  ├─
                    │                GP9   ├─
                    │                GP8   ├─
               3V3 ─┤ 3V3            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V
                    └──────────────────────┘

    Wiring: GP0 → head port TX, GP1 ← head port RX, GND
    Power:  USB from hub
    PIO:    Only 2 SMs used (TX + RX), 10 SMs free!
    GPIOs:  Only 2 used, 26 free for future expansion
```

**The simplest node. Two wires + ground + USB power.**

---

## 2. Storage Node (Pico2 + SD Card — 6 wires)

```
                    ┌──────────────────────┐
                    │     Pico2 (RP2350)   │
                    │                      │
       Link TX  ───┤ GP0            GP28  ├─
       Link RX  ───┤ GP1            GP27  ├─
                    │ GP2            GP26  ├─
                    │ GP3            RUN   ├─
                    │ GP4            GP22  ├─
                    │ GP5            GP21  ├─
                    │ GP6            GP20  ├─
                    │ GP7            GP19  ├─
                    ├──────────────────────┤
               GND ─┤ GND            GP18  ├─
                    │                GP17  ├─
                    │                GP16  ├─
        SD MOSI ───┤ GP15           GP11  ├─
        SD SCK  ───┤ GP14           GP10  ├─
        SD CS   ───┤ GP13           GP9   ├─
        SD MISO ───┤ GP12           GP8   ├─
               3V3 ─┤ 3V3            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V
                    └──────────────────────┘

    Wiring: 2 link wires + 4 SD SPI wires + GND
    Power:  USB from hub
    SPI1:   SD card @ 25 MHz
```

---

## 3. Head Node (Pico2W + W5500 — central switch)

```
                    ┌──────────────────────────────────┐
                    │        Pico2W (RP2350)           │
                    │     PIO SWITCHING FABRIC         │
                    │                                  │
     Port 0 TX  ───┤ GP0                      GP28  ├─
     Port 0 RX  ───┤ GP1                      GP27  ├─ Interlink RX
     Port 1 TX  ───┤ GP2                      GP26  ├─ Interlink TX
     Port 1 RX  ───┤ GP3                      RUN   ├─
     Port 2 TX  ───┤ GP4                      GP22  ├─
     Port 2 RX  ───┤ GP5                  ┌── GP21  ├─ W5500 INT
     Port 3 TX  ───┤ GP6                  │   GP20  ├─ W5500 RST
     Port 3 RX  ───┤ GP7                  │   GP19  ├─ W5500 MOSI
     Port 4 TX  ───┤ GP8                  │   GP18  ├─ W5500 SCK
     Port 4 RX  ───┤ GP9                  │   GP17  ├─ W5500 CS
     Port 5 TX  ───┤ GP10                 └── GP16  ├─ W5500 MISO
     Port 5 RX  ───┤ GP11                     GP15  ├─
                    ├──────────────────────────────────┤
               GND ─┤ GND                      GP14  ├─
               3V3 ─┤ 3V3                      GP13  ├─
              VBUS ─┤ VBUS                     GP12  ├─
                    └──────────────────────────────────┘

    PIO allocation (12 SMs total):
      PIO0: SM0=Port0 TX, SM1=Port0 RX, SM2=Port1 TX, SM3=Port1 RX
      PIO1: SM0=Port2 TX, SM1=Port2 RX, SM2=Port3 TX, SM3=Port3 RX
      PIO2: SM0=Port4 TX, SM1=Port4 RX, SM2=Port5 TX, SM3=Port5 RX

    SPI0:  W5500 Ethernet @ 40 MHz (GP16-GP21)
    WiFi:  CYW43 onboard (backup/OTA)
```

**All 12 PIO state machines used as a 6-port crossbar switch.**

---

## Wiring Diagram (Physical)

```
    USB Hub (20-port, powered)
    ════════════════════════════════════════════════════
    │ │ │ │ │ │ │ │ │ │ │ │ │ │
    ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼

    HEAD 1 ─────────── Interlink ──────────── HEAD 2
    ╔══╗                                      ╔══╗
    ║  ║─── 2 wires ──→ Worker 1             ║  ║─── 2 wires ──→ Worker 6
    ║  ║─── 2 wires ──→ Worker 2             ║  ║─── 2 wires ──→ Worker 7
    ║  ║─── 2 wires ──→ Worker 3             ║  ║─── 2 wires ──→ Worker 8
    ║  ║─── 2 wires ──→ Storage 1            ║  ║─── 2 wires ──→ Storage 2
    ║  ║─── 2 wires ──→ Worker 4             ║  ║─── 2 wires ──→ Storage 3
    ║  ║─── 2 wires ──→ Worker 5             ║  ║─── 2 wires ──→ Worker 9
    ╚══╝                                      ╚══╝
     │                                          │
     └── Ethernet (W5500) ──→ Switch ←── Ethernet (W5500) ──┘

    Total wires per worker: 2 signal + 1 GND = 3 wires
    Total wires per head:   12 signal + 2 interlink + 6 SPI + GND
```

---

## Comparison: Star vs Ring

| Property | Star (current) | Ring (old) |
|----------|----------------|------------|
| Worker GPIOs | 2 | 8 |
| Worker PIO SMs | 2 | 8 |
| Worker firmware | Simple (no relay) | Complex (relay+snoop) |
| Latency to any node | O(1) — 1 hop | O(N) — up to N hops |
| Head PIO SMs | 12 (all used) | 8 |
| Max nodes per head | 6 | Unlimited (ring) |
| Total cluster max | 12 workers + 2 heads | Unlimited |
| Single point of failure | Head (mitigated by 2) | None |
| Wiring complexity | Simple (star cables) | Complex (daisy-chain) |

---

## Signal Specifications

| Parameter | Value |
|-----------|-------|
| Encoding | Manchester (IEEE 802.3) |
| Bit rate | 20 Mbps per link |
| Symbol rate | 10 MHz |
| Logic levels | 3.3V CMOS |
| W5500 SPI | 40 MHz |
| SD SPI | 25 MHz |
| Max wire length | ~30 cm (star cables shorter than ring) |
| System clock | 450 MHz (overclock) |

---

## Bill of Materials

| Component | Worker | Storage | Head |
|-----------|--------|---------|------|
| Pico2 | 1 | 1 | — |
| Pico2W | — | — | 1 |
| W5500 module | — | — | 1 |
| SD reader | — | 1 | — |
| Signal wires | 2 | 2 | 14 (12 ports + 2 interlink) |
| USB cable | 1 | 1 | 1 |
