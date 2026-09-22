# Analog Expansion Path

Adding analog circuitry later is cheap **if v1 reserves the right things**, and expensive if it
doesn't. Almost none of this needs building now — but four items must be on the v1 PCB or you're
buying a board respin ($40 and three weeks) to get them.

## 1. Three hybrid architectures

These are independent and can coexist. Listed in increasing order of effort.

### A. Analog FX insert on the master bus — *free in v1*

Daisy renders everything, sends the master bus out through an analog filter / drive / compressor,
and takes it back in.

**The Seed3 has a stereo audio input that this design otherwise doesn't use.** Route it to a
rear jack pair and you have a full analog insert loop for the cost of two jacks. This is the
highest ratio of character to effort available, and it works on day one with no daughterboard —
you can patch a guitar pedal or a Eurorack filter into it.

### B. Analog voices triggered by the Daisy — *the 808 approach*

Analog drum circuits live on a daughterboard. The Daisy sequences them: fires a trigger pulse,
sets CVs for tune and decay, and takes their summed audio back through the same audio input.

This is the one worth planning for. An 808-style bridged-T bass drum is the sound that genuinely
doesn't translate to DSP, and it's a well-documented circuit.

### C. Per-voice analog processing — *v2+, expensive*

Each digital voice gets its own analog filter/VCA. Requires the 8-channel DAC and individual
outs from the v2 list, plus eight analog channels and eight returns. High effort, and B gets
most of the same character for a fraction of the work. Deprioritise this.

**Recommended first analog board: two voices (BD + SD) plus a stereo output filter/drive.** Not
a full eight-voice analog board — hats and metallic percussion are six square oscillators and
filters, which the digital versions already nail, so the analog versions buy you very little.

## 2. What v1 must reserve

### 2.1 Power headroom — the one that actually bites

Analog audio circuits want **±12 V**. Generating that from a 500 mA USB budget alongside 30 RGB
LEDs is not viable.

Add an unpopulated **2.1 mm DC barrel jack diode-OR'd with USB 5 V**, and route a raw
**VIN (9–12 V)** trace to the expansion header. Cost: one jack and one Schottky, about $1.50,
populated only when you build the analog board.

Do **not** put the ±12 V generator on the main board. A switching converter next to the audio
codec is a noise problem you'd be solving for no benefit until the analog board exists. Let the
daughterboard make its own rails from VIN, where you can filter them locally.

### 2.2 Expansion header

One 2×10 2.54 mm header, pinout fixed now so the daughterboard can be designed independently:

| Pin | Signal | Pin | Signal |
| ---: | --- | ---: | --- |
| 1, 2 | +5 V | 3, 4 | AGND |
| 5 | VIN (9–12 V raw) | 6 | PGND |
| 7–10 | TRIG 0–3 | 11–14 | TRIG 4–7 |
| 15 | SPI SCK | 16 | SPI MOSI |
| 17 | SPI CS_EXT | 18 | I2C SDA |
| 19 | I2C SCL | 20 | AUDIO RETURN |

Both SPI (for a DAC8568-class CV DAC) and I2C (for an MCP4728, digipots, or muxes) are exposed,
because you won't know which the analog board wants until you design it. Two pins of insurance.

### 2.3 Eight trigger outputs — populate these in v1

One **74HC595** on the SPI bus already running the OLED gives 8 trigger outputs for $0.40 and
one chip-select pin.

Populate this in v1 even though the analog board doesn't exist yet: eight trigger outs let the
machine drive Eurorack, a Volca, an SH-101's gate, or an external drum module immediately. It's
a feature on its own, not just future-proofing.

### 2.4 Audio return path

Route the Seed3's stereo audio input to a rear jack pair **and** to the expansion header (pin
20). Costs two jacks. Enables architecture A on day one and architecture B later.

### 2.5 Ground discipline

Already required by the LED design: separate analog and digital ground pours, joined at a single
star point near the USB connector. Bring **AGND** to the expansion header on its own pins (3, 4),
separate from PGND (pin 6). Getting this wrong means the analog board hums and you can't fix it
without a respin of both boards.

## 3. Firmware: analog voices cost almost nothing

This is where the `IVoice` seam from [firmware §5](02-firmware.md#5-voice-engine) pays off a
second time. An analog voice is just another implementation:

```cpp
class AnalogVoice : public IVoice {
    void Trigger(float velocity) override {
        gate_pending_ = true;               // fired at the exact sample offset
        cv_[TUNE]     = params_[TUNE];      // written to the external DAC
        cv_[DECAY]    = params_[DECAY];
    }
    float Process() override { return 0.f; } // audio arrives via the ADC input
};
```

The sequencer, parameter locks, macro knobs, mute groups, pattern storage and UI are all
unchanged — an analog voice is p-lockable and sequencable the moment it exists. That is the
entire payoff for defining the interface in Phase 2 rather than hard-coding eight DaisySP
objects.

**Trigger timing:** fire the 74HC595 write from the audio callback at the exact sample the
voice's `trigger_delay` counter reaches zero, using the same mechanism as digital voices. An
8-bit SPI burst at 8 MHz is ~1 µs against a 20.8 µs per-sample budget — affordable, but measure
it rather than assuming. If it turns out too costly, DMA the write with the offset pre-computed.

Do **not** fire gates at block boundaries. That reintroduces exactly the ±0.67 ms jitter the
timing model exists to avoid, and an analog kick is where you'd hear it most.

## 4. Cost of the insurance

| Item | Cost in v1 | Populated in v1? |
| --- | ---: | --- |
| DC barrel jack + Schottky | $1.50 | No — footprint only |
| 2×10 expansion header | $0.80 | Yes |
| 74HC595 + passives | $0.60 | Yes — useful immediately |
| 2 × 3.5 mm audio-in jacks | $2.00 | Yes — enables the analog insert loop |
| **Total** | **$4.90** | |

Five dollars against a $40 respin and three weeks of lead time.
