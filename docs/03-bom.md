# Bill of Materials

Quantity-1 prices in USD. **Only the Seed3 price is verified** (daisy.audio, Sept 2026);
everything else is a good-faith estimate from typical Mouser/LCSC/AliExpress pricing and will
move. Re-cost before ordering.

## Core

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 1 | Daisy Seed3 (with headers) | — | $29.99 | $29.99 | **Verified price.** 2×20 pin headers pre-soldered; JTAG pads bare |
| 1 | 2×5 1.27 mm header, 10-pin | Amphenol 20021111-00010T4LF | $0.50 | $0.50 | JTAG pads ship bare; fit **centred** on the 14-position footprint |
| 1 | ST-Link V3 MINIE | — | $12.00 | $12.00 | One-time tool cost; reusable |
| 5 | Main PCB, 4-layer, ~180×100 mm | JLCPCB | $8.00 | $40.00 | 4-layer for the ground plane; qty 5 minimum |
| 2 | 20-pin female header (Seed3 socket) | — | $1.00 | $2.00 | Socket it — do not solder the module down |

## Controls

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 6 | 9 mm vertical pot, B10k | Alpha RD901F | $1.20 | $7.20 | Linear taper; log tapers are wrong for macro params |
| 6 | Knob, Davies 1900 style | — | $1.50 | $9.00 | |
| 2 | Rotary encoder w/ switch, 24 detent | Bourns PEC11R-4215F-S0024 | $2.50 | $5.00 | Direct to GPIO, not the shift register |
| 2 | Encoder knob | — | $1.50 | $3.00 | |
| 30 | MX-compatible switch, clear housing | Gateron KS-9 clear | $0.40 | $12.00 | Clear top so the LED reaches the cap |
| 30 | Hot-swap socket | Kailh MX | $0.10 | $3.00 | Worth it — changes switch feel without desoldering a legended panel |
| 30 | Translucent keycap, DSA/XDA blank | — | $0.50 | $15.00 | Legends go on the PCB silkscreen |

## LEDs and drivers

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 30 | RGB LED, reverse-mount | SK6812 MINI-E | $0.15 | $4.50 | Fits under an MX switch |
| 1 | Level shifter, 3.3 V → 5 V | 74AHCT125 | $0.50 | $0.50 | **Not optional** — see hardware §3.4 |
| 30 | 100 nF X7R 0805 | — | $0.02 | $0.60 | One per LED |
| 1 | 1000 µF 6.3 V electrolytic | — | $0.50 | $0.50 | LED rail bulk |

*Fallback option if the SK6812 DMA driver proves troublesome: 2 × TLC5947 @ ~$4.00 = $8.00,
plus 2 × 1 kΩ current-set resistors. Adds $3.50, removes RGB, removes timing risk.*

## Digital glue

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 2 | 8:1 analog mux | CD4051BE | $0.60 | $1.20 | One populated, one for expansion |
| 4 | 8-bit shift register (PISO) | CD4021BE | $0.60 | $2.40 | libDaisy has a stock driver for the 4021 |
| 1 | OLED 2.42" 128×64 SPI | SSD1309 module | $12.00 | $12.00 | 0.96" SSD1306 is $4 and too small to read |

## Audio output

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 2 | 1/4" TS jack, PCB mount | PJ-612A | $2.00 | $4.00 | Stereo line out |
| 1 | 3.5 mm TRS jack | PJ-320D | $1.00 | $1.00 | Headphones |
| 1 | Headphone amp | TPA6132A2 | $2.00 | $2.00 | Or NJM4556AD if you prefer through-hole |
| — | Output caps, resistors, films | — | — | $3.00 | **Topology TBD** — confirm Seed3 output is single-ended vs differential first |

## MIDI

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| 3 | 3.5 mm TRS jack (MIDI Type A) | PJ-320D | $1.00 | $3.00 | In, Out **and Thru** |
| 1 | Optoisolator | H11L1 | $1.00 | $1.00 | Schmitt output; no extra buffering needed on input |
| 1 | Hex inverter/buffer | 74HCT14 | $0.40 | $0.40 | 6 gates: 2 for Out, 2 for Thru |
| — | 220 Ω / 10 Ω / 33 Ω resistors | — | — | $0.70 | |

## Power and misc

| Qty | Part | Example P/N | Unit | Ext | Notes |
| ---: | --- | --- | ---: | ---: | --- |
| — | Passives (R/C, 0805) | — | — | $5.00 | Buy an assortment book once |
| 1 | Ferrite bead + bulk caps | — | $1.50 | $1.50 | USB rail filtering |
| 8 | M3 standoff + screws | — | $0.40 | $3.20 | |
| 1 | Enclosure body (3D print or laser-cut acrylic) | — | $12.00 | $12.00 | The PCB is the panel in v1 |
| 1 | USB-C cable | — | $5.00 | $5.00 | |

## Analog expansion reservations

Insurance so that adding analog circuitry later does not require a board respin. See
[docs/05-analog-expansion.md](05-analog-expansion.md).

| Qty | Part | Example P/N | Unit | Ext | Populate in v1? |
| ---: | --- | --- | ---: | ---: | --- |
| 1 | 2.1 mm DC barrel jack + 1N5819 Schottky | PJ-002A | $1.50 | $1.50 | Footprint only — analog needs ±12 V |
| 1 | 2x10 2.54 mm header | — | $0.80 | $0.80 | Yes |
| 1 | 8-bit shift register (SIPO) | 74HC595 | $0.40 | $0.40 | Yes — 8 trigger outs, drives external gear now |
| — | 595 passives | — | — | $0.20 | Yes |
| 2 | 3.5 mm audio-IN jack | PJ-320D | $1.00 | $2.00 | Yes — analog FX insert loop on day one |

## Totals

| Section | Cost |
| --- | ---: |
| Core | $84.49 |
| Controls | $54.20 |
| LEDs and drivers | $6.10 |
| Digital glue | $15.60 |
| Audio output | $10.00 |
| MIDI | $5.10 |
| Power and misc | $26.70 |
| Analog expansion reservations | $4.90 |
| **Total (v1, first unit)** | **≈ $207** |
| *less one-time tooling (ST-Link, passives assortment, 4 spare PCBs)* | *−$57* |
| **Marginal cost of a second unit** | **≈ $150** |

Budget **$250–300 all-in** for the first build. That covers a second PCB spin, which you will
need, and the parts you'll destroy learning.

## Deferred to v2

| Qty | Part | Unit | Ext | Enables |
| ---: | --- | ---: | ---: | --- |
| 1 | microSD push-push socket | $1.50 | $1.50 | Sample loading |
| 1 | 8-channel I2S DAC (PCM1681) | $8.00 | $8.00 | Individual voice outputs |
| 6 | 1/4" TS jack | $2.00 | $12.00 | Individual outs |
| 4 | 3.5 mm jack + TL074 conditioning | $2.50 | $10.00 | CV/gate + analog clock in/out |
| 1 | Analog voice daughterboard (BD + SD + output filter) | ~$60 | $60.00 | PCB, DAC8568 CV DAC, ±12 V DC-DC, discrete voice circuits |
| 1 | USB-A jack + 5 V load switch | $2.50 | $2.50 | USB MIDI host — **costs pins D29/D30**, keep them free in v1 |
| | **v2 add-on total** | | **$94.00** | |

Leave footprints for the SD socket and the clock jacks on the v1 PCB even if you don't populate
them. Unpopulated footprints are free; a board respin is $40 and three weeks.
