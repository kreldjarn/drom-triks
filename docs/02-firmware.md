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
| **`BOOT_SRAM`** | **480 kB** (bootloader reserves 32 kB at the end of SRAM) | **Yes — ~7 MB above the app image** |
| `BOOT_QSPI` | ~7.75 MB | No — the program lives there, and execution is slower |

`BOOT_SRAM` gives 480 kB of program space, which is far more than this firmware needs, runs at
internal-flash speed, and leaves **~7 MB of QSPI writable for patterns, kits and settings** —
everything above the staged app image (see §8 for the layout).

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
    controls.cpp        10 kHz shift-register scan, quadrature decode, key debounce
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

**Gates need a second countdown, and it does not exist yet.** `trigger_delay` schedules the moment
a hit *starts*; a digital voice then has an envelope and needs nothing more. A trigger output does:
the 74HC595 bit has to be cleared again, and
[doc 05 §4.5](05-analog-expansion.md#45-six-knobs-on-an-empty-slot) maps DECAY to gate width on an
empty cartridge slot, so the width is a per-track value rather than a constant. That wants a
`gate_remaining` counter alongside `trigger_delay`, decremented in the same per-sample loop and
clearing the bit at zero. Worth building with the 595 driver rather than after it — retrofitting a
second timebase into the sample loop is exactly the kind of change that reintroduces jitter.

## 4. Threading model

Two contexts, one direction of ownership. The audio callback owns all sequencer and voice
state; the main loop owns the UI and never touches that state directly.

**Three** contexts, one direction of ownership. The audio callback owns all sequencer and voice
state; the main loop owns the UI and never touches that state directly; and a timer interrupt owns
the panel scan, because neither of the other two runs fast enough for it.

```
  scan ISR (10 kHz)      main loop (1 kHz)              audio callback (1.5 kHz)
  ─────────────────      ─────────────────              ────────────────────────
  clock the CD4021s      read scan state                drain command queue
  decode quadrature  ──► run UI state machine  ──────►  advance clock
  accumulate detents     push UiCommand into ring       fire due steps
  debounce keys @1 kHz                                  render 12 voices + FX
                         read AudioState (atomics) ◄──  publish AudioState
                         render LEDs + OLED
```

- **UI → audio**: a lock-free single-producer/single-consumer ring buffer of `UiCommand`
  structs, drained at the top of each callback. Every pattern edit, parameter change and
  transport action goes through it. The audio side then needs no locks at all, because it is
  the only writer.
- **Audio → UI**: a small `AudioState` struct of relaxed atomics (current step, playing flag,
  per-voice envelope levels for meters). Stale by one block is fine for a display.
- **Scan ISR → UI**: accumulated detent counts and debounced key state, read once per main-loop
  pass. The ISR never pushes commands itself — it has no business knowing what a key means.

Never allocate, never take a lock, never touch flash in the audio callback.

### The scan ISR is a real context with real rules

It exists because the encoders need 10 kHz ([hardware §3.2](01-hardware.md#32-keys-and-encoders--two-cd4021-chains))
and the main loop runs at 1 kHz. Three things follow, none of them optional:

- **It must be lower priority than the SAI DMA interrupt.** A 400 kHz bit-bang holding off the
  audio transfer is a dropout, and it would be blamed on the DSP.
- **Its interval is a parameter of the feel, not just of correctness.** `Ui::StepFor` derives
  encoder step size from the time between detents, so jitter in the scan interval becomes jitter
  in acceleration — a knob that feels different depending on what else the firmware is doing.
  Drive it from a hardware timer, not from a counter in the main loop.
- **The debouncer still runs at 1 kHz.** libDaisy's `Switch` tracks state per call, so the ISR
  decimates: quadrature every pass, keys every tenth.

## 5. Voice engine

DaisySP ships Mutable-Instruments-derived drum models, which is a large head start — four of the
eight voices are mostly configuration:

| # | Voice | Implementation |
| --- | --- | --- |
| 1 | BD | `AnalogBassDrum` / `SyntheticBassDrum` (switchable model) |
| 2 | SD | `SyntheticSnareDrum` — see below; the analog model's DECAY does not work |
| 3 | CH | `HiHat<SquareNoise, LinearVCA>` |
| 4 | OH | `HiHat<RingModNoise, SwingVCA>` |
| 5 | LT | custom: sine + pitch envelope + drive (~40 lines) |
| 6 | CP | custom: 3 retriggered noise bursts through a BPF + reverberant body |
| 7 | RS | custom: two detuned squares through a BPF, 808-rimshot style |
| 8 | FM | custom: 2-op FM with operator feedback, EFM-style — metallic perc, cowbell, blips, sub |

### Machines are selectable per track

A track's sound is not fixed to its legend. `MachineSlot` holds per-track storage sized for the
largest machine, and switching **constructs in place** — placement-new, never an allocation, since
the audio side owns voice state. A `static_assert` in `Make<>` is what stops a new machine
outgrowing the slot silently.

| Machine | Character |
| --- | --- |
| Machine | −40 dB @ DECAY 0.5 | Character |
| --- | ---: | --- |
| `BD 808` | 141 ms | the Mutable-derived model above: long sine, pitch envelope, little click |
| `BD 909` | 101 ms | fast drop under a short tail, with a separate beater click. Hits hardest — peak 0.45 against 0.30 |
| `BD BOOM` | 263 ms | deep sine, long sweep. **1089 ms at DECAY 0.85 against the 808's 496** |
| `SD 909` | 443 ms | balanced noise and body |
| `SD 808` | 74 ms | the 808 model with the envelope that makes DECAY work — see below |
| `SD PUNCH` | 68 ms | transient-forward; its noise dies rather than hissing on |
| `GLITCH` | 49–339 ms | bursts of crushed square grains at deliberately inharmonic ratios |
| `TRI` | **1.6 s – 4 s+** | a struck metal bar by modal synthesis — see below |
| `CH` `OH` `TOM` `CLAP` `RIM` `FM` | | as the table above |
| `SILENT` | | an unpopulated cartridge slot, or a deliberately dead track |

**`TRI` is the one machine that is not an envelope times an oscillator.** It is six very high-Q
resonators at the free-free bar ratios 1 : 2.76 : 5.40 : 8.93 : 13.34 : 18.64, struck with a
1.5 ms noise burst. Those ratios are physics rather than taste: harmonic partials fuse into a
pitched tone and stop sounding like metal, which is the same reason `GLITCH` picks inharmonic grain
ratios. SNAP stretches them further apart, because a real triangle is a *bent* bar and its partials
are messier than the ideal.

Two consequences. It rings for **seconds** — at full DECAY the time constant is nearly four, so a
hit is audible for the better part of twenty. That is what the instrument does, and it means a
triangle track overlaps itself by design. And it is the only machine whose feedback path can in
principle run away, so a test sweeps 81 points of its parameter space and asserts every one stays
bounded, finite and non-growing. A 625-point sweep during development found none, with a worst
peak of 0.71.

**`GLITCH`'s SNAP is a chaos control, and it reaches both ends on purpose.** At zero the RNG seed
resets on every trigger, so a hit is bit-identical each time and a pattern is reproducible. Above
zero the seed advances per hit and no two are alike. A machine that never repeats is fun and
impossible to arrange with, so the knob has to reach both.

The selection lives in the `Kit` as a `uint8_t` per track, so it survives a save and is
**range-checked on load** — a byte out of flash is not a guarantee. Changing machine rebinds the
`VoiceSlot`, which clears the lock mask (the previous step's locks were applied to a voice that no
longer exists) and pushes every stored parameter into the new one, because a freshly constructed
machine starts at its own defaults and knows nothing of what the knobs say.

**The panel legend becomes a default rather than a truth.** BD/SD/CH/OH… is silkscreened, and
[hardware §5](01-hardware.md#5-mechanical) puts those legends in cut metal. Worth deciding
deliberately before the panel is made — see [hardware §2](01-hardware.md#2-panel-layout).

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

### Tracks 9–12 are analog cartridge slots

The voice array is **12 entries**, not 8. The upper four are analog cartridges
([doc 05](05-analog-expansion.md)) — additive to the digital voices rather than replacing them, so
an empty slot is simply a silent track and there is no fallback logic anywhere.

`AnalogVoice` is the third implementation of the same interface: `SetParam` stages a CV,
`Trigger` flushes the staged CVs over SPI and latches a gate bit into the 74HC595, and `Process`
returns zero because the audio comes back through the ADC input. The reason the CV write is on SPI
rather than I²C is subtle and load-bearing — `VoiceSlot::ApplyLocks` runs at `delay_ == 0`, the same
sample as the gate, so a bus too slow for the audio callback pushes every parameter lock 1–2 ms
behind its own trigger. [Doc 05 §4.2](05-analog-expansion.md#42-why-the-bus-choice-decides-whether-p-locks-work)
has the full argument.

All twelve machines are statically allocated. `Machine` is now **46.2 kB** — the patch at 30.1 kB
plus twelve 1 kB machine slots, the voice slots, the channel strips and the queue. With
`Storage`'s staging buffer that is roughly 76 kB of the 128 kB DTCM, so the rule that neither is
ever a stack local matters more than it did at 21.5 kB, not less.

### Percussive FM

Track 8 is modelled on the Machinedrum's EFM machines. Three things separate percussive FM from
a bell, and only the first is obvious:

1. **The modulation index must decay, faster than the amplitude.** A static index gives an organ
   or a bell; an index that collapses in a few milliseconds gives a transient with a body behind
   it. Measured attack-to-tail brightness on the current settings: **59× for punchy perc, 25× for
   a metal blip, 5.9× for a cowbell.** This is most of what people mean by "punch".
2. **Operator feedback** — the modulator folded into its own phase. Past roughly 0.6 it breaks
   into noise, which is what makes metallic percussion read as metal rather than a tuned tone.
3. **Bit and rate reduction.** The hardware being imitated ran 12-bit converters; that grit is
   part of the sound, not a flaw. DaisySP's `Decimator` provides it.

A **third operator** modulates the carrier in parallel with op2, at a ratio deliberately
incommensurate with it so the two sideband families never line up. It engages only in the upper
half of `SNAP` and rides a squared copy of the index envelope, so it decays faster than op2 —
an attack thickener rather than a drone.

It was first built as a *stack* (op3 → op2), and an A/B with op3 disabled showed that barely
changed the output at all: once op2 is deep enough to sound metallic it is already near-chaotic,
so feeding it more does very little. In parallel the effect is consistent — periodicity drops
about 10% at every setting where it is active — but it is a refinement, not a transformation.
Two-operator FM at these indices is already dense, which leaves a third operator limited room.

`SNAP` sets index depth, collapse rate and op3 depth together, so turning it up makes a hit
*sharper* rather than merely brighter.

`DRIVE` is a layered grit control rather than one effect: operator feedback first, bit reduction
over the top, then sample-rate reduction at the extreme. There is no spare macro to give these
separate knobs, and stacking them gives one usable sweep from clean to destroyed. Only the bottom
third of the downsample range is musical — DaisySP's `Decimator` maps its factor to a hold of up
to 96 samples, which past about 0.3 is a buzz rather than a drum.

### DaisySP's AnalogSnareDrum has an unusable DECAY

Worth recording, because the symptom is a long ringing pitched tail that no setting shortens and
the obvious suspects are all wrong. The model gives its body resonators a Q of
`2000 · 2^(decay·7)`, so even at DECAY 0 they ring for about a second. Measured across the whole
range, the tail never fell below −40 dB inside five seconds, and it was not even monotonic:

```
DECAY      0.00   0.10   0.25   0.50   0.75   1.00
-40 dB ms   988   1217   1447   1218   1218   2626
```

`SyntheticSnareDrum` behaves: 103 ms at DECAY 0 rising smoothly to seconds at full. That is what
the `SD 909` machine uses.

**The wrapper this section proposed now exists.** `SD 808` holds the model's own decay fixed — it
is a timbre control there, not a time one — and gates the output with an `AdEnv` driven by the
DECAY macro. Measured across the knob:

```
DECAY      0.00   0.25   0.50   0.75   1.00
-40 dB ms    11     29     73    142    292
```

Monotonic, a 26× range, and 11 ms at DECAY 0 where the bare model gave 988. A test asserts all
three properties, because the failure it guards against is a knob that looks connected and is not.

Every other DaisySP voice scales correctly, so this is specific to that one class rather than a
general problem with the library.

### Two envelope mistakes worth not repeating

Both of these made every custom voice sound like a sustained tone rather than a struck one, and
neither shows up in a spectrum — only in the amplitude shape over time.

**`AdEnv` defaults to a linear decay.** Real percussion decays roughly exponentially. A linear
decay holds near full level and then drops, so a hit reads as a tone that cuts off. All four
custom voices now call `SetCurve(-4)`, which puts the envelope near 35% a quarter of the way
through its decay.

**Decay time must be spread exponentially across the knob.** A linear map from 15 ms to 1.4 s
puts the midpoint at 715 ms, so almost the whole range is "long" and every percussive setting is
crammed into the first few degrees of travel. `DecayTime()` maps it as `0.02 · 60^v` — 20 ms at
zero, ~155 ms at halfway, 1.2 s at full.

With both fixed, an FM hit's RMS falls from 0.50 to silence in **70 ms** in a clean exponential
curve. Before, it had only reached 0.20 after 190 ms.

**The index envelope needs a floor.** Let it reach zero and the tail is a bare carrier sine — a
noise attack followed by a beep, which is the least percussive thing this voice can do. A floor
of 0.12 keeps real harmonic content in the tail: measured at a 268 Hz carrier, the tail shows
9,740 Hz of zero crossings where a bare carrier would give about 540.

**Index is in turns, not radians.** `FastSin` takes a 0..1 phase, so textbook FM indices of 0–9
would mean nine whole cycles of phase modulation — noise at every setting rather than a tone.
The first implementation had exactly that bug, and it showed up as a zero-crossing measurement
pinned near Nyquist across the entire parameter range.

The oscillators use a parabolic sine approximation rather than `sinf` — two evaluations per
sample per voice makes it worth it, and the ~0.1% error is orders of magnitude below the grit
this voice adds on purpose.

**CPU budget.** At 480 MHz / 48 kHz you have ~10,000 cycles per sample. Eight voices at a
worst case of ~150 cycles each is ~1,200 cycles; master FX maybe 500. That's **under 20 %**,
leaving comfortable room for sample streaming, a second FX send, or more voices. This design is
not CPU-constrained, and you should not spend time optimising it until measurement says
otherwise.

### Filters, FX and LFOs — what is shared and what is per voice

Rough per-sample costs for the planned additions, against that ~10,000 cycle budget. Estimates, and
[Phase 2](04-development-plan.md#phase-2--voice-engine-23-weeks) is explicit that they must be
re-measured on the H750 rather than trusted from a host build:

| | cycles |
| --- | ---: |
| Two serial `Svf` per digital voice (8 × 2) | 800 |
| Saturator per voice, driving them | 80 |
| Master reverb + stereo delay | 450 |
| Master compressor | 120 |
| Twelve LFOs, evaluated per block | <15 |

That lands near 30 % with everything on. **CPU is not what constrains this design.**

**One delay and one reverb, shared, with a per-voice send and pan.** Per-voice delay *lines* were
considered and do not fit: one second of stereo float at 48 kHz is 384 kB per voice, 3 MB across
eight, against 480 kB of SRAM under `BOOT_SRAM`. The only place that fits is SDRAM, which would tie
the design to the Seed3 permanently and contradict
[production §5](11-production.md#5-the-module-question). Sends plus pan give the same stereo spread
for four bytes of state per voice, and it is what Elektron do.

**What is actually implemented** (`src/engine/fx.h`): the delay is ours, because DaisySP has no
stereo cross-feed delay and because the buffer has to be caller-provided. The reverb and compressor
are DaisySP's `ReverbSc` and `Compressor`, which live in **DaisySP-LGPL** — a separate library under
LGPL-2.1, where DaisySP proper is MIT. `MasterFx` is the seam, so replacing either is one file.

Two consequences:

- **Licensing bites on distribution, not development.** Prototypes carry no obligation. Shipping
  units means the licence text, an acknowledgement, and — since this is a static link — an offer of
  object files so a user can relink. It does **not** require opening our firmware; that is GPL.
  Recorded as a ship-time item in [production §3](11-production.md#3-three-things-to-build-into-v1-firmware).
- **`ReverbSc` is `float aux_[98936]`, 395 kB, held by value.** It cannot be a member of `Machine`
  (§2's 21.5 kB rule), so it is injected by pointer, and on the Seed3 it has to live in SDRAM. That,
  not the licence, is what would force writing our own: a board without SDRAM cannot host it, and
  the replacement is a Freeverb-shaped plate of about 50 kB.

The delay buffer is caller-provided for the same reason. Under `BOOT_SRAM` the staged app image
occupies the 480 kB AXI SRAM, leaving only the 128 kB DTCM and a 32 kB `.sram1_bss` for statics —
so on the Seed3 both buffers want SDRAM. Keeping them out of the engine means that is a board
decision rather than an engine one.

**Per-voice digital FX only reaches tracks 1–8.** `AnalogVoice::Process` returns zero — cartridge
audio comes back through the ADC — so filters, saturation and compression on tracks 9–12 depend on
the SAI2 per-cartridge digitising path, which is still
[an open check](05-analog-expansion.md#62-digitising-the-cartridges--now-affordable).

### LFOs

One per track, config held in `Kit` like any other parameter, which makes LFO settings **p-lockable
for free**. Four things about them are easy to get wrong:

- **Phase is runtime state and must never live in `Patch`.** `Patch` is memcpy-saved, so a stored
  phase would make every pattern load snap a free-running LFO to it — which is exactly what FREE
  mode means it must not do. Config in `Kit`, phase in `Machine`, on the audio side.
- **A trig-synced reset has to honour `trigger_delay`.** The reset is part of the trigger, so
  block-quantising it puts ±0.67 ms of phase jitter on every trig-synced LFO — the same error
  `VoiceSlot` exists to avoid. The *continuous* modulation needs no such precision and is evaluated
  per block; only the reset is sample-accurate. FREE mode never resets, so it sidesteps this
  entirely.
- **The value chain gains a third layer**: `effective = (locked ? lock : base) + depth × lfo(t)`,
  now evaluated continuously rather than only at trigger. The restore in `ApplyLocks` must land on
  **base**, not base+LFO, or the modulation bakes itself into the knob value — the same bug class
  as a lock leaking into later steps.
- **DEST, WAVE and MODE are enums stored as normalised floats.** `Kit.params` is `float` and
  `ParamLock.value` is a `uint16_t` over 0..1, which quantises cleanly for any enum under about 16
  entries — but the rounding has to be deliberate. A destination that drifts to its neighbour
  through float comparison would be a miserable bug to find.

A p-locked SPEED change in FREE mode must alter the phase *increment* without touching the
accumulator, or every locked step clicks.

**What is implemented** (`src/engine/lfo.h`, wired in `VoiceSlot`):

| | |
| --- | --- |
| Modes | FREE, TRIG, HOLD, ONE |
| Waves | triangle, sine, square, saw, ramp, random (S&H) |
| Rate | tempo-relative — SPEED is cycles per beat, swept exponentially, × a quantised power-of-two MULT. `Machine` pushes tempo only when it changes, since recalculating costs a `pow` per track |
| Destination | any of the 32 parameters, plus off. A `static_assert` ties `Lfo::kDestSlots` to `ParamId::Count + 1` so adding a page cannot silently make a parameter unreachable |
| Update | phase advances once per block; the **reset** happens on the trigger's exact sample |

`Lfo` deliberately does not know what a `ParamId` is — the destination is an opaque index the slot
interprets. That is what keeps it dependency-free and testable on its own.

Three behaviours are worth knowing because they are silent when wrong, and each has a test:

- **Modulation is re-derived from base-or-lock every block, never accumulated.** Accumulating walks
  the stored value instead of modulating it, which reads as a patch that drifts rather than as a
  broken LFO.
- **Moving DEST restores the parameter it left.** Otherwise the old destination stays frozen
  wherever the modulation last put it.
- **The locked value is the modulation origin on a locked step**, so a p-lock and an LFO compose
  rather than fight. `PreModValue` scans the step's lock list rather than shadowing all 32
  parameters — it runs once per block, not per sample.

## 6. Sequencer data model

```cpp
struct ParamLock { uint8_t param_id; uint16_t value; };   // 4 bytes (2-byte aligned)

struct Step {
    uint8_t   flags;        // active | accent | tie
    uint8_t   velocity;     // 0–127
    int8_t    micro;        // −23..+23 ticks @ 96 PPQN (just under one step)
    uint8_t   probability;  // 0–100 %
    uint8_t   ratchet;      // 1–8 retriggers
    uint8_t   lock_count;
    ParamLock locks[8];
};                                              // 38 bytes

struct Track  { Step steps[64]; uint8_t length, swing, speed, direction; };
struct Pattern{ Track tracks[12]; uint16_t bpm_x10; uint8_t kit_id; };
```

**28.6 kB per pattern** at 12 tracks — measured from `sizeof`, not estimated. `ParamLock` aligns to
4 bytes rather than 3, which takes `Step` to 38 at eight lock slots. A whole `Patch` (pattern plus
kit plus header) is **30.1 kB**, and 128 patterns is **4.00 MB** against the ~7 MB of writable QSPI,
leaving 2.81 MB. There is still no reason to pack the struct and pay for unaligned access.

### The format freeze

Two numbers are baked into `sizeof(Patch)`, and through it into the QSPI slot stride and every
pattern already in flash: **`kMaxLocks`** and **`ParamId::Count`**. Changing either invalidates
every stored pattern — `SaveHeader` rejects them rather than reinterpreting, which is the correct
behaviour and also means they are gone.

So both were chosen once, deliberately, **before Phase 6 writes anything real to flash**:

| | Value | Why not larger |
| --- | ---: | --- |
| `kMaxLocks` | **8** | 12 works but takes patterns to 5.5 MB and DTCM to ~87 kB; 16 overflows the chip and the `static_assert` in `storage_layout.h` catches it |
| `ParamId::Count` | **32** | Four pages of eight is what the panel can reach without menu diving (§7) |

Pages are nearly free and lock slots are not: a page of eight parameters costs 288 bytes on the
`Kit`, while each extra lock slot costs 4 bytes × 64 steps × 12 tracks ≈ 3 kB of `Patch`. That is
why most of the 32-entry parameter space is declared but reserved — the space is cheap, and
declaring it now is what avoids a second format break later.

**Anything that changes `sizeof(Patch)` belongs on the near side of that line.** After Phase 6 it
costs you your own patterns; after units ship it costs a migrator, or a firmware update that eats
someone else's work.

The four extra tracks are the analog cartridge slots. They carry steps, locks and micro-timing
like any other track whether or not a cartridge is plugged in — which is what lets the trigger
outputs drive external gear on their own, and what stops a pattern meaning something different
depending on what's in the slots. **The slot map lives in the kit, not the pattern**, so a pattern
loads and plays against whatever hardware is present.

Per-track `length` and `speed` give polymeter for free: a 7-step hat track against a 16-step
kick is one byte of state and the single highest ratio of musical interest to implementation
effort in the whole sequencer.

**Swing is per track for the same reason**, and moved off the pattern to get there. A swung hat
over a straight kick is the groove technique; a pattern-wide swing control cannot express it.
50 is straight, 75 pushes odd steps a quarter step late, and the range is capped there.

**Micro-timing is capped at ±23 ticks — just under one step.** At 24 ticks per 16th that's
±(23/24) of a step at 1/24-step resolution, about ±5 ms per tick at 120 BPM. Going further
breaks the data model's meaning: a step pushed a full step late is indistinguishable from the
next step early, and the UI has no honest way to draw it.

Because a step can move up to a step either way, the tick handler cannot assume *position ==
step*. It checks the neighbouring positions too — and must de-duplicate, since on a 1- or
2-step cycle that window wraps onto itself and would otherwise evaluate the same step (and roll
its probability) up to three times per tick.

**`micro` is clamped to the track's own step length at use, and that is load-bearing.** "±23 ticks,
just under a step" is only true while a step *is* 24 ticks. A track at double speed has 12 and at
quadruple speed 6, so the stored value reaches two positions and then four — past the ±1 window
that finds a displaced step, which then never finds it. Measured before the clamp: a speed-1 track
at maximum micro and swing played **4 of its 8 steps**, and a speed-2 track at maximum micro played
**none at all**. It is a silent failure at the intersection of three unrelated settings, it predates
swing, and a test now sweeps all three speeds for it.

**Parameter locks** are the feature worth building the data model around. Hold a step key, turn
a knob, and that knob's value is recorded for that step only. Eight lock slots per step, because
with four pages of eight parameters four slots is a rationing exercise rather than an expressive
limit.

`VoiceSlot::locked_mask_` carries one bit per parameter and **must be at least `ParamId::Count`
bits wide**. As a `uint8_t` it silently stopped restoring anything above index 7 the moment the
parameter space grew past one page — a `static_assert` now pins it.

The mechanism lives in `VoiceSlot`, which owns the **base** value of every parameter — what the
knob says — separately from what the voice currently holds:

1. On trigger, **restore first**: any parameter the *previous* step locked is written back to its
   base. This is what stops a lock leaking into every later step, which is the bug that makes a
   p-lock feel like it permanently moved the knob.
2. Then apply this step's locks and fire.

Only previously-locked parameters are touched, so an unlocked step costs zero parameter writes —
worth having when this runs per trigger inside the audio callback.

One subtlety: a knob moved *while* a lock is held must update the base without disturbing the
locked value, and the restore must then land on the **new** base. Otherwise the knob appears dead
until the next unlocked step, and then jumps back to where it used to be.

## 7. UI state machine

Modes, with `SHIFT` as a held modifier rather than a latched state:

| | 16 step keys | 8 macro encoders |
| --- | --- | --- |
| **PLAY** (default) | toggle steps on the selected track | the current page, for the selected track |
| **hold a step** | — | **write a p-lock** on that step |
| **SHIFT + hold a step** | — | **step detail** — velocity, micro, probability, ratchet |
| **SHIFT** alone | — | **master FX**, in two banks of eight |
| **MUTE** (held) | — (track keys mute/unmute) | unchanged |
| **PATTERN** (held) | load that slot; **SHIFT + step** saves to it | unchanged |
| **REC + play** | live record from track keys, quantise optional | live-record turns as locks |

**MUTE and PATTERN are held, not latched**, for the same reason SHIFT is: a held modifier has no
state to get stuck in, which matters more on something you play than saving a finger does.

One rule covers the three macro layers rather than three: **SHIFT makes a knob global — unless you
are already holding a step, in which case it makes it local to that step.**

| Control | Does |
| --- | --- |
| Nav encoder 0 | tempo; the pattern bank in PATTERN mode |
| Nav encoder 1 | page; the master-FX bank while SHIFT is held |
| Macro encoder push | that parameter back to its default |
| TAP | tap tempo, rolling average, restarted by a gap over 2 s |

Both nav encoders **clamp rather than reject** at their limits. A detent is normally ±1, but the
10 kHz scan coalesces a fast spin into a larger delta, and rejecting that would make a quick flick
near either end do nothing — which reads as a dead encoder rather than as a limit.

### The UI never touches flash

A QSPI write stalls for milliseconds (§8), and the patch has to be snapshotted by the audio side
before it can be written at all. So PATTERN mode does not save; it posts a `Ui::StorageRequest`
that the main loop collects with `TakeStorageRequest()` and turns into a snapshot plus a
`Storage` call. The UI stays pure logic and stays testable headless.

### Four pages of eight

Thirty-two parameters reach eight encoders, so one navigation encoder selects the page — INST,
FLTR, FX, LFO — and the eight macros address that page for the selected track.
`ParamAt(page, slot)` in `src/engine/params.h` is the whole mapping.

Two consequences worth stating. A page change **ends every gesture in flight**, for the same reason
a track change does: the encoders now point somewhere else, and a spin spanning the change would
write the old page's accumulated value to the new page's parameter. And a **reserved** parameter
emits no command and never claims the screen — the UI draws it as inactive, because a live-looking
knob that does nothing is precisely what [hardware §2](01-hardware.md#2-panel-layout) forbids.

Four pages is also the ceiling. The rule below — if a feature requires menu diving, it is
mis-designed — is what stops this becoming six.

### Encoders delete the pickup problem and add an acceleration one

Eight knobs address twelve tracks across four pages, so a *pot's* physical position is wrong the
instant you change either. That needed soft takeover — the parameter stays put until the knob sweeps through the
stored value — plus a screen affordance explaining why the knob appeared dead. All of it is gone:
an endless encoder has no position to disagree with, so `Ui` carries no `caught_` flags, no raw
positions and no pickup state, and a p-lock is immediate rather than something you sweep into.

What replaces it is **acceleration**, and it is not optional. A detented encoder gives ~24 steps
per revolution, so one fixed step size is either too coarse to tune a parameter or needs ten
revolutions to cross its range. `Ui::StepFor` picks the step from the interval between detents:
a deliberate click is 1/256, a spin is 1/16.

Two details that are easy to get wrong and unpleasant to debug:

- **A gesture accumulates from its own last value, not from the patch.** The queue is drained a
  block later, so re-reading the patch per detent would keep seeing a stale value and silently
  drop most of a fast spin.
- **The first detent of a gesture always re-seeds and is always fine.** Without that guard a turn
  at `now_ms_ == 0` sees a zero interval, reads as a fast spin, and starts from zero rather than
  from the stored value. Gestures are also dropped whenever what a macro points at changes — a
  track change, or entering and leaving lock mode — or a gesture spanning the change would write
  the old target's value to the new one.

**LED language** — consistent enough to read without thinking:

- Step key: off = inactive; dim→bright = velocity; hue = track colour
- Playhead: white, and it **wins over everything** — it is the one thing you track with your eyes
  while playing, so it must never be ambiguous
- Locked step: hue shifted toward white with a slow pulse. Measured at equal velocity, a locked
  step reads 0.42 whiteness against 0.12 for a plain one
- Track key: hue = track identity; dim = has content; bright = selected; red = muted
- An active step never renders below 25 % brightness, so velocity 1 is still visibly on

**The renderer enforces the current budget.** Thirty-four SK6812s at full white draw **2.0 A**, far
past any USB supply, and a per-LED clamp cannot see the total — only the whole frame knows the sum.
So `LedRenderer` scales the entire frame if it would exceed **400 mA**. The worst case the panel can
actually produce measures **396 mA**.

The screen *explains* — it shows the parameter name and value when you touch a knob, and the
pattern overview otherwise. It never becomes the only way to reach a function. If a feature
requires menu diving, it's mis-designed.

**There is no soft takeover, because there are no pots.** Six knobs addressing twelve tracks would
make a pot's physical position wrong the instant you change track, and the pickup behaviour that
fixes it — the parameter stays put until the knob sweeps through the stored value — lands hardest
on the p-lock gesture this whole data model exists for. Endless encoders have no position to
disagree with, so `Ui` carries no pickup state at all and a lock is immediate. See the encoder
notes above for what replaces it.

The screen shows the parameter, its value and a bar while a knob is turning, and says **LOCK** with
the step number when that value is going to a step rather than to the track:

```
BD   TUNE
LOCK step 7    52
████████████░░░░░░░░
```

Showing the track's value while locking would be actively misleading, so the display reads the
in-flight edit value rather than the patch.

## 8. Persistence

QSPI flash, laid out in slots. **Every number below is derived from `sizeof` in
`src/io/storage_layout.h`, not written down here** — an earlier version of this table was
hand-computed when tracks were 8, and when `Patch` grew from ~11.5 kB to 17.3 kB the 128 pattern
slots silently overran their region by 112 kB into the songs area. The header now carries
`static_asserts` for that, and the host test suite prints the map.

**The app image lives in QSPI too, and user data must start above it.** Under `BOOT_SRAM` the
image is staged at chip offset `0x40000` and can grow to the 480 kB SRAM limit, so the first
1 MB of the chip is off limits:

```
0x000000  reserved      (256 kB)  Daisy boot layout — do not touch
0x040000  app image     (480 kB)  staged here, copied to SRAM at boot
0x0B8000  slack         (288 kB)
─────────────────────────────────  user data starts at the 1 MB mark
0x100000  settings      (4 kB)      1 slot   × 4 kB
0x101000  kits          (128 kB)    32 slots × 4 kB
0x121000  patterns      (4.0 MB)    128 slots × 32 kB
0x521000  songs         (64 kB)     16 slots × 4 kB
0x531000  free          (~2.8 MB)   reserved for sample data
```

**Slots are 4 kB-aligned, and that is a correctness requirement rather than tidiness.**
`QSPIHandle::Erase` aligns its *start* address **down** to a 4 kB sector
(`lib/libDaisy/src/per/qspi.cpp`), so a slot starting mid-sector means saving slot N erases the
tail of slot N−1. `sizeof(Patch)` is 30,796 B, which rounds to a **32,768 B stride** — eight
sectors, 1.9 kB of it padding. Paying that is much cheaper than the alternative, and 128 slots
occupy 4.0 MB of a chip with ~2.8 MB left over.

The sample reservation shrank from 4.3 MB to 2.8 MB to pay for it, which is affordable because
[Phase 7](04-development-plan.md#phase-7--expansion) streams samples from **SD over SDMMC**, not
from QSPI. The free region is a convenience, not the sample plan.

`PersistentStorage::Init()` **defaults its offset to 0**, which would place settings directly on
top of the app image — the first save would corrupt the firmware executing it. Always pass the
offset explicitly:

```cpp
storage.Init(defaults, kSettingsBase);   // never Init(defaults)
```

### `PersistentStorage` is for settings only, never for a patch

`PersistentStorage<T>` is the obvious vehicle for saving a patch and it must not be used for one.
`StoreSettingsIfChanged()` declares its `SaveStruct` as a **stack local**
(`lib/libDaisy/src/util/PersistentStorage.h`), and it keeps two more full copies of `T` as members.
At `T = Patch` that is **17.3 kB of stack and 34.5 kB of `.bss`**.

Under `STM32H750IB_sram.lds` `.data`, `.bss`, the heap and the stack all share the **128 kB
DTCM**, and the script defines **no `_Min_Stack_Size`** — so there is nothing for the link to fail
against. A stack overflow there quietly scribbles over `.bss` instead, which is the same class of
fault the "`Machine` is never a stack local" rule exists to prevent, arriving via a library.

So:

- **Settings** — small, fixed, compared with `operator!=` — keep `PersistentStorage`.
- **Patterns and kits** — write through `QSPIHandle::Erase`/`Write` against a **static staging
  buffer**, one `Patch`-sized object owned by the storage module. It is already the shape the
  rest of the firmware uses, and the copy has to exist somewhere regardless; putting it in `.bss`
  deliberately is the difference between a known 17 kB and an invisible one.

### What is implemented

| | |
| --- | --- |
| `src/io/flash.h` | `IFlash` — the seam, mirroring `IVoice`/`IChannel`. Plus `RamFlash`, which models NOR semantics: erase sets **0xFF**, and a write can only **clear** bits |
| `src/io/storage.h` | `Storage` — slot arithmetic, header stamping, validation. Owns the one staging buffer, so like `Machine` it must never be a local |
| `src/io/settings.h` | `Settings` — serial number, MIDI routing, LED clamp, 512 bytes reserved for calibration, and the `operator!=` `PersistentStorage` requires |
| `src/platform/qspi_flash.h` | The libDaisy-backed `IFlash`. The only file outside `main.cpp` that includes libDaisy |

Modelling NOR's write-only-clears-bits rule in the fake is what makes the tests worth having: a
slot that is under-erased reads back as the **AND** of the old and new saves, behind a perfectly
valid header. A fake that just memcpy'd would pass.

Three things were checked against `lib/libDaisy/src/per/qspi.cpp` rather than assumed:

- **`Erase` takes an inclusive end address, not a length.** Passing a size clears one sector and
  leaves the rest of the slot holding the previous save.
- **It aligns the start down to 4 kB**, as §8 already said — confirmed at `qspi.cpp:443`.
- **It cannot overrun the slot.** The loop only uses a 64 kB block erase when the address is
  64 kB-aligned *and* at least 64 kB remains before `end_addr`, stepping in 4 kB sectors
  otherwise. So `Erase(addr, addr + stride - 1)` clears exactly `[addr, addr + stride)` and never
  catches a neighbour. Worth recording, because it is the first question anyone asks and it is not
  obvious from the signature.

Addresses are **chip offsets**. `Write` and `Erase` both mask with `0x0FFFFFFF` and `GetData` adds
`0x90000000`, so offsets and absolute addresses behave identically — the layout uses offsets.

### Saving needs a snapshot, not a memcpy

The main loop must not copy the patch itself. The audio side is its only writer (§4), so a copy
taken mid-edit can tear — and **a torn `Patch` passes every header check**, because the magic,
version and size are all still correct. It surfaces later as one wrong step: precisely the
"sounds like corruption" failure `SaveHeader` exists to prevent and structurally cannot catch.

So `Machine::RequestSnapshot(Patch*)` queues a command, the audio side copies at a block boundary,
and `SnapshotReady()` tells the main loop when it may write. The copy costs ~30 kB of memcpy inside
one audio block — about 4 % of a 667 µs block, once, on an explicit save.

### Why not a ValueTree

A JUCE-style `AudioProcessorValueTreeState` is the obvious reach for "how do we save patches",
and it is the wrong tool here. It exists to solve problems this machine does not have — host
automation, thread-safe parameter passing from a GUI thread, undo, dynamic parameter discovery —
and it brings observers, pointer chasing and allocation, all of which are hostile to an audio
callback that must not allocate.

Meanwhile `Pattern` is **trivially copyable**, so saving really is a `memcpy`. A tree would mean
writing a serialiser to get back to where we already are. (What does *not* follow is that
`PersistentStorage<Patch>` will do the writing — see above for why a 17 kB `T` is the wrong shape
for it.)

Two things a ValueTree *would* have given us are worth taking on their own:

**A static parameter descriptor table** (`src/engine/params.h`). One `constexpr` place that knows
each parameter's name and default, living in flash, costing nothing at runtime. That is what the
display and MIDI learn need.

**A versioned save format** (`src/io/patch.h`). This is the one that bites. Because saving is a
memcpy, adding a single field to `Step` silently reinterprets every pattern already in flash —
and the result *sounds like corruption*, not like a version mismatch, so it is unobvious enough
to cost an evening. Every save carries a header:

```cpp
struct SaveHeader { uint32_t magic; uint16_t version; uint16_t payload_size; };
```

On load, a mismatch is **rejected, never reinterpreted**; the caller falls back to defaults.

A **patch** is a `Pattern` plus a `Kit` — the sequence and all twelve tracks' base parameter
values. Kits also get their own slots, because a kit is worth reusing across patterns.

**Do flash writes from the main loop, never the audio callback**, and prefer to write on an
explicit save action rather than autosaving during playback. With `BOOT_SRAM` a QSPI write
won't fault, but the write still stalls for milliseconds — enough to starve an audio buffer if
you do it from the wrong context.

## 9. MIDI

Full implementation — three simultaneous transports (DIN in/out/thru, USB device, optional USB
host), a routing matrix that makes the machine a usable MIDI hub, the complete channel-voice and
realtime message set, MIDI learn, and a SysEx protocol that covers both pattern backup and
computer control.

The part that interacts with this document is **clock recovery**: incoming clock is timestamped in
the UART interrupt against the audio sample counter and fed through a PLL, so the sequencer runs
from a smoothed tempo estimate rather than raw clock edges. Triggering directly off clock bytes
would import the source's jitter — up to 1 ms from a USB source, which is quantised to USB frames —
and throw away the sample-accurate timing of §3.

See **[06-midi.md](06-midi.md)** for the full specification.
