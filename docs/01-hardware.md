# Hardware Design

## 1. Board choice: Daisy Seed3

Three Daisy boards are currently sold: **Seed3** ($29.99), **Seed2 DFM** ($29.99), and the
**Patch Submodule** ($39.99, sold out at time of writing).

**Use the Seed3.** Reasoning:

| | Seed3 | Seed2 DFM | Patch SM |
| --- | --- | --- | --- |
| Mounting | Through-hole 2×20 headers | SMT castellated | SMT castellated |
| Prototyping | Breadboard + socket | Reflow required | Reflow required |
| Audio out | Codec is TI TAC5242, 32-bit/192 kHz, −120 dB noise floor | Differential I/O | Single-ended + Eurorack CV circuitry |
| Extra hardware | None | None | Eurorack CV in/out you'd pay for and not use |

Seed3 specs that drive the rest of this design:

- ARM Cortex-M7 @ 480 MHz (STM32H750)
- 65 MB RAM (64 MB SDRAM + internal) — a 10-minute audio buffer, i.e. samples are never memory-bound
- 8 MB QSPI flash
- **31 GPIO, 12 × 16-bit ADC, 2 × 12-bit DAC** — this is the budget everything below fits into
- USB-C, pin-to-pin compatible with the original Seed footprint

Two Seed3 caveats worth knowing up front:

- The **JTAG header ships unpopulated** (the 2×20 main pin headers do come pre-soldered). Fit a
  **10-pin 2×5 1.27 mm** header *centred on the 14-position footprint* — the outer two positions
  each side are unwired alignment aids for a 14-pin ST-LINK-V3MINIE cable, so soldering flush to
  one end misaligns every signal. Do this on day one, or you will be debugging a real-time audio
  system with printf over USB.
- The **output stage is unverified** in this plan. The TAC5242 supports both differential and
  single-ended output, and the Seed3 schematic does not state which configuration Electrosmith
  used. Download the Seed3 schematic PDF and confirm before committing the output stage to
  copper (see [Phase 5](04-development-plan.md#phase-5--pcb-34-weeks-mostly-waiting)). If differential, the output
  jacks need a differential receiver (or one side terminated), not a plain DC-blocking cap.

## 2. Panel layout

```
  ┌────────────────────────────────────────────────────────────────────┐
  │   ┌──────────────────────┐                                         │
  │   │  128×64 OLED         │      (ENC1)        (ENC2)               │
  │   │  SSD1309 2.42"       │      value/tempo   nav/page             │
  │   └──────────────────────┘                                         │
  │                                                                    │
  │   (E1) (E2) (E3) (E4) (E5) (E6) (E7) (E8)          (VOL)           │
  │   ─ eight macros, meaning set by the current page ─  master, analog │
  │                                                                    │
  │   [BD] [SD] [CH] [OH] [LT] [CP] [RS] [FM]     ← 8 digital tracks   │
  │   [C1] [C2] [C3] [C4]                         ← 4 cartridge tracks │
  │                                                                    │
  │   [PLAY] [REC] [SHIFT] [PATT] [SONG] [TAP]    ← 6 transport keys   │
  │                                                                    │
  │   [1][2][3][4][5][6][7][8][9][10][11][12][13][14][15][16]          │
  │                                        ← 16 step keys, RGB backlit │
  └────────────────────────────────────────────────────────────────────┘
```

**34 keys total** (16 step + 12 track + 6 transport), all RGB-backlit, **10 encoders** (8 macro +
2 navigation, all with push switches), and **one analog master volume pot**.

Tracks 9–12 are the analog cartridge slots from [doc 05](05-analog-expansion.md) — additive to the
eight digital voices, not substitutes. Their keys are populated in v1 even before a carrier exists:
the sequencer tracks work regardless, driving the 74HC595 trigger outputs into Eurorack or an
external drum module, and an unpopulated slot is simply a silent track.

### The macros, and the pages they address

The eight macro encoders are *macros*, not per-voice controls. They always address the currently
selected track, and one of the two navigation encoders selects which **page** of eight they reach.
Four pages, 32 parameters:

| Page | What the eight knobs are |
| --- | --- |
| **INST** | TUNE · DECAY · TONE · SNAP · DRIVE · LEVEL · PAN · *reserved* |
| **FLTR** | SAT · CUT 1 · RES 1 · MODE 1 · CUT 2 · RES 2 · MODE 2 · *reserved* |
| **FX** | DLY SND · REV SND · *six reserved* |
| **LFO** | SPEED · MULT · FADE · DEST · WAVE · MODE · DEPTH · PHASE |

Most of FX, and the two reserved slots, are declared but not wired to anything yet. That is
deliberate: the parameter count is baked into `sizeof(Patch)` and therefore into every pattern in
flash, so the space was frozen at 32 before Phase 6 rather than grown later — see
[firmware §6](02-firmware.md#6-sequencer-data-model). The UI draws a reserved slot as inactive
rather than as a live knob that does nothing.

**Eight rather than six is what makes the pages work.** At six the LFO page does not fit — Elektron
run that page at eight and so must we — and the filter page wants seven. Electrically it costs one
more CD4021 and no pins ([§3.2](#32-keys-and-encoders--two-cd4021-chains)); mechanically it costs
nothing, because the macro row grows from 175 mm to ~225 mm on a board that is 321 mm wide for
other reasons ([§5](#5-mechanical)).

The INST page is where the fixed mapping is load-bearing:

| Knob | BD | SD | CH / OH | CP | RS | FM |
| --- | --- | --- | --- | --- | --- | --- |
| TUNE | base freq | base freq | base freq | body freq | pitch | carrier |
| DECAY | decay | decay | decay | tail length | decay | decay |
| TONE | tone/attack | tone | filter | filter | filter | ratio |
| SNAP | punch | snappy | metallicity | spread | mix | index |
| DRIVE | drive | drive | drive | drive | drive | drive |
| LEVEL | level | level | level | level | level | level |
| PAN | placement in the shared stereo field — identical on every voice | | | | | |

This is the single most important UX decision in the build. 12 tracks × 6 params = 72 knobs if
done literally; the macro mapping gets you the same control surface for 8 knobs and makes muscle
memory transfer between voices. Every voice must implement every parameter on a page it uses, even
where the mapping is a stretch — a knob that does nothing on some tracks is worse than a knob that
does something mild.

**That rule is about a knob being dead on *some* tracks.** A slot left unused consistently across a
whole page is a different thing and is honest, which is why the eighth INST slot stays reserved
rather than being filled with an invented parameter.

Cartridges can't be tabulated here because the point of them is that they change. Each one carries
its own mapping in an on-board EEPROM and the UI reads it at boot
([doc 05 §3.1](05-analog-expansion.md#31-addressing--one-ic-switch-solves-three-problems)) — which
is also what keeps the rule above enforceable on hardware that didn't exist when this table was
written.

**Why encoders rather than pots, and why one pot anyway.** Six knobs addressing twelve tracks
means a pot's physical position is wrong the moment you change track, which needs soft-takeover:
you turn, nothing happens, and you have to sweep to the stored value before the knob engages.
That is tolerable on a synth and bad on a sequencer, because it lands hardest on the p-lock
gesture — hold a step, nudge a value — which is the feature the whole data model exists for.
Endless encoders have no position to disagree with, so the pickup problem disappears along with
the code that managed it, and a p-lock becomes immediate.

The exception is **master volume**, which stays a real pot in the **analog output path** between
the codec and the headphone amp. A physical volume control wants absolute position you can read
at a glance and grab in a hurry, it should keep working regardless of what the firmware is doing,
and in the analog path it costs **zero pins** and zero latency. The firmware never sees it.

## 3. I/O topology

The pin budget is the constraint that makes or breaks this design, so it's worked out explicitly.

### 3.1 Analog inputs → almost none left

With the macros on encoders, the only pot in the design is **master volume, and it is not wired to
the MCU at all** — it sits in the analog output path (§2). That deletes the pot multiplexer, both
CD4051s and their three select lines from the earlier design.

What remains is CV and expression input, and there are now enough free ADC pins to take them
**directly**: D15 and D16 are ADC-capable and unassigned, so two CV/expression jacks need no mux
and no select lines. A CD4051 only earns its place when you need more analog inputs than spare ADC
pins, and after the encoder change that is no longer true — there are seven spare ADC pins for at
most two jacks.

Leave the jack footprints unpopulated in v1. They cost nothing and the pins are already free.

### 3.2 Keys and encoders → two CD4021 chains

**58 inputs on 4 pins**: 34 keys, 16 encoder quadrature lines and 8 encoder push switches.

Two chains sharing clock and latch, with a data line each:

| Chain | Inputs | Chips | Sampled |
| --- | ---: | ---: | --- |
| Encoders | 20 quadrature + 10 push | 4 × CD4021 | 10 kHz |
| Keys | 34 | 5 × CD4021 | read at 10 kHz, decimated to 1 kHz |

`SR_CLK` and `SR_LATCH` drive both; `ENC_DATA` and `KEY_DATA` come back separately. 40 bits at
10 kHz is a 400 kHz shift clock, comfortable for a CD4021 at 3V3. **Lengthening a chain costs no
pins** — that is the whole point of a shift register, and it is why 64 inputs cost one pin more
than 30 did.

Going from six macros to eight added a fourth chip to the encoder chain and changed nothing else.
The chains are clocked together and the **key** chain is the longer one at 40 bits, so taking the
encoder chain from 24 to 32 bits leaves the shift clock exactly where it was. The Phase 1
lost-counts risk is unchanged.

Use the CD4021 specifically, not the more common 74HC165 — libDaisy ships a
`ShiftRegister4021` driver in `src/dev/sr_4021.h` that is templated on chain length, so this is
zero custom code. The 74HC165 is faster and cheaper but has inverted latch polarity and you'd be
writing and debugging your own driver for no benefit.

**The decimation is a real trap.** Debounce with libDaisy's `Switch` class, whose 8-bit
shift-register debounce tracks state *per call* — so it must still be called at **1 kHz** even
though the chain is now sampled ten times faster. Calling it at 10 kHz shortens the debounce
window tenfold and lets contact bounce through as repeated presses.

### 3.3 Encoders → the fast chain, not direct GPIO

An earlier draft put encoders on direct GPIO and warned specifically against the shift-register
chain. **Read that warning carefully: it was about the rate, not the chain.** A fast flick
generates quadrature edges faster than a *1 kHz* scan resolves, and you lose counts — which feels
like a broken knob rather than a sampling artifact. A detented encoder turned hard produces
roughly 500 edges/s, so 1 kHz is marginal and 10 kHz is not.

Eight encoders on direct GPIO would need **16 pins**, which the budget does not have and never
did. On the 10 kHz chain they cost nothing beyond the chips.

**This must be proved on the breadboard in [Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks)**,
because it changes the PCB. **Fallback if the fast scan still drops counts:** a ~$1 I²C
co-processor (ATtiny/STM32G0-class) that decodes all eight encoders and exposes counts over I²C.
Same pin cost, but a second firmware, a second toolchain and a second flashing path — worth
avoiding if the chain works, and a real answer if it doesn't.

The **push switches** go on the same chain: a button press is slow, so there is no rate question.
Eight free buttons is a genuine gain over pots — push-to-default on a macro is the obvious use.

### 3.4 LEDs → SK6812 chain on SPI+DMA

34 RGB LEDs on **1 data pin**. SK6812 MINI-E is the reverse-mount part used in the mechanical
keyboard world; it sits under a Cherry MX switch and lights a translucent keycap.

The 800 kHz one-wire protocol is driven by encoding each LED bit as 3 SPI bits at 2.4 MHz and
pushing the buffer with DMA — standard technique, ~100 lines, zero CPU after the transfer
starts. libDaisy has no stock driver for this; it's the one piece of custom hardware code
in the build.

**The reverse-mount choice is not settled.** A top-mount SK6812 MINI lights the same cap through
the same switch cutout and is a far easier part to buy at 3,400 pieces — see
[production §2](11-production.md#open-question-does-the-led-have-to-be-reverse-mount). Decide it at
the Phase 5 schematic; it changes the footprint but nothing above it.

> **Fallback if the DMA driver fights you:** 2 × **TLC5947** (24-channel 12-bit PWM, SPI,
> daisy-chainable). Hardware greyscale, no timing sensitivity, ~$8. You lose RGB and gain
> certainty. Decide by the end of [Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks) —
> it changes the PCB.

Two gotchas that will cost you an evening each if missed:

- **Level shifting.** SK6812 at 5 V needs V_IH ≈ 3.5 V; the Seed3 drives 3.3 V. Put a
  **74AHCT125** between them ($0.50). The "just try it" approach works on the bench at room
  temperature and fails in the enclosure.
- **Current.** 34 LEDs at full white is 34 × 60 mA = **2.0 A** — far beyond USB budget. Clamp
  global brightness in software (~25%) and never render full white. At 25 % single-hue you're
  around 205 mA, which is fine. Add a **1000 µF bulk cap** on the LED rail and 100 nF per LED.

### 3.5 Display, MIDI, audio

- **OLED**: SSD1309 2.42" 128×64, SPI — **5 pins** (SCK, MOSI, CS, DC, RST). libDaisy's
  `OledDisplay<SSD130x4WireSpiTransport>` drives it directly. The 0.96" SSD1306 is the same
  driver and half the price, but it's too small to read parameter values from playing distance.
- **MIDI**: UART — **2 pins**, for **In, Out and Thru**. H11L1 optoisolator on input,
  74HCT14 buffering Out and Thru (six gates, two each — one chip covers both). TRS Type-A jacks
  (3.5 mm) rather than 5-pin DIN: smaller, and the standard since 2018. Thru is buffered straight
  off the opto, so it costs **no MCU pin** and keeps working even if the firmware is wedged.
  USB MIDI device comes free on the USB-C port. Full spec in [06-midi.md](06-midi.md).
- **Audio**: stereo line out on the Seed3's codec pins, plus a headphone amp (TPA6132A2)
  on a 3.5 mm jack. Output stage detail pending the schematic check in §1.

### 3.6 Pin map

Derived from libDaisy's own peripheral tables, not from the datasheet by eye. Three constraints
did most of the work.

**`SPI2` cannot be used.** Its only SCLK pin is PD3, which is not broken out on the Seed
footprint. An earlier draft put the LED chain on SPI2. The usable instances are **SPI1, SPI3 and
SPI6**.

**SDMMC 4-bit collides with SPI3.** The SD card's 4-bit lanes are PC8/PC9/PC10/PC11/PC12/PD2,
and SPI3 needs PC10 for SCLK and PC12 for MOSI. Running the card in **1-bit mode** (PC8, PC12,
PD2 only) frees three pins. 1-bit SDMMC still moves several MB/s against the ~200 kB/s a stereo
48 kHz sample stream needs, so nothing is lost.

**The trigger-out shift register cannot share the display's SPI bus.** The 74HC595 shifts in
every byte that crosses MOSI, and the OLED is drawn from the main loop while triggers fire from
the audio callback — so an OLED transfer landing between loading the 595 and latching it would
emit whatever the display happened to be drawing as gate pulses. The 595 gets three dedicated
pins instead, taken from the lanes the 1-bit SD card freed.

| Daisy | STM32 | Net | Function |
| --- | --- | --- | --- |
| D0 | PB12 | `SR_CLK` | CD4021 clock, both chains |
| D1 | PC11 | `TRIG_DATA` | 74HC595 trigger outs |
| D2 | PC10 | `TRIG_CLK` | |
| D3 | PC9 | `TRIG_LATCH` | |
| D4 | PC8 | `SD_D0` | SDMMC 1-bit (phase 7) |
| D5 | PD2 | `SD_CMD` | |
| D6 | PC12 | `SD_CLK` | |
| D7 | PG10 | `SR_LATCH` | CD4021 latch, both chains |
| D8 | PG11 | — | SPI1 SCK: driven by the peripheral, **do not route** (test point only) |
| D9 | PB4 | `KEY_DATA` | 5 × CD4021, 34 keys |
| D10 | PB5 | `LED_DATA` | SPI1 MOSI → 74AHCT125 → SK6812 |
| D11 | PB8 | `ENC_DATA` | 4 × CD4021, 10 encoders + push |
| **D12** | PB9 | — | **spare** |
| D13 | PB6 | `MIDI_TX` | USART1 |
| D14 | PB7 | `MIDI_RX` | USART1 |
| **D15** | PC0 · **A0** | `CV_IN_1` | direct ADC, footprint only |
| **D16** | PA3 · **A1** | `CV_IN_2` | direct ADC, footprint only |
| **D17** | PB1 · **A2** | — | **spare, ADC-capable** |
| D18 | PA7 · A3 | `DISP_MOSI` | SPI6 MOSI |
| **D19** | PA6 · **A4** | — | **spare, ADC-capable** |
| **D20** | PC1 · **A5** | — | **spare, ADC-capable** |
| **D21** | PC4 · **A6** | — | **spare, ADC-capable** |
| D22 | PA5 · A7 | `DISP_SCK` | SPI6 SCLK |
| D23 | PA4 · A8 | `DISP_CS` | |
| D24 | PA1 · A9 | `DISP_DC` | |
| D25 | PA0 · A10 | `DISP_RST` | |
| **D26** | PD11 | — | **reserved: SAI2 SD_A** (see §6) |
| **D27** | PG9 | — | **reserved: SAI2 FS_B** |
| **D28** | PA2 · **A11** | — | **reserved: SAI2 SCK_B** |
| D29 | PB14 | — | **reserved: USB MIDI host D−** |
| D30 | PB15 | — | **reserved: USB MIDI host D+** |

**18 assigned, 5 reserved, 7 spare, 1 consumed-but-unrouted.**

That is a very different budget from the pot-based design, which ran 26 assigned with 2 spare.
Moving the six macros onto encoders and onto the shift-register chain freed nine pins — the pot
mux input, the second mux input, three shared select lines and four direct encoder GPIOs — and
spent one on `ENC_DATA`.

Notes on the choices:

- **The three SAI2 pins are the point of the exercise.** `SAI2_FS` is only available on PG9,
  which used to be `ENC1_A`; freeing it, with PD11 and PA2 also now free, makes a second audio
  input stream possible without touching the display or spending an ADC spare. That is what
  decides whether analog cartridges can be digitised individually — see §6.
- **ADC is only on D15–D25 and D28.** Two CV/expression inputs sit directly on D15/D16 with no
  multiplexer; four more ADC-capable pins remain spare.
- **LEDs on SPI1, display on SPI6**, deliberately that way round. The SK6812 driver needs only
  MOSI, so its bus clock is wasted — and SPI1's clock (PG11/D8) is *not* ADC-capable, while
  SPI6's (PA5/D22) is. Swapping them would throw away an ADC pin for nothing.
- **`SR_CLK` and `SR_LATCH` drive both CD4021 chains**, with a data line each. Clocking them
  together is what keeps 58 inputs down to four pins (§3.2).
- **Nothing touches D29/D30.**

## 4. Power

Single **USB-C** at 5 V into the Seed3, which regulates its own 3V3. Budget:

| Rail | Load | Current |
| --- | --- | --- |
| 5 V | Seed3 + codec | ~250 mA |
| 5 V | 34 × SK6812 @ 25 % brightness | ~205 mA |
| 5 V | OLED | ~30 mA |
| 3V3 | Logic (mux, SRs, level shifter) | ~20 mA |
| | **Total** | **~505 mA** |

**The four cartridge track LEDs push this just past USB 2.0's 500 mA**, so the brightness clamp is
now load-bearing rather than merely prudent — drop it to ~23 % and you're back under, and any USB-C
supply makes the question moot. Worth knowing before someone "temporarily" raises the clamp to
debug an LED and browns out the codec.

Analog cartridges never draw from this rail: the carrier makes its own ±12 V from VIN
([doc 05 §6.1](05-analog-expansion.md#61-committed)).

**A metal enclosure bonds to the star ground at exactly one point, and the jacks are where that
goes wrong.** There are **twelve** of them — 2 line out, 1 headphone, 3 MIDI, 2 audio in, 4 trigger
out. A metal-bushing jack bolted through a metal panel grounds its sleeve to the chassis, so twelve
of those is twelve parallel ground paths and the result is hum that cannot be fixed without taking
the instrument apart.

The BOM is already right: **PJ-320D and PJ-612A are plastic-bodied PCB-mount**, so their sleeves
stay isolated and the panel only needs clearance holes. Keep them. The Neutrik NMJ4HCD2 offered as
an alternative in [doc 08](08-sourcing-sweden.md) has a metal bushing — a nicer jack, and the wrong
choice here unless the chassis is deliberately designed as the ground plane.

Keep the LED return current
off the analog ground: **star-ground at the USB connector**, separate the LED ground pour from
the audio ground pour, and join them at one point. 34 LEDs PWMing at audio rates into a shared
ground plane is an audible buzz, and it is very hard to fix after layout.

## 5. Mechanical

**The panel is a 1.5 mm metal plate, and the switches mount into it.** That is a change from an
earlier draft where the PCB itself was the front panel; it resolves the hot-swap socket problem
below, and it is where the design is going anyway.

### Board size — the number that sets everything else

**321 × 187 mm.** Worth stating plainly because an earlier BOM line said "~180 × 100 mm" and priced
five 4-layer boards at $40, which is out by **3.3× in area** and much more in cost.

What sets the width is *not* the step keys, which is the intuitive answer and the wrong one:

| Row | Width |
| --- | ---: |
| 16 step keys @ 19.05 mm | 304.8 mm |
| **12 track keys** | **228.6 mm** ← the floor |
| 8 macros + volume | ~225.0 mm |
| OLED + 2 nav encoders | 138.0 mm |
| 6 transport keys | 114.3 mm |

Two rows of 8 steps would narrow the board to 245 mm and make it taller, for a **16 %** area saving
— not the halving you would guess, because the twelve track keys still have to fit. One row of 16
is the x0x idiom, reads without counting, and costs very little more. **Don't re-litigate this at
layout.**

At either size the board is too large to panelise, so there is no assembly efficiency to recover
there, and it sits in a pricing tier well above hobby sizes.

### Build the plate in FR4 before you build it in metal

The plate holds 34 switches, and its cutouts have to line up with 34 PCB footprints inside about
0.2 mm. Order a **1.5 mm FR4 plate from the same PCB run** — same fab, same CAD, roughly $10 for
five — and prove the geometry before anything is cut in aluminium:

- 34 switch cutouts against 34 footprints
- the plate-to-PCB standoff stack
- encoder shaft and bushing length through panel plus gap
- the OLED window

FR4 is a legitimate MX plate material, not a mock-up, so the first units can ship on it. Metal is
then a cosmetic and structural upgrade over a geometry already verified, rather than a $150 bet.

### What the plate decides, before layout rather than after

- **1.5 mm thickness is not a preference.** MX clip geometry is designed around it; thicker and the
  clips do not engage.
- **Plate-to-PCB spacing** (~5 mm for MX) sets the standoff height the layout has to allow for.
- **Encoder shaft length** must clear panel plus gap. The PEC11R ships in several shaft and bushing
  lengths, and ordering the wrong one costs a week for no good reason.
- **The PCB stops needing a cosmetic finish.** Black soldermask and white silkscreen legends existed
  to make the board presentable as a panel. Legends now go on the metal — engraved, screen-printed
  or anodised.

The LEDs are unaffected: the switch body passes through the plate cutout, so the SK6812 still
shines up through the switch's own window into the keycap.

### Hot-swap sockets now have the plate they assume

Kailh sockets expect a plate to take the insertion force; with the switch soldered to nothing and
the board unsupported, the socket's solder joints are the only retention and the board flexes on
every swap. The plate solves that outright — it is how a mechanical keyboard is built, and it is
why the sockets are worth having rather than a liability.

Switches: Cherry MX / Gateron with clear or translucent housings (the SMD LED cutout in the switch
body is what the SK6812 shines through), plate-mount.

Pots: one Alpha 9 mm dual-gang (RD901F) for master volume, with a Davies-1900-style knob. Encoders:
Bourns PEC11R with detents and a push switch.

## 6. Reservations for analog expansion

Analog voices arrive as **cartridges** — one voice per card, four slots on a carrier board that
hangs off the expansion header, additive to the eight digital voices. Full detail in
[docs/05-analog-expansion.md](05-analog-expansion.md); the short version is that the carrier holds
everything cartridge-specific, so **v1 needs no new signals for it** — only these four items, or
adding it later costs a board respin:

| Reservation | Cost | Populate in v1? |
| --- | ---: | --- |
| 2.1 mm DC barrel jack + Schottky, diode-OR'd with USB 5 V | $1.50 | Footprint only — analog circuits need ±12 V, which USB can't supply |
| 2×12 expansion header footprint (5 V, VIN, AGND, 8 triggers, SPI, I2C, audio return) | $0.90 | 2×10 populated; pins 21–24 carry the SAI2 signals below |
| 74HC595 → 8 trigger outputs | $0.60 | Yes — four go to cartridge slots, four to Eurorack |
| Stereo audio **input** jacks | $2.00 | Yes — the Seed3's unused audio input becomes an analog FX insert loop |
| 4 × 3.5 mm trigger-**out** jacks | $4.00 | Yes — otherwise the eight trigger outputs reach an unpopulated header |

**$9.00 against a ~$175 respin and three weeks of lead time.** The ±12 V generator itself belongs
on the carrier, not here — a switching converter next to the audio codec is a noise problem
you'd be solving before there's any benefit.

### The SAI2 pins are now reserved, not wished for

Cartridges are summed to a stereo pair on the carrier, which is why the reservations above stay
cheap. Digitising them *individually* instead — so their level, pan and FX happen in DSP like every
other track — needs a second audio input stream on **SAI2**, and until the encoder change there
were no pins for it.

There are now. `SAI2_FS` is only available on **PG9**, which was `ENC1_A` under the pot design and
is free under this one; `SAI2_SCK_B` (PA2) and `SAI2_SD_A` (PD11) are free too. **D26, D27 and D28
are reserved for this in the pin map** — three signals, no display rework, no ADC spare spent.

Two things still to confirm in [Phase 5](04-development-plan.md#phase-5--pcb-34-weeks-mostly-waiting)
before layout, neither of them a blocker:

- **The alternate functions.** libDaisy hardcodes `GPIO_AF10_SAI2` for every pin with PA2
  special-cased to AF8 (`src/per/sai.cpp:407-416`), which matches the pin set above — but confirm
  against the STM32H750 table rather than against libDaisy's assumptions.
- **Whether the external ADC needs MCLK.** `SAI2_MCLK_A` is PA1, which is `DISP_DC` and not free.
  An ADC that derives its clocks from SCK avoids the question; one that demands MCLK would need
  the display moved to 3-wire SPI. Pick the part with this in mind.

Route D26/D27/D28 to the two spare header positions (pins 21–24, three signals plus a ground) and
digitising becomes a carrier-board upgrade rather than a respin. Reasoning and consequences in
[doc 05 §6.2](05-analog-expansion.md#62-digitising-the-cartridges--now-affordable).
