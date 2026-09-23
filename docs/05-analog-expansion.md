# Analog Expansion Path

Analog voices are **cartridges**: one voice per card, four slots on a carrier board that hangs off
the expansion header, **additive to the eight digital voices** rather than replacing them. A
machine with a full carrier is a 12-track instrument.

Almost none of this needs building now — but a handful of items must be on the v1 PCB or you're
buying a board respin (~$175 for five and three weeks) to get them.

## 1. Why cartridges, and why additive

A fixed analog daughterboard commits you to a voice selection at layout time, which is the one
decision you have the least information about. Cartridges move that decision to runtime: build an
808-style bridged-T kick, listen to it for a month, and replace it without touching the carrier or
the firmware.

**Additive, not substituting.** Four cartridges take tracks 9–12 rather than displacing digital
voices. This is the cheaper choice in every direction:

- A missing cartridge is a silent track. No fallback logic, no "which voice is really on track 3",
  no patterns that mean different things depending on what's plugged in.
- An analog kick layered under the synthetic one is a good sound, and with additive tracks it's
  just two tracks triggered together rather than a special layering mode.
- The eight digital voices stay exactly as they are, so nothing in the existing engine changes
  shape.

The cost is 12 tracks everywhere — see §5.

## 2. Three hybrid architectures

These are independent and can coexist. Listed in increasing order of effort.

### A. Analog FX insert on the master bus — *free in v1*

Daisy renders everything, sends the master bus out through an analog filter / drive / compressor,
and takes it back in.

**The Seed3 has a stereo audio input that this design otherwise doesn't use.** Route it to a
rear jack pair and you have a full analog insert loop for the cost of two jacks. This is the
highest ratio of character to effort available, and it works on day one with no daughterboard —
you can patch a guitar pedal or a Eurorack filter into it.

### B. Cartridge voices triggered by the Daisy — *the 808 approach*

The rest of this document. The Daisy sequences each cartridge: fires a trigger pulse, sets CVs for
tune and decay, and takes the summed audio back through the same audio input.

An 808-style bridged-T bass drum is the sound that genuinely doesn't translate to DSP, and it's a
well-documented circuit. Hats and metallic percussion are six square oscillators and filters, which
the digital versions already nail — **build cartridges for the voices that gain from being analog,
not for a full analog kit.**

### C. Per-voice analog processing of the *digital* voices — *v2+, expensive*

Each digital voice gets its own analog filter/VCA. Requires the 8-channel DAC and individual outs
from the v2 list, plus eight analog channels and eight returns. High effort, and B gets most of the
same character for a fraction of the work. Deprioritise this.

## 3. The carrier

```
main board ──2×12── carrier ──┬── slot 0 [cartridge]
                              ├── slot 1
                              ├── slot 2
                              └── slot 3
```

Everything cartridge-specific lives on the carrier. **The v1 main board needs no new signals for
this** — the header below already carries all of it.

Per slot the carrier provides: one trigger line, five CV lines from its own DAC bank, one channel
of the I²C switch, a VCA channel, and power. The cartridge itself carries no digital bus beyond an
ID EEPROM — see §3.2 for why the DACs ended up here rather than on the cards.

### The expansion header — pinout fixed now

One 2×12 2.54 mm footprint. **This pinout is frozen**, which is what lets the carrier be designed
months later without touching the main board. Change it only with a respin in hand.

| Pin | Signal | Pin | Signal |
| ---: | --- | ---: | --- |
| 1, 2 | +5 V | 3, 4 | AGND |
| 5 | VIN (9–12 V raw) | 6 | PGND |
| 7–10 | TRIG 0–3 | 11–14 | TRIG 4–7 |
| 15 | SPI SCK | 16 | SPI MOSI |
| 17 | SPI CS_EXT | 18 | I²C SDA |
| 19 | I²C SCL | 20 | AUDIO RETURN |
| 21–24 | *reserved* — see §6.2 | | |

Pins 1–20 are populated in v1 and are the whole of what the cartridge design needs. The two extra
positions (pins 21–24) carry the SAI2 signals of §6.2 — `FS`, `SCK`, `SD_A` plus a ground, the
ground being there deliberately because a 3 MHz bit clock wants a return path next to it.

Two things to notice about what is *not* here. There is **no MISO** — nothing on the carrier needs
to talk back over SPI, because the DAC bank is write-only and cartridge identification goes over
I²C, which is bidirectional on one wire. And both buses are exposed even though the carrier design now
uses each for something specific, because when this pinout was frozen it wasn't yet known which
would be wanted. That insurance is what made the SPI-vs-I²C decision in §3.2 a free choice rather
than a respin.

### 3.1 Addressing — one I²C switch solves three problems

Put an **8-channel I²C switch (TCA9548A-class, ~$1)** on the carrier, one channel per slot. That
single part buys:

- **Identical cartridges.** Every card can use the same parts at the same default addresses, with
  no address straps and no per-slot BOM variation. Two of the same cartridge in two slots just
  works.
- **Presence detection.** "Does anything ACK on channel N."
- **Self-description.** A few-cent EEPROM (24AA02-class) per cartridge carrying its name, how many
  of the six macros it actually uses and what they map to, and CV calibration constants. A new
  cartridge type then needs no firmware change.

Self-description matters more than it looks. [Hardware §2](01-hardware.md#2-panel-layout) is
explicit that a knob doing nothing is worse than one doing something mild, so a cartridge declaring
only three useful parameters leaves three dead knobs. The EEPROM is what lets the UI know to map
the spare macros to something — or grey them out honestly.

### 3.2 CV — on SPI, and on the carrier

Two decisions here, and the second one was wrong in an earlier draft.

**SPI, not I²C.** This determines whether parameter locks work on cartridge tracks. An I²C
transaction is ~200 µs and cannot happen in the audio callback; an SPI DAC write is ~1–2 µs and
can. See §4.2 for why that difference lands exactly on p-locks.

**The DACs live on the carrier, not on the cartridges.** An earlier draft put an 8-channel DAC on
each cartridge and daisy-chained the four on the single `CS_EXT` line. That design breaks the
moment a slot is empty: **a daisy chain with removable links is severed by removing one**, so
pulling the cartridge out of slot 1 leaves slots 2–4 with no data. Empty slots are not an edge
case — §4.5 is explicit that a machine with no carrier at all is the normal starting state.

Three DAC8568-class chips on the carrier instead, permanently chained, their outputs fanned out to
the slots as CV lines:

| | DAC per cartridge | DACs on the carrier |
| --- | --- | --- |
| Chips | 4 | **3** |
| Chain survives an empty slot | **no** | yes |
| Cartridge carries | DAC + EEPROM + SPI | EEPROM only |
| Slot pins used | 4 (SPI) | 5 (CV) |
| Burst per write | ~16 µs | **~12 µs** |

The channel count lands exactly: 4 slots × 5 CVs (TUNE, DECAY, TONE, SNAP, DRIVE) plus 4 LEVEL CVs
for the quad VCA of §3.3 is **24 channels, precisely three chips**. LEVEL never reaches a
cartridge — it drives the carrier's VCA, which is also what applies velocity.

A write clocks through all three: 3 × 32 bits at 8 MHz is ~12 µs, 1.8 % of the 667 µs audio block.
At the current ~20 % CPU there is ample slack for that burst to land inside a single sample.
Measure it on the scope during bring-up rather than trusting the arithmetic, the same way
[§4.3](#43-trigger-timing) says to measure the trigger write.

Moving the DACs also makes a cartridge a much smaller thing — an EEPROM, a trigger input, five CV
inputs and the voice circuit, with no digital bus to get wrong. Full design in
[doc 10](10-cartridge.md).

### 3.3 Mixing — analog now, digital later

The four cartridges are summed on the carrier into the stereo return. Per-slot level comes from a
**quad VCA (SSM2164 lineage — V2164/AS2164, ~$3)**, one channel per slot, driven from the
cartridge's own DAC.

The alternative is digitising each cartridge separately so their level, pan and FX happen in DSP
like every other track. That is genuinely better, and since the encoder change it is **affordable**
— see §6.2. Build the VCA anyway: it is $3, it needs none of that to work, and per-slot analog
level stays useful even once DSP mixing exists.

| | Cost | Main-board pins | You get |
| --- | ---: | ---: | --- |
| Passive sum | ~$1 | 0 | One stereo pair. No per-track level, pan or FX |
| **Quad VCA + sum** | ~$3 | 0 | Per-slot level, velocity, sidechain ducking |
| Multi-channel ADC | ~$8 + rework | 3, now reserved | Full DSP parity, individual outs, per-track recording |

Why a VCA rather than a digital volume control: the obvious parts are digital pots (AD5206/AD8403)
or a dedicated volume control (TI PGA4311, 4-channel, ~$10–15). Both are stepped — 256 taps — which
zippers when a LEVEL lock moves it per step. The quad VCA is continuous, a third of the price, and
takes a CV from the DAC that's already there for tune and decay.

### 3.4 When to build it — schematic early, board late

**Draw the carrier schematic before the Phase 5 layout. Don't fab it.**

Those are separate decisions and they pull in opposite directions.

Drawing it early is close to free and it is the only way to find out the header pinout is wrong
while changing it still costs nothing. A signal you discover you need after the main board is in
copper costs a respin; the same discovery a week earlier costs an afternoon in KiCad. The pinout
above is a prediction about a board that doesn't exist, and predictions are worth testing.

Building it early is the opposite. The slot connector has to carry whatever a cartridge needs —
how many CVs, at what ranges, at what current, with what grounding — and **you do not know any of
that until you have built one analog voice.** Committing the slot pinout before then is
precisely the mistake §1 says cartridges exist to avoid, just moved down one level: a fixed
daughterboard commits you to a voice selection, and a prematurely fixed *slot* commits you to a
voice's electrical envelope. The difference is that a wrong carrier makes every future cartridge
wrong too.

So the order is: main board → one analog voice on perfboard, fed by the trigger outputs and a
bench supply → carrier designed around what that voice actually needed → cartridges. The
instrument is complete and playable at step one, which is what makes waiting affordable.

## 4. Firmware

### 4.1 `AnalogVoice` is just another `IVoice`

```cpp
class AnalogVoice : public IVoice {
    void Trigger(float velocity) override {
        cv_[LEVEL] = velocity;              // the VCA channel
        FlushDirtyCv();                     // SPI burst to the carrier, ~12 us
        gate_pending_ = true;               // latched into the 74HC595 this sample
    }
    void SetParam(ParamId id, float v) override { cv_[id] = v; dirty_ |= 1u << id; }
    float Process() override { return 0.f; } // audio arrives via the ADC input
};
```

The sequencer, parameter locks, macro knobs, mute groups, pattern storage and UI are all unchanged.
That is the entire payoff for defining [the `IVoice` seam](02-firmware.md#5-voice-engine) in Phase 2
rather than hard-coding eight DaisySP objects.

Cartridge voices are **statically allocated**, one per slot, like every other voice — `Machine` is
already too large to be a stack local and this doesn't change that.

### 4.2 Why the bus choice decides whether p-locks work

A pot move reaches a voice through: mux → ADC → main loop → SPSC ring → audio callback →
`VoiceSlot::SetBase` → `voice_->SetParam`. A parameter lock reaches it through
`VoiceSlot::ApplyLocks()`, which runs at `delay_ == 0` — **the same sample as the gate**.

If `SetParam` can only enqueue the CV for a main-loop I²C write, that write lands 1–2 ms after the
trigger. On a knob move that's imperceptible. On a p-lock it means the first millisecond of the hit
uses the *previous* step's tune and then slews into place, which on a kick with a fast pitch
envelope is audible and wrong — and parameter locks are the feature
[the data model exists for](02-firmware.md#6-sequencer-data-model).

Making I²C work would mean teaching `Sequencer::EmitTick` to emit CV-only events a step ahead for
analog tracks — including rolling probability early and remembering the outcome — plus an audio→main
path wider than the current relaxed-atomic `AudioState`. **Putting the DACs on SPI avoids all of
it**, because the write fits in the callback next to the trigger.

### 4.3 Trigger timing

Fire the 74HC595 write from the audio callback at the exact sample the voice's `trigger_delay`
counter reaches zero, using the same mechanism as digital voices. An 8-bit SPI burst at 8 MHz is
~1 µs against a 20.8 µs per-sample budget — affordable, but measure it rather than assuming. If it
turns out too costly, DMA the write with the offset pre-computed.

Do **not** fire gates at block boundaries. That reintroduces exactly the ±0.67 ms jitter the
timing model exists to avoid, and an analog kick is where you'd hear it most.

**Write the CVs before the gate, not with it.** A DAC output needs to settle and the analog circuit
needs its tune voltage stable when the trigger arrives. `FlushDirtyCv()` ahead of the gate inside
the same `Trigger()` call gives ~12 µs of lead, which is enough for a settling DAC but should be
confirmed against the cartridge's own CV input filter — a heavily filtered CV input needs more
lead than the DAC itself does.

### 4.4 Patterns survive a cartridge swap

Because every voice is six normalised macros, a p-lock recorded against one cartridge still means
something on another — TUNE is TUNE. **Store the slot map in the kit, not the pattern**, so a
pattern plays with whatever is plugged in rather than refusing to load.

### 4.5 Six knobs on an empty slot

A cartridge track with nothing plugged in has six macro knobs pointing at nothing, and
[hardware §2](01-hardware.md#2-panel-layout) is explicit that a knob doing nothing is worse than
one doing something mild. Since the panel ships with four cartridge tracks and the carrier is a
later phase, this is the *normal* state of a new machine, not an edge case.

Resist inventing a mapping. The macro labels are silkscreened — a knob marked TUNE that secretly
sets a gate width is worse than an inert one, because now it lies. Instead:

- **DECAY maps to gate width** on the trigger output. It is the one macro with an honest analogue
  on a bare gate, and external modules do care about pulse length.
- **The other five read as unavailable** on the screen, and the OLED names the slot as empty
  rather than showing six meaningless values.

The rule the labels have to satisfy is that a knob never silently does nothing. A knob the screen
explicitly says is unavailable satisfies it; a knob that appears to work and doesn't is the
failure the rule exists to prevent.

The track itself is fully useful meanwhile: steps, locks, length, speed, probability, ratchets and
micro-timing all work and all reach the 74HC595, so tracks 9–12 are a four-channel trigger
sequencer for Eurorack or an external drum module from the first power-on — which is only true
because the four spare trigger jacks are populated in v1. They were on the v2 list until this
section made a claim that needed them, at which point $4 of jacks was cheaper than the asterisk.

**Variable gate width needs a counter the timing model doesn't have yet.** `trigger_delay` counts
down to the moment a gate goes *on*; nothing counts it back off. DECAY-as-gate-width needs a
second per-track countdown, set at fire time and clearing the 595 bit when it expires. See
[firmware §3](02-firmware.md#3-timing-model).

### 4.6 Swap with the power off

Hot-plugging into a live summing bus pops, and inserting an unpowered card across ±12 V is worse.
Detect cartridges at boot, plus a manual rescan in the UI. This is a firmware and documentation
decision, not a connector one — don't spend money on sequenced-power contacts for it.

## 5. What 12 tracks costs

| | 8 tracks | 12 tracks |
| --- | ---: | ---: |
| `kNumTracks` (`src/seq/pattern.h:18`) | 8 | 12 |
| Pattern | 11.0 kB | 16.6 kB |
| `Machine` | ~15.5 kB | 21.5 kB |
| 128 patterns in QSPI | 1.38 MB | 2.07 MB of ~7 MB |
| Keys | 30 | 34 |
| CD4021s in the chain | 4 | 5 |
| SK6812s | 30 | 34 |

None of that is close to a limit. The fifth CD4021 costs **no pins** — it's a chain — and the four
extra LEDs are ~24 mA, though they do take the 5 V budget just past USB 2.0's 500 mA and make the
brightness clamp load-bearing ([hardware §4](01-hardware.md#4-power)). The real cost is panel area
for four more track keys, which is a [layout](01-hardware.md#2-panel-layout) problem rather than an
electrical one.

**The main board goes to 12 tracks in v1, carrier or no carrier.** The four cartridge keys, LEDs
and sequencer tracks are populated from the start: they drive the 74HC595 trigger outputs into
Eurorack or an external drum module on their own, and an empty slot is a silent track. Waiting for
the carrier to exist before widening the panel would mean a respin to add four keys later.

Four of the eight 74HC595 trigger lines go to cartridge slots; the remaining four still reach the
Eurorack jacks.

## 6. What v1 must reserve

### 6.1 Committed

| Reservation | Cost | Populate in v1? |
| --- | ---: | --- |
| 2.1 mm DC barrel jack + Schottky, diode-OR'd with USB 5 V | $1.50 | Footprint only |
| 2×12 expansion header footprint, 2×10 populated | $0.90 | Yes — pins 21–24 carry the SAI2 signals of §6.2 |
| 74HC595 → 8 trigger outputs | $0.60 | Yes — drives Eurorack and external drum modules immediately |
| 2 × 3.5 mm audio-in jacks | $2.00 | Yes — enables architecture A on day one |
| 4 × 3.5 mm trigger-out jacks | $4.00 | Yes — §4.5 claims the trigger tracks drive Eurorack from power-on; these are what make that true |
| **Total** | **$9.00** | |

**Power.** Analog audio circuits want **±12 V**, which is not viable from a 500 mA USB budget
alongside 34 RGB LEDs. Route a raw **VIN (9–12 V)** trace to the header and let each carrier make
its own rails locally, where they can be filtered next to the circuits that care. Do **not** put a
switching converter next to the audio codec on the main board.

**Ground discipline.** Already required by the LED design: separate analog and digital ground
pours joined at a single star point near the USB connector. Bring **AGND** to the header on its own
pins, separate from PGND. Getting this wrong means the carrier hums and you can't fix it without a
respin of both boards.

### 6.2 Digitising the cartridges — now affordable

Digitising the four cartridges separately, so their level, pan and FX happen in DSP like every
other track, needs a second audio input stream on **SAI2**. Under the pot-based control design
this was very likely impossible; the move to encoders changed that, and it is worth recording why
a UI decision unlocked an audio feature.

What libDaisy tells us, verified rather than recalled:

- Its only worked example of SAI2 on Seed pins is `daisy_patch.cpp:203-207`:
  `fs = D27, mclk = D24, sck = D28, sb = D25, sa = D26`.
- The alternate-function handling in `src/per/sai.cpp:407-416` is hardcoded — `GPIO_AF10_SAI2` for
  every pin, with PA2/D28 special-cased to AF8.
- `sai.cpp:198` calls `HAL_SAI_InitProtocol(..., 2)`. **Two slots, hardcoded.**

**Four channels therefore has exactly one route, and it is a libDaisy fork.** An earlier draft
offered two — TDM on one data line, or both data lines for the cost of a fourth pin — but the
second isn't real: the two data lines are `sa` (PD11/D26) and `sb` (**PA0/D25**), and D25 is
`DISP_RST`. Taking it means moving the OLED to an RC power-on reset, which is a display change to
buy an audio feature. So the honest position is that four-channel capture means patching
libDaisy's hardcoded slot count, and "a carrier upgrade rather than a respin" rests on being
willing to carry that fork.

**Two channels needs no fork.** If two of the four cartridges being individually digitised (and
the other two summed) is worth more than the fork is worth avoiding, that option is free.

**`SAI2_FS` exists only on PG9.** That pin was `ENC1_A` when the six macros were pots and the two
navigation encoders sat on direct GPIO, and nothing could be moved out of its way — every other
pin was spoken for. Putting all eight encoders on the 10 kHz CD4021 chain
([hardware §3.2](01-hardware.md#32-keys-and-encoders--two-cd4021-chains)) freed nine pins, PG9
among them, along with PD11 and PA2. **D26, D27 and D28 are now reserved for SAI2** in the pin map
and routed to header pins 21–24.

Two open questions, neither blocking:

| Question | Where it lands |
| --- | --- |
| Do the alternate functions match the STM32H750 table? | Confirm from the datasheet, not from libDaisy's hardcoded AF |
| Does the chosen ADC need MCLK? | `SAI2_MCLK_A` is PA1 = `DISP_DC`, not free. Prefer an ADC that derives its clocks from SCK |

Two consequences worth keeping in view. Four channels needs TDM on one data line — which means
patching libDaisy's hardcoded slot count — or both data lines, which costs a fourth pin. And
**digitising removes DRIVE and LEVEL from the CV path entirely**: they become DSP on the returned
audio, sample-accurate and free, cutting the CV count from 24 to 16 and taking the two most-swept
macros out of the analog domain.

If it all works, the quad VCA in §3.3 becomes optional rather than the answer. Build the carrier
with it anyway — it is $3, it works without any of the above, and per-slot analog level is useful
even when DSP mixing exists.

## 7. Open items

| Item | Settle in | Why it matters |
| --- | --- | --- |
| Game Boy shell and PCB dimensions, measured | Before the first cartridge layout | [Doc 10](10-cartridge.md) sizes the card from recalled figures |
| Edge-connector pitch, and single- or double-sided contacts | Before the first cartridge layout | Decides the slot pinout's usable width |
| Fab surcharge for hard gold on edge fingers | Carrier/cartridge quote | Not trivial, and it recurs per cartridge |
| SAI2 alternate functions vs. the H750 table | Phase 5, before layout | libDaisy hardcodes its AF; confirm rather than assume |
| Whether the chosen ADC needs MCLK | Part selection | `SAI2_MCLK_A` is `DISP_DC` and not free |
| DAC daisy-chain burst measured on the scope | Carrier bring-up | 16 µs is arithmetic, not measurement |
| Cartridge CV settling time vs. 16 µs of lead | Carrier bring-up | Too little lead smears the attack of every locked step |
| Panel layout for 34 keys | Phase 5 | The only real cost of 12 tracks |
| Carrier schematic drawn against the frozen pinout | Phase 5, before layout | The cheapest moment to discover a missing header signal |
| What a cartridge slot must carry electrically | After the first perfboard voice | Fixing it early commits every future cartridge to a guess |
