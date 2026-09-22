# Hardware Design

## 1. Board choice: Daisy Seed3

Three Daisy boards are currently sold: **Seed3** ($29.99), **Seed2 DFM** ($29.99), and the
**Patch Submodule** ($39.99, sold out at time of writing).

**Use the Seed3.** Reasoning:

| | Seed3 | Seed2 DFM | Patch SM |
| --- | --- | --- | --- |
| Mounting | Through-hole 2×20 headers | SMT castellated | SMT castellated |
| Prototyping | Breadboard + socket | Reflow required | Reflow required |
| Audio out | Codec is TI TAC5242, 32-bit/192 kHz, −120 dB noise floor | Differential I/O | Single-ended + Eurorack CV circuitry |
| Extra hardware | None | None | Eurorack CV in/out you'd pay for and not use |

Seed3 specs that drive the rest of this design:

- ARM Cortex-M7 @ 480 MHz (STM32H750)
- 65 MB RAM (64 MB SDRAM + internal) — a 10-minute audio buffer, i.e. samples are never memory-bound
- 8 MB QSPI flash
- **31 GPIO, 12 × 16-bit ADC, 2 × 12-bit DAC** — this is the budget everything below fits into
- USB-C, pin-to-pin compatible with the original Seed footprint

Two Seed3 caveats worth knowing up front:

- The **JTAG header ships unpopulated**. Solder a 2×5 1.27 mm header on before you start, or
  you will be debugging a real-time audio system with printf over USB. Do this on day one.
- The **output stage is unverified** in this plan. The TAC5242 supports both differential and
  single-ended output, and the Seed3 schematic does not state which configuration Electrosmith
  used. Download the Seed3 schematic PDF and confirm before committing the output stage to
  copper (see [Phase 5](04-development-plan.md#phase-5--pcb-34-weeks-mostly-waiting)). If differential, the output
  jacks need a differential receiver (or one side terminated), not a plain DC-blocking cap.

## 2. Panel layout

```
  ┌────────────────────────────────────────────────────────────────────┐
  │   ┌──────────────────────┐                                         │
  │   │  128×64 OLED         │      (ENC1)        (ENC2)               │
  │   │  SSD1309 2.42"       │      value/tempo   nav/page             │
  │   └──────────────────────┘                                         │
  │                                                                    │
  │    (P1)   (P2)   (P3)   (P4)   (P5)   (P6)                         │
  │    TUNE   DECAY  TONE   SNAP   DRIVE  LEVEL                        │
  │                                                                    │
  │   [BD] [SD] [CH] [OH] [LT] [CP] [RS] [FM]     ← 8 track keys       │
  │                                                                    │
  │   [PLAY] [REC] [SHIFT] [PATT] [SONG] [TAP]    ← 6 transport keys   │
  │                                                                    │
  │   [1][2][3][4][5][6][7][8][9][10][11][12][13][14][15][16]          │
  │                                        ← 16 step keys, RGB backlit │
  └────────────────────────────────────────────────────────────────────┘
```

**30 keys total** (16 step + 8 track + 6 transport), all RGB-backlit, **6 pots**, **2 encoders**.

The six pots are *macros*, not per-voice controls. They always address the currently selected
track, and the labels are fixed across all eight voices:

| Pot | BD | SD | CH / OH | CP | RS | FM |
| --- | --- | --- | --- | --- | --- | --- |
| TUNE | base freq | base freq | base freq | body freq | pitch | carrier |
| DECAY | decay | decay | decay | tail length | decay | decay |
| TONE | tone/attack | tone | filter | filter | filter | ratio |
| SNAP | punch | snappy | metallicity | spread | mix | index |
| DRIVE | drive | drive | drive | drive | drive | drive |
| LEVEL | level | level | level | level | level | level |

This is the single most important UX decision in the build. 8 voices × 6 params = 48 knobs if
done literally; the macro mapping gets you the same control surface for 6 knobs and makes muscle
memory transfer between voices. Every voice must implement all six, even where the mapping is
a stretch — a knob that does nothing on some tracks is worse than a knob that does something
mild.

## 3. I/O topology

The pin budget is the constraint that makes or breaks this design, so it's worked out explicitly.

### 3.1 Pots → one analog multiplexer

libDaisy's ADC driver has native multiplexer support:

```cpp
void InitMux(Pin adc_pin, size_t mux_channels,  // 1–8
             Pin mux_0, Pin mux_1, Pin mux_2,
             ConversionSpeed speed = SPEED_8CYCLES_5);
// read back with: hw.adc.GetMuxFloat(chn, idx)
```

One **CD4051** 8:1 mux covers all 6 pots on **1 ADC pin + 3 select pins = 4 pins**, with
2 channels spare for a future CV input or expression pedal. The driver handles the select-line
sequencing and DMA in the background; you just read floats.

Populate the footprint for a **second CD4051** sharing the same 3 select lines and consuming one
more ADC pin. It costs $0.60 and 4 mm² and gives you 8 more analog inputs when you inevitably
decide six knobs wasn't enough.

### 3.2 Keys → CD4021 shift register chain

30 keys on **3 pins** (clock, latch, data) via **4 × CD4021** (32 inputs, 2 spare).

Use the CD4021 specifically, not the more common 74HC165 — libDaisy ships a
`ShiftRegister4021` driver in `src/dev/sr_4021.h` that is templated on chain length, so this is
zero custom code. The 74HC165 is faster and cheaper but has inverted latch polarity and you'd be
writing and debugging your own driver for no benefit at a 1 kHz scan rate.

Debounce with libDaisy's `Switch` class (8-bit shift-register debounce). Scan the chain at
**1 kHz from the main loop**, and call the edge checks at exactly the same rate — the debouncer
tracks state per call, so a mismatched rate silently drops events.

### 3.3 Encoders → direct GPIO

**Do not put encoders on the shift register chain.** A fast knob flick generates quadrature
edges faster than a 1 kHz scan resolves, and you'll lose counts — which feels like a broken
knob, not a sampling artifact.

Wire A/B directly to GPIO (**4 pins** for two encoders) and use libDaisy's `Encoder` class. The
encoder *push switches* can go on the shift register chain — a button press is slow.

### 3.4 LEDs → SK6812 chain on SPI+DMA

30 RGB LEDs on **1 data pin**. SK6812 MINI-E is the reverse-mount part used in the mechanical
keyboard world; it sits under a Cherry MX switch and lights a translucent keycap.

The 800 kHz one-wire protocol is driven by encoding each LED bit as 3 SPI bits at 2.4 MHz and
pushing the buffer with DMA — standard technique, ~100 lines, zero CPU after the transfer
starts. libDaisy has no stock driver for this; it's the one piece of custom hardware code
in the build.

> **Fallback if the DMA driver fights you:** 2 × **TLC5947** (24-channel 12-bit PWM, SPI,
> daisy-chainable). Hardware greyscale, no timing sensitivity, ~$8. You lose RGB and gain
> certainty. Decide by the end of [Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks) —
> it changes the PCB.

Two gotchas that will cost you an evening each if missed:

- **Level shifting.** SK6812 at 5 V needs V_IH ≈ 3.5 V; the Seed3 drives 3.3 V. Put a
  **74AHCT125** between them ($0.50). The "just try it" approach works on the bench at room
  temperature and fails in the enclosure.
- **Current.** 30 LEDs at full white is 30 × 60 mA = **1.8 A** — far beyond USB budget. Clamp
  global brightness in software (~25%) and never render full white. At 25 % single-hue you're
  around 180 mA, which is fine. Add a **1000 µF bulk cap** on the LED rail and 100 nF per LED.

### 3.5 Display, MIDI, audio

- **OLED**: SSD1309 2.42" 128×64, SPI — **5 pins** (SCK, MOSI, CS, DC, RST). libDaisy's
  `OledDisplay<SSD130x4WireSpiTransport>` drives it directly. The 0.96" SSD1306 is the same
  driver and half the price, but it's too small to read parameter values from playing distance.
- **MIDI**: UART — **2 pins**. H11L1 optoisolator on input, buffer + 2×220 Ω on output.
  TRS Type-A jacks (3.5 mm) rather than 5-pin DIN: smaller, and now the standard.
- **Audio**: stereo line out on the Seed3's codec pins, plus a headphone amp (TPA6132A2)
  on a 3.5 mm jack. Output stage detail pending the schematic check in §1.

### 3.6 Pin budget

| Function | Pins | Notes |
| --- | ---: | --- |
| Pot mux (CD4051) | 4 | 1 ADC + 3 select |
| 2nd mux (expansion) | 1 | shares the 3 select lines |
| Key chain (CD4021 ×4) | 3 | clock, latch, data |
| Encoders A/B ×2 | 4 | direct GPIO, switches on the chain |
| LED data (SPI2) | 2 | MOSI + SCK consumed by the peripheral |
| OLED (SPI1) | 5 | SCK, MOSI, CS, DC, RST |
| MIDI UART | 2 | TX, RX |
| Trigger outs (74HC595 CS) | 1 | shares the OLED SPI bus |
| **Subtotal** | **22** | |
| SD card (SDMMC 4-bit) | 6 | phase 7, for samples |
| **Total** | **28** | of 31 available |

Three pins spare after the SD card. Use the select lines for the mux rather than ADC-capable
pins — don't burn a 16-bit ADC channel on a digital select line.

## 4. Power

Single **USB-C** at 5 V into the Seed3, which regulates its own 3V3. Budget:

| Rail | Load | Current |
| --- | --- | --- |
| 5 V | Seed3 + codec | ~250 mA |
| 5 V | 30 × SK6812 @ 25 % brightness | ~180 mA |
| 5 V | OLED | ~30 mA |
| 3V3 | Logic (mux, SRs, level shifter) | ~20 mA |
| | **Total** | **~480 mA** |

Comfortable on USB 2.0's 500 mA and trivial on any USB-C supply. Keep the LED return current
off the analog ground: **star-ground at the USB connector**, separate the LED ground pour from
the audio ground pour, and join them at one point. 30 LEDs PWMing at audio rates into a shared
ground plane is an audible buzz, and it is very hard to fix after layout.

## 5. Mechanical

**v1:** the main PCB *is* the front panel — black soldermask, white silkscreen legends, switches
mounted directly. Body is 3D-printed or laser-cut acrylic with standoffs. This costs nothing
extra and looks intentional.

**v2:** separate 1.5 mm aluminium panel over a PCB sandwich, once the layout is proven and you
know you won't be moving a knob 2 mm to the left.

Switches: Cherry MX / Gateron with clear or translucent housings (the SMD LED cutout in the
switch body is what the SK6812 shines through). Add **hot-swap sockets** — $3 for the set, and
you can change switch feel without desoldering 30 switches from a panel you've already legended.

Pots: Alpha 9 mm vertical (RD901F) with Davies-1900-style knobs. Encoders: Bourns PEC11R with
detents and a push switch.

## 6. Reservations for analog expansion

Analog circuitry is a planned future direction, and four items must be on the **v1** PCB or
adding it later costs a board respin. Full detail in
[docs/05-analog-expansion.md](05-analog-expansion.md); the short version:

| Reservation | Cost | Populate in v1? |
| --- | ---: | --- |
| 2.1 mm DC barrel jack + Schottky, diode-OR'd with USB 5 V | $1.50 | Footprint only — analog circuits need ±12 V, which USB can't supply |
| 2×10 expansion header (5 V, VIN, AGND, 8 triggers, SPI, I2C, audio return) | $0.80 | Yes |
| 74HC595 → 8 trigger outputs | $0.60 | Yes — drives Eurorack and external drum modules immediately |
| Stereo audio **input** jacks | $2.00 | Yes — the Seed3's unused audio input becomes an analog FX insert loop |

**$4.90 against a $40 respin and three weeks of lead time.** The ±12 V generator itself belongs
on the daughterboard, not here — a switching converter next to the audio codec is a noise problem
you'd be solving before there's any benefit.
