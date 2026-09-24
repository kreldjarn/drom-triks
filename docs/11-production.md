# Production Readiness

The plan is a few prototypes, then **100+ if they turn out nice and fun**. This document is the
short list of things that are nearly free to decide now and expensive or impossible to retrofit
once units exist in other people's hands.

Nothing here asks you to spend money yet. It asks you not to foreclose the option.

## 1. The economics change shape, not just scale

Almost every trade recorded elsewhere in these docs is qty-1 reasoning — "$9 of insurance against a
~$175 respin", "setup dominates at qty 1". At a hundred units the one-time costs amortise and
**per-unit BOM becomes the number that matters.**

| | Qty 1 | ~Qty 100 |
| --- | ---: | ---: |
| Switches, keycaps, sockets, LEDs | $39 | ~$25 |
| Encoders + volume pot (10) | $42 | ~$27 |
| OLED | $12 | ~$8 |
| Seed3 | $30 | ~$28 |
| PCB (4-layer, 321 × 187 mm) | ~$30–40 | ~$15–25 |
| Metal panel, anodised, legended | ~$80–170 | ~$35–50 |
| Turnkey PCBA | — | ~$15–25 |
| **All-in** | **$236 + panel** | **~$145–175** |

Boutique hardware generally needs **2.5–3.5× BOM** to be viable once labour, support, returns and
distribution are counted, which puts retail around **$400–550**. That is where comparable
grooveboxes sit, so the shape works — but it is worth knowing before unit 30 rather than after.

Treat every figure here as shape rather than quote. They want re-deriving against real vendor
quotes at the point of committing.

## 2. Design for assembly — the one that bites now

Hand-reflowing 34 reverse-mount SK6812s is fine once and impossible a hundred times, so **turnkey
PCBA stops being optional**. That is a constraint on *part selection*, and part selection is
happening now.

**Every part should be orderable through the assembly house**, ideally as a catalogue/basic part.
Anything else attracts per-part setup fees, or simply cannot be placed.

Two parts in the current BOM fail that test today:

| Part | Problem | What to do |
| --- | --- | --- |
| **SK6812 MINI-E** | Mechanical-keyboard channel, not a general distributor line. 100 units is 3,400 LEDs | Identify a direct supplier and confirm they will sell at that quantity — or retire the part entirely by going top-mount, below |
| **Quad VCA (cartridge carrier)** | The SSM2164 is discontinued; the V2164/AS2164 that replaced it come from boutique synth suppliers with real supply gaps | Find a second source, or design the carrier so a different VCA topology drops in |

Neither is fatal and neither is urgent this month. Both become expensive the moment a board is laid
out around them.

### Open question: does the LED have to be reverse-mount?

Worth settling before layout, because it may retire the riskier of those two parts outright.

The switch's SMD cutout faces the PCB either way ([hardware §5](01-hardware.md#5-mechanical)), so
both of these light the same keycap:

| | Where it sits | Sourcing |
| --- | --- | --- |
| **SK6812 MINI-E** (current) | bottom face, shining up through a hole in the board | mechanical-keyboard channel only — the row above |
| **SK6812 MINI**, top-mount | top face, inside the switch's own cutout | an ordinary 3535 addressable RGB, stocked broadly |

**What is not in question is RGB.** The LED language in
[firmware §7](02-firmware.md#7-ui-state-machine) is built on hue — track colour on every step key,
a white playhead that outranks everything, red for mute, and a lock rendered as a shift toward
white measured at 0.42 against 0.12 for a plain step. Single-colour LEDs inside the switches, the
classic backlit-keyboard approach, would collapse all of that onto brightness, which is already
carrying velocity. That trade is already priced as the TLC5947 fallback in
[hardware §3.4](01-hardware.md#34-leds--sk6812-chain-on-spidma) and it is a worse instrument, not
just a different one.

What *is* in question is the mounting, and the argument against top-mount is assembly.
[The BOM](03-bom.md#assembly) says the reverse-mount parts are what "makes the board a two-sided
reflow job". **That framing wants checking against the actual layout**, because the Kailh hot-swap
sockets are already bottom-side SMD: if the LEDs share that face, reverse-mount is not adding a
reflow pass and the top face is through-hole switches only. If so the assembly argument for MINI-E
is real and top-mount costs a second pass. If not, top-mount is free and removes a supply risk.

Decide it at the Phase 5 schematic, before anything is placed.

## 3. Three things to build into v1 firmware

- **A serial number and per-unit calibration in the settings region.** ✅ **Done** — `Settings` in
  `src/io/settings.h` carries a 48-bit serial (0 meaning "never programmed", which is how a test
  fixture spots a fresh board) and 512 bytes reserved for calibration, inside the 4 kB slot at
  `kSettingsBase`. What remains is the fixture that writes them.
- **A hidden test mode.** A hundred boards need every key, LED, encoder, jack and the codec
  verified without attaching a debugger. As a boot-time key combination this is an afternoon; as an
  afterthought it is a fixture.
- **Firmware update over USB DFU, not ST-Link.** The `BOOT_SRAM` bootloader already gives this —
  the point is to *keep* it working and documented, because it is how a unit in someone else's
  hands gets fixed.
- **An LGPL compliance package.** The reverb and compressor come from DaisySP-LGPL (LGPL-2.1), so
  shipping units means including the licence text, acknowledging the library, and offering the
  linkable object files so a user can substitute their own build. None of this touches our own
  source — that would be GPL, not LGPL — and none of it applies to prototypes. It is a page and a
  zip, and it is much easier to assemble alongside the first build than to reconstruct later. The
  alternative is replacing both with our own, which `MasterFx` is shaped to allow.

## 4. EMC posture: free now, brutal later

A 480 MHz MCU driving 3,400 LEDs is a genuine emissions risk, and **the metal enclosure stops being
cosmetic and becomes the thing that gets you through a test**.

Compliance testing itself (CE/UKCA, FCC) runs ~$3–10k and is **out of scope until you commit to
selling**. Being USB-C powered removes mains safety from the picture entirely, which is a large
simplification. What is worth doing now, because it costs nothing and a respin to fix EMC is
miserable:

- **Series resistors (33–100 Ω) at the source of every fast digital line** — the SK6812 data line
  especially, which is an 800 kHz signal down a 34-device chain, and the 400 kHz shift-register
  clock. Slower edges, less radiation, no functional cost.
- **The ground split and single star point** already mandated in [§4](01-hardware.md#4-power) — now
  doing double duty.
- **The LED brightness clamp** already enforced in `LedRenderer` — likewise.
- **One chassis bond point, isolated jacks** ([§4](01-hardware.md#4-power)).

## 5. The module question

The obvious cost lever at a hundred units is the $28 Daisy Seed3, and it is worth being precise
about it, because the easy version of the answer is aimed at the wrong target.

### 5.1 What the module supplies, and what this design uses

| Seed3 supplies | drom-triks uses |
| --- | --- |
| 64 MB SDRAM | the master FX buffers, ~450 kB — see below. `Machine` is ~30 kB and samples stream from SD, so nothing else wants it |
| 8 MB QSPI | 2.7 MB of patterns, kits and songs; 4.3 MB reserved ([firmware §8](02-firmware.md#8-persistence)) |
| 480 MHz Cortex-M7 | under 20 % for eight voices plus master FX ([firmware §5](02-firmware.md#5-voice-engine)) |
| 31 broken-out GPIO | 18 assigned — but 31 is the number that forced 1-bit SDMMC, the SAI2 pin scramble, "SPI2 is unusable", and D29/D30 held hostage to USB host ([hardware §3.6](01-hardware.md#36-pin-map)) |
| TAC5242 codec, power tree, USB-C | genuinely used, and good |

The 64 MB SDRAM is the headline spec in [hardware §1](01-hardware.md#1-board-choice-daisy-seed3), and
the design uses **about 450 kB of it** — the master delay line plus `ReverbSc`'s 395 kB `aux_` array
([firmware §5](02-firmware.md#5-voice-engine)). Sample storage is bounded by spare QSPI or by an SD
card streamed in Phase 7; neither wants a 10-minute RAM buffer.

> An earlier revision of this section said nothing touched the SDRAM at all. That was true before
> the master effects existed and is not true now. It does not change the conclusion, but for a
> reason worth stating: **the 450 kB is only in SDRAM because of `BOOT_SRAM`.** On the Seed3 the
> staged app image occupies the 480 kB AXI SRAM, so statics have the 128 kB DTCM and little else.
> An H743 booting from its own 2 MB flash leaves that whole 512 kB free, and the FX buffers fit in
> it with room to spare. The SDRAM requirement is an artefact of the H750's tiny internal flash,
> not of the audio design — which makes it an argument *for* §5's candidate, not against it.

A processor and memory that "suit our purposes exactly" would still mean *less* memory, not custom
memory: 450 kB of use does not justify 64 MB of part.

### 5.2 Three different projects wear the same name

**Bare STM32H750 with SDRAM, on our board.** Still the wrong trade, but note *why*: the module is
doing the SDRAM routing — a 100 MHz length-matched bus and a BGA-class part — and that is the
expensive part of what you would be taking on. This is the version to keep refusing.

**Drop the SDRAM and right-size the MCU.** An **STM32H743VIT6 or H750VBT6 in LQFP100**, no external
DRAM. 0.5 mm pitch, no BGA, no memory bus to match. The argument above evaporates, because the hard
thing it names is no longer in the design. Two structural wins come with it, and both are worth more
than the money:

- **The `BOOT_SRAM` hazard stops existing.** The H743VI has 2 MB of internal flash, so there is no
  staged app image, no 480 kB ceiling, and no `PersistentStorage` offset landmine at `0x100000` —
  the single most expensive-to-reverse decision in the project
  ([firmware §8](02-firmware.md#8-persistence)) simply goes away.
- **~80 usable GPIO instead of 31.** 4-bit SDMMC, USB host *and* device, SAI2 free, D29/D30 no
  longer reserved. Most of the pin cleverness in [hardware §3](01-hardware.md#3-io-topology) is
  working around the Seed footprint rather than around the chip.

The assembly objection is already paid for: 34 reverse-mount SK6812s make this a two-sided reflow
job and turnkey PCBA mandatory (§2). An LQFP100 on the top side is incremental, not a new category.

**Something much smaller — M4, RP2350, and similar.** Under-20 % CPU says an F446 could probably run
the digital voices. Don't. It forecloses sample playback and the SAI2 cartridge digitising
([expansion §6.2](05-analog-expansion.md#62-digitising-the-cartridges--now-affordable)) to save single-digit dollars.

### 5.3 The cost is software, not BOM

Roughly: MCU, codec, QSPI, regulators and crystal land somewhere around $18–25 against $28, so the
saving across a hundred units is **$600–1,000** — less than one board spin, and inside the error
bars on every other figure in §1. **Money is not the reason to do this.** Shape, not quote, as above.

The real bill is the port, and libDaisy is single-chip and single-board by construction (paths below
are relative to `lib/libDaisy/`):

| What | Evidence |
| --- | --- |
| One target device | `-DSTM32H750xx -DSTM32H750IB`, one `startup_stm32h750xx.s` |
| Three linker scripts, all H750IB | `core/STM32H750IB_{flash,sram,qspi}.lds` |
| A 16 MHz crystal in the PLL maths | `HSE_VALUE=16000000`, `PLLM = 4` in `src/sys/system.cpp` |
| A board-shaped `Init` | `DaisySeed::Init` hard-codes the IS25LP064A, its QSPI pins, the SDRAM and the codec |

The HAL underneath is the generic STM32H7xx one, and the *peripheral* drivers — SPI, SAI, I²C, ADC,
`ShiftRegister4021`, `OledDisplay`, `MidiUartHandler`, `PersistentStorage` — are chip-level and would
largely survive. The board layer would be forked, not configured.

**The timing asymmetry is the part that matters.** libDaisy coupling in this repo is currently
**131 lines, all of them in `src/main.cpp`**; `src/engine/` is DaisySP-only by design and already
builds natively. That is the lifetime minimum. Phases 4–6 add display, persistence, MIDI and the DFU
flow on top of it, so the port gets steadily more expensive from here — which is an argument for
*deciding* early even though the prototypes get built on the module either way.

### 5.4 The argument that should actually decide it

**Seed3 supply risk.** Electrosmith is a small company and there is no second source for a Seed3. At
a hundred units a discontinuation or a long stockout halts production outright; an STM32 can be
bought from three distributors. That is a better reason to own the silicon than $6 a unit, and it is
the one that should decide it.

### 5.5 Where this lands

Build the prototypes on the Seed3. It is a *prototyping* decision that de-risks everything else, the
module is socketed ([BOM](03-bom.md#core)), and none of the firmware, panel or UX work is wasted
either way.

Make the call at the 100-unit commit point, and when you do, the candidate is **H743VI in LQFP100,
no SDRAM, no module** — which is cheaper *and structurally simpler* than what we have now. Verify
the pin table against the datasheet before it is load-bearing; every pin claim above is derived the
way [hardware §3.6](01-hardware.md#36-pin-map) warns against, and has not been checked.

## 6. What not to do

**Do not panelise-optimise the board.** At 321 × 187 mm it is too large to panelise at all, and 2×8
step keys would not fix that either (245 × 212 mm). It is a floor, not a choice — see
[hardware §5](01-hardware.md#5-mechanical).

**Do not take the bare-H750-with-SDRAM version of §5.** If the module ever comes off this board, it
comes off along with the external DRAM, not in spite of it.

## 7. Sequencing

Nothing here changes the phase plan. It changes what "done" has to include:

| When | What |
| --- | --- |
| Now, at BOM time | Second-source the SK6812 and the quad VCA; prefer catalogue parts everywhere else |
| Phase 5, at schematic | Series resistors on fast lines; one chassis bond; isolated jacks |
| Phase 5, at layout | FR4 plate from the same run, before any metal is cut |
| Phase 6 | Serial number, calibration and test mode alongside the rest of persistence |
| Phase 6, before the libDaisy surface grows | Decide §5 — the port is cheapest while `main.cpp` is the only coupling |
| Before committing to 100+ | Real vendor quotes; Seed3 supply commitment or a custom-MCU plan; pre-compliance EMC scan if you can get cheap access to a chamber |
