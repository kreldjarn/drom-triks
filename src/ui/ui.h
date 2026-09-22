#pragma once
#include <cmath>
#include <cstdint>
#include "../machine.h"

namespace drom {

inline constexpr int kNumPots      = 6;
inline constexpr int kNumStepKeys  = 16;

/// Panel state machine. Pure logic: it takes debounced key edges and pot
/// positions and emits Commands. No hardware, no drawing — which is what lets
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
        // Nothing is caught until a knob is moved: on power-up the physical
        // positions bear no relation to the loaded patch.
        for(int i = 0; i < kNumPots; ++i)
        {
            caught_[i]   = false;
            last_raw_[i] = -1.f;
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
        if(step < 0 || step >= kNumStepKeys || !machine_)
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
        if(pot < 0 || pot >= kNumPots || !machine_)
            return;

        const ParamId id     = static_cast<ParamId>(pot);
        const float   stored = StoredValue(id);

        last_pot_    = pot;
        last_pot_ms_ = now_ms_;

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

        Command c;
        c.track = static_cast<uint8_t>(selected_track_);
        c.param = static_cast<uint8_t>(pot);
        c.value = raw;
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
    bool pot_caught(int pot) const { return pot >= 0 && pot < kNumPots && caught_[pot]; }

    float value(ParamId id) const { return StoredValue(id); }

    /// Which pot was touched most recently, and how long ago. The display uses
    /// this to explain the knob you are actually holding.
    int      last_pot() const { return last_pot_; }
    uint32_t since_last_pot_ms() const { return now_ms_ - last_pot_ms_; }

    /// Where the knob physically sits, which after a track change may be a
    /// long way from the stored value.
    float last_raw(int pot) const
    {
        return (pot >= 0 && pot < kNumPots) ? last_raw_[pot] : 0.f;
    }

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
    static constexpr float kCatchTolerance = 0.02f;

    bool Valid(int step) const { return machine_ && step >= 0 && step < kNumStepKeys; }

    const Track &CurrentTrack() const
    {
        return machine_->patch().pattern.tracks[selected_track_];
    }

    float StoredValue(ParamId id) const
    {
        return machine_->patch().kit.params[selected_track_][static_cast<int>(id)];
    }

    void ReleaseAllPots()
    {
        for(int i = 0; i < kNumPots; ++i)
            caught_[i] = false;
    }

    void ToggleStep(int step)
    {
        Command c;
        c.type  = Command::Type::ToggleStep;
        c.track = static_cast<uint8_t>(selected_track_);
        c.step  = static_cast<uint8_t>(step);
        machine_->Push(c);
    }

    Machine *machine_     = nullptr;
    Mode     mode_        = Mode::Play;
    uint32_t now_ms_      = 0;
    uint32_t last_pot_ms_ = 0;
    int      last_pot_    = -1;
    int    selected_track_ = 0;
    int    held_step_      = -1;
    bool   shift_          = false;
    bool   wrote_lock_while_held_ = false;
    bool   caught_[kNumPots]   = {};
    float  last_raw_[kNumPots] = {};
};

} // namespace drom
