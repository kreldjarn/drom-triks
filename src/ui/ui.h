#pragma once
#include <cmath>
#include <cstdint>
#include "../io/storage_layout.h"
#include "../machine.h"

namespace drom {

/// Eight macro encoders addressing one page at a time. The two navigation
/// encoders (value/tempo, nav/page) are separate and do not address parameters.
///
/// Macros used to map 1:1 onto ParamId. They no longer can: the parameter space
/// is four pages deep and the panel is one page wide, so the mapping goes
/// through `page_`. See docs/01-hardware.md §2.
inline constexpr int kNumMacros   = kParamsPerPage;
inline constexpr int kNumStepKeys = 16;

static_assert(kNumMacros == kParamsPerPage,
              "one macro encoder per parameter slot on a page");

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
        Pattern,  ///< step keys load a pattern; with SHIFT, save one
    };

    /// The six transport keys. MUTE and PATTERN are **held**, like SHIFT, for
    /// the reason the class comment gives: a held modifier has no state to get
    /// stuck in, and on an instrument you play that matters more than saving a
    /// finger.
    enum class Key : uint8_t
    {
        Play = 0, Rec, Shift, Patt, Song, Tap
    };

    /// Flash work the main loop must do. The UI cannot touch Storage itself:
    /// a QSPI write stalls for milliseconds, so it belongs nowhere near the
    /// audio callback, and the patch has to be snapshotted by the audio side
    /// first anyway. See docs/02-firmware.md §8.
    struct StorageRequest
    {
        enum class Type : uint8_t { None = 0, SavePattern, LoadPattern };
        Type    type = Type::None;
        uint8_t slot = 0;
    };

    /// Returns the pending request and clears it. Poll once per main-loop pass.
    StorageRequest TakeStorageRequest()
    {
        const StorageRequest r = storage_req_;
        storage_req_           = StorageRequest{};
        return r;
    }

    void Init(Machine *machine)
    {
        machine_ = machine;
        mode_  = Mode::Play;
        page_           = 0;
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

    /// Select which page the eight macro encoders address.
    ///
    /// Ends any gesture in flight for the same reason a track change does: the
    /// encoders now point somewhere else, and a spin spanning the change would
    /// write the old page's accumulated value to the new page's parameter.
    void SetPage(int page)
    {
        if(page < 0 || page >= kNumPages || page == page_)
            return;
        page_ = page;
        EndGestures();
    }

    int page() const { return page_; }

    /// The parameter macro encoder `enc` currently addresses.
    ParamId ParamForMacro(int enc) const
    {
        return ParamAt(page_, (enc < 0 || enc >= kNumMacros) ? 0 : enc);
    }

    void TransportPress(Key k)
    {
        if(!machine_)
            return;
        switch(k)
        {
            case Key::Shift: shift_ = true; EndGestures(); break;
            case Key::Patt:  mode_ = Mode::Pattern; break;
            case Key::Play:  Transport(); break;
            case Key::Rec:   rec_armed_ = !rec_armed_; break;
            case Key::Tap:   Tap(); break;
            case Key::Song:  break; // song mode is Phase 7
        }
    }

    void TransportRelease(Key k)
    {
        switch(k)
        {
            // Releasing SHIFT changes what every macro points at, exactly like
            // a page change, so any gesture in flight has to re-seed.
            case Key::Shift: shift_ = false; EndGestures(); break;
            case Key::Patt:  if(mode_ == Mode::Pattern) mode_ = Mode::Play; break;
            default: break;
        }
    }

    /// Set or clear MUTE. Held, like PATTERN.
    void SetMuteHeld(bool held)
    {
        mode_ = held ? Mode::Mute : (mode_ == Mode::Mute ? Mode::Play : mode_);
    }

    /// The two navigation encoders. 0 is value/tempo, 1 is page.
    void NavTurn(int nav, int delta)
    {
        if(!machine_ || delta == 0)
            return;
        // Clamp rather than reject. A detent is normally +/-1, but the scan
        // coalesces a fast spin into a larger delta — and rejecting that would
        // mean a quick flick near either end does nothing at all, which reads
        // as a dead encoder.
        if(nav == 1)
        {
            // While SHIFT is held the macros are on master FX, so the page
            // encoder picks which bank of eight rather than which page.
            if(shift_)
                SetFxBank(Clampi(fx_bank_ + delta, 0, kNumFxParams / kNumMacros - 1));
            else
                SetPage(Clampi(page_ + delta, 0, kNumPages - 1));
            return;
        }
        if(mode_ == Mode::Pattern)
        {
            SetPatternBank(Clampi(pattern_bank_ + delta, 0,
                                  static_cast<int>(kPatternSlots) / kNumStepKeys - 1));
            return;
        }
        Command c;
        c.type  = Command::Type::SetTempo;
        c.value = Clampf(machine_->state().tempo.load(std::memory_order_relaxed)
                             + static_cast<float>(delta),
                         20.f, 300.f);
        machine_->Push(c);
    }

    /// Pushing a macro encoder returns its parameter to the default. The push
    /// switches come free on the chain (hardware §3.3) and this is the obvious
    /// use for them.
    void EncoderPush(int enc)
    {
        const Target t = TargetFor(enc);
        if(t.kind == Target::Kind::None || !machine_)
            return;
        Emit(enc, t, DefaultFor(t));
    }

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
        // physical position to strand, so the macros simply address the
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

        // In PATTERN mode a step key is a slot, not a step. Sixteen keys over
        // 128 slots, so the value encoder picks the bank.
        if(mode_ == Mode::Pattern)
        {
            storage_req_.slot = static_cast<uint8_t>(pattern_bank_ * kNumStepKeys + step);
            storage_req_.type = shift_ ? StorageRequest::Type::SavePattern
                                       : StorageRequest::Type::LoadPattern;
            return;
        }

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
        if(mode_ == Mode::Pattern || step != held_step_)
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

        const Target tgt = TargetFor(enc);
        if(tgt.kind == Target::Kind::None)
            return; // a declared-but-unwired slot writes nothing and claims no screen

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

        float v = continuing ? edit_value_[enc] : CurrentValueFor(tgt);
        v += delta * (continuing ? StepFor(enc) : kFineStep);
        Emit(enc, tgt, Clampf(v, 0.f, 1.f));
    }

    /// What a macro encoder is pointing at right now.
    ///
    /// Three layers, resolved in this order:
    ///   SHIFT + a held step -> that step's detail (velocity, micro, ...)
    ///   SHIFT alone         -> master FX
    ///   neither             -> the current page's parameter for this track
    ///
    /// One rule to remember rather than three: SHIFT makes a knob global,
    /// unless you are already holding a step, in which case it makes it local
    /// to that step.
    struct Target
    {
        enum class Kind : uint8_t { None = 0, Param, Fx, StepField };
        Kind    kind  = Kind::None;
        uint8_t index = 0;
    };

    Target TargetFor(int enc) const
    {
        if(enc < 0 || enc >= kNumMacros || !machine_)
            return {};
        if(shift_ && held_step_ >= 0)
        {
            if(enc >= static_cast<int>(StepField::Count))
                return {};
            return {Target::Kind::StepField, static_cast<uint8_t>(enc)};
        }
        if(shift_)
        {
            const int i = fx_bank_ * kNumMacros + enc;
            return FxReserved(i) ? Target{}
                                 : Target{Target::Kind::Fx, static_cast<uint8_t>(i)};
        }
        const ParamId id = ParamAt(page_, enc);
        return ParamReserved(id)
                   ? Target{}
                   : Target{Target::Kind::Param, static_cast<uint8_t>(id)};
    }


    // ---- state, for the display and LEDs -----------------------------------

    Mode mode() const { return mode_; }
    int  selected_track() const { return selected_track_; }
    int  held_step() const { return held_step_; }
    bool shift() const { return shift_; }
    bool rec_armed() const { return rec_armed_; }
    int  fx_bank() const { return fx_bank_; }
    int  pattern_bank() const { return pattern_bank_; }

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

    static int Clampi(int v, int lo, int hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static float Clampf(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// Normalised current value of whatever the target points at.
    float CurrentValueFor(const Target &t) const
    {
        switch(t.kind)
        {
            case Target::Kind::Fx: return machine_->patch().kit.fx[t.index];
            case Target::Kind::StepField: return StepFieldValue(t.index);
            case Target::Kind::Param:
                return CurrentValue(static_cast<ParamId>(t.index));
            default: return 0.f;
        }
    }

    float DefaultFor(const Target &t) const
    {
        switch(t.kind)
        {
            case Target::Kind::Fx:    return kFxDefault[t.index];
            case Target::Kind::Param: return kParamInfo[t.index].def;
            // Centre for the bipolar one, full for the rest — "default" for a
            // step field means "as if you had never touched it".
            case Target::Kind::StepField:
                return static_cast<StepField>(t.index) == StepField::Micro ? 0.5f : 1.f;
            default: return 0.f;
        }
    }

    float StepFieldValue(uint8_t field) const
    {
        if(!Valid(held_step_))
            return 0.f;
        const Step &s = CurrentTrack().steps[held_step_];
        switch(static_cast<StepField>(field))
        {
            case StepField::Velocity:    return static_cast<float>(s.velocity) / 127.f;
            case StepField::Micro:
                return (static_cast<float>(s.micro) / static_cast<float>(kMicroRange)
                        + 1.f) * 0.5f;
            case StepField::Probability: return static_cast<float>(s.probability) / 100.f;
            case StepField::Ratchet:     return static_cast<float>(s.ratchet - 1) / 7.f;
            default: return 0.f;
        }
    }

    /// Records the gesture and pushes the command the target implies.
    void Emit(int enc, const Target &tgt, float v)
    {
        edit_value_[enc]   = v;
        last_turn_ms_[enc] = now_ms_;
        turning_[enc]      = true;
        last_macro_        = enc;
        last_macro_ms_     = now_ms_;

        Command c;
        c.track = static_cast<uint8_t>(selected_track_);
        c.param = tgt.index;
        c.value = v;
        switch(tgt.kind)
        {
            case Target::Kind::Fx:
                c.type = Command::Type::SetFxParam;
                break;
            case Target::Kind::StepField:
                c.type = Command::Type::SetStepField;
                c.step = static_cast<uint8_t>(held_step_);
                // Step detail is not a p-lock, so it must not suppress the
                // toggle-on-release: you held the step to edit it, not to
                // turn it off.
                break;
            default:
                if(held_step_ >= 0)
                {
                    c.type = Command::Type::SetStepLock;
                    c.step = static_cast<uint8_t>(held_step_);
                    wrote_lock_while_held_ = true;
                }
                else
                    c.type = Command::Type::SetKitParam;
                break;
        }
        machine_->Push(c);
    }

    void SetFxBank(int bank)
    {
        const int n = kNumFxParams / kNumMacros;
        if(bank < 0 || bank >= n || bank == fx_bank_)
            return;
        fx_bank_ = bank;
        EndGestures();
    }

    void SetPatternBank(int bank)
    {
        const int n = static_cast<int>(kPatternSlots) / kNumStepKeys;
        if(bank < 0 || bank >= n)
            return;
        pattern_bank_ = bank;
    }

    void Transport()
    {
        Command c;
        c.type = machine_->state().playing.load(std::memory_order_relaxed)
                     ? Command::Type::Stop
                     : Command::Type::Start;
        machine_->Push(c);
    }

    /// Tap tempo over a rolling average of the last few intervals.
    ///
    /// A gap longer than kTapTimeoutMs restarts the average rather than folding
    /// a pause into it — otherwise the first tap after a rest drags the tempo
    /// down by however long you hesitated.
    void Tap()
    {
        if(last_tap_ms_ != 0 && now_ms_ - last_tap_ms_ <= kTapTimeoutMs)
        {
            const uint32_t interval = now_ms_ - last_tap_ms_;
            tap_sum_ += interval;
            ++tap_count_;
            if(tap_count_ >= 1)
            {
                const float ms  = static_cast<float>(tap_sum_)
                                  / static_cast<float>(tap_count_);
                Command        c;
                c.type  = Command::Type::SetTempo;
                c.value = Clampf(60000.f / ms, 20.f, 300.f);
                machine_->Push(c);
            }
        }
        else
        {
            tap_sum_   = 0;
            tap_count_ = 0;
        }
        last_tap_ms_ = now_ms_;
    }

    static constexpr uint32_t kTapTimeoutMs = 2000;

    Machine *machine_       = nullptr;
    Mode     mode_          = Mode::Play;
    uint32_t now_ms_        = 0;
    uint32_t last_macro_ms_ = 0;
    int      last_macro_    = -1;
    int      page_          = 0;
    int    selected_track_ = 0;
    int    held_step_      = -1;
    bool   shift_          = false;
    bool   wrote_lock_while_held_ = false;
    bool   rec_armed_      = false;
    int    fx_bank_        = 0;
    int    pattern_bank_   = 0;
    uint32_t last_tap_ms_  = 0;
    uint32_t tap_sum_      = 0;
    uint32_t tap_count_    = 0;
    StorageRequest storage_req_{};
    float    edit_value_[kNumMacros]   = {};
    uint32_t last_turn_ms_[kNumMacros] = {};
    bool     turning_[kNumMacros]      = {};
};

} // namespace drom
