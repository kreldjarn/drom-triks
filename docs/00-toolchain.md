# Phase 0 — Toolchain Setup

**Goal:** edit → build → flash → breakpoint in under 30 seconds, and the four
assumptions the rest of the build rests on proven on real hardware.

## 1. Host setup (macOS)

### Compiler

The toolchain must include **newlib** (the bare-metal C library). Install ARM's official build
by unpacking the tarball — no sudo, no package manager:

```sh
mkdir -p ~/.local/opt && cd ~/.local/opt
curl -LO https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
tar -xf arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
```

Then add it to your shell profile:

```sh
export PATH="$HOME/.local/opt/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin:$PATH"
```

**Two package-manager routes that look right and aren't:**

| | Why not |
| --- | --- |
| `brew install arm-none-eabi-gcc` | The formula ships **without newlib**. Every build dies on `fatal error: stdint.h: No such file or directory`, and there is no `arm-none-eabi-newlib` formula to add alongside it. |
| `brew install --cask gcc-arm-embedded` | Correct contents, but the cask installs a `.pkg` via `sudo -E`. On machines whose sudoers policy forbids environment preservation it fails with `sudo: sorry, you are not allowed to preserve the environment` — common on managed/work Macs. |

The cask is fine where `sudo -E` is permitted. The tarball works everywhere, which is why it's
the instruction above.

### Flashing and debugging tools

```sh
brew install dfu-util open-ocd
```

OpenOCD's formula is `open-ocd`; the binary is `openocd`. Neither is needed to *build* — only to
flash and debug, so a broken Homebrew doesn't block Phase 0 compilation.

## 2. Clone and build

```sh
git clone --recurse-submodules git@github.com:kreldjarn/drom-triks.git
cd drom-triks
make libs        # libDaisy + DaisySP, a few minutes, once
make             # the firmware, seconds
```

**`--recurse-submodules` is not optional.** libDaisy has its own nested submodules (CMSIS, the
STM32 HAL drivers). Without them the build dies on a missing `stm32h7xx_hal.h`. If you already
cloned without it:

```sh
git submodule update --init --recursive
```

## 3. Install the bootloader (once per board)

The firmware is built with `APP_TYPE = BOOT_SRAM`, which requires the Daisy bootloader in
internal flash. Without it the board will not run this image.

1. Put the Seed in DFU mode: **hold BOOT, tap RESET, release BOOT**
2. `make program-boot`

Then flash the app the same way with `make program-dfu`.

## 4. Solder the JTAG header

Two different headers, don't confuse them:

- The **2×20 main pin headers** (the ones that meet a breadboard) **come pre-soldered** on the
  standard Seed3. Nothing to do.
- The **JTAG/SWD debug header is unpopulated.** Electrosmith left it off deliberately, to cut
  height and cost and because it got in the way of commercial customers embedding the module.

**Solder it before writing any Phase 1 code.** The alternative is debugging a real-time audio
system with printf, which does not work: printing from an audio callback changes the timing you
are trying to observe.

### The gotcha

The footprint has **14 positions**, but the header you want is a **10-pin (2×5) 1.27 mm** part —
e.g. Amphenol **20021111-00010T4LF**. The four extra positions are not wired to anything; they
exist only to help a 14-pin ST-LINK-V3MINIE cable align.

**Install the 10-pin header centred on the footprint, leaving two positions free at each end.**
Soldering it flush to one end puts every pin on the wrong signal. Centred, it also works with
older ST-Links and J-Link probes.

Electrosmith's orientation photos show a Seed Rev7 rather than a Seed3, but the wiring is the
same.

## 5. The debug loop

Two ways in, and the fast one is worth setting up properly:

| | Command | Speed | Use for |
| --- | --- | --- | --- |
| **ST-Link + GDB** | `make debug-server`, then `make gdb` | seconds | everyday iteration |
| **DFU** | `make program-dfu` | ~20 s + button dance | standalone running, no debugger attached |

Under `BOOT_SRAM` the ELF is linked for SRAM at `0x24000000`, so GDB can `load` straight into
RAM and skip the bootloader's QSPI staging entirely. That's what makes the 30-second target
achievable.

In VS Code, install **cortex-debug** and press F5 — `.vscode/launch.json` is configured for the
ST-Link and builds first.

## 6. Bring-up checklist

`src/main.cpp` exists only to prove these four things. Run it and confirm each:

| # | Test | Pass looks like | Fail means |
| --- | --- | --- | --- |
| 1 | **Init + main loop** | User LED blinks at 1 Hz | Hard fault, or the audio callback is overrunning |
| 2 | **Audio** | 440 Hz sine, both channels, −12 dBFS | Codec init or output stage wiring |
| 3 | **QSPI under BOOT_SRAM** | `boot_count` **increments across a power cycle** | The write silently failed — do not proceed to Phase 6 |
| 4 | **USB log** | The lines below appear in a serial monitor | Logging unavailable; debugging gets much harder |

Expected output from the second boot onward:

```
drom-triks phase 0 bring-up
qspi: state=USER boot_count=2 (magic ok)
audio: 48000 Hz, block 48
ready
```

On the very first boot `state=FACTORY` and `boot_count=1` — that's correct, it means the struct
had never been written. **Power-cycle and check it says `USER` and `2`.** A `boot_count` that
resets to 1 on every boot is the failure this test exists to catch, and it is silent otherwise.

While test 2 is running, measure the noise floor with the sine muted. This is the only baseline
you'll get before the panel wiring adds its own noise, so record the number.

## 7. The QSPI offset, and why it matters

`PersistentStorage::Init()` takes an address offset that **defaults to 0** — the base of the
chip. Under `BOOT_SRAM` the app image is staged at chip offset `0x40000` and can reach `0xB8000`,
so the default would place user data directly on top of the running firmware's own image.

The bring-up code uses `0x100000` (the 1 MB mark), permanently clear of it. See
[02-firmware.md §8](02-firmware.md#8-persistence) for the full layout. Never call
`storage.Init(defaults)` without the second argument.

## 7b. If the playground's audio is choppy

`host/build/play --check` reports what fraction of real time the device actually delivered. It
should be ~100%. Anything well below that will be heard as chopping, because the device is
starved.

If it is low, test against real hardware to separate the engine from the device:

```sh
host/build/play --devices
host/build/play --check --device <id>    # a built-in output, not a virtual one
```

Same binary at 100% on hardware and 50% on a virtual device means the problem is the device.

**The case we actually hit: a quarantined virtual-audio app.** Background Music was correctly
installed in `/Applications`, but still carried `com.apple.quarantine` from being downloaded.
macOS then applies **App Translocation** and runs it from a random read-only mount — *even from
`/Applications`* — which breaks the coordination between the app and the CoreAudio driver it
installs. The device then delivered exactly 50% of real time to every client, including a
forty-line CoreAudio sine with none of our code in it.

```sh
ps -Ao comm= | grep -i "Background Music.app"     # AppTranslocation in the path = this bug
sudo xattr -dr com.apple.quarantine "/Applications/Background Music.app"
# then quit the app from its menu-bar icon and relaunch it
```

The tell is that it is intermittent across launches rather than permanent, since translocation
is decided per launch. That is also why the same app can record fine one day and not the next.

## 8. Done when

- `make` builds clean
- All four bring-up tests pass, including the power-cycle check
- You can set a breakpoint in `main()` and hit it from VS Code with F5
- The JTAG header is soldered and the ST-Link is reliable

Then start [Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks).
