#pragma once
#include <cstdint>
#include <cstring>
#include "../engine/params.h"
#include "../seq/pattern.h"

namespace drom {

/// The eight voices' base parameter values — the "sound" half of a patch.
struct Kit
{
    float params[kNumTracks][static_cast<int>(ParamId::Count)];
    char  name[16];
};

/// Fixed track identities. The panel is legended in silkscreen, so these are
/// not user-editable and can live in flash.
inline constexpr const char *kTrackName[kNumTracks]
    = {"BD", "SD", "CH", "OH", "LT", "CP", "RS", "FM"};

inline constexpr uint32_t kPatchMagic   = 0x4D4F5244; // 'DROM'
inline constexpr uint16_t kPatchVersion = 1;

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
