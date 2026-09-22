# Making the KiCad Schematic

Phase 5. Work through this in order — the first step is the one that isn't done yet, and drawing
before it is settled means redrawing.

## 0. The prerequisite: a pin map

[Hardware §3.6](01-hardware.md#36-pin-budget) has a pin **budget** (28 of 31 used) but not a pin
**map**. Nothing can be drawn until each function is assigned to a named Daisy pin, and the
assignment is not free — it's constrained by which STM32 peripheral instance reaches which pin.

The constraints, from libDaisy's own headers:

| Constraint | Consequence |
| --- | --- |
| **ADC is only on D15–D25 and D28** (aliases A0–A11) | The pot mux's analog input must land in that range. D31/D32 exist only on the Seed2 DFM. |
| **D29/D30 are PB14/PB15** | Reserved for USB MIDI host ([06-midi.md §1](06-midi.md#1-transports)). Assign nothing to them. |
| **SPI1/SPI2/SPI3, USART1/UART4/5/7, SDMMC** each reach fixed pins | Two SPI devices (OLED + LED chain) must be on different peripheral instances, or share one bus with separate chip selects. |
| Mux select lines are digital | Don't spend a 16-bit ADC pin on one. |

**Deliverable:** a table of `Daisy pin → net name → peripheral`, checked against libDaisy so the
firmware can actually configure it. Worth doing as a spreadsheet and pasting into
`docs/01-hardware.md` before opening KiCad.

## 1. Tooling

```sh
brew install --cask kicad     # 10.0.6 at time of writing
```

The **Daisy Seed symbol and footprint ship in KiCad's stock libraries**
(`Electrosmith_Daisy_Seed`) — check there before downloading anything. If your version predates
it, Electrosmith publish a library zip:
`https://daisy.nyc3.cdn.digitaloceanspaces.com/libraries/DaisyKiCad-main.zip`

The Seed3 is pin-compatible with the original Seed footprint, so the stock part is correct.

Keep the KiCad project in this repo under `hardware/`, and commit the `.kicad_sch` / `.kicad_pcb`
files — they're text and diff usefully.

## 2. Hierarchical sheets, not one big page

This design is too big for a single sheet. One sheet per subsystem, which also matches how you'll
bring the board up:

| Sheet | Contents |
| --- | --- |
| `power` | USB-C in, 5 V rail, bulk/decoupling, DC jack footprint, star ground point |
| `daisy` | The Seed3 + its headers, JTAG header, boot/reset |
| `controls` | 6 pots → CD4051, second CD4051 footprint |
| `keys` | 4 × CD4021 chain, 30 switches + hot-swap sockets, encoders |
| `leds` | 74AHCT125 level shifter, SK6812 chain, per-LED decoupling |
| `display` | SSD1309 OLED header |
| `midi` | H11L1 in, 74HCT14 out + thru, three TRS jacks |
| `audio` | Output stage, headphone amp, jacks, audio **in** jacks |
| `expansion` | 2×10 header, 74HC595 trigger outs |

Draw `power` and `daisy` first and get them right; everything else hangs off them.

## 3. Before drawing the audio sheet — read the Seed3 schematic

The one genuinely open hardware question:
**are the Seed3's audio outputs single-ended or differential?** The TAC5242 supports both and
Electrosmith haven't documented which they used. It decides whether the output needs a
differential receiver or a plain DC-blocking cap, so resolve it before committing the audio
sheet. Download the Seed3 schematic PDF from [docs.daisy.audio](https://docs.daisy.audio/) and
look.

## 4. Draw, annotate, ERC

- Use **net labels liberally** and global labels between sheets; wire-spaghetti across a design
  this size is unreadable and error-prone.
- Name nets for what they are (`MUX_SEL0`, `KEY_LATCH`, `LED_DATA`), not where they go.
- Annotate (Tools → Annotate Schematic).
- **Run ERC and get it to zero**, adding power flags where needed. Every real error you leave
  here becomes a cut trace and a bodge wire later.

## 5. Project-specific things ERC will not catch

- **Level shifter is not optional.** SK6812 at 5 V wants V_IH ≈ 3.5 V; the Seed3 drives 3.3 V.
  The 74AHCT125 goes between them ([hardware §3.4](01-hardware.md#34-leds--sk6812-chain-on-spidma)).
- **LED current.** 30 × 60 mA is 1.8 A at full white — far past USB. Clamped in firmware, but the
  rail and bulk cap (1000 µF) must still be drawn for the real peak.
- **Ground split.** Separate analog and LED/digital ground pours joined at **one** star point near
  USB. 30 LEDs PWMing into a shared ground plane is an audible buzz and very hard to fix after
  layout. Draw it as separate nets (`AGND` / `PGND`) now so layout can honour it.
- **Analog expansion reservations** ([05-analog-expansion.md](05-analog-expansion.md)): DC barrel
  jack footprint, 2×10 header, 74HC595, audio-in jacks. $4.90 against a respin.
- **Unpopulated footprints are free**: SD socket, second CD4051, clock jacks. Draw them now.

## 6. Decide before layout, not during

**SK6812 vs TLC5947** for the LEDs. This changes the board, and
[Phase 1](04-development-plan.md#phase-1--breadboard-rig-12-weeks) is where it gets settled — the
DMA driver either works on a breadboard or it doesn't.

## 7. Footprints, then PCB

- Assign footprints (Tools → Assign Footprints). Check every one against the real part's
  datasheet — especially the switches, hot-swap sockets, pots and jacks, where a wrong land
  pattern is unrecoverable.
- Print the board outline 1:1 on paper and **put the actual parts on it** before ordering. It
  catches mechanical collisions no DRC will.
- Update PCB from schematic, then layout (a separate job from this document).

## 8. Fab

JLCPCB with **Europackage** (customs prepaid via a Luxembourg remailer), or Aisler for EU
manufacture with no customs at all. 4-layer for the ground plane. Order qty 5; expect a second
spin and budget for it rather than being disappointed by it.
