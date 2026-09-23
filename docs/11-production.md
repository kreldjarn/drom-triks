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
| Encoders + volume pot | $35 | ~$22 |
| OLED | $12 | ~$8 |
| Seed3 | $30 | ~$28 |
| PCB (4-layer, 321 × 187 mm) | ~$30–40 | ~$15–25 |
| Metal panel, anodised, legended | ~$80–170 | ~$35–50 |
| Turnkey PCBA | — | ~$15–25 |
| **All-in** | **$228 + panel** | **~$140–170** |

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
| **SK6812 MINI-E** | Mechanical-keyboard channel, not a general distributor line. 100 units is 3,400 LEDs | Identify a direct supplier and confirm they will sell at that quantity, before the design depends on the part |
| **Quad VCA (cartridge carrier)** | The SSM2164 is discontinued; the V2164/AS2164 that replaced it come from boutique synth suppliers with real supply gaps | Find a second source, or design the carrier so a different VCA topology drops in |

Neither is fatal and neither is urgent this month. Both become expensive the moment a board is laid
out around them.

## 3. Three things to build into v1 firmware

- **A serial number and per-unit calibration in the settings region.** There is already a 4 kB
  settings slot at `kSettingsBase` ([firmware §8](02-firmware.md#8-persistence)) with room to
  spare. Trivial now; impossible to retrofit into units already shipped.
- **A hidden test mode.** A hundred boards need every key, LED, encoder, jack and the codec
  verified without attaching a debugger. As a boot-time key combination this is an afternoon; as an
  afterthought it is a fixture.
- **Firmware update over USB DFU, not ST-Link.** The `BOOT_SRAM` bootloader already gives this —
  the point is to *keep* it working and documented, because it is how a unit in someone else's
  hands gets fixed.

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

## 5. What not to do

**Do not replace the Seed3 with a bare STM32H750 to save $30 a unit.** That module is doing the
SDRAM routing, QSPI, codec, power and USB — it *is* the hard part of this board — and dropping it
also drops libDaisy's board support, the pin map and the bootloader flow. At a hundred units the
saving does not come close to the cost of doing it yourself. Revisit at thousands, not before.

**Do not panelise-optimise the board.** At 321 × 187 mm it is too large to panelise at all, and 2×8
step keys would not fix that either (245 × 212 mm). It is a floor, not a choice — see
[hardware §5](01-hardware.md#5-mechanical).

## 6. Sequencing

Nothing here changes the phase plan. It changes what "done" has to include:

| When | What |
| --- | --- |
| Now, at BOM time | Second-source the SK6812 and the quad VCA; prefer catalogue parts everywhere else |
| Phase 5, at schematic | Series resistors on fast lines; one chassis bond; isolated jacks |
| Phase 5, at layout | FR4 plate from the same run, before any metal is cut |
| Phase 6 | Serial number, calibration and test mode alongside the rest of persistence |
| Before committing to 100+ | Real vendor quotes; pre-compliance EMC scan if you can get cheap access to a chamber |
