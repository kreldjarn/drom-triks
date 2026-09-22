# MIDI

Full MIDI implementation: three simultaneous transports, a routing matrix, complete channel-voice
and realtime message support, SysEx backup, MIDI learn, and a clock recovery scheme that doesn't
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
otherwise prime GPIO. It also pushes the [pin budget](01-hardware.md#36-pin-budget) from 28 to 30
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

- **In**: H11L1 optoisolator. Schmitt-trigger output, so no extra conditioning. The isolation is
  not optional — it's what stops ground loops between gear.
- **Out**: two 74HCT14 gates in parallel for drive, 2×220 Ω series.
- **Thru**: two more gates off the opto output. **Hardware thru is zero-latency** and keeps
  working even when the firmware is busy or crashed. The 74HCT14 has six gates; Out takes two,
  Thru takes two, so one chip covers both.
- **Jacks**: 3.5 mm TRS **Type A** (the MIDI Association standard since 2018). Ship a TRS→DIN
  adapter with the unit if you care about older gear.

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
| **SysEx** | Pattern/kit dump and load, config — see §8 |
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

Backup and restore, which matters as soon as you've written patterns you'd be upset to lose.

Manufacturer ID **`0x7D`** — the non-commercial/educational ID, correct for a DIY instrument and
guaranteed not to collide with real gear.

```
F0 7D <dev> <cmd> <data...> F7
```

| cmd | Direction | Payload |
| ---: | --- | --- |
| `0x01` | request → | Pattern dump (pattern #) |
| `0x02` | → reply | Pattern data |
| `0x03` | request → | Kit dump (kit #) |
| `0x04` | → reply | Kit data |
| `0x05` | request → | Global settings dump |
| `0x06` | → reply | Settings data |
| `0x0F` | request → | Device inquiry: firmware version, build hash |

Pattern data is ~9.2 kB, which must be **7-bit encoded** for SysEx (8 bytes → 7 bytes of payload,
so ~10.5 kB on the wire). Chunk it and throttle: dumping a full 128-pattern bank at MIDI speed
takes about 6 minutes over DIN, a few seconds over USB. Send the bank dump over USB only.

Also respond to **Universal Device Inquiry** (`F0 7E 7F 06 01 F7`) — it's how software finds the
device.

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
