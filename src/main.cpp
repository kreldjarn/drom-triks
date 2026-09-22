// Phase 0 bring-up.
//
// This is throwaway code with one job: prove the four assumptions the rest of
// the build rests on, so that a failure in Phase 2+ is a bug in Phase 2+ code
// and not a toolchain or board problem.
//
//   1. Init + main loop        -> the user LED blinks
//   2. Audio path              -> a 440 Hz sine on both channels, for a noise-floor measurement
//   3. QSPI write under BOOT_SRAM -> boot_count increments across power cycles
//   4. USB logging             -> results are readable without a debugger
//
// Expected serial output on a healthy board, second boot onward:
//
//   drom-triks phase 0 bring-up
//   qspi: state=USER boot_count=2 (magic ok)
//   audio: 48000 Hz, block 48
//   ready
//
// See docs/00-toolchain.md for the full pass/fail checklist.

#include "daisy_seed.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

static DaisySeed  hw;
static Oscillator osc;

// QSPI user-data base, as an offset from the chip base (0x90000000).
//
// Under BOOT_SRAM the app image is staged at chip offset 0x40000 and can grow
// to the 480 kB SRAM limit, i.e. up to 0xB8000. Anything at or past 1 MB is
// therefore permanently clear of it, with slack to spare.
//
// PersistentStorage defaults this offset to 0, which would put user data on top
// of the app image: the first pattern save would corrupt the firmware that was
// executing the save. Never take the default here.
static constexpr uint32_t kUserDataOffset = 0x100000;

// Guards against reading a stale or never-initialised struct as valid settings.
static constexpr uint32_t kSettingsMagic = 0x44524F4D;  // 'DROM'

struct BringUpSettings
{
    uint32_t magic;
    uint32_t boot_count;

    // PersistentStorage compares with != to decide whether an erase/write is
    // actually needed, so this operator is required, not optional.
    bool operator!=(const BringUpSettings &o) const
    {
        return magic != o.magic || boot_count != o.boot_count;
    }
};

static PersistentStorage<BringUpSettings> storage(hw.qspi);

static void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                          AudioHandle::InterleavingOutputBuffer out,
                          size_t                                size)
{
    for(size_t i = 0; i < size; i += 2)
    {
        const float sig = osc.Process();
        out[i]          = sig;
        out[i + 1]      = sig;
    }
}

int main(void)
{
    hw.Init();

    // true = block until a serial monitor attaches. Worth it here because the
    // QSPI result prints once at startup and is the whole point of the test.
    hw.StartLog(true);
    hw.PrintLine("drom-triks phase 0 bring-up");

    // --- 3. QSPI round-trip --------------------------------------------------
    // The real test is a power cycle: boot_count must survive it. A value that
    // resets to 1 every boot means the write silently failed.
    BringUpSettings defaults{kSettingsMagic, 0};
    storage.Init(defaults, kUserDataOffset);

    BringUpSettings &settings = storage.GetSettings();
    const bool       magic_ok = settings.magic == kSettingsMagic;

    settings.boot_count++;
    storage.Save();

    hw.PrintLine("qspi: state=%s boot_count=%u (magic %s)",
                 storage.GetState() == PersistentStorage<BringUpSettings>::State::FACTORY
                     ? "FACTORY"
                     : "USER",
                 static_cast<unsigned>(settings.boot_count),
                 magic_ok ? "ok" : "BAD");

    // --- 2. Audio ------------------------------------------------------------
    // Block size 48 is a placeholder for bring-up only. The real firmware uses
    // 32 for a 0.667 ms callback; see docs/02-firmware.md §3.
    hw.SetAudioBlockSize(48);
    const float sample_rate = hw.AudioSampleRate();

    osc.Init(sample_rate);
    osc.SetWaveform(Oscillator::WAVE_SIN);
    osc.SetFreq(440.f);
    // -12 dBFS, not full scale: leaves headroom so a noise-floor reading measures
    // the codec rather than the output stage clipping.
    osc.SetAmp(0.25f);

    hw.StartAudio(AudioCallback);
    hw.PrintLine("audio: %u Hz, block 48", static_cast<unsigned>(sample_rate));
    hw.PrintLine("ready");

    // --- 1. Main loop --------------------------------------------------------
    // 1 Hz blink. If this stops, the audio callback is overrunning or something
    // has hard-faulted.
    bool led_state = false;
    while(1)
    {
        led_state = !led_state;
        hw.SetLed(led_state);
        System::Delay(500);
    }
}
