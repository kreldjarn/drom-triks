# Development Plan

Sized for evenings and weekends. **~4–6 months to a playable instrument**, with something
audible at the end of every phase.

The ordering follows one rule: **never let hardware block firmware**. Phases 2–4 are the bulk of
the work and all run on a breadboard, so a three-week PCB lead time costs you nothing.

## Phase 0 — Toolchain (~1 week)

- `arm-none-eabi-gcc`, libDaisy + DaisySP as submodules, Makefile from the Daisy template
- **Solder the JTAG header onto the Seed3 and get a debugger attached.** Not optional. The
  alternative is debugging a real-time audio system with printf.
- Verify the `APP_TYPE = BOOT_SRAM` bootloader flow end to end, including a QSPI write — this
  validates the single most expensive-to-reverse decision in the project
- Blink an LED, output a sine, confirm audio quality and noise floor

**Done when:** you can edit, build, flash and set a breakpoint in under 30 seconds.

## Phase 1 — Breadboard rig (~1–2 weeks)

Every I/O primitive proven in isolation, before any of it is committed to copper.

- 1 × CD4021 + 8 buttons → confirm the stock `ShiftRegister4021` driver and `Switch` debounce
- **2 encoders on a CD4021 scanned at 10 kHz → confirm no lost counts on a hard spin.** This is
  the second-highest-risk item in the build: eight encoders on direct GPIO would need 16 pins
  that don't exist, so if the fast scan drops counts the fallback is a ~$1 I²C co-processor and
  a second firmware. **Decision point — it changes the PCB**
- Check the `Switch` debouncer still runs at 1 kHz off the 10 kHz scan, not at 10 kHz
- Turn two or three candidate encoders and pick one on feel; you cannot choose detent torque
  from a datasheet
- 8 × SK6812 + 74AHCT125 → **write the SPI+DMA driver.** This is the highest-risk custom code
  in the build; do it early enough that the TLC5947 fallback is still on the table
- SSD1309 OLED over SPI
- MIDI in/out loopback

**Done when:** one program reads every input and drives every output simultaneously, with the
audio callback running and no glitches. **Decision point:** SK6812 or TLC5947 — this changes the
PCB, so it must be settled here.

## Phase 2 — Voice engine (~2–3 weeks)

- Define `IVoice` **first**, before writing any voice. Retrofitting it later means touching the
  sequencer, mixer, UI and storage. It's also what makes samples (Phase 7) and analog voices
  ([expansion](05-analog-expansion.md)) additions rather than rewrites
- Four voices from DaisySP (BD, SD, CH, OH) — mostly configuration
- Four custom voices (LT, CP, RS, FM)
- Map all six macro params consistently across every voice
- **Develop them on the host first.** `make -C host run` renders the real engine to a WAV with no
  board involved — DaisySP has no hardware dependency. Tune against reference records in a DAW,
  iterate in seconds, and arrive at hardware with the voices already right
- **Then trigger over MIDI from a DAW** once a board exists, to play them in real time
- Profile CPU per voice and record the numbers

**Done when:** all eight voices sound good played from Ableton, and you know your CPU budget.

## Phase 3 — Sequencer core (~2–3 weeks)

- Sample-accurate 96 PPQN clock with sub-block trigger offsets
- Pattern data model, 12 tracks × 64 steps (8 digital voices + 4 analog cartridge slots)
- Play/stop/tempo, swing, per-track length and speed (polymeter — one byte, large musical payoff)
- Probability, ratchets, micro-timing
- MIDI clock in/out with **interrupt timestamping + PLL recovery**, sync source priority
  ([06-midi.md §6](06-midi.md#6-clock-and-sync)) — build this with the clock, not after it
- SPSC command queue between UI and audio contexts

**Done when:** it plays a pattern, locks to external MIDI clock, and the timing is tight enough
to sit against a reference track without drifting or smearing.

## Phase 4 — UI and UX (~2–3 weeks)

The phase that always takes longer than planned, because this is the actual instrument.

- Mode state machine, `SHIFT` as a held modifier
- **Parameter locks** — hold a step, turn a knob
- Encoder acceleration (mandatory: 24 detents/rev is unusable at one fixed step size)
- Encoder push switches: push-to-default on a macro
- LED language: velocity as brightness, track as hue, playhead as white flash
- OLED pages that explain rather than gate
- Live record with optional quantise
- Mutes, pattern chaining
- Full MIDI message map, both note modes, routing matrix, MIDI learn
  ([06-midi.md](06-midi.md)) — it binds to the UI, so it belongs here

**Done when:** you can write a pattern from scratch without looking at the screen, and you catch
yourself jamming instead of testing.

## Phase 5 — PCB (~3–4 weeks, mostly waiting)

**Start the schematic during Phase 3** and run this in parallel. The fab lead time is dead time
otherwise.

- **First task: download the Seed3 schematic and confirm whether the audio outputs are
  single-ended or differential.** The TAC5242 supports both and Electrosmith hasn't documented
  which they used. This determines the entire output stage, and it's the one unknown in the
  hardware plan
- **Second task: confirm the SAI2 alternate functions against the STM32H750 table.** D26/D27/D28
  are reserved for a second audio input stream — freed by moving the encoders onto the CD4021
  chain — but libDaisy hardcodes its AF rather than looking it up, so verify rather than trust.
  Also pick an external ADC that does not need MCLK: `SAI2_MCLK_A` is `DISP_DC` and not free.
  See [doc 05 §6.2](05-analog-expansion.md#62-digitising-the-cartridges--now-affordable).
  It decides what lands on header pins 21–24, so settle it *before* layout
- KiCad schematic (Electrosmith publish Seed footprints), then 4-layer layout
- Ground discipline: separate LED and audio ground pours, single star point at USB
- **Include the analog-expansion reservations** from [doc 05](05-analog-expansion.md): barrel
  jack footprint, 2×12 header footprint (2×10 populated), 74HC595 trigger outputs, audio-in
  jacks. $5.00 against a respin
- **Draw the cartridge carrier schematic too — but don't fab it.** It is the only way to find
  out the expansion header pinout is missing a signal while fixing it is still free rather than
  a respin. The carrier *board* waits until one analog voice exists on perfboard and you know
  what a slot actually has to carry ([doc 05 §3.4](05-analog-expansion.md#34-when-to-build-it--schematic-early-board-late))
- Leave unpopulated footprints for the SD socket, CV/expression jacks and clock jacks
- Order from JLCPCB (qty 5), assemble, bring up rail by rail

**Done when:** the Phase 4 firmware runs unmodified on the PCB. **Expect a second spin** — budget
for it rather than being disappointed by it.

## Phase 6 — Persistence, FX, polish (~2 weeks)

- QSPI save/load for patterns, kits and settings, with slot layout and wear levelling
- Writes from the main loop only, on explicit save — never during playback
- Master FX: drive, compressor, reverb
- Per-voice FX sends
- Encoder acceleration curve tuning, factory reset, firmware version display
- Song mode / pattern chaining
- SysEx pattern/kit dump and restore ([06-midi.md §8](06-midi.md#8-sysex))

**Done when:** you can power-cycle it and lose nothing.

## Phase 7 — Expansion

Now that the instrument works, pick whichever of these you actually want. They're independent.

- **Samples.** `SampleVoice : IVoice` drops into the existing array. SD card over SDMMC, streamed
  into SDRAM (64 MB ≈ 11 minutes of mono 48 kHz — you will not run out). Layer sample + synth per
  track for a Rytm-style hybrid
- **Analog cartridges.** From [doc 05](05-analog-expansion.md) and [doc 10](10-cartridge.md), in
  this order: the 808-style bridged-T kick on perfboard driven by the trigger outputs and a bench
  supply, *then* the four-slot carrier designed around what that voice turned out to need, then
  the same circuit as a cartridge.
  Building the carrier first means guessing a slot's electrical envelope before any voice
  exists to measure. Resist a full analog kit: hats and metallic percussion are square
  oscillators and filters that the digital voices already nail
- **Individual outs.** PCM1681 8-channel I2S DAC + six jacks
- **CV/gate and analog clock.** The eight triggers from the 74HC595 are already there if you
  populated it in Phase 5
- **USB MIDI host.** Play it from a controller keyboard with no computer. Firmware-only *if*
  D29/D30 were left free in Phase 5; a respin otherwise
- **Enclosure v2.** Aluminium panel, now that you know you won't be moving a knob 2 mm left

## Risk register

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Seed3 output stage config unknown | Output stage wrong → respin | Read the schematic in Phase 5 before layout |
| SK6812 DMA driver misbehaves | LED rework → respin | Prove it in Phase 1; TLC5947 fallback decided there |
| LED ground noise in audio | Audible buzz, hard to fix post-layout | Separate pours, star ground, brightness clamp |
| Block-quantised trigger timing | Sounds subtly bad, hard to diagnose later | Sub-block trigger offsets from Phase 3 |
| UI scope creep | Phase 4 never ends | Freeze the mode list before starting; new ideas go to a v2 list |
| 10 kHz encoder scan drops counts | Knobs feel broken; 16 GPIOs don't exist as a fallback | Prove it in Phase 1; I²C co-processor fallback decided there |
| No `IVoice` seam | Samples/analog become rewrites | Define it in Phase 2, before the first voice |
| QSPI write faults | Data loss on save | `BOOT_SRAM` validated in Phase 0 |
| Analog expansion needs ±12 V | Power respin | Barrel jack footprint in v1 ($1.50) |
| SAI2 alternate functions differ from libDaisy's assumption | Cartridges are sum-only; header pins 21–24 wasted | Check the H750 AF table in Phase 5, before layout; the quad VCA ships either way |
| Cartridge CV bus too slow for the audio callback | P-locks land 1–2 ms behind their own trigger on analog tracks | SPI DACs written from the callback, not I²C from the main loop; measure the burst at bring-up |
| External clock jitter imported into groove | Sounds loose, blamed on the sequencer | Interrupt timestamping + PLL, built in Phase 3 |
| D29/D30 assigned to other I/O | USB MIDI host needs a respin | Reserve them in the Phase 5 pin map |

## Sequencing summary

```
Month 1   ├ Phase 0 ─┤├─ Phase 1 ─┤├──── Phase 2 ────
Month 2   ──── Phase 2 ────┤├──────── Phase 3 ────────
Month 3   ──── Phase 3 ────┤├──────── Phase 4 ────────
          └───────── Phase 5 schematic + fab (parallel) ─────────┘
Month 4   ──── Phase 4 ────┤├─ Phase 5 bring-up ─┤├─ Phase 6 ─
Month 5+  ─ Phase 6 ─┤├──────────── Phase 7 ────────────
```
