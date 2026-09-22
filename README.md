# drom-triks

A synthesis-based hardware drum machine built on the [Electrosmith Daisy](https://daisy.audio)
platform: 8 synth voices, a 16-step x0x-style sequencer with parameter locks, and a
panel of real knobs, encoders and backlit keys.

Sample playback and analog voice circuitry are explicit future phases. Both are planned for
rather than retrofitted: the voice layer is built around an interface that a sample player or an
analog voice can implement without touching the sequencer, and the v1 PCB reserves the power,
trigger and audio-return paths an analog daughterboard will need.

## Documents

| Doc | Contents |
| --- | --- |
| [docs/00-toolchain.md](docs/00-toolchain.md) | Phase 0 setup, build, flashing, bring-up checklist |
| [docs/01-hardware.md](docs/01-hardware.md) | Board choice, panel layout, I/O topology, pin budget, analog/power design |
| [docs/02-firmware.md](docs/02-firmware.md) | Module layout, timing model, voice engine, sequencer data model, UI state machine |
| [docs/03-bom.md](docs/03-bom.md) | Bill of materials with part numbers and costed lines |
| [docs/04-development-plan.md](docs/04-development-plan.md) | Phased build plan, milestones, risk register |
| [docs/05-analog-expansion.md](docs/05-analog-expansion.md) | Hybrid architectures and what v1 must reserve |
| [docs/06-midi.md](docs/06-midi.md) | Transports, routing matrix, message map, clock recovery, SysEx |
| [docs/07-test-equipment.md](docs/07-test-equipment.md) | What to buy, when — and why the scope comes last |

## Design targets

- 8 voices, all synthesised, ~20% CPU at 48 kHz — headroom for FX, samples and analog hybrids
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

## Status

**Phase 0.** `src/main.cpp` is the bring-up program — it blinks, plays a sine, and round-trips
QSPI to prove the `BOOT_SRAM` decision. Builds clean; **not yet run on hardware.**
