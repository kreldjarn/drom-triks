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

- Seed3 + 1 × CD4051 + 6 pots → confirm `AdcChannelConfig::InitMux` and check for crosstalk
- 1 × CD4021 + 8 buttons → confirm the stock `ShiftRegister4021` driver and `Switch` debounce
- 1 encoder on direct GPIO → confirm no lost counts on a fast flick
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
- **Trigger the voices over MIDI from a DAW.** This is the ordering trick that makes the phase
  work: you get to tune all eight voices against reference records before a sequencer exists
- Profile CPU per voice and record the numbers

**Done when:** all eight voices sound good played from Ableton, and you know your CPU budget.

## Phase 3 — Sequencer core (~2–3 weeks)

- Sample-accurate 96 PPQN clock with sub-block trigger offsets
- Pattern data model, 8 tracks × 64 steps
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
- Soft-takeover / pickup on pots (mandatory: six knobs address eight voices)
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
- KiCad schematic (Electrosmith publish Seed footprints), then 4-layer layout
- Ground discipline: separate LED and audio ground pours, single star point at USB
- **Include the analog-expansion reservations** from [doc 05](05-analog-expansion.md): barrel
  jack footprint, 2×10 header, 74HC595 trigger outputs, audio-in jacks. $4.90 against a respin
- Leave unpopulated footprints for the SD socket, second CD4051 and clock jacks
- Order from JLCPCB (qty 5), assemble, bring up rail by rail

**Done when:** the Phase 4 firmware runs unmodified on the PCB. **Expect a second spin** — budget
for it rather than being disappointed by it.

## Phase 6 — Persistence, FX, polish (~2 weeks)

- QSPI save/load for patterns, kits and settings, with slot layout and wear levelling
- Writes from the main loop only, on explicit save — never during playback
- Master FX: drive, compressor, reverb
- Per-voice FX sends
- Pot calibration, factory reset, firmware version display
- Song mode / pattern chaining
- SysEx pattern/kit dump and restore ([06-midi.md §8](06-midi.md#8-sysex))

**Done when:** you can power-cycle it and lose nothing.

## Phase 7 — Expansion

Now that the instrument works, pick whichever of these you actually want. They're independent.

- **Samples.** `SampleVoice : IVoice` drops into the existing array. SD card over SDMMC, streamed
  into SDRAM (64 MB ≈ 11 minutes of mono 48 kHz — you will not run out). Layer sample + synth per
  track for a Rytm-style hybrid
- **Analog voices.** The daughterboard from [doc 05](05-analog-expansion.md). Start with an
  808-style BD + SD and a stereo output filter, not a full analog eight-voice board
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
| No `IVoice` seam | Samples/analog become rewrites | Define it in Phase 2, before the first voice |
| QSPI write faults | Data loss on save | `BOOT_SRAM` validated in Phase 0 |
| Analog expansion needs ±12 V | Power respin | Barrel jack footprint in v1 ($1.50) |
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
