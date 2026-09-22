#pragma once
#include <cmath>
#include <cstdint>
#include "../io/patch.h"

namespace drom {

inline constexpr int kNumPots      = 6;
inline constexpr int kNumStepKeys  = 16;

/// Panel state machine. Pure logic: it takes debounced key edges and pot
/// positions, and mutates the patch. No hardware, no drawing — which is what
/// lets the whole interaction model be tested headless, before a panel exists.
///
/// SHIFT is a held modifier rather than a latched mode, so there is no state
/// to get stuck in.
class Ui
{
  public:
    enum class Mode : uint8_t
    {
        Play = 0, ///< step keys toggle steps on the selected track
        Mute,     ///< track keys mute/unmute instead of selecting
    };

    void Init(Patch *patch)
    {
        patch_ = patch;
        mode_  = Mode::Play;
        selected_track_ = 0;
        held_step_      = -1;
        shift_          = false;
        // Nothing is caught until a knob is moved: on power-up the physical
        // positions bear no relation to the loaded patch.
        for(int i = 0; i < kNumPots; ++i)
        {
            caught_[i]   = false;
            last_raw_[i] = -1.f;
        }
    }

    // ---- input ------------------------------------------------------------

    void SetShift(bool held) { shift_ = held; }

    void SetMode(Mode m) { mode_ = m; }

    void TrackPress(int track)
    {
        if(track < 0 || track >= kNumTracks)
            return;
        if(mode_ == Mode::Mute)
        {
            patch_->pattern.tracks[track].muted = !patch_->pattern.tracks[track].muted;
            return;
        }
        if(track != selected_track_)
        {
            selected_track_ = track;
            // The knobs now point at a different voice's values, so every pot
            // is stale until it is moved back through the stored position.
            ReleaseAllPots();
        }
    }

    void StepPress(int step)
    {
        if(step < 0 || step >= kNumStepKeys || !patch_)
            return;
        held_step_ = step;

        // A held step is a p-lock target, not a toggle — the toggle happens on
        // release, and only if no lock was written. Otherwise writing a lock
        // would also flip the step off under your finger.
        wrote_lock_while_held_ = false;
    }

    void StepRelease(int step)
    {
        if(step != held_step_)
            return;
        if(!wrote_lock_while_held_ && !shift_)
            ToggleStep(step);
        held_step_ = -1;
    }

    /// Raw pot position, 0..1, straight from the ADC.
    void PotMove(int pot, float raw)
    {
        if(pot < 0 || pot >= kNumPots || !patch_)
            return;

        const ParamId id     = static_cast<ParamId>(pot);
        const float   stored = StoredValue(id);

        // Soft takeover (pickup). Six knobs address eight voices, so after a
        // track change the physical position is meaningless. The parameter
        // stays put until the knob passes through the stored value, which
        // stops a track change from jumping six parameters at once.
        if(!caught_[pot])
        {
            const float prev = last_raw_[pot];
            const bool  crossed
                = prev >= 0.f && ((prev <= stored && raw >= stored)
                                  || (prev >= stored && raw <= stored));
            if(crossed || std::fabs(raw - stored) <= kCatchTolerance)
                caught_[pot] = true;
        }
        last_raw_[pot] = raw;

        if(!caught_[pot])
            return;

        if(held_step_ >= 0)
        {
            WriteLock(held_step_, id, raw);
            wrote_lock_while_held_ = true;
        }
        else
        {
            patch_->kit.params[selected_track_][pot] = raw;
        }
    }

    // ---- state, for the display and LEDs -----------------------------------

    Mode mode() const { return mode_; }
    int  selected_track() const { return selected_track_; }
    int  held_step() const { return held_step_; }
    bool shift() const { return shift_; }
    bool pot_caught(int pot) const { return pot >= 0 && pot < kNumPots && caught_[pot]; }

    float value(ParamId id) const { return StoredValue(id); }

    bool step_active(int step) const
    {
        return Valid(step) && CurrentTrack().steps[step].active();
    }

    bool step_has_lock(int step) const
    {
        return Valid(step) && CurrentTrack().steps[step].lock_count > 0;
    }

    bool track_muted(int track) const
    {
        return track >= 0 && track < kNumTracks && patch_->pattern.tracks[track].muted;
    }

  private:
    static constexpr float kCatchTolerance = 0.02f;

    bool Valid(int step) const { return patch_ && step >= 0 && step < kNumStepKeys; }

    const Track &CurrentTrack() const { return patch_->pattern.tracks[selected_track_]; }
    Track       &CurrentTrack() { return patch_->pattern.tracks[selected_track_]; }

    float StoredValue(ParamId id) const
    {
        return patch_->kit.params[selected_track_][static_cast<int>(id)];
    }

    void ReleaseAllPots()
    {
        for(int i = 0; i < kNumPots; ++i)
            caught_[i] = false;
    }

    void ToggleStep(int step)
    {
        Step &s = CurrentTrack().steps[step];
        if(s.active())
        {
            s.flags &= static_cast<uint8_t>(~kStepActive);
            // Clearing the step clears its locks too: a lock on an inactive
            // step is invisible state that surprises you when it comes back.
            s.lock_count = 0;
        }
        else
        {
            s.flags |= kStepActive;
        }
    }

    void WriteLock(int step, ParamId id, float value)
    {
        Step &s = CurrentTrack().steps[step];

        // Locking an inactive step activates it — otherwise the knob appears
        // to do nothing and you have to guess that the step needed enabling.
        s.flags |= kStepActive;

        const uint8_t  pid = static_cast<uint8_t>(id);
        const uint16_t v   = static_cast<uint16_t>(value * 65535.f + 0.5f);

        for(uint8_t i = 0; i < s.lock_count; ++i)
            if(s.locks[i].param_id == pid)
            {
                s.locks[i].value = v;
                return;
            }

        if(s.lock_count < kMaxLocks)
        {
            s.locks[s.lock_count].param_id = pid;
            s.locks[s.lock_count].value    = v;
            ++s.lock_count;
        }
        // Past kMaxLocks the write is dropped. The UI should say so rather
        // than silently ignoring the knob.
    }

    Patch *patch_ = nullptr;
    Mode   mode_  = Mode::Play;
    int    selected_track_ = 0;
    int    held_step_      = -1;
    bool   shift_          = false;
    bool   wrote_lock_while_held_ = false;
    bool   caught_[kNumPots]   = {};
    float  last_raw_[kNumPots] = {};
};

} // namespace drom
