# Sourcing (Sweden)

Where to buy everything in the [BOM](03-bom.md) and [test equipment](07-test-equipment.md) list
from Sweden, organised to minimise customs pain.

**Link types are marked.** ✅ = a product page I verified. 🔎 = a distributor search link (always
works, but confirm the exact part before ordering). Prices are approximate and move.

## 1. The rule that shapes everything: customs

**The EU abolished the €150 customs-duty exemption on 1 July 2026.** Duty is now charged on
every consignment from outside the EU regardless of value — €3 for goods at or under €150. On top
of that:

- **25 % Swedish VAT** on goods + shipping + duty, on everything from outside the EU
- **PostNord handling: SEK 175** when both a VAT and a customs declaration are needed
- A further ~€2 per-shipment customs handling fee is expected from around November 2026

These are **per shipment, not per item**. Two consequences drive the whole plan:

1. **Buy inside the EU wherever you can.** No duty, no VAT surprise, no PostNord fee, and days
   instead of weeks.
2. **When you must go outside, consolidate into one order.** Three small AliExpress orders cost
   three lots of fees; one order costs one.

A €12 non-EU order can easily land at €35 delivered. That changes which vendor is actually
cheapest, and it's why the ordering below is grouped by vendor rather than by part.

## 2. Decision first: Seed3 or the original Seed?

**No EU dealer stocks the Seed3.** Electrosmith's [dealer list](https://daisy.audio/pages/dealers)
has exactly one Swedish entry — Electrokit in Malmö — and they stock the **original Daisy Seed**,
not the Seed3.

| | **Seed3**, direct from [daisy.audio](https://daisy.audio/products/seed3) ✅ | **Daisy Seed 65MB**, [Electrokit](https://www.electrokit.com/en/electrosmith-daisy-seed-embedded-dsp-platform) ✅ |
| --- | --- | --- |
| Price | $29.99 | **559 SEK inc. VAT** (art. 41020659) |
| Shipping | ~$12 from the US | Domestic |
| + 25 % VAT, €3 duty, SEK 175 PostNord | yes | none |
| **Landed cost** | **~700–750 SEK** | **559 SEK** |
| Delivery | weeks | **1–3 working days**, 137 in stock |
| Codec | TAC5242, 32-bit/192 kHz, −120 dB | AK4556, 24-bit/48 kHz |
| USB | USB-C | micro-USB |
| Headers | 2×20 pre-soldered | pre-soldered |

**The older board is cheaper delivered and arrives this week.** It is pin-compatible, runs the
same libDaisy, and every line of firmware in this repo works on it unchanged.

What you give up is the codec — and the codec is a real part of why this design picked the Seed3.
A −120 dB noise floor is the difference between "quiet" and "inaudible" on a machine with 30 LEDs
PWMing next to the audio ground.

**Suggestion: buy one Electrokit Seed now** to unblock Phases 0–4 (all of which are breadboard
work where the codec is irrelevant), and order a Seed3 direct later, consolidated with any other
US order, for the final build. They're pin-compatible, so nothing is wasted.

Also note the original Seed has **micro-USB**, not USB-C — worth knowing before you design a panel
cutout around it.

## 3. Order 1 — Electrokit (Malmö, SE) 🇸🇪

No customs, 1–3 days. [electrokit.com](https://www.electrokit.com/en/) 🔎

| Item | Notes |
| --- | --- |
| Daisy Seed 65MB ✅ | Art. 41020659 — see §2 |
| Breadboards + jumper wire kit | Phase 1 rig |
| CD4051 8:1 analog mux ×2 | Also at Elfa/Reichelt/TME |
| CD4021 shift register ×4 | " |
| 74HCT14 hex Schmitt inverter | MIDI out + thru |
| 74AHCT125 level shifter | 3.3 V → 5 V for the LEDs — **not optional** |
| 74HC595 shift register | 8 trigger outputs |
| Resistor + capacitor assortment | Buy the book once |
| 3.5 mm TRS jacks ×5 | 2 MIDI + 1 headphone + 2 audio-in |
| Electrolytic + ceramic caps | 1000 µF LED rail, 100 nF decoupling |
| USB-C cable | |

Electrokit is the single best first stop: Swedish, hobbyist-oriented, and carries most of the
glue logic. Search each part number at checkout to confirm.

## 4. Order 2 — Elfa Distrelec (SE) or TME (PL) 🇸🇪🇵🇱

For anything Electrokit lacks. Both are inside the EU — **no customs either way**.

- [elfa.se](https://www.elfa.se/en/) 🔎 — Swedish, next-day, 150k products
- [tme.eu](https://www.tme.eu/en/) 🔎 — Polish, huge catalogue, subsidised DHL (~€25 for 2 kg, 48 h)
- [reichelt.de](https://www.reichelt.de/) 🔎 — German, cheap, ~2 days
- [se.farnell.com](https://se.farnell.com/) 🔎 — good for semiconductors and dev boards

| Item | MPN | Search |
| --- | --- | --- |
| Optoisolator, MIDI in | **H11L1** | [Elfa](https://www.elfa.se/en/search?q=H11L1) 🔎 · [TME](https://www.tme.eu/en/katalog/?search=H11L1) 🔎 |
| Headphone amp | **TPA6132A2** | [Farnell](https://se.farnell.com/search?st=TPA6132A2) 🔎 |
| 9 mm vertical pot, B10k ×6 | **Alpha RD901F-40-20R1-B10K** | [TME](https://www.tme.eu/en/katalog/?search=RD901F) 🔎 |
| Rotary encoder + switch ×2 | **Bourns PEC11R-4215F-S0024** | [Elfa](https://www.elfa.se/en/search?q=PEC11R) 🔎 · [TME](https://www.tme.eu/en/katalog/?search=PEC11R) 🔎 |
| JTAG header, 10-pin 1.27 mm | **Amphenol 20021111-00010T4LF** | [TME](https://www.tme.eu/en/katalog/?search=20021111-00010T4LF) 🔎 |
| 1/4" TS jacks ×2 | PJ-612A or Neutrik NMJ4HCD2 | [Elfa](https://www.elfa.se/en/search?q=jack%206.3mm%20pcb) 🔎 |
| 2×20 female header ×2 | 2.54 mm | any of the above 🔎 |
| 2×10 expansion header | 2.54 mm | " |
| DC barrel jack + 1N5819 | PJ-002A | " |
| Ferrite bead, bulk caps | | " |
| M3 standoffs + screws | | " |

**Fit the JTAG header centred** on its 14-position footprint — two positions free at each end.
See [hardware §1](01-hardware.md#1-board-choice-daisy-seed3).

## 5. Order 3 — Mechanical keyboard parts (EU) 🇩🇪🇳🇱

The switches, keycaps, hot-swap sockets and **SK6812 MINI-E** LEDs come from the mechanical
keyboard world, not general distributors. Both shops below are inside the EU.

- [keycapsss.com](https://keycapsss.com/) 🔎 — Leipzig, DE. Has the SK6812 MINI-E ✅:
  [product page](https://keycapsss.com/keyboard-parts/parts/114/sk6812-mini-e-rgb-smd-led)
- [splitkb.com](https://splitkb.com/) 🔎 — Netherlands, ships in 2 working days

| Item | Qty | Notes |
| --- | ---: | --- |
| **SK6812 MINI-E** reverse-mount RGB LED | 30 (buy 50) | Reverse-mount so it shines up through the switch |
| MX-compatible switch, **clear/transparent housing** | 30 | Clear top matters — it's the light path |
| **Kailh MX hot-swap socket** | 30 | Change switch feel later without desoldering a legended panel |
| Translucent blank keycap, DSA/XDA | 30 | Legends go on the PCB silkscreen |

Buy ~10 % spares on the LEDs. They're tiny, reverse-mount, and you will lose or cook one or two.

## 6. Order 4 — PCB (Phase 5, months away)

| Option | Cost (5× 4-layer ~180×100 mm) | Customs |
| --- | ---: | --- |
| **[JLCPCB](https://jlcpcb.com/)** 🔎 | ~$40 | Use **"Europackage"** — ships via a Luxembourg remailer with **customs prepaid**, so no PostNord surprise |
| **[Aisler](https://aisler.net/en)** ✅ | ~€60+ | EU-made (NL/DE), no customs at all, simplest |
| [Eurocircuits](https://www.eurocircuits.com/) 🔎 | ~€100 | Belgian, excellent quality, priciest |

JLCPCB with Europackage is the value pick and sidesteps the customs problem. Aisler is worth the
premium if you'd rather not think about it at all.

**Don't order this yet** — settle the SK6812-vs-TLC5947 question in Phase 1 first, since it
changes the board.

## 7. Order 5 — Test equipment

From [07-test-equipment.md](07-test-equipment.md). Nothing here is needed before the parts arrive.

| Item | Where | Notes |
| --- | --- | --- |
| **Pinecil V2** soldering iron | [pine64eu.com](https://pine64eu.com/product/pinecil-smart-mini-portable-soldering-iron/) ✅ (EU, free shipping) · **Droneit ships from Sweden** 🔎 | Get a fine conical/knife tip for 1.27 mm |
| Solder, flux, braid | Electrokit / Elfa 🔎 | Flux matters more than you'd think at this pitch |
| Multimeter | Electrokit / Elfa 🔎 | The continuity beeper is the function you'll live in |
| **ST-LINK-V3MINIE** | [ST eStore](https://estore.st.com/en/stlink-v3minie-cpn.html) ✅ · [Mouser](https://www.mouser.com/ProductDetail/STMicroelectronics/STLINK-V3MINIE?qs=MyNHzdoqoQKcLQe5Jawcgw%3D%3D) ✅ · [Digi-Key SE](https://www.digikey.se/en/products/result?keywords=STLINK-V3MINIE) 🔎 | Prefer an EU-warehoused seller |
| **8-ch USB logic analyser** | AliExpress ~€12 🔎 · Elfa 🔎 | Highest-value instrument here. Works with free [PulseView/sigrok](https://sigrok.org/) |
| Bench PSU with current limit | Elfa / Electrokit 🔎 | Before first PCB power-on |
| USB power meter | AliExpress / Electrokit 🔎 | Check against the ~480 mA budget |
| Magnifier or USB microscope | Electrokit 🔎 | 1.27 mm is past naked-eye reliability |
| Audio interface + [REW](https://www.roomeqwizard.com/) (free) | [Thomann](https://www.thomann.de/se/) 🔎 (DE, EU) | You may already own one. Beats a scope for audio |
| *Oscilloscope (Phase 7 only)* | Elfa / [Batronix](https://www.batronix.com/) 🔎 | Rigol DHO800 ~€350, 12-bit |

If you order the logic analyser and power meter from AliExpress, **put them in one order** —
the customs fees are per shipment.

## 8. Suggested sequence

| When | Order | Rough cost |
| --- | --- | ---: |
| **Now** | Electrokit: Seed + breadboard + logic ICs + passives + jacks | ~1200 SEK |
| **Now** | Pinecil + multimeter + logic analyser (if you lack them) | ~700 SEK |
| **Now** | ST-LINK-V3MINIE + the 10-pin JTAG header | ~250 SEK |
| Phase 1 | Elfa/TME: pots, encoders, H11L1, TPA6132A2 | ~400 SEK |
| Phase 1 | keycapsss/splitkb: LEDs, switches, sockets, caps | ~500 SEK |
| Phase 5 | JLCPCB (Europackage) + remaining connectors | ~600 SEK |
| Phase 7 | Oscilloscope, analog parts | ~4000 SEK |

**To start Phase 0 you need exactly three things:** a Daisy board, the ST-LINK, and the JTAG
header. Everything else can wait until you're through the toolchain and into the breadboard rig.

## 9. Caveats

- Electrokit's Daisy is the **original Seed**, not the Seed3 — see §2 before ordering.
- I verified the ✅ links resolve (HTTP 200); the 🔎 links are searches or vendor front pages, so
  **confirm the exact part number** at checkout. Distributor stock and prices move constantly.
- **Digi-Key, TME and Tullverket could not be automatically checked** — they return 403 to any
  scripted request, which is anti-bot protection rather than a broken link. They're live; open
  them in a browser.
- The customs rules above changed in July 2026 and a further handling fee is expected around
  November 2026. Check [Tullverket](https://www.tullverket.se/) if a large order is involved.
- Prices exclude shipping unless stated. Swedish VAT is 25 % and is already in Electrokit's
  listed prices; most B2B distributors quote ex-VAT.
