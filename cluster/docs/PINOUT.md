# PicoCluster — Node Pinout Diagrams

All nodes use a **single unified firmware image**. Hardware detection at boot
determines role. Pins are physically wired differently per node type.

## Pin Map Summary

| GPIO | Worker | Head (W5500) | Storage (SD) |
|------|--------|--------------|--------------|
| 0 | Ring 0 TX | Ring 0 TX | Ring 0 TX |
| 1 | Ring 0 RX | Ring 0 RX | Ring 0 RX |
| 2 | Ring 1 TX | Ring 1 TX | Ring 1 TX |
| 3 | Ring 1 RX | Ring 1 RX | Ring 1 RX |
| 4 | Ring 2 TX | Ring 2 TX | Ring 2 TX |
| 5 | Ring 2 RX | Ring 2 RX | Ring 2 RX |
| 6 | Ring 3 TX | Ring 3 TX | Ring 3 TX |
| 7 | Ring 3 RX | Ring 3 RX | Ring 3 RX |
| 8 | — | — | — |
| 9 | — | — | — |
| 10 | — | — | — |
| 11 | — | — | — |
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
| 22-28 | — | — | — |

---

## 1. Worker Node (Pico2 — bare compute)

```
                    ┌──────────────────────┐
                    │     Pico2 (RP2350)   │
                    │                      │
          Ring0 TX ─┤ GP0            GP28  ├─ (ADC2)
          Ring0 RX ─┤ GP1            GP27  ├─ (ADC1)
          Ring1 TX ─┤ GP2            GP26  ├─ (ADC0)
          Ring1 RX ─┤ GP3            RUN   ├─
          Ring2 TX ─┤ GP4            GP22  ├─
          Ring2 RX ─┤ GP5            GP21  ├─
          Ring3 TX ─┤ GP6            GP20  ├─
          Ring3 RX ─┤ GP7            GP19  ├─
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
              VBUS ─┤ VBUS           VSYS  ├─ 5V in
                    └──────────────────────┘

    Wiring: 8 signal wires (GP0-GP7) + GND to ring bus
    Power:  USB from hub (5V via VBUS)
    Notes:  GP8-GP28 unused, leave floating or pull down
```

**Connections:**
- GP0 → next node's GP1 (Ring 0: Express 1)
- GP2 → next node's GP3 (Ring 1: Express 2)
- GP4 → next node's GP5 (Ring 2: Normal)
- GP6 → next node's GP7 (Ring 3: Storage)
- GND → shared ground bus

---

## 2. Head Node (Pico2W + W5500 Ethernet)

```
                    ┌──────────────────────┐
                    │   Pico2W (RP2350)    │
                    │                      │
          Ring0 TX ─┤ GP0            GP28  ├─ (ADC2)
          Ring0 RX ─┤ GP1            GP27  ├─ (ADC1)
          Ring1 TX ─┤ GP2            GP26  ├─ (ADC0)
          Ring1 RX ─┤ GP3            RUN   ├─
          Ring2 TX ─┤ GP4            GP22  ├─
          Ring2 RX ─┤ GP5        ┌── GP21  ├─ W5500 INT
          Ring3 TX ─┤ GP6        │   GP20  ├─ W5500 RST
          Ring3 RX ─┤ GP7        │   GP19  ├─ W5500 MOSI (SPI0 TX)
                    ├───────────────────────┤
               GND ─┤ GND        │   GP18  ├─ W5500 SCK  (SPI0 SCK)
                    │            │   GP17  ├─ W5500 CS   (SPI0 CSn)
                    │            └── GP16  ├─ W5500 MISO (SPI0 RX)
                    │                GP15  ├─
                    │                GP14  ├─
                    │                GP13  ├─
                    │                GP12  ├─
                    │                GP11  ├─
                    │                GP10  ├─
                    │                GP9   ├─
                    │                GP8   ├─
               3V3 ─┤ 3V3            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V in
                    └──────────────────────┘

                    ┌──────────────────────┐
                    │   W5500 Module       │
                    ├──────────────────────┤
                    │ MISO ──────── GP16   │
                    │ MOSI ──────── GP19   │
                    │ SCK  ──────── GP18   │
                    │ CS   ──────── GP17   │
                    │ RST  ──────── GP20   │
                    │ INT  ──────── GP21   │
                    │ 3V3  ──────── 3V3    │
                    │ GND  ──────── GND    │
                    │ RJ45 ──────── LAN    │
                    └──────────────────────┘

    SPI0 @ 40 MHz — W5500 Ethernet controller
    Wiring: 8 ring wires + 6 SPI wires + power
    Power:  USB from hub (5V via VBUS)
    WiFi:   CYW43 onboard (Pico2W) — available as backup
```

**Connections:**
- GP0-GP7 → Ring bus (same as worker)
- GP16-GP21 → W5500 module (SPI0)
- Ethernet cable → network switch/router
- GND → shared ground bus

---

## 3. Storage Node (Pico2W + SD Card Reader)

```
                    ┌──────────────────────┐
                    │   Pico2W (RP2350)    │
                    │                      │
          Ring0 TX ─┤ GP0            GP28  ├─ (ADC2)
          Ring0 RX ─┤ GP1            GP27  ├─ (ADC1)
          Ring1 TX ─┤ GP2            GP26  ├─ (ADC0)
          Ring1 RX ─┤ GP3            RUN   ├─
          Ring2 TX ─┤ GP4            GP22  ├─
          Ring2 RX ─┤ GP5            GP21  ├─
          Ring3 TX ─┤ GP6            GP20  ├─
          Ring3 RX ─┤ GP7            GP19  ├─
                    ├──────────────────────┤
               GND ─┤ GND            GP18  ├─
                    │                GP17  ├─
                    │                GP16  ├─
                    │   SD MOSI ──── GP15  ├─ SD MOSI (SPI1 TX)
                    │   SD SCK  ──── GP14  ├─ SD SCK  (SPI1 SCK)
                    │   SD CS   ──── GP13  ├─ SD CS   (SPI1 CSn)
                    │   SD MISO ──── GP12  ├─ SD MISO (SPI1 RX)
                    │                GP11  ├─
                    │                GP10  ├─
                    │                GP9   ├─
                    │                GP8   ├─
               3V3 ─┤ 3V3            GND   ├─ GND
              VBUS ─┤ VBUS           VSYS  ├─ 5V in
                    └──────────────────────┘

                    ┌──────────────────────┐
                    │   SD Card Module     │
                    ├──────────────────────┤
                    │ MISO ──────── GP12   │
                    │ MOSI ──────── GP15   │
                    │ SCK  ──────── GP14   │
                    │ CS   ──────── GP13   │
                    │ 3V3  ──────── 3V3    │
                    │ GND  ──────── GND    │
                    └──────────────────────┘

    SPI1 @ 25 MHz — SD card (SDHC, SPI mode)
    Wiring: 8 ring wires + 4 SPI wires + power
    Power:  USB from hub (5V via VBUS)
    WiFi:   CYW43 onboard (Pico2W) — available for OTA/debug
```

**Connections:**
- GP0-GP7 → Ring bus (same as worker)
- GP12-GP15 → SD card module (SPI1)
- GND → shared ground bus

---

## Ring Bus Wiring (All Nodes)

The 4 rings form a **unidirectional chain**. Each node's TX connects to the
next node's RX on the same ring:

```
    ┌────────┐     ┌────────┐     ┌────────┐     ┌────────┐
    │  HEAD  │     │ NODE 1 │     │ NODE 2 │     │ NODE N │
    │        │     │        │     │        │     │        │
    │ GP0 TX─┼────►│GP1 RX  │     │        │     │        │
    │        │     │GP0 TX──┼────►│GP1 RX  │     │        │
    │        │     │        │     │GP0 TX──┼─···─►GP1 RX  │
    │ GP1 RX │◄────────────────────────────────────GP0 TX──┤  ← ring wraps back
    │        │     │        │     │        │     │        │
    └────────┘     └────────┘     └────────┘     └────────┘

    Ring 0 (Express 1):  GP0→GP1  (20 Mbps Manchester)
    Ring 1 (Express 2):  GP2→GP3  (20 Mbps Manchester)
    Ring 2 (Normal):     GP4→GP5  (20 Mbps Manchester)
    Ring 3 (Storage):    GP6→GP7  (20 Mbps Manchester)
```

**Each wire is a single GPIO-to-GPIO connection (3.3V logic, Manchester encoded).**

Last node in the chain wraps all TX outputs back to the head's RX inputs.

---

## Physical Layout Example (14 nodes)

```
    USB Hub (20-port, powered)
    ┌─────────────────────────────────────────────────┐
    │ ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●   │
    └─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬───┘
      │ │ │ │ │ │ │ │ │ │ │ │ │ │
      ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼ ▼
    ┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐┌─┐
    │H1││H2││S1││S2││S3││W1││W2││W3││W4││W5││W6││W7││W8││W9│
    └─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘└─┘
     ▲   │                                      │
     │   H1 = Head #1 (Pico2W + W5500, ingress) │
     │   H2 = Head #2 (Pico2W + W5500, egress)  │
     │   S1-S3 = Storage (Pico2 + SD reader)    │
     │   W1-W9 = Workers (Pico2, bare)          │
     │                                           │
     └──── Ring wires (GP0-GP7) daisy-chain ─────┘
```

---

## Bill of Materials (Per Node Type)

| Component | Worker | Head | Storage |
|-----------|--------|------|---------|
| Pico2 / Pico2W | Pico2 | Pico2W | Pico2 or Pico2W |
| W5500 module | — | 1 | — |
| SD card reader | — | — | 1 |
| Ring wires (30AWG) | 8 + GND | 8 + GND | 8 + GND |
| SPI wires | — | 6 | 4 |
| USB cable | 1 | 1 | 1 |

---

## Signal Specifications

| Parameter | Value |
|-----------|-------|
| Ring encoding | Manchester (IEEE 802.3) |
| Ring bit rate | 20 Mbps |
| Ring symbol rate | 10 MHz |
| Ring logic levels | 3.3V CMOS |
| W5500 SPI clock | 40 MHz |
| SD SPI clock | 25 MHz |
| Wire length (max) | ~20 cm recommended |
| System clock | 450 MHz (overclock) |
