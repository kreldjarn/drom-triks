#pragma once
#include <cstdint>
#include <cstring>
#include "../engine/params.h"
#include "../seq/pattern.h"

namespace drom {

/// Every track's base parameter values — the "sound" half of a patch.
///
/// The cartridge tracks carry parameters like any other, so a kit stays
/// meaningful across a cartridge swap: the macros are normalised, so a
/// stored TUNE still means TUNE on whatever is plugged in.
struct Kit
{
    float params[kNumTracks][static_cast<int>(ParamId::Count)];

    /// Which synthesis machine each track runs. Stored as the underlying type
    /// rather than MachineId so this header stays free of DaisySP — a load must
    /// range-check it, because a byte from flash is not a guarantee.
    uint8_t machine[kNumTracks];

    /// Master delay, reverb and compressor settings. Global rather than
    /// per-track, and in the Kit rather than the Pattern because a kit is the
    /// "sound" half of a patch and delay time is part of a sound. Frozen at 16
    /// for the same reason ParamId is frozen at 32 — the count is baked into
    /// sizeof(Patch). See docs/02-firmware.md §6.
    float fx[kNumFxParams];

    char  name[16];
};

/// What each track powers on as, chosen so a default kit sounds exactly as it
/// did before machines were selectable.
inline constexpr MachineId kDefaultMachine[kNumTracks] = {
    MachineId::BdAnalog, MachineId::SdSynth, MachineId::HatClosed,
    MachineId::HatOpen,  MachineId::Tom,     MachineId::Clap,
    MachineId::RimShot,  MachineId::FmPerc,
    MachineId::Silent,   MachineId::Silent,
    MachineId::Silent,   MachineId::Silent,
};

/// Fixed track identities. The panel is legended in silkscreen, so these are
/// not user-editable and can live in flash. C1–C4 are the cartridge slots;
/// their legend is deliberately generic because what is in them changes, and
/// the cartridge's own name comes from its on-board EEPROM at boot.
inline constexpr const char *kTrackName[kNumTracks]
    = {"BD", "SD", "CH", "OH", "LT", "CP", "RS", "FM", "C1", "C2", "C3", "C4"};

inline constexpr uint32_t kPatchMagic = 0x4D4F5244; // 'DROM'

/// Bumped to 2 when tracks went from 8 to 12. `Pattern` and `Kit` both changed
/// size, so a v1 save read as v2 would be reinterpreted rather than rejected —
/// exactly the failure SaveHeader exists to catch. `payload_size` alone would
/// have caught this one, but only because the size happened to change.
///
/// Bumped to 4 when each track gained a selectable machine. Still before
/// Phase 6, so still free — which is the rule the freeze actually states.
///
/// Bumped to 3 for the format freeze: kMaxLocks 4 -> 8 (resizing Step) and
/// ParamId::Count 6 -> 32 (resizing Kit). Both were taken at once, and
/// deliberately before Phase 6 writes anything real to flash, because every
/// such change invalidates every stored pattern. Anything that alters
/// sizeof(Patch) belongs on this side of that line — see
/// docs/02-firmware.md §6.
inline constexpr uint16_t kPatchVersion = 4;

/// Guards a saved struct against being read by a different firmware version.
///
/// This is the second thing a ValueTree would have given us, and the one that
/// actually bites: `Pattern` is trivially copyable, so saving is a memcpy —
/// but that means adding one field to `Step` silently reinterprets every
/// pattern already in flash. The result sounds like corruption rather than
/// like a version mismatch, and it is unobvious enough to cost an evening.
struct SaveHeader
{
    uint32_t magic        = kPatchMagic;
    uint16_t version      = kPatchVersion;
    uint16_t payload_size = 0;
};

struct Patch
{
    SaveHeader header;
    Pattern    pattern;
    Kit        kit;
};

inline void InitKit(Kit &k)
{
    for(int t = 0; t < kNumTracks; ++t)
        for(int p = 0; p < static_cast<int>(ParamId::Count); ++p)
            k.params[t][p] = kParamInfo[p].def;
    for(int i = 0; i < kNumFxParams; ++i)
        k.fx[i] = kFxDefault[i];
    for(int t = 0; t < kNumTracks; ++t)
        k.machine[t] = static_cast<uint8_t>(kDefaultMachine[t]);
    std::memset(k.name, 0, sizeof(k.name));
}

inline void InitPatch(Patch &patch)
{
    patch = Patch{};
    patch.header.magic        = kPatchMagic;
    patch.header.version      = kPatchVersion;
    patch.header.payload_size = static_cast<uint16_t>(sizeof(Pattern) + sizeof(Kit));
    InitKit(patch.kit);
}

/// Reject rather than reinterpret. A caller that gets false should fall back
/// to defaults, not try to salvage the bytes.
inline bool ValidatePatch(const Patch &patch)
{
    return patch.header.magic == kPatchMagic
           && patch.header.version == kPatchVersion
           && patch.header.payload_size
                  == static_cast<uint16_t>(sizeof(Pattern) + sizeof(Kit));
}

} // namespace drom
