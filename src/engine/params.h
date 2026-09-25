#pragma once
#include "voice.h"

namespace drom {

/// Static parameter metadata.
///
/// This is the part of a JUCE-style ValueTree worth having — one place that
/// knows a parameter's name and default — without the tree, the observers or
/// the allocation. A `constexpr` table costs nothing at runtime and lives in
/// flash rather than RAM.
struct ParamInfo
{
    const char *name;
    float       def;
    /// Declared so the format is frozen, but not yet wired to anything. The UI
    /// shows these as inactive rather than as a knob that does nothing, which
    /// is the distinction docs/01-hardware.md §2 cares about.
    bool        reserved;
};

#define DROM_P(id, label) {label, kParamDefault[static_cast<int>(ParamId::id)], false}
#define DROM_R(id)        {"--",  kParamDefault[static_cast<int>(ParamId::id)], true}

inline constexpr ParamInfo kParamInfo[static_cast<int>(ParamId::Count)] = {
    // Page 1: INST
    DROM_P(Tune, "TUNE"),   DROM_P(Decay, "DECAY"), DROM_P(Tone, "TONE"),
    DROM_P(Snap, "SNAP"),   DROM_P(Drive, "DRIVE"), DROM_P(Level, "LEVEL"),
    DROM_P(Pan, "PAN"),     DROM_P(Note, "NOTE"),

    // Page 2: FLTR
    DROM_P(SatDrive, "SAT"),
    DROM_P(Filter1Cutoff, "CUT 1"), DROM_P(Filter1Res, "RES 1"),
    DROM_P(Filter1Mode, "MODE 1"),
    DROM_P(Filter2Cutoff, "CUT 2"), DROM_P(Filter2Res, "RES 2"),
    DROM_P(Filter2Mode, "MODE 2"),
    DROM_R(FltrRsv1),

    // Page 3: FX  — sends to the one shared delay and the one shared reverb
    DROM_P(DelaySend, "DLY SND"), DROM_P(ReverbSend, "REV SND"),
    DROM_R(FxRsv1), DROM_R(FxRsv2), DROM_R(FxRsv3),
    DROM_R(FxRsv4), DROM_R(FxRsv5), DROM_R(FxRsv6),

    // Page 4: LFO
    DROM_P(LfoSpeed, "SPEED"), DROM_P(LfoMult, "MULT"),
    DROM_P(LfoFade, "FADE"),   DROM_P(LfoDest, "DEST"),
    DROM_P(LfoWave, "WAVE"),   DROM_P(LfoMode, "MODE"),
    DROM_P(LfoDepth, "DEPTH"), DROM_P(LfoStartPhase, "PHASE"),
};

#undef DROM_P
#undef DROM_R

/// Master effect parameters. Global rather than per-track, so they live in the
/// Kit — a kit is the "sound" half of a patch, and delay time is part of a
/// sound. Frozen at 16 for the same reason ParamId is frozen at 32: the count
/// is baked into sizeof(Patch). See docs/02-firmware.md §6.
enum class FxId : uint8_t
{
    DelayTime = 0,
    DelayFeedback,
    DelayWidth,
    DelayTone,

    ReverbSize,
    ReverbDamp,
    ReverbLevel,
    ReverbPreDelay,

    CompThreshold,
    CompRatio,
    CompAttack,
    CompRelease,
    CompMakeup,

    FxRsv1,
    FxRsv2,
    FxRsv3,

    Count
};

inline constexpr int kNumFxParams = static_cast<int>(FxId::Count);

inline constexpr float kFxDefault[kNumFxParams] = {
    // delay: time  fb    width tone
    0.375f, 0.35f, 0.5f, 0.6f,
    // reverb: size damp  level predelay
    0.5f,   0.5f,  0.0f, 0.0f,
    // comp: thresh ratio attack release makeup
    1.0f,   0.0f,  0.2f, 0.3f, 0.0f,
    // reserved
    0.5f, 0.5f, 0.5f,
};

inline constexpr const char *kFxName[kNumFxParams] = {
    "DLY TIME", "DLY FB",  "DLY WID", "DLY TONE",
    "REV SIZE", "REV DAMP", "REV LVL", "REV PRE",
    "CMP THR",  "CMP RAT", "CMP ATK", "CMP REL", "CMP MKP",
    "--", "--", "--",
};

inline constexpr const char *kPageName[kNumPages] = {"INST", "FLTR", "FX", "LFO"};

/// Which page a parameter lives on, and its position within that page — i.e.
/// which of the eight macro encoders reaches it.
inline constexpr int ParamPage(ParamId id)
{
    return static_cast<int>(id) / kParamsPerPage;
}

inline constexpr int ParamSlot(ParamId id)
{
    return static_cast<int>(id) % kParamsPerPage;
}

/// The parameter reached by encoder `slot` while `page` is selected.
inline constexpr ParamId ParamAt(int page, int slot)
{
    return static_cast<ParamId>(page * kParamsPerPage + slot);
}

/// Master-FX slots that are declared but not wired to anything, by the same
/// convention as ParamReserved: the UI shows them inactive rather than live.
inline bool FxReserved(int i)
{
    return i < 0 || i >= kNumFxParams || kFxName[i][0] == '-';
}

inline bool ParamReserved(ParamId id)
{
    const int i = static_cast<int>(id);
    return i < 0 || i >= static_cast<int>(ParamId::Count) || kParamInfo[i].reserved;
}

inline const char *ParamName(ParamId id)
{
    const int i = static_cast<int>(id);
    return (i >= 0 && i < static_cast<int>(ParamId::Count)) ? kParamInfo[i].name : "?";
}

inline float ParamDefault(ParamId id)
{
    const int i = static_cast<int>(id);
    return (i >= 0 && i < static_cast<int>(ParamId::Count)) ? kParamInfo[i].def : 0.5f;
}

} // namespace drom
