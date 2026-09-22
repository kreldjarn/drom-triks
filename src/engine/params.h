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
};

inline constexpr ParamInfo kParamInfo[static_cast<int>(ParamId::Count)] = {
    {"TUNE", 0.5f},
    {"DECAY", 0.5f},
    {"TONE", 0.5f},
    {"SNAP", 0.5f},
    {"DRIVE", 0.0f},
    {"LEVEL", 0.8f},
};

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
