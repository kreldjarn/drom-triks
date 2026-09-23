#pragma once
#include <cmath>
#include <cstdint>
#include "../machine.h"

namespace drom {

/// Six macro encoders, one per ParamId. The two navigation encoders
/// (value/tempo, nav/page) are separate and do not address parameters.
inline constexpr int kNumMacros   = 6;
inline constexpr int kNumStepKeys = 16;

static_assert(kNumMacros == static_cast<int>(ParamId::Count),
              "one macro encoder per parameter");

/// Panel state machine. Pure logic: it takes debounced key edges and encoder
/// detents and emits Commands. No hardware, no drawing — which is what lets
/// the whole interaction model be tested headless, before a panel exists.
///
/// It never writes the patch. The audio side is the sole writer of pattern,
/// voice and sequencer state, which is what lets that side run without a
/// single lock. The UI reads the patch only to draw, where a stale or torn
/// field costs at worst one frame of wrong brightness.
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

    void Init(Machine *machine)
    {
        machine_ = machine;
        mode_  = Mode::Play;
        selected_track_ = 0;
        held_step_      = -1;
        shift_          = false;
        for(int i = 0; i < kNumMacros; ++i)
        {
            edit_value_[i]   = 0.f;
            last_turn_ms_[i] = 0;
            turning_[i]      = false;
        }
    }

    // ---- input ------------------------------------------------------------

    /// Called from the main loop with a free-running millisecond counter.
    void SetTime(uint32_t ms) { now_ms_ = ms; }

    void SetShift(bool held) { shift_ = held; }

    void SetMode(Mode m) { mode_ = m; }

    void TrackPress(int track)
    {
        if(track < 0 || track >= kNumTracks)
            return;
        if(mode_ == Mode::Mute)
        {
            Command c;
            c.type  = Command::Type::SetTrackMute;
            c.track = static_cast<uint8_t>(track);
            c.value = track_muted(track) ? 0.f : 1.f;
            machine_->Push(c);
            return;
        }
        // No pickup logic on a track change: an endless encoder has no
        // physical position to strand, so the six macros simply address the
        // new track's values from the next detent onward. The in-flight edit
        // values must still be dropped, or a gesture continuing across the
        // change would apply the old track's value to the new one.
        if(track != selected_track_)
        {
            selected_track_ = track;
            EndGestures();
        }
    }

    void StepPress(int step)
    {
        if(step < 0 || step >= kNumStepKeys || !machine_)
            return;
        held_step_ = step;

        // A held step is a p-lock target, not a toggle — the toggle happens on
        // release, and only if no lock was written. Otherwise writing a lock
        // would also flip the step off under your finger.
        wrote_lock_while_held_ = false;

        // Entering lock mode changes what a macro edits, from the track's base
        // to this step's lock, so any gesture in flight has to re-seed.
        EndGestures();
    }

    void StepRelease(int step)
    {
        if(step != held_step_)
            return;
        if(!wrote_lock_while_held_ && !shift_)
            ToggleStep(step);
        held_step_ = -1;
        EndGestures();
    }

    /// One or more detents on macro encoder `enc`. `delta` is signed detents
    /// since the last call — normally +/-1, more if the scan coalesced a fast
    /// spin.
    ///
    /// Endless encoders are why there is no soft-takeover machinery here: an
    /// encoder has no physical position to disagree with the stored value, so
    /// selecting a different track can never strand six knobs. That also makes
    /// a p-lock immediate — hold a step and nudge, with no sweep to pick up
    /// the value first.
    void EncoderTurn(int enc, int delta)
    {
        if(enc < 0 || enc >= kNumMacros || !machine_ || delta == 0)
            return;

        const ParamId id = static_cast<ParamId>(enc);

        // Continue from what we last pushed if the knob is still being turned,
        // rather than re-reading the patch. The command queue is drained by the
        // audio side a block later, so a fast spin would otherwise keep reading
        // a stale value and silently drop detents.
        //
        // The first detent of a gesture is always fine and always re-seeds from
        // the patch. Without the `turning_` guard a turn at now_ms_ == 0 would
        // see a zero interval, read as a fast spin, and start from zero instead
        // of the stored value.
        const bool continuing
            = turning_[enc] && (now_ms_ - last_turn_ms_[enc] <= kEditContinueMs);

        float v = continuing ? edit_value_[enc] : CurrentValue(id);
        v += delta * (continuing ? StepFor(enc) : kFineStep);
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;

        edit_value_[enc]   = v;
        last_turn_ms_[enc] = now_ms_;
        turning_[enc]      = true;
        last_macro_        = enc;
        last_macro_ms_     = now_ms_;

        Command c;
        c.track = static_cast<uint8_t>(selected_track_);
        c.param = static_cast<uint8_t>(enc);
        c.value = v;
        if(held_step_ >= 0)
        {
            c.type = Command::Type::SetStepLock;
            c.step = static_cast<uint8_t>(held_step_);
            wrote_lock_while_held_ = true;
        }
        else
        {
            c.type = Command::Type::SetKitParam;
        }
        machine_->Push(c);
    }

    // ---- state, for the display and LEDs -----------------------------------

    Mode mode() const { return mode_; }
    int  selected_track() const { return selected_track_; }
    int  held_step() const { return held_step_; }
    bool shift() const { return shift_; }

    float value(ParamId id) const { return StoredValue(id); }

    /// The value this macro last pushed — what the screen should show while the
    /// knob is being turned. Reading the patch instead would lag by a block,
    /// and when locking a step it would show the track's value rather than the
    /// step's, which is the opposite of what you are editing.
    float edit_value(int enc) const
    {
        return (enc >= 0 && enc < kNumMacros) ? edit_value_[enc] : 0.f;
    }

    /// Which macro was turned most recently, and how long ago. The display
    /// uses this to explain the knob you are actually holding.
    int      last_macro() const { return last_macro_; }
    uint32_t since_last_macro_ms() const { return now_ms_ - last_macro_ms_; }

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
        return track >= 0 && track < kNumTracks
               && machine_->patch().pattern.tracks[track].muted;
    }

  private:
    /// How long a macro keeps accumulating from its own last value rather than
    /// re-reading the patch. Only has to outlive the queue round-trip.
    static constexpr uint32_t kEditContinueMs = 250;

    // Acceleration. A detented encoder gives ~24 steps per revolution, so a
    // single fixed step size is either too coarse to tune a parameter or needs
    // ten revolutions to cross its range. The interval between detents picks
    // the step instead: a deliberate click is fine, a spin is coarse.
    static constexpr float kFineStep   = 1.f / 256.f; ///< ~10 turns end to end
    static constexpr float kMidStep    = 1.f / 64.f;  ///< ~3 turns
    static constexpr float kCoarseStep = 1.f / 16.f;  ///< ~2/3 of a turn

    /// Drops every in-flight gesture, so the next detent re-seeds from the
    /// patch. Called whenever what a macro points at changes.
    void EndGestures()
    {
        for(int i = 0; i < kNumMacros; ++i)
            turning_[i] = false;
    }

    float StepFor(int enc) const
    {
        const uint32_t dt = now_ms_ - last_turn_ms_[enc];
        if(dt <= 8)  return kCoarseStep;
        if(dt <= 25) return kMidStep;
        return kFineStep;
    }

    /// What a nudge starts from. Holding a step that already carries a lock for
    /// this parameter continues from the lock, not from the track value —
    /// otherwise re-tweaking a locked step would jump it back to the base first.
    float CurrentValue(ParamId id) const
    {
        if(held_step_ >= 0 && held_step_ < kMaxSteps)
        {
            const Step &s = CurrentTrack().steps[held_step_];
            for(uint8_t i = 0; i < s.lock_count; ++i)
                if(s.locks[i].param_id == static_cast<uint8_t>(id))
                    return s.locks[i].as_float();
        }
        return StoredValue(id);
    }

    bool Valid(int step) const { return machine_ && step >= 0 && step < kNumStepKeys; }

    const Track &CurrentTrack() const
    {
        return machine_->patch().pattern.tracks[selected_track_];
    }

    float StoredValue(ParamId id) const
    {
        return machine_->patch().kit.params[selected_track_][static_cast<int>(id)];
    }

    void ToggleStep(int step)
    {
        Command c;
        c.type  = Command::Type::ToggleStep;
        c.track = static_cast<uint8_t>(selected_track_);
        c.step  = static_cast<uint8_t>(step);
        machine_->Push(c);
    }

    Machine *machine_       = nullptr;
    Mode     mode_          = Mode::Play;
    uint32_t now_ms_        = 0;
    uint32_t last_macro_ms_ = 0;
    int      last_macro_    = -1;
    int    selected_track_ = 0;
    int    held_step_      = -1;
    bool   shift_          = false;
    bool   wrote_lock_while_held_ = false;
    float    edit_value_[kNumMacros]   = {};
    uint32_t last_turn_ms_[kNumMacros] = {};
    bool     turning_[kNumMacros]      = {};
};

} // namespace drom
