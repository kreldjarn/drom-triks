# drom-triks

A synthesis-based hardware drum machine built on the [Electrosmith Daisy](https://daisy.audio)
platform: 8 synth voices plus 4 analog cartridge slots, a 16-step x0x-style sequencer with
parameter locks, and a panel of endless encoders and backlit keys.

Sample playback and analog voice circuitry are explicit future phases. Both are planned for
rather than retrofitted: the voice layer is built around an interface that a sample player or an
analog voice can implement without touching the sequencer, and the v1 PCB reserves the power,
trigger and audio-return paths the analog hardware will need.

Analog voices arrive as **cartridges** — one voice per card in a Game Boy cartridge shell, four
swappable slots on a carrier board, adding tracks 9–12 rather than displacing digital voices.
Carrier in [docs/05](docs/05-analog-expansion.md), the cards themselves in
[docs/10](docs/10-cartridge.md).

## Documents

| Doc | Contents |
| --- | --- |
| [docs/00-toolchain.md](docs/00-toolchain.md) | Phase 0 setup, build, flashing, bring-up checklist |
| [docs/01-hardware.md](docs/01-hardware.md) | Board choice, panel layout, I/O topology, pin budget, analog/power design |
| [docs/02-firmware.md](docs/02-firmware.md) | Module layout, timing model, voice engine, sequencer data model, UI state machine |
| [docs/03-bom.md](docs/03-bom.md) | Bill of materials with part numbers and costed lines |
| [docs/04-development-plan.md](docs/04-development-plan.md) | Phased build plan, milestones, risk register |
| [docs/05-analog-expansion.md](docs/05-analog-expansion.md) | Analog voice cartridges, the carrier board, and what v1 must reserve |
| [docs/06-midi.md](docs/06-midi.md) | Transports, routing matrix, message map, clock recovery, SysEx |
| [docs/07-test-equipment.md](docs/07-test-equipment.md) | What to buy, when — and why the scope comes last |
| [docs/09-schematic.md](docs/09-schematic.md) | Step-by-step for the KiCad schematic, and what blocks it |
| [docs/10-cartridge.md](docs/10-cartridge.md) | Analog cartridge design, slot pinout, and the Game Boy form factor |
| [docs/11-production.md](docs/11-production.md) | Going from one to a hundred: design for assembly, EMC posture, what not to do |

## Design targets

- 8 synth voices at ~20% CPU at 48 kHz, plus 4 analog cartridge slots — 12 tracks, one sequencer
- Sample-accurate trigger timing (no block-quantised jitter), digital and analog alike
- Parameter locks per step, Elektron-style: hold a step, turn a knob
- Everything editable without entering a menu; the screen explains, it doesn't gate
- Full MIDI: DIN in/out/thru, USB device, PLL-smoothed external clock, SysEx backup
- Powered and programmed over a single USB-C cable

## Build

```sh
git clone --recurse-submodules git@github.com:kreldjarn/drom-triks.git
cd drom-triks
make libs      # libDaisy + DaisySP, once
make           # firmware
```

`--recurse-submodules` matters: libDaisy has nested submodules and the build fails without them.
Full setup, including the toolchain and the bring-up checklist, in
[docs/00-toolchain.md](docs/00-toolchain.md).

### Hearing it without hardware

The voice engine and sequencer have no hardware dependency, so they build and run natively:

**Play it on a Mac, right now:**

```sh
make -C host play
```

```sh
host/build/play --devices        # list output devices
host/build/play --device 84      # bypass a virtual device (BlackHole, Background Music…)
host/build/play --record         # start recording; r toggles it, saved on quit
host/build/play --check          # verify audio opens and runs at real-time rate
```

Real-time audio through CoreAudio, the keyboard standing in for the panel, and the LEDs drawn as
truecolour blocks using the *same* renderers the firmware will use. Not a mock-up of the
instrument — the instrument, with a different set of pins.

```sh
make -C host run                      # renders host/build/out.wav
afplay host/build/out.wav
host/build/render out.wav --solo 4    # one voice alone, for tuning by ear
host/build/render out.wav --trace     # exact sample each step fires on
host/build/render --selftest          # voices must be silent until triggered
make -C host test                     # every suite
host/build/play --check               # verify audio opens, without a listener
```

Macro values live at the top of `host/render.cpp` — that's where voice tuning happens.

`src/engine/` depends on DaisySP only — never libDaisy — which is what lets the *same* code the
firmware compiles also run on a laptop. Voices can be tuned against reference records and
sequencer timing checked sample-by-sample long before a board arrives.

## Status

**Phase 0 complete** on the host side; **nothing yet run on hardware.** `src/main.cpp` is the
bring-up program — it blinks, plays a sine, and round-trips QSPI to prove the `BOOT_SRAM`
decision.

**Phase 2 complete.** All eight voices — BD, SD, CH, OH from DaisySP; tom, clap, rimshot and
2-op FM built from primitives — behind the `IVoice` seam.

**Phase 3 core complete.** Sample-accurate 96 PPQN sequencer with **micro-timing** (±23 ticks,
~5 ms per tick at 120 BPM), swing, probability, ratchets, polymeter and four playback
directions, and **parameter locks**. Every timing and lock behaviour is verified against
expected values by `make -C host test`, not trusted.

**MIDI clock recovery done.** PI loop with a fast acquisition phase, outlier rejection, and a
Tight/Smooth tradeoff exposed as a user setting. Locked to a 90 BPM master, the sequencer holds
step spacing to **1 sample (0.02 ms)**; under ±1 ms source jitter, Smooth keeps the tempo
estimate inside **0.06 BPM**.

**UI state machine done.** Panel logic as pure state — step editing, track select, mute, encoder
acceleration, and parameter locks written by holding a step and turning a macro. Endless encoders
mean there is no soft-takeover machinery to get wrong and a p-lock needs no pickup sweep. Testable
headless, so the interaction model is verified before a panel exists.

**Patch format done.** A patch is a `Pattern` plus a `Kit`, saved with a versioned header so a
firmware change rejects stale flash rather than reinterpreting it.

**Integration done.** A `Machine` owns the patch, voices, sequencer and clock, and is the sole
writer of all of it. The UI emits `Command`s into a lock-free SPSC queue drained at the top of
each audio block, and reads back a small struct of relaxed atomics — the threading model from
[docs/02-firmware.md §4](docs/02-firmware.md#4-threading-model), implemented and tested with a
real two-thread producer/consumer run.

**LED and display rendering done.** Both are pure functions of machine + UI state, so the whole
LED language is verified headless — including the frame-level **current budget**, which keeps the
panel under 400 mA where an unclamped all-white frame would pull 2.0 A.

Next: pattern chaining, master FX, and the MIDI note/CC layer — all still board-free.
