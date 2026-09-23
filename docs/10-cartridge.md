# Cartridge Design

One analog voice per card, in a **Game Boy cartridge shell**. This document designs the first one —
an 808-style bridged-T bass drum — and fixes the slot interface every later cartridge inherits.

Read [doc 05](05-analog-expansion.md) first: it covers the carrier, the four slots and why analog
voices are cartridges at all. This is the card that plugs into them.

## 1. Why a Game Boy shell

It started as an aesthetic idea and survived on its merits.

- **No connector on the cartridge.** Gold fingers on the board edge, nothing to place or solder.
  That matters when you might build ten of these.
- **32 contacts** against the ~18 signals a slot needs, so there is room to spend the surplus on
  grounds (§4) rather than cramming.
- **Designed for power-off insertion**, which [doc 05 §4.6](05-analog-expansion.md#46-swap-with-the-power-off)
  requires anyway — hot-plugging a live summing bus pops, and an unpowered card across ±12 V is
  worse.
- **The shell protects the analog circuit** and gives a label area, which a bare card does not.
- Shells and repro edge connectors are available from the flashcart community.

The real constraint it imposes is **height**, and §3 covers the one part choice that depends on it.

## 2. Does the circuit fit?

A DMG cartridge is roughly **57 × 65 × 8 mm** outside, leaving on the order of **50 × 55 mm** of
board — about 27 cm², doubled if both sides are populated.

| | |
| --- | ---: |
| Parts in the BD design (§3) | ~55 |
| Tallest part (SOIC-16) | 1.75 mm |
| Board area needed, 0603, one side | ~15 cm² |
| Board area available, one side | ~27 cm² |

**It fits with room for a considerably more complex voice**, which matters because the bass drum is
the simplest cartridge anyone will want to build.

> **Measure a real cartridge before laying one out.** The dimensions above are from memory, not
> from a caliper or a datasheet, and the whole design is sized against them. The same goes for the
> edge-connector pitch and whether the contacts are single- or double-sided.

## 3. The bass drum

```
edge ──┬─ ±12V ─► local RC filtering + decoupling
       ├─ I²C  ─► 24AA02 EEPROM   (name, macro map, CV calibration)
       ├─ TRIG ─► pulse shaper ─┬─► CLICK: HPF, depth set by SNAP ──┐
       │                        └─► EXCITE ─► bridged-T resonator   │
       ├─ CV_TUNE  ─► OTA ch A: voltage-controlled R in the T       │
       ├─ CV_DECAY ─► OTA ch B: feedback depth (Q)                  │
       ├─ CV_TONE  ─► one-pole LPF after the resonator              │
       ├─ CV_DRIVE ─► gain into a diode clipper                     │
       └─ AUDIO ◄── output buffer ◄── summing ◄────────────────────┘
```

The bridged-T network in an op-amp's feedback path is the 808 bass drum: excite it with a short
pulse and it rings at a frequency set by the network and decays at a rate set by how much of the
output is fed back. The original sets both with fixed parts; the cartridge puts an OTA in each
place so TUNE and DECAY become CVs.

| Part | Package | Role |
| --- | --- | --- |
| TL074 | SOIC-14 | resonator, summing, buffers |
| LM13700 | SOIC-16 | dual OTA — TUNE and DECAY |
| TL072 | SOIC-8 | drive stage, output buffer |
| 24AA02 | SOT-23-5 | ID EEPROM |
| 3 × MMBT3904 | SOT-23 | pulse shaping |
| 2 × 1N4148 | SOD-323 | clipper |
| ~45 passives | 0603 / 0805 | |

### Two part choices that are not free

**The bridged-T capacitors must be C0G/NP0, not X7R.** X7R's capacitance changes with applied
voltage and it is microphonic — in a resonant filter that means the kick's pitch moves with its own
level, and knocking the case rings the voice. C0G reaches ~100 nF in 1206, which covers the
network. This also sidesteps the height problem: through-hole film caps would be the obvious analog
choice and they do not fit an 8 mm shell.

**Buffer the audio output to low impedance on the card.** Edge fingers oxidise and wear, and
contact resistance in a high-impedance signal path modulates the audio. The op-amp output stage is
not optional garnish.

## 4. The slot interface

32 contacts, ~18 used. **This pinout is inherited by every future cartridge**, so it is worth
spending the spare contacts well rather than compacting it.

| Pin | Signal | Pin | Signal |
| ---: | --- | ---: | --- |
| 1, 2 | +12 V | 3, 4 | −12 V |
| 5 | +5 V | 6, 7 | PGND |
| 8 | AGND | 9 | TRIG |
| 10 | AGND | 11 | CV_TUNE |
| 12 | AGND | 13 | CV_DECAY |
| 14 | AGND | 15 | CV_TONE |
| 16 | AGND | 17 | CV_SNAP |
| 18 | AGND | 19 | CV_DRIVE |
| 20 | AGND | 21 | I²C SDA |
| 22 | I²C SCL | 23 | PRESENT |
| 24 | AGND | 25 | AUDIO_OUT |
| 26 | AGND | 27–32 | spare |

**An AGND between every CV line.** The trigger edge is the fastest thing on this connector and the
tune CV is the most sensitive; coupling one into the other puts a pitch blip on every hit, which is
audible, intermittent-looking, and a genuinely miserable fault to chase later. Guard traces cost
nothing here because the contacts exist anyway.

**Pin 26 is the audio return and sits next to pin 25 deliberately** — the signal and its return
should be adjacent so the loop area is small.

**`PRESENT` is grounded on the card.** Detection then does not depend on the I²C enumeration
working, which matters because a cartridge with a dead EEPROM should still be visibly *there*
rather than silently absent.

**There is no LEVEL CV.** Level lives on the carrier's quad VCA
([doc 05 §3.3](05-analog-expansion.md#33-mixing--analog-now-digital-later)), which is also what
applies velocity, so it never reaches a cartridge.

**There is no SPI.** The DAC bank is on the carrier, for reasons that are worth understanding
before designing a second cartridge — see [doc 05 §3.2](05-analog-expansion.md#32-cv--on-spi-and-on-the-carrier).
A card carrying its own DAC would have to rejoin a daisy chain that an empty neighbouring slot
would already have severed.

## 5. The EEPROM, and what a cartridge declares

A 24AA02 at the standard address, reachable through the carrier's I²C switch. It carries:

- **Name**, for the OLED — "808 BD" rather than "C1"
- **Which of the six macros this card actually uses**, so the UI can grey out the rest rather than
  leaving knobs that appear to work and don't. [Hardware §2](01-hardware.md#2-panel-layout) is
  explicit that a dead knob is worse than a mild one, and the EEPROM is what makes that rule
  enforceable on a card that did not exist when the firmware shipped.
- **CV calibration** — the offset and scale that map a 0..1 macro onto this card's useful range.
  Two cards of the same design will not have identical response; this is where that is absorbed
  rather than in per-cartridge firmware.

The card's I²C address can be the part's default on every cartridge, because the carrier's switch
gives each slot its own bus segment. That is what keeps two identical cards in two slots working
with no straps and no per-slot BOM.

## 6. Cost

| Item | Qty | Unit | Ext |
| --- | ---: | ---: | ---: |
| PCB, 2-layer, hard-gold edge fingers | 1 | ~$8 | $8.00 |
| Shell (3D printed, or a salvaged cart) | 1 | ~$3 | $3.00 |
| TL074 / TL072 / LM13700 | 3 | ~$1.20 | $3.60 |
| 24AA02 EEPROM | 1 | $0.15 | $0.15 |
| Transistors, diodes | 5 | $0.10 | $0.50 |
| Passives (C0G where it matters) | ~45 | ~$0.05 | $2.25 |
| **Per cartridge** | | | **≈ $17.50** |

The hard gold is the line to watch. It is a per-order surcharge at most fabs rather than a per-board
one, so **panelise cartridges** — five kicks in one order amortise it; one kick does not.

## 7. Open items

| Item | Settle by | Why |
| --- | --- | --- |
| Shell, PCB and edge dimensions, measured | Before the first layout | §2 is sized on recalled figures |
| Edge pitch; single- or double-sided contacts | Before the first layout | Decides whether 32 contacts is really the budget |
| Hard-gold surcharge, per order vs per board | First quote | Decides whether to panelise |
| CV input filtering vs. the ~12 µs of DAC lead | Bring-up | [Doc 05 §4.3](05-analog-expansion.md#43-trigger-timing) — too much filtering smears the attack of every locked step |
| Bridged-T component values for the tuning range | Breadboard, before layout | The one part of this that wants an afternoon with a scope and an ear |
