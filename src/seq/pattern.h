#pragma once
#include <cstdint>
#include "../engine/voice.h"

// Pattern data model. Plain data, no behaviour — the sequencer interprets it.
// Kept free of libDaisy so it builds on the host (see CLAUDE.md).

namespace drom {

inline constexpr int kPpqn = 96;

/// 24 ticks per 16th step. 96 PPQN divides cleanly by MIDI's 24 PPQN for clock
/// output, and gives 1/24-of-a-step micro-timing resolution — about 5 ms at
/// 120 BPM, which is the granularity that makes a groove sit differently.
inline constexpr int kTicksPerStep = kPpqn / 4;

inline constexpr int kMaxSteps = 64;

/// Eight digital voices plus four analog cartridge slots.
///
/// The cartridge tracks are additive, not substitutions: they exist in the
/// sequencer whether or not a carrier board is attached, so their steps, locks
/// and micro-timing drive the trigger outputs for external gear on their own,
/// and an unpopulated slot is simply a silent track. Making them conditional
/// would mean a pattern meaning different things depending on what is plugged
/// in. See docs/05-analog-expansion.md §1.
inline constexpr int kNumDigitalVoices  = 8;
inline constexpr int kNumCartridgeSlots = 4;
inline constexpr int kNumTracks = kNumDigitalVoices + kNumCartridgeSlots;

/// Micro-timing is capped at just under one step in each direction. Beyond
/// that "which step is this" stops being answerable — a step pushed a full
/// step late is indistinguishable from the next step early, and the UI would
/// have no honest way to draw it.
inline constexpr int kMicroRange = kTicksPerStep - 1; // +/-23

enum StepFlags : uint8_t
{
    kStepActive = 1 << 0,
    kStepAccent = 1 << 1,
    kStepTie    = 1 << 2,
};

struct Step
{
    uint8_t   flags       = 0;
    uint8_t   velocity    = 100; ///< 0–127
    int8_t    micro       = 0;   ///< ticks, +/-kMicroRange
    uint8_t   probability = 100; ///< percent
    uint8_t   ratchet     = 1;   ///< retriggers within the step, 1–8
    uint8_t   lock_count  = 0;
    ParamLock locks[kMaxLocks] = {};

    bool active() const { return (flags & kStepActive) != 0; }
    bool accent() const { return (flags & kStepAccent) != 0; }
};

/// The per-step fields SHIFT + a held step exposes on the macro encoders.
/// docs/02-firmware.md §7 calls this "step detail".
enum class StepField : uint8_t
{
    Velocity = 0,
    Micro,
    Probability,
    Ratchet,
    Count
};

enum class Direction : uint8_t
{
    Forward = 0,
    Reverse,
    PingPong,
    Random,
};

struct Track
{
    Step      steps[kMaxSteps];
    uint8_t   length    = 16;
    /// Powers of two relative to 16ths: 0 = 1x, +1 = double time, -1 = half.
    int8_t    speed     = 0;
    Direction direction = Direction::Forward;
    bool      muted     = false;

    /// Per-track length is what gives polymeter — a 7-step hat against a
    /// 16-step kick is one byte of state and the highest ratio of musical
    /// interest to implementation effort in the whole sequencer.
    int ticks_per_step() const
    {
        return speed >= 0 ? (kTicksPerStep >> speed) : (kTicksPerStep << (-speed));
    }

    /// PingPong walks out and back without repeating the endpoints.
    int positions_per_cycle() const
    {
        if(direction == Direction::PingPong && length > 2)
            return 2 * length - 2;
        return length;
    }
};

struct Pattern
{
    Track    tracks[kNumTracks];
    uint16_t bpm_x10 = 1200;
    uint8_t  swing   = 50; ///< 50 = straight; above that delays odd steps
    uint8_t  kit_id  = 0;
};

} // namespace drom
