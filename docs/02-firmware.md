# Firmware Design

## 1. Toolchain and boot configuration

C++17, `arm-none-eabi-gcc`, libDaisy + DaisySP as git submodules, plain Makefiles (the Daisy
templates work; PlatformIO adds a layer you'll fight when debugging).

### Build for the bootloader with `APP_TYPE = BOOT_SRAM`

This is a day-one decision that is expensive to reverse, because it determines where your
pattern data can live.

| APP_TYPE | Program space | QSPI available for data? |
| --- | --- | --- |
| `BOOT_NONE` | 128 kB internal flash | Yes, but 128 kB is too small |
| **`BOOT_SRAM`** | **480 kB** (bootloader reserves 32 kB at the end of SRAM) | **Yes — all 8 MB** |
| `BOOT_QSPI` | ~7.75 MB | No — the program lives there, and execution is slower |

`BOOT_SRAM` gives 480 kB of program space, which is far more than this firmware needs, runs at
internal-flash speed, and leaves the **entire 8 MB QSPI free for patterns, kits and settings**.

The alternative bites specifically here: with `BOOT_QSPI` the program executes from QSPI in
memory-mapped mode, and *writing* to QSPI while executing from it hard-faults. You would
discover this the first time a user saved a pattern.

```make
# Makefile
APP_TYPE = BOOT_SRAM
```

## 2. Module layout

```
src/
  main.cpp              init, audio callback, 1 kHz control loop
  hw/
    board.h/.cpp        pin map; one Hardware struct owning every peripheral
    controls.cpp        mux pot scan + smoothing + soft-takeover, key debounce, encoders
    leds.cpp            SK6812 SPI-DMA driver + RGB frame buffer
    display.cpp         OLED page renderer
  engine/
    voice.h             IVoice interface — the seam samples slot into later
    voices/             bd, sd, hh, clap, tom, rim, fm
    mixer.cpp           per-voice gain/pan, accent bus, FX sends
    fx.cpp              master drive, compressor, reverb
  seq/
    clock.cpp           sample-accurate PPQN clock, swing, MIDI sync
    pattern.h           data model
    sequencer.cpp       step advance, probability, ratchets
    song.cpp            pattern chaining
  io/
    midi.cpp            in/out, clock, notes, CC
    storage.cpp         QSPI persistence
  ui/
    ui.cpp              mode state machine, LED language, screen pages
```

## 3. Timing model

This is where drum machines are won or lost. A groovebox that is 1 ms late at random doesn't
sound broken, it just sounds *bad*, and you can't A/B your way to the cause after the fact.

**Audio:** 48 kHz, block size 32 → 0.667 ms callback period.

**The naive approach** — advance the sequencer once per audio block — quantises every trigger to
a 0.667 ms grid. That's ±0.67 ms of jitter on every hit, which is audible as smearing on closed
hats and completely wrecks flams and micro-timing.

**The approach to use:** the clock runs in the audio callback with sample resolution, and
triggers carry a sub-block delay.

```cpp
// Clock state, advanced once per block but evaluated per sample.
uint32_t samples_per_tick;   // sr * 60 / (bpm * PPQN), Q16 fixed point
uint32_t tick_accumulator;

// Each voice owns a countdown. The sequencer sets it; the voice starts at zero.
struct Voice {
    int32_t trigger_delay;   // samples until this hit sounds; -1 = idle
    float   pending_velocity;
};
```

The sequencer computes *which sample within the block* a step lands on and writes that offset to
the voice, rather than splitting the audio block. Cheaper, exact, and it makes micro-timing and
swing fall out for free — they're just offsets added to `trigger_delay`.

Use **96 PPQN** internally (not 24). MIDI clock is 24 PPQN, so you divide cleanly on output,
and 96 gives you 1/24-of-a-step micro-timing resolution to play with.

## 4. Threading model

Two contexts, one direction of ownership. The audio callback owns all sequencer and voice
state; the main loop owns the UI and never touches that state directly.

```
  main loop (1 kHz)                       audio callback (1.5 kHz)
  ─────────────────                       ────────────────────────
  scan pots/keys/encoders                 drain command queue
  run UI state machine       ──────►      advance clock
  push UiCommand into SPSC ring           fire due steps
                                          render 8 voices + FX
  read AudioState (atomics)  ◄──────      publish AudioState
  render LEDs + OLED
```

- **UI → audio**: a lock-free single-producer/single-consumer ring buffer of `UiCommand`
  structs, drained at the top of each callback. Every pattern edit, parameter change and
  transport action goes through it. The audio side then needs no locks at all, because it is
  the only writer.
- **Audio → UI**: a small `AudioState` struct of relaxed atomics (current step, playing flag,
  per-voice envelope levels for meters). Stale by one block is fine for a display.

Never allocate, never take a lock, never touch flash in the audio callback.

## 5. Voice engine

DaisySP ships Mutable-Instruments-derived drum models, which is a large head start — four of the
eight voices are mostly configuration:

| # | Voice | Implementation |
| --- | --- | --- |
| 1 | BD | `AnalogBassDrum` / `SyntheticBassDrum` (switchable model) |
| 2 | SD | `AnalogSnareDrum` / `SyntheticSnareDrum` (switchable) |
| 3 | CH | `HiHat<SquareNoise, LinearVCA>` |
| 4 | OH | `HiHat<RingModNoise, CymbalVoice>` |
| 5 | LT | custom: sine + pitch envelope + drive (~40 lines) |
| 6 | CP | custom: 3 retriggered noise bursts through a BPF + reverberant body |
| 7 | RS | custom: two detuned squares through a BPF, 808-rimshot style |
| 8 | FM | custom: 2-operator FM — covers metallic perc, cowbell, sub |

All eight implement one interface, and that interface is the seam that makes sample support a
later addition rather than a rewrite:

```cpp
class IVoice {
public:
    virtual void  Init(float sample_rate)        = 0;
    virtual void  Trigger(float velocity)        = 0;
    virtual void  SetParam(ParamId id, float v)  = 0;  // TUNE/DECAY/TONE/SNAP/DRIVE/LEVEL
    virtual float Process()                      = 0;
    virtual ~IVoice() = default;
};
```

A `SampleVoice` implementing this same interface drops into the same array. The sequencer,
p-locks, mixer and UI never learn that anything changed. Build this seam in Phase 2 even though
samples are Phase 7 — retrofitting it later means touching every one of those subsystems.

**CPU budget.** At 480 MHz / 48 kHz you have ~10,000 cycles per sample. Eight voices at a
worst case of ~150 cycles each is ~1,200 cycles; master FX maybe 500. That's **under 20 %**,
leaving comfortable room for sample streaming, a second FX send, or more voices. This design is
not CPU-constrained, and you should not spend time optimising it until measurement says
otherwise.

## 6. Sequencer data model

```cpp
struct ParamLock { uint8_t param_id; uint16_t value; };   // 3 bytes

struct Step {
    uint8_t   flags;        // active | accent | tie
    uint8_t   velocity;     // 0–127
    int8_t    micro;        // −48..+48 ticks @ 96 PPQN
    uint8_t   probability;  // 0–100 %
    uint8_t   ratchet;      // 1–8 retriggers
    uint8_t   lock_count;
    ParamLock locks[4];
};                                              // 18 bytes

struct Track  { Step steps[64]; uint8_t length; uint8_t speed; uint8_t direction; };
struct Pattern{ Track tracks[8]; uint16_t bpm_x10; uint8_t swing; uint8_t kit_id; };
```

**~9.2 kB per pattern.** 128 patterns is 1.2 MB — nothing against 64 MB of SDRAM, and it
persists comfortably into the 8 MB QSPI with room for kits and wear-levelling.

Per-track `length` and `speed` give polymeter for free: a 7-step hat track against a 16-step
kick is one byte of state and the single highest ratio of musical interest to implementation
effort in the whole sequencer.

**Parameter locks** are the feature worth building the data model around. Hold a step key, turn
a pot, and that pot's value is recorded for that step only. The engine applies locks on trigger
and restores the pattern value after. Four lock slots per step is plenty in practice and keeps
`Step` at a cache-friendly 18 bytes.

## 7. UI state machine

Modes, with `SHIFT` as a held modifier rather than a latched state:

| Mode | 16 step keys | Pots |
| --- | --- | --- |
| **PLAY** (default) | toggle steps on selected track | macros for selected track |
| **hold a step key** | — | **write a p-lock** on that step |
| **SHIFT + step** | step detail: velocity, micro, probability, ratchet | edit that step's params |
| **MUTE** | — (track keys mute/unmute) | macros |
| **PATTERN** | select / chain patterns | — |
| **REC + play** | live record from track keys, quantise optional | live-record pot moves as locks |

**LED language** — consistent enough to read without thinking:

- Step key: off = inactive; dim→bright = velocity; hue = track colour
- Playhead: white flash on the current step
- Locked step: hue shifted toward white, or a slow pulse
- Track key: hue = track identity; dim = has content; bright = selected; red = muted

The screen *explains* — it shows the parameter name and value when you touch a knob, and the
pattern overview otherwise. It never becomes the only way to reach a function. If a feature
requires menu diving, it's mis-designed.

**Soft takeover on pots is mandatory**, since six physical knobs address eight voices. When you
switch tracks, the knob positions no longer match the stored values. Use pickup mode (the
parameter doesn't move until the knob crosses the stored value) with the screen showing both
the physical and stored positions so the jump is visible rather than mysterious.

## 8. Persistence

QSPI flash via libDaisy's `PersistentStorage`, laid out in slots:

```
0x000000  settings      (4 kB)   global config, calibration
0x001000  kits          (256 kB) 32 kits × 8 voices × params
0x041000  patterns      (2 MB)   128 patterns
0x241000  songs         (64 kB)  pattern chains
0x251000  free          (~5.7 MB) reserved for sample data
```

**Do flash writes from the main loop, never the audio callback**, and prefer to write on an
explicit save action rather than autosaving during playback. With `BOOT_SRAM` a QSPI write
won't fault, but the write still stalls for milliseconds — enough to starve an audio buffer if
you do it from the wrong context.

## 9. MIDI

- **In**: notes trigger voices (one note per track, GM drum map by default); CC maps to macro
  params; clock/start/stop/continue for external sync.
- **Out**: notes mirror the sequencer so the machine can drive other gear; clock at 24 PPQN.
- **Sync priority**: external clock when present, internal otherwise, with a UI indicator —
  silently switching sync sources is a debugging nightmare on stage.

USB MIDI via libDaisy's `MidiUsbHandler` comes almost free alongside the UART path and makes
DAW integration trivial. Wire both from the start.
