// drom-triks on the Electrosmith Seed3 Desktop Dev Kit.
//
// The real machine, sequencer and UI, played from the kit's 16 keys, 8 pots
// and two toggles until the panel exists. Everything the kit does not have is
// folded onto what it does in src/ui/devkit_panel.h, which is pure logic and
// tested on the host; this file is only the wiring. docs/02-firmware.md §10
// has the layout.
//
// The kit has no screen, so the display renderer's lines go to the Seed's own
// USB port as serial text instead — the same words a screen would show.
//
// Threads, as in docs/02-firmware.md §4, minus the scan ISR: the kit has pots
// and plain keys and no encoders, so nothing needs sampling faster than the
// 1 kHz main loop.
//
//   main loop (1 kHz)   read controls, run Ui, flash work, LEDs, status text
//   audio (1.5 kHz)     Machine::Process — drains the command queue, renders

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "daisysp.h"
#include "desktop_devkit.h"
#include "usbd_def.h"

#include "io/storage.h"
#include "machine.h"
#include "platform/qspi_flash.h"
#include "ui/devkit_panel.h"
#include "ui/display.h"

// ST's USB device handle for the Seed's own port, defined in libDaisy's C code.
extern "C" USBD_HandleTypeDef hUsbDeviceFS;

using namespace daisy;
using namespace drom;
namespace devkit = daisy::desktop_devkit;

namespace {

devkit::Hardware hw;

/// One line of status text to the Seed's USB serial port, or nothing.
///
/// Not libDaisy's Logger: after two packets it switches to
/// `while(!Transmit()) {}`, which spins forever once nobody on the host is
/// reading the port. That froze the main loop — keys, pots and LEDs dead while
/// audio played on — the first time a terminal was closed. A status line is
/// worth dropping; the panel is not.
///
/// Two buffers, and the index only advances on a successful send: the CDC
/// transfer reads its buffer asynchronously, so the one just handed off must
/// not be reformatted while it may still be on the wire.
void Status(const char *fmt, ...)
{
    // Before enumeration (no cable, or only the kit's USB attached) the CDC
    // class data is null, and CDC_Transmit_FS dereferences it unchecked.
    if(hUsbDeviceFS.pClassData == nullptr)
        return;

    static char buf[2][160];
    static int  idx = 0;
    char       *b   = buf[idx];

    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(b, sizeof(buf[0]) - 2, fmt, ap);
    va_end(ap);
    if(n < 0)
        return;
    if(n > static_cast<int>(sizeof(buf[0])) - 3)
        n = static_cast<int>(sizeof(buf[0])) - 3;
    b[n++] = '\r';
    b[n++] = '\n';

    if(hw.seed.usb_handle.TransmitInternal(reinterpret_cast<uint8_t *>(b),
                                           static_cast<size_t>(n))
       == UsbHandle::Result::OK)
        idx ^= 1;
}

// Machine (~50 kB) is far too large for a stack frame — see CLAUDE.md. It stays
// in DTCM because the audio callback walks it every sample.
Machine         g_machine;
Ui              g_ui;
DevkitPanel     g_panel;
DisplayRenderer g_display;
QspiFlash       g_flash;

// --- flash-side buffers, in SDRAM ---------------------------------------------
//
// Storage holds a ~31 kB staging Patch and a load needs another to land in.
// With Machine and the board class, those two push .bss ~47 kB past the
// 128 kB DTCM, and the D2 SRAM has no linker section. They are only touched
// on an explicit save or load, so SDRAM's latency costs nothing.
//
// Both have constructors (Patch has member initialisers), and SDRAM does not
// work until hw.Init() — a DSY_SDRAM_BSS object of either type would be
// constructed before main() and fault. So: raw storage here, placement-new
// after init.
alignas(Storage) uint8_t DSY_SDRAM_BSS g_storage_mem[sizeof(Storage)];
alignas(Patch) uint8_t DSY_SDRAM_BSS   g_loaded_mem[sizeof(Patch)];
Storage *g_storage = nullptr;

/// Where a load lands before the audio side adopts it. Must outlive the
/// RequestLoad, which a static buffer does.
Patch *g_loaded = nullptr;

// --- master FX buffers, in SDRAM ----------------------------------------------
//
// The delay's 768 kB only fits SDRAM; the reverb's ~165 kB goes to AXI SRAM,
// below. DSY_SDRAM_BSS objects exist before SDRAM is initialised in hw.Init(),
// so only plain arrays go there.
//
// The delay buffer's length is the top of DLY TIME's range — the parameter is
// stored as 0..1 and mapped onto whatever buffer the board provides — so
// changing this changes what every saved delay time sounds like.
constexpr size_t  kDelayFrames = 2 * 48000; // 2 s at 48 kHz
float DSY_SDRAM_BSS g_delay[kDelayFrames * 2];
constexpr size_t    kReverbFloats = PlateReverb::BufferFloats(48000.f);
// AXI SRAM, not SDRAM: the plate reads 14 taps and 12 lines scattered over
// its buffer every sample, and from SDRAM those cache misses made it cost
// more than ReverbSc. ld/drom_sram.lds defines the section.
float __attribute__((section(".sram_bss"))) g_reverb[kReverbFloats];

// --- the kit's physical layout -----------------------------------------------

/// Panel key -> the kit's button/LED index. Identity: button 0..7 is the top
/// row left to right, 8..15 the bottom. One table, so a different reading of
/// the board is a one-line fix — the boot sweep lights them in this order to
/// make a mismatch obvious.
constexpr int kKeyToButton[kDevkitKeys] = {0, 1, 2,  3,  4,  5,  6,  7,
                                           8, 9, 10, 11, 12, 13, 14, 15};

constexpr uint32_t kBootSweepMs = 40;  ///< per key, so ~0.6 s in all
constexpr uint32_t kLedFrameMs  = 16;  ///< ~60 Hz; the I2C frame takes ~0.5 ms
constexpr uint32_t kStatusMs    = 100; ///< serial text no faster than 10 Hz
constexpr uint32_t kToggleMs    = 20;  ///< SW18 must hold still this long
constexpr uint32_t kCpuMs       = 5000; ///< CPU load report interval

/// The kit's LEDs are wired active-low, and `Hardware::SetLed` gamma-corrects
/// *before* inverting — so the curve comes out backwards and dim levels look
/// nearly as bright as full. Gamma here, invert, and write the raw count.
void WriteLed(int led, float level)
{
    if(level < 0.f) level = 0.f;
    if(level > 1.f) level = 1.f;
    const float    duty = level * level * std::sqrt(level); // ~gamma 2.5
    const uint16_t raw  = static_cast<uint16_t>((1.f - duty) * 4095.f + 0.5f);
    hw.led_driver.SetLedRaw(led, raw);
}

/// SW18 has no debouncer in the board class (it is a raw GPIO read), and a
/// layer change is not free — entering or leaving PATTERN presses or releases
/// a held mode — so a bouncing lever must settle before it counts.
DevkitPanel::Layer ReadLayer(uint32_t now)
{
    static int      last   = -1;
    static uint32_t since  = 0;
    static auto     stable = DevkitPanel::Layer::Steps;

    const int pos = hw.tog_sw18.Read();
    if(pos != last)
    {
        last  = pos;
        since = now;
    }
    else if(now - since >= kToggleMs)
    {
        switch(pos)
        {
            case Switch3::POS_UP: stable = DevkitPanel::Layer::Steps; break;
            case Switch3::POS_DOWN: stable = DevkitPanel::Layer::Patterns; break;
            default: stable = DevkitPanel::Layer::Tracks; break;
        }
    }
    return stable;
}

const char *LayerName(DevkitPanel::Layer l)
{
    switch(l)
    {
        case DevkitPanel::Layer::Steps: return "STEPS";
        case DevkitPanel::Layer::Tracks: return "TRACKS";
        default: return "PATTERNS";
    }
}

/// Bank 0's slots, read from their headers. Only the first bank: the kit has
/// no encoder to page through the other seven.
uint16_t ScanSlots()
{
    uint16_t mask = 0;
    for(int i = 0; i < kDevkitKeys; ++i)
        if(g_storage->PeekPatch(static_cast<uint32_t>(i)) == Storage::Result::Ok)
            mask |= static_cast<uint16_t>(1u << i);
    return mask;
}

/// A beat to start from when slot 1 is empty, so a first boot makes sound the
/// moment PLAY is pressed.
void SeedDemoPattern(Patch &p)
{
    auto on = [&](int trk, int step, int vel) {
        p.pattern.tracks[trk].steps[step].flags    = kStepActive;
        p.pattern.tracks[trk].steps[step].velocity = static_cast<uint8_t>(vel);
    };
    on(0, 0, 110); on(0, 6, 80); on(0, 10, 110);
    on(1, 4, 115); on(1, 12, 115);
    for(int i = 0; i < 16; i += 2)
        on(2, i, i % 4 ? 60 : 95);
}

/// Reported over USB every few seconds. The default kit's absolute worst case
/// — every track on every step at maximum DECAY, with reverb, delay and
/// compressor engaged — measures 48 % (src/bench.cpp, docs/02-firmware.md §10).
/// Past 100 % the callback overruns and starves the main loop, so a frozen
/// panel with audio still playing is what an overload looks like.
CpuLoadMeter g_load;

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                   AudioHandle::InterleavingOutputBuffer out,
                   size_t                                size)
{
    (void)in;
    g_load.OnBlockStart();
    // `size` counts samples across both channels; the machine wants frames.
    g_machine.Process(out, size / 2);
    g_load.OnBlockEnd();
}

} // namespace

int main(void)
{
    hw.Init();

    // Seed's own USB-C, not the kit's: that is the port the bootloader's DFU
    // also uses, so flashing and reading status share one cable. Raw CDC rather
    // than StartLog — see Status() for why the Logger cannot be used here.
    hw.seed.usb_handle.Init(UsbHandle::FS_INTERNAL);

    g_flash.Init(&hw.seed.qspi);
    g_storage = new(g_storage_mem) Storage;
    g_loaded  = new(g_loaded_mem) Patch;
    g_storage->Init(&g_flash);

    // 32 frames is a 0.667 ms callback — docs/02-firmware.md §3.
    hw.seed.SetAudioBlockSize(32);
    const float sr = hw.seed.AudioSampleRate();
    // At any rate but 48 kHz the reverb buffer is undersized, and the reverb
    // stays silent rather than overrunning it.
    g_machine.Init(sr, g_delay, kDelayFrames, g_reverb, kReverbFloats);

    // Slot 1 is what the machine wakes up with. Loaded through the audio side
    // like any other load, so ApplyKit pushes it to every voice.
    int current_slot = -1;
    if(g_storage->LoadPatch(0, *g_loaded) == Storage::Result::Ok)
    {
        g_machine.RequestLoad(g_loaded);
        current_slot = 0;
    }
    else
        SeedDemoPattern(g_machine.mutable_patch()); // safe: audio not started

    g_ui.Init(&g_machine);
    g_panel.Init(&g_ui, &g_machine);
    uint16_t slots = ScanSlots();
    g_panel.SetSlots(slots, current_slot);

    g_load.Init(sr, 32);
    hw.seed.StartAudio(AudioCallback);

    Status("drom-triks on the desktop dev kit");
    Status("audio %u Hz, block 32; slot 1 %s", static_cast<unsigned>(sr),
                      current_slot == 0 ? "loaded" : "empty, demo pattern");

    const uint32_t boot      = System::GetNow();
    uint32_t       last_ms   = boot;
    uint32_t       last_led  = 0;
    uint32_t       last_stat = 0;
    uint32_t       last_cpu  = 0;
    int            pending_save = -1;
    bool           last_shift   = false;
    auto           last_layer   = DevkitPanel::Layer::Steps;
    DisplayLines   shown, lines;
    bool           first_status = true;

    while(1)
    {
        // 1 kHz, off the SysTick millisecond. The controls' filters and
        // debouncers are initialised for exactly this rate (AnalogControl at
        // 1000 Hz, DigitalControl's 8-sample shift), so the rate is part of
        // their behaviour rather than just a scheduling choice.
        const uint32_t now = System::GetNow();
        if(now == last_ms)
            continue;
        last_ms = now;

        hw.UpdateAllControls();
        g_panel.SetTime(now);

        const bool shift = hw.tog_sw17.Pressed();
        const auto layer = ReadLayer(now);
        g_panel.SetShift(shift);
        g_panel.SetLayer(layer);

        for(int k = 0; k < kDevkitKeys; ++k)
        {
            const auto &b = hw.button[kKeyToButton[k]];
            if(b.RisingEdge())
                g_panel.KeyDown(k);
            else if(b.FallingEdge())
                g_panel.KeyUp(k);
        }
        for(int i = 0; i < kDevkitPots; ++i)
            g_panel.Pot(i, hw.pot[i].Value());

        // Flash work. Saving is two-phase: the audio side snapshots, then the
        // write happens here, where a stall of a few ms costs a late LED frame
        // and nothing audible. See docs/02-firmware.md §8.
        const auto req = g_ui.TakeStorageRequest();
        if(req.type == Ui::StorageRequest::Type::SavePattern)
        {
            g_machine.RequestSnapshot(&g_storage->staging());
            pending_save = req.slot;
        }
        else if(req.type == Ui::StorageRequest::Type::LoadPattern)
        {
            const auto r = g_storage->LoadPatch(req.slot, *g_loaded);
            if(r == Storage::Result::Ok)
            {
                g_machine.RequestLoad(g_loaded);
                current_slot = req.slot;
            }
            Status("load slot %u: %s", req.slot + 1u, Storage::Describe(r));
            g_panel.SetSlots(slots, current_slot);
        }
        if(pending_save >= 0 && g_machine.SnapshotReady())
        {
            const auto r = g_storage->SaveStaged(static_cast<uint32_t>(pending_save));
            if(r == Storage::Result::Ok)
            {
                slots |= static_cast<uint16_t>(1u << pending_save);
                current_slot = pending_save;
            }
            Status("save slot %d: %s", pending_save + 1, Storage::Describe(r));
            g_panel.SetSlots(slots, current_slot);
            pending_save = -1;
        }

        if(now - last_led >= kLedFrameMs)
        {
            last_led = now;
            float level[kDevkitKeys];
            g_panel.Render(level);

            // Boot sweep: each key lights in panel order, so a wrong
            // kKeyToButton shows up as a sweep that jumps around.
            const uint32_t t = now - boot;
            if(t < kBootSweepMs * kDevkitKeys)
                for(int k = 0; k < kDevkitKeys; ++k)
                    level[k] = (k == static_cast<int>(t / kBootSweepMs)) ? 1.f : 0.f;

            for(int k = 0; k < kDevkitKeys; ++k)
                WriteLed(kKeyToButton[k], level[k]);
            hw.UpdateLeds();
            hw.seed.SetLed((now & 1023) < 64); // heartbeat: the main loop is alive
        }

        // The screen the kit does not have, as text. Only on change, so a
        // terminal left open is a log of what you did rather than a flood.
        if(shift != last_shift || layer != last_layer || first_status)
        {
            Status("keys: %s   shift: %s", LayerName(layer),
                              shift ? "ON" : "off");
            last_shift   = shift;
            last_layer   = layer;
            first_status = false;
        }
        // CPU, every few seconds, with the peak since the last report. Past
        // ~100 % the callback overruns and starves this loop, so the line
        // that would say so may never print — a silent panel is the symptom.
        if(now - last_cpu >= kCpuMs)
        {
            last_cpu = now;
            Status("cpu avg %d%%  max %d%%",
                              static_cast<int>(g_load.GetAvgCpuLoad() * 100.f + 0.5f),
                              static_cast<int>(g_load.GetMaxCpuLoad() * 100.f + 0.5f));
            g_load.Reset();
        }

        if(now - last_stat >= kStatusMs)
        {
            last_stat = now;
            g_display.Render(g_machine, g_ui, lines);
            // Row 2 is the playhead in the overview and changes every step, so
            // it is printed but never the reason to print.
            bool changed = false;
            for(int r = 0; r < DisplayLines::kRows; ++r)
                if(r != 2 && std::strcmp(lines.text[r], shown.text[r]) != 0)
                    changed = true;
            if(changed)
            {
                shown = lines;
                Status("%s | %s | %s | %s", lines.text[0], lines.text[1],
                                  lines.text[2], lines.text[3]);
            }
        }
    }
}
