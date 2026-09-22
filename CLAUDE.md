# drom-triks

Synthesis-based hardware drum machine on the Electrosmith Daisy Seed3. C++17 firmware on
libDaisy + DaisySP. Design docs live in `docs/` — read the relevant one before proposing
architecture changes; most decisions here have a recorded reason.

## Build

```sh
make libs      # libDaisy + DaisySP — only needed after a submodule update
make -j8       # firmware
```

The toolchain is ARM's tarball build at `~/.local/opt/arm-gnu-toolchain-*/bin`, added to `PATH`
in `~/.zshrc`. It is deliberately not installed through Homebrew:

- **Never suggest `brew install arm-none-eabi-gcc`.** That formula ships without newlib, so every
  build dies on `fatal error: stdint.h: No such file or directory`, and there is no
  `arm-none-eabi-newlib` formula to pair with it.
- The `gcc-arm-embedded` cask has the right contents but installs via `sudo -E`, which this
  machine's sudoers policy forbids.

Cloning needs `--recurse-submodules`; libDaisy has nested submodules of its own and the build
fails without them. Details in `docs/00-toolchain.md`.

## Hard constraints

These are the ones that cause real, hard-to-diagnose bugs. Check work against them.

**The audio callback is sacred.** No allocation, no locks, no flash access, no logging. UI → audio
goes through the SPSC command queue; audio → UI through relaxed atomics. The audio side owns all
sequencer and voice state and is its only writer.

**QSPI writes must be at offset ≥ `0x100000`.** Under `BOOT_SRAM` the app image is staged at chip
offset `0x40000` and can grow to `0xB8000`. `PersistentStorage::Init()` **defaults its offset to
0**, which would put user data directly on top of the running firmware — the first save corrupts
the code executing it. Always pass the offset explicitly. See `docs/02-firmware.md` §8.

**`APP_TYPE = BOOT_SRAM`, never `BOOT_QSPI`.** BOOT_QSPI executes in place from the same flash we
need to write patterns to.

**Triggers are sample-accurate.** Voices carry a `trigger_delay` countdown in samples. Never
quantise a trigger to an audio block boundary — that is ±0.67 ms of jitter on every hit, and it
sounds bad in a way that is very hard to trace back afterwards.

**The pin budget is 28 of 31.** D29/D30 are reserved for USB MIDI host (OTG HS is hard-wired to
PB14/PB15, so it cannot move to other pins). Don't spend them without saying so.

**Voices go through `IVoice`.** That interface is what makes sample playback and analog voices
later additions rather than rewrites of the sequencer, mixer and UI. Don't bypass it.

## Verifying APIs

libDaisy and DaisySP are vendored in `lib/`. Read the headers there rather than recalling an API
from memory — several signatures are not what you would guess. For example `PersistentStorage`
decides whether to write by comparing with `operator!=`, not `operator==`, so a settings struct
needs that operator specifically.

## Style

- C++17, 4-space indent, trailing-underscore `snake_case_` for private members (matches libDaisy)
- Comments explain **why**, not what; put gotchas and non-obvious decisions next to the code
- Durable hardware assumptions belong in `docs/`, not scattered through comments

## Git

- **Never write "Co-authored by Claude Code", or any similar attribution, in a commit message or
  pull request description.** This applies to every variant and phrasing, including trailers
  naming a specific model.
- Don't commit or push unless asked.
- `build/` and `lib/*/build/` are gitignored — never commit build artifacts.
