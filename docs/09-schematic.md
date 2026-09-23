# Making the KiCad Schematic

Phase 5. Work through this in order — the first step is the one that isn't done yet, and drawing
before it is settled means redrawing.

## 0. The pin map — done

[Hardware §3.6](01-hardware.md#36-pin-map) now carries a complete `Daisy pin → net → peripheral`
table, derived from libDaisy's peripheral pin tables rather than read off a datasheet. Three
constraints shaped it, and two overturned earlier assumptions:

- **SPI2 is unusable** — its only SCLK pin (PD3) isn't broken out. The LED chain was assigned to
  it; it now uses SPI1.
- **SDMMC 4-bit collides with SPI3**, so the SD card runs 1-bit (still several MB/s against the
  ~200 kB/s a sample stream needs).
- **The trigger shift register can't share the display bus** — the 595 shifts in every byte on
  MOSI, and the display is drawn from the main loop while triggers fire from the audio callback.
  It has three dedicated pins.

26 pins assigned, 2 spare (both ADC-capable), 2 reserved for USB MIDI host, 1 consumed by a
peripheral but left unrouted.

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
| `controls` | 8 encoders → 3 × CD4021, master volume pot in the audio path, CV jack footprints |
| `keys` | 5 × CD4021 chain, 34 switches + hot-swap sockets |
| `leds` | 74AHCT125 level shifter, SK6812 chain, per-LED decoupling |
| `display` | SSD1309 OLED header |
| `midi` | H11L1 in, 74HCT14 out + thru, three TRS jacks |
| `audio` | Output stage, headphone amp, jacks, audio **in** jacks |
| `expansion` | 2×12 header footprint (2×10 populated), 74HC595 trigger outs |

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
- Name nets for what they are (`SR_CLK`, `ENC_DATA`, `LED_DATA`), not where they go.
- Annotate (Tools → Annotate Schematic).
- **Run ERC and get it to zero**, adding power flags where needed. Every real error you leave
  here becomes a cut trace and a bodge wire later.

## 5. Project-specific things ERC will not catch

- **Level shifter is not optional.** SK6812 at 5 V wants V_IH ≈ 3.5 V; the Seed3 drives 3.3 V.
  The 74AHCT125 goes between them ([hardware §3.4](01-hardware.md#34-leds--sk6812-chain-on-spidma)).
- **LED current.** 34 × 60 mA is 2.0 A at full white — far past USB. Clamped in firmware, but the
  rail and bulk cap (1000 µF) must still be drawn for the real peak.
- **Ground split.** Separate analog and LED/digital ground pours joined at **one** star point near
  USB. 34 LEDs PWMing into a shared ground plane is an audible buzz and very hard to fix after
  layout. Draw it as separate nets (`AGND` / `PGND`) now so layout can honour it.
- **Analog expansion reservations** ([05-analog-expansion.md](05-analog-expansion.md)): DC barrel
  jack footprint, 2×12 header footprint, 74HC595, audio-in jacks. $5.00 against a respin.
- **Unpopulated footprints are free**: SD socket, CV/expression jacks, clock jacks. Draw them now.

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
