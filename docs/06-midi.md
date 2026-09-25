# MIDI

Full MIDI implementation: three simultaneous transports, a routing matrix, complete channel-voice
and realtime message support, SysEx backup and control, MIDI learn, and a clock recovery scheme that doesn't
smear the timing the [sequencer](02-firmware.md#3-timing-model) works so hard to keep tight.

libDaisy does more of this than expected. `MidiEvent.h` already parses every channel-voice
message, SysEx, Song Position Pointer and all System Realtime messages, and
`MidiUsbTransport::Config::Periph` has three modes — `INTERNAL`, `EXTERNAL` and **`HOST`**. So
USB MIDI host (plug a controller straight into the drum machine, no computer) is a supported
path, not a research project.

## 1. Transports

Three, all active at once, all merging into one event stream:

| # | Transport | libDaisy | Hardware cost | Notes |
| --- | --- | --- | --- | --- |
| 1 | **DIN/TRS MIDI** | `MidiUartHandler` | 2 pins + jacks/opto | Lowest jitter — the one to sync from |
| 2 | **USB device** | `MidiUsbHandler`, `INTERNAL` | free (the USB-C port) | Class compliant; DAW sees it with no driver |
| 3 | **USB host** *(optional)* | `MidiUsbHandler`, `HOST` | **D29 + D30** + USB-A jack | Plug in a keyboard/controller directly |

### The USB host catch

`HOST` mode needs the OTG HS peripheral, which is hard-wired to **PB14/PB15 — Daisy pins D29 and
D30**. It is not "any 2 spare pins": those two specific pins must be left unassigned, and they're
otherwise prime GPIO. It also pushes the [pin budget](01-hardware.md#36-pin-map) from 28 to 30
of 31.

**Recommendation: build v1 with transports 1 and 2, and keep D29/D30 unassigned** so host mode
stays a firmware change rather than a respin. Populate the USB-A jack only if you find yourself
wanting to play the thing from a keyboard without a laptop in the chain.

## 2. Hardware

```
                    ┌──────────┐
   TRS IN  ─────────│  H11L1   │──── UART RX ──► Daisy
                    │  opto    │
                    └────┬─────┘
                         │ (buffered, zero latency)
   TRS THRU ◄────────────┴──── 74HCT14
   TRS OUT  ◄─── 74HCT14 ◄──── UART TX ◄──── Daisy
```

- **In**: H11L1 optoisolator. Schmitt-trigger output, so no comparator or edge conditioning —
  but the output is **open-collector and needs a pull-up**, which is easy to leave off a schematic
  because "Schmitt output" reads like it is already driven. The isolation is not optional — it's
  what stops ground loops between gear.
- **Out**: two 74HCT14 gates in parallel for drive, 2×220 Ω series.
- **Thru**: two more gates off the opto output. **Hardware thru is zero-latency** and keeps
  working even when the firmware is busy or crashed. The 74HCT14 has six gates; Out takes two,
  Thru takes two, so one chip covers both.
- **Jacks**: 3.5 mm TRS **Type A** (the MIDI Association standard since 2018). Ship a TRS→DIN
  adapter with the unit if you care about older gear.

### 2.1 Rails and levels

This block spans two rails, and which part sits on which is the thing to get right before layout.

| Part | Rail | Why |
| --- | --- | --- |
| H11L1 output stage (pin 5 V_CC) | **3V3** | Spec'd 3–16 V, so 3V3 is in range. Its open-collector output then pulls up to 3V3 and feeds `MIDI_RX` (PB7) directly |
| Pull-up on the opto output | to **3V3** | 10 kΩ per the datasheet's typical application circuit |
| 74HCT14 | **5 V** | HCT is a 4.5–5.5 V family, and MIDI Out/Thru is a 5 V current loop through 220 Ω — 5 V is what the standard wants anyway |

**Running the opto at 3V3 rather than 5 V is the decision that matters**, and it is free. At 5 V its
output would present 5 V to an MCU pin, and whether PB7 is 5 V-tolerant is a datasheet question
this project would rather not have an answer riding on. At 3V3 the question does not arise.

**A 3V3 opto output driving 5 V-powered HCT inputs is fine, and is the whole reason the part is
HCT rather than HC.** HCT has TTL input thresholds — V_IH around 2 V — so 3V3 clears them
comfortably. This is the same trick as the 74AHCT125 in front of the SK6812 chain
([hardware §3.4](01-hardware.md#34-leds--sk6812-chain-on-spidma)), running in the same direction.

The consequence worth stating plainly: **nothing in the MIDI block puts 5 V on an MCU pin.**

Still to confirm against the real datasheet before the schematic is committed — none of it changes
the topology above, but all of it sets component values:

- **I_F(ON)**, the LED current that guarantees the output switches. H11L1/L2/L3 are graded by it,
  and the input series resistor follows from whichever grade you buy. MIDI's loop is nominally
  5 mA, so the grade is not a free choice.
- **V_OL at the sink current** the 10 kΩ pull-up implies, to confirm a valid logic low at 3V3.
- **Propagation delay.** Ample at 31.25 kBd on any reading, but worth a number rather than a
  shrug, because hardware Thru latency is a claim this document makes in §2.

Soft thru (route In→Out in firmware) is also supported and is what you want for merging; hardware
thru is what you want for reliability. Both exist, and §3 decides which is active.

## 3. Routing matrix

The "full" part. Every source can be routed to every destination independently:

| | → Engine | → MIDI Out | → USB Out | → Thru |
| --- | :---: | :---: | :---: | :---: |
| **MIDI In** | ✓ | ✓ (soft thru) | ✓ (bridge) | ✓ (hardware) |
| **USB In** | ✓ | ✓ (bridge) | — | — |
| **Host In** | ✓ | ✓ | ✓ | — |
| **Sequencer** | — | ✓ | ✓ | — |

This makes the machine a usable MIDI hub: USB↔DIN bridging alone means a laptop can drive DIN-only
gear through it. Stored as a bitmask per source in global settings.

**Loop guard:** soft thru plus a bridge can feed a message back to its own source. Tag every event
with its origin port and refuse to emit it back there.

## 4. Message map

### Receive

| Message | Action |
| --- | --- |
| **Note On/Off** | Trigger a voice. Velocity → voice velocity. Mapping per §5. |
| **Control Change** | Macro params (§4.1), plus MIDI learn targets |
| **Program Change** | Select pattern (0–127 → patterns 1–128) |
| **Pitch Bend** | Assignable: global tune, or selected-track tune |
| **Channel Pressure** | Assignable via MIDI learn |
| **Poly Key Pressure** | Ignored (no musical meaning here) |
| **Timing Clock** | Sync — see §6 |
| **Start / Stop / Continue** | Transport. Start resets to step 1; Continue resumes in place |
| **Song Position Pointer** | Jump to position. Essential for DAW scrubbing |
| **SysEx** | Pattern/kit dump, load and write; parameter and transport control — see §8 |
| **Active Sensing** | Ignored, but must not choke the parser |
| **System Reset** | Panic: all voices off, transport stop |

### Transmit

| Message | When |
| --- | --- |
| **Note On/Off** | Every sequencer trigger, per-track channel and note |
| **Timing Clock** | 24 PPQN, derived from the internal 96 PPQN clock |
| **Start / Stop / Continue** | Transport changes |
| **Song Position Pointer** | On pattern jump or transport reposition |
| **Control Change** | Pot moves, so a DAW can record knob automation |
| **Program Change** | On pattern change (optional — off by default, it surprises people) |
| **SysEx** | On an explicit dump request |

### 4.1 CC map

The macro philosophy from the [panel design](01-hardware.md#2-panel-layout) maps onto MIDI
cleanly: because all eight voices expose the same six parameters, **the same six CCs mean the same
thing on every track's channel**.

| CC | Parameter | | CC | Parameter |
| ---: | --- | --- | ---: | --- |
| 20 | TUNE | | 24 | DRIVE |
| 21 | DECAY | | 25 | LEVEL |
| 22 | TONE | | 26 | PAN |
| 23 | SNAP | | 27 | FX SEND |

Global channel:

| CC | Parameter | | CC | Parameter |
| ---: | --- | --- | ---: | --- |
| 14 | Tempo (coarse) | | 17 | Master drive |
| 15 | Swing | | 18 | Reverb send |
| 16 | Pattern length | | 19 | Accent amount |

Plus the standard ones: **CC 120** all sound off, **CC 123** all notes off, **CC 121** reset
controllers. Implement these — DAWs send them on stop and users notice when they're missing.

## 5. Note mapping — two modes

Because neither convention is right for everyone:

**GM mode (default).** One channel (default 10), General MIDI drum notes. Works instantly with any
DAW drum track or pad controller.

| Track | Note | | Track | Note |
| --- | ---: | --- | --- | ---: |
| BD | 36 (C1) | | LT | 45 (A1) |
| SD | 38 (D1) | | CP | 39 (D#1) |
| CH | 42 (F#1) | | RS | 37 (C#1) |
| OH | 46 (A#1) | | FM | 47 (B1) |

**Multi mode.** Each track gets its own channel (default: track N → channel N). Any note triggers
it, so a keyboard plays the voice chromatically — the note number offsets TUNE. This is the mode
that makes the FM and tom voices playable as instruments rather than one-shots.

Both modes are per-track overridable: channel, note, and out-channel/out-note are stored per track
in the pattern's kit.

## 6. Clock and sync

The genuinely hard part, and where most DIY drum machines feel bad.

### The problem

Slaving naively — advance a step whenever a Timing Clock byte arrives — imports every bit of the
source's jitter directly into your groove. And the jitter is real:

- **USB MIDI is quantised to 1 ms USB frames.** A USB clock source cannot be more accurate than
  that, ever.
- DIN MIDI is better but a clock byte still takes 320 µs to transmit at 31250 baud.
- Interrupt latency and any main-loop polling add their own.

Against a sequencer built for [sample-accurate triggers](02-firmware.md#3-timing-model), triggering
straight off clock bytes throws that precision away.

### The design

**Timestamp, then filter.**

1. **Timestamp in the UART RX interrupt**, capturing the *audio sample counter* — the only clock
   that actually matters. Timestamping in the 1 kHz main loop has already cost you ±1 ms.
2. **Feed timestamps to a PLL** that estimates tempo and phase. The sequencer runs from the PLL's
   smoothed estimate, never from raw clock edges.
3. **Expose the tradeoff** as a user setting, because it's a musical choice, not a technical one:

| Setting | Behaviour | For |
| --- | --- | --- |
| **Tight** | Short PLL time constant, follows the source closely | Tempo automation, live tempo changes |
| **Smooth** (default) | Long time constant, high inertia | Steady-tempo DAW sync — rejects USB frame jitter |

4. **Sync source priority**: external when clock is present, internal otherwise, **with a visible
   indicator**. Switching sync silently is miserable to debug on stage.
5. **Timeout**: no clock for ~500 ms → fall back to internal at the last-known tempo, don't stop.

### Acquisition is a separate problem from tracking

The loop gains that reject jitter are far too sluggish to *find* an unknown tempo — from a
120 BPM start, Smooth gains need thousands of clocks to reach 174. So the first four clocks seed
the period directly from the measured interval, and the loop takes over after that. Measured:
**within 1 BPM of a 174 BPM source in 2 clocks**, about a twelfth of a beat.

### Outlier rejection

A dropped or doubled byte must not be filtered as though it were tempo information; one glitch
would drag the estimate for seconds. Any interval more than half a period off is treated as a
resync rather than a measurement.

### Measured behaviour

From `make -C host clocktest`:

| | Tight | Smooth |
| --- | ---: | ---: |
| Tempo spread under ±1 ms jitter | 0.40 BPM | **0.06 BPM** |
| Follows a 120→140 ramp | closer | laggier |

Under USB-style 1 ms frame quantisation, Smooth holds tempo to within 0.01 BPM. Driving the
sequencer from a 90 BPM master, steady-state step spacing is within **1 sample (0.02 ms)** of the
master's grid.

### Transmit

Emit clock from the audio callback's tick evaluation, at the sample the tick lands on — not from
the main loop. Downstream gear deserves the same precision you kept for yourself.

### Latency compensation

One global offset (±50 ms) applied to the sync estimate, so the machine can be nudged into
alignment with a DAW that has its own buffer latency. Store it in settings; it's a
set-once-per-rig value.

## 7. MIDI learn

Hold a pot or step key, send any CC, and the binding is made. Stored in QSPI settings (not in the
pattern — it's a rig property). A learn table of 32 entries is plenty:

```cpp
struct MidiBinding {
    uint8_t channel;    // 0–15, or 0xFF = any
    uint8_t cc;
    uint8_t target;     // ParamId
    uint8_t track;      // 0–7, or 0xFF = selected track
};
```

Clearing: hold the same control and send nothing for 3 seconds, or a global "clear all learns" in
the settings page.

## 8. SysEx

Two jobs, not one. **Backup and restore**, which matters as soon as you've written patterns you'd
be upset to lose — and **control**, so the machine can be driven from a computer.

Manufacturer ID **`0x7D`** — the non-commercial/educational ID, correct for a DIY instrument and
guaranteed not to collide with real gear.

```
F0 7D <dev> <cmd> <data...> F7
```

`<dev>` is a device id so several units can share a MIDI chain. `0x00` is the default; `0x7F` is
broadcast and is only honoured for commands that cannot ambiguously reply.

### 8.1 Librarian

| cmd | Direction | Payload |
| ---: | --- | --- |
| `0x01` | → request | Pattern dump — `<slot>` |
| `0x02` | ← reply | Pattern data — `<slot> <encoded…>` |
| `0x03` | → request | Kit dump — `<slot>` |
| `0x04` | ← reply | Kit data — `<slot> <encoded…>` |
| `0x05` | → request | Settings dump |
| `0x06` | ← reply | Settings data — `<encoded…>` |
| `0x07` | → write | Pattern into a slot — `<slot> <encoded…>` |
| `0x08` | → write | Kit into a slot — `<slot> <encoded…>` |
| `0x09` | → write | Settings — `<encoded…>` |
| `0x0A` | ← reply | Ack/nak — `<cmd> <status>` |
| `0x0F` | → request | Device inquiry |
| `0x10` | ← reply | `<fw major> <fw minor> <patch version> <build hash ×5>` |

The write commands are what the original table was missing: it could dump but not restore, which
is half a librarian.

**Every write is acked or nak'd** (`0x0A`). A silent write is indistinguishable from a lost cable.

| status | Meaning |
| ---: | --- |
| `0x00` | ok |
| `0x01` | bad slot |
| `0x02` | version mismatch |
| `0x03` | size mismatch |
| `0x04` | bad encoding or truncated |
| `0x05` | busy |

### 8.2 Control

These map one-to-one onto the `Command` structs the UI already emits
([firmware §4](02-firmware.md#4-threading-model)), so they add no new surface on the audio side —
the same routing, range-checking and single-writer ownership apply.

| cmd | Payload |
| ---: | --- |
| `0x20` | Set track parameter — `<track> <param> <val14>` |
| `0x21` | Set master FX parameter — `<fx id> <val14>` |
| `0x22` | Set machine — `<track> <machine>` |
| `0x23` | Toggle step — `<track> <step>` |
| `0x24` | Set step lock — `<track> <step> <param> <val14>` |
| `0x25` | Clear step locks — `<track> <step>` |
| `0x26` | Set step field — `<track> <step> <field> <val14>` |
| `0x27` | Set track mute — `<track> <0\|1>` |
| `0x28` | Set tempo — `<bpm×10 as val14>` |
| `0x29` | Transport — `<0 stop \| 1 start \| 2 continue>` |

### 8.3 Query

The dumps in §8.1 are the way to read a whole pattern. These exist for a host that wants to follow
along without pulling 30 kB to find out one number.

| cmd | Direction | Payload |
| ---: | --- | --- |
| `0x30` | → request | Track parameter — `<track> <param>` |
| `0x31` | ← reply | `<track> <param> <val14>` |
| `0x32` | → request | Transport state |
| `0x33` | ← reply | `<playing> <bpm×10 val14> <position ×12>` |

### 8.4 Value encoding

**`val14` is two 7-bit bytes, MSB first**, spanning 0–16383 over the parameter's 0..1 range. That
is the MIDI idiom, and 14 bits is comfortably finer than any knob resolves — `Ui::StepFor`'s
finest step is 1/256.

**Bulk payloads are 7-bit encoded**: seven data bytes become eight on the wire — one byte carrying
the seven stripped high bits, then the seven stripped bytes. So payloads grow by **8/7**.

### 8.5 Reject, never reinterpret

Every bulk payload carries the same `SaveHeader` — magic, version, `payload_size` — that flash
saves carry, and it is checked on exactly the same terms
([firmware §8](02-firmware.md#8-persistence)). A host sending a v3 pattern to a v4 device gets a
`0x02` nak, not a reinterpreted struct. The failure this prevents *sounds like corruption* rather
than like a mismatch, which is precisely why it cannot be left to chance on the wire either.

### 8.6 Size and timing

Derived from `sizeof`, not estimated:

| | Raw | On the wire (×8/7) | Over DIN @ 3,125 B/s |
| --- | ---: | ---: | ---: |
| One pattern (`Patch`) | 30.1 kB | 34.5 kB | **11.3 s** |
| One kit | 1.6 kB | 1.8 kB | 0.6 s |
| A 128-pattern bank | 3.77 MB | 4.31 MB | **~24 minutes** |

**Send bulk dumps over USB only.** Twenty-four minutes is not a backup, it is an outage — and the
same transfer over USB MIDI is seconds. Chunk large payloads and throttle: a single 34 kB SysEx
message is legal but many hosts handle a sequence of ~1 kB frames far more gracefully, and chunking
is what lets a librarian show a progress bar.

> An earlier revision of this section said a pattern was ~9.2 kB and a bank took about six minutes.
> Both were right when `Step` was 22 bytes across 8 tracks. `Step` is now 38 bytes across 12, and
> the `Kit` carries master FX and a machine per track — see
> [firmware §6](02-firmware.md#6-sequencer-data-model).

### 8.7 Two implementation rules

**Parse in the main loop, never in the MIDI interrupt.** The command queue is
single-producer/single-consumer and [firmware §4](02-firmware.md#4-threading-model) is explicit
that the ISR never pushes commands. A SysEx parser in the UART interrupt makes it two-producer,
which does not crash — it corrupts a pattern occasionally. Clock timestamping stays in the ISR
because it feeds the PLL, not the queue.

**Decode bulk loads straight into `Storage::staging()`.** It is already a static `Patch`-sized
buffer, and `Machine::RequestLoad` already exists to hand it to the audio side at a block boundary.
A SysEx pattern load and a flash pattern load then become the same operation, and the protocol
costs no additional RAM.

### 8.8 Host side

Also respond to **Universal Device Inquiry** (`F0 7E 7F 06 01 F7`) — it is how software finds the
device.

Everything above is deliberately a **binary protocol on a class-compliant USB MIDI device**, which
means no driver on any platform and several clients for one piece of firmware work: a patch
librarian, a DAW script, a CI harness driving real hardware, a browser editor over WebMIDI, and an
MCP server for driving the machine from an LLM.

**None of those belong on the MCU.** MCP in particular is JSON-RPC and wants a parser and an
allocator, against the rule that the audio side never allocates — and it is a host-side protocol by
design. The bridge is a couple of hundred lines of Python or TypeScript over `mido` or `node-midi`,
living beside the firmware rather than inside it.

## 9. Implementation notes

**Threading.** MIDI parsing runs in the main loop and pushes into the same SPSC queue as the UI
([firmware §4](02-firmware.md#4-threading-model)). The exception is clock byte timestamping, which
must happen in the interrupt.

**Never allocate in the parser.** Incoming SysEx needs a fixed 16 kB static buffer; a malformed or
hostile stream must not be able to grow it.

**Running status** is handled by libDaisy's parser, but test it — some older gear leans on it hard.

**Buffer sizing.** A 256-event ring per port is ample; a full-speed DIN stream is ~1000
bytes/second.

**Panic path.** MIDI panic (CC 123, System Reset, or a UI combo) must reach the voices even if the
sequencer is wedged — route it straight to the voice array.

## 10. What this costs

| | Pins | Parts | Phase |
| --- | ---: | --- | --- |
| DIN in/out | 2 | H11L1, 74HCT14, 2 jacks | 3 |
| **DIN thru** | **0** | +1 jack (gates already there) | 3 |
| USB device | 0 | — | 3 |
| USB host *(optional)* | **2 (D29/D30)** | USB-A jack, 5 V switch | 7 |

MIDI work splits across phases rather than landing in one: basic in/out and clock in
[Phase 3](04-development-plan.md#phase-3--sequencer-core-23-weeks) because the sequencer needs sync
anyway, the full message map and learn in
[Phase 4](04-development-plan.md#phase-4--ui-and-ux-23-weeks) alongside the UI it binds to, and
SysEx in [Phase 6](04-development-plan.md#phase-6--persistence-fx-polish-2-weeks) with the rest of
persistence.
