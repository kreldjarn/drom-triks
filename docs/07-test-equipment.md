# Test Equipment

**Short answer on the oscilloscope: not yet.** For Phases 0–6 a ~$15 logic analyser will earn its
keep many times over and a scope will mostly sit idle. The scope becomes genuinely necessary when
you start the [analog cartridges](05-analog-expansion.md) — buy it then, with better
information about what you need.

Prices are approximate, for rough budgeting only.

## Essential — can't start without these

| Item | ~Cost | Why |
| --- | ---: | --- |
| **Temperature-controlled soldering iron** | $30–110 | Pinecil V2 (~$30) is excellent value; Hakko FX-888D (~$110) if you want a bench unit. Get a **fine conical or knife tip** — the JTAG header is 1.27 mm pitch |
| **Solder, flux, desoldering braid** | $25 | Flux matters more than people expect at 1.27 mm. Leaded solder is easier if you're allowed it |
| **Multimeter** | $30–50 | The **continuity beeper** is the single most-used function in this build: check every rail against ground *before* first power-on. Any decent $40 meter is fine |
| **ST-Link V3 MINIE** | $12 | Already in the [BOM](03-bom.md). Your primary debugging tool — it replaces most of what people reach for a scope to do |
| **Breadboard + jumper wires** | $20 | [Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks) lives on these |
| **Magnification** | $15–40 | A loupe or cheap USB microscope. 1.27 mm pitch is past the point where eyes alone are reliable |

**Subtotal: roughly $130–160.**

## The one instrument worth buying early

| Item | ~Cost | Why |
| --- | ---: | --- |
| **8-channel USB logic analyser** | $15 | The highest-value instrument in this entire list |

Nearly all the debugging in Phases 1–6 is *digital protocol* work: SPI to the OLED and the LED
chain, the CD4021 shift-register scan, the 74HC595 triggers, MIDI UART, SDMMC. A logic analyser
decodes those directly into readable transactions. A scope shows you wiggly lines and makes you
decode them by eye.

A generic "Saleae clone" (8 channels, 24 MHz) is around $15 and works with **PulseView/sigrok**,
which is free and open source and ships protocol decoders for SPI, I²C, UART — **and WS281x**,
which covers the SK6812 chain.

That last one matters: the SK6812's 800 kHz one-wire protocol is the riskiest custom code in the
build ([hardware §3.4](01-hardware.md#34-leds--sk6812-chain-on-spidma)). Its bit period is 1.25 µs,
so at 24 MHz you get ~30 samples per bit — ample to see exactly why a frame is wrong. Debugging
that by ear, or by watching LEDs misbehave, is miserable.

Saleae's own Logic 8 (~$400) is lovely and unnecessary here.

## For audio measurement — better than a scope

| Item | ~Cost | Why |
| --- | ---: | --- |
| **Audio interface with line in** | $100–200 | You may already own one |
| **REW (Room EQ Wizard)** or ARTA | free | Noise floor, THD+N, frequency response |

This is the part people get wrong. **For audio measurements an audio interface is a far better
instrument than an oscilloscope.** A typical scope gives ~8 bits of effective resolution; a $150
audio interface gives 24. Measuring a −120 dB noise floor with a scope is not possible; with an
interface and REW it's routine.

[Phase 0](04-development-plan.md#phase-0--toolchain-1-week) asks you to record the noise floor with
the sine muted. That baseline is the only clean one you'll get before panel wiring adds its own
noise, and this is the kit that produces it.

Also: **monitors or headphones you trust.** You're building an instrument; at some point the
measurement that matters is whether it sounds right.

## For PCB bring-up (Phase 5)

| Item | ~Cost | Why |
| --- | ---: | --- |
| **Bench PSU with current limiting** | $60–90 | Set a low current limit, power the new board, watch. Turns "released the magic smoke" into "hit the limit, found the short" |
| **USB power meter** | $15 | Checks real draw against the ~480 mA budget in [hardware §4](01-hardware.md#4-power) — especially LED brightness |
| **Hot air rework station** | $50–100 | Only if you hand-assemble SMD or need to rework it. Deferrable |

The bench supply is the one I'd genuinely recommend before first power-on of a new PCB. A shorted
rail on a $40 board is a cheap lesson only if you catch it in the first second.

## Oscilloscope — when, and which

Buy one when you start the **analog cartridges**, where you're debugging envelopes, filter
responses and trigger pulses in the analog domain and there is no substitute.

Before that it's largely redundant: the logic analyser covers digital timing better, the audio
interface covers audio better, and the ST-Link covers program state better.

If/when you do buy: **Rigol DHO800 series (~$350)** is the current value pick — 12-bit, which
genuinely matters for analog audio work. A used Rigol DS1054Z (~$200) remains a solid budget
option. Two channels is enough; four is nicer.

## Summary

| Stage | Add | Running total |
| --- | --- | ---: |
| **Phases 0–1** | Essentials + logic analyser | ~$150–175 |
| **Phases 2–4** | Audio interface + REW (if you don't own one) | ~$250–375 |
| **Phase 5** | Bench PSU, USB power meter | ~$325–480 |
| **Phase 7 (analog)** | Oscilloscope | ~$675–830 |

If you already own an audio interface and headphones — likely, given the project — **you can start
for about $150**, and the logic analyser is $15 of that.
