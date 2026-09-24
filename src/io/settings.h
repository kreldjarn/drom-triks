#pragma once
#include <cstdint>
#include "patch.h"

// Global, per-unit settings: the one thing that stays in libDaisy's
// PersistentStorage rather than going through Storage.
//
// It qualifies because it is small, fixed-size and rarely written, which is
// exactly the shape PersistentStorage is built for — and exactly what a Patch
// is not. See docs/02-firmware.md §8.

namespace drom {

/// Reserved words for per-unit calibration. Cartridge CV needs an offset and a
/// gain per channel, and docs/11-production.md §3 wants that space to exist
/// before any unit ships, because it cannot be retrofitted into hardware in
/// someone else's hands. Reserving it costs 512 bytes of a 4 kB slot.
inline constexpr int kCalWords = 128;

inline constexpr uint32_t kSettingsMagic  = 0x54455344; // 'DSET'
inline constexpr uint16_t kSettingsVersion = 1;

struct Settings
{
    uint32_t magic   = kSettingsMagic;
    uint16_t version = kSettingsVersion;

    /// Written once at manufacture. 0 means "never programmed", which is how a
    /// test fixture knows a board is fresh.
    uint16_t serial_lo = 0;
    uint32_t serial_hi = 0;

    /// Per-source routing bitmask — MIDI in, USB in, host in, sequencer. See
    /// docs/06-midi.md §3.
    uint8_t midi_routes[4] = {0, 0, 0, 0};

    /// Global LED clamp. docs/01-hardware.md §4 makes this load-bearing rather
    /// than cosmetic: the panel can draw past USB 2.0's 500 mA without it.
    uint8_t led_brightness_pct = 23;

    uint8_t reserved[3] = {0, 0, 0};

    float calibration[kCalWords] = {};

    /// PersistentStorage decides whether a write is needed by comparing with
    /// `operator!=`, **not** `operator==` — so this operator is required, not
    /// optional. Getting it wrong means either never saving or erasing flash on
    /// every call. See CLAUDE.md.
    bool operator!=(const Settings &o) const
    {
        if(magic != o.magic || version != o.version || serial_lo != o.serial_lo
           || serial_hi != o.serial_hi || led_brightness_pct != o.led_brightness_pct)
            return true;
        for(int i = 0; i < 4; ++i)
            if(midi_routes[i] != o.midi_routes[i])
                return true;
        for(int i = 0; i < kCalWords; ++i)
            if(calibration[i] != o.calibration[i])
                return true;
        return false;
    }

    uint64_t serial() const
    {
        return (static_cast<uint64_t>(serial_hi) << 16) | serial_lo;
    }
};

static_assert(sizeof(Settings) <= 4096,
              "Settings must fit its 4 kB slot — see storage_layout.h");

} // namespace drom
