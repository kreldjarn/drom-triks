#pragma once
#include <cmath>
#include <cstdint>
#include "../machine.h"
#include "ui.h"

namespace drom {

/// The Electrosmith Seed3 Desktop Dev Kit, standing in for the panel.
///
/// The kit has 16 keys in two rows of 8, each with a single-colour LED, 8 pots
/// and two toggles. The panel has 34 RGB keys and 10 encoders. This class
/// folds one onto the other and drives the real `Ui`, so it adds no behaviour
/// of its own: every gesture ends in a call the panel would make too.
///
/// Like `Ui` it is pure logic (no libDaisy), so the mapping is tested on the
/// host by `host/devkittest.cpp`. See docs/02-firmware.md §10 for the layout.
///
///   SW18 (3-way)  what the 16 keys are:  UP = steps, CENTER = tracks,
///                                        DOWN = pattern slots
///   SW17 (2-way)  SHIFT
///   pots 1-8      the eight macros, on whatever layer Ui says they address
///
/// SHIFT is a toggle here rather than a held key. `Ui` prefers held modifiers
/// because they cannot get stuck, but a toggle cannot get stuck *invisibly*:
/// its lever shows where it is. It also leaves all 16 keys for steps, which
/// is what makes SHIFT + hold a step (step detail) reachable at all.
inline constexpr int kDevkitKeys = 16;
inline constexpr int kDevkitPots = 8;

static_assert(kDevkitKeys == kNumStepKeys, "one kit key per step");
static_assert(kDevkitPots == kNumMacros, "one pot per macro");
static_assert(kNumTracks == 12,
              "the tracks layer puts 12 tracks before its 4 function keys");

class DevkitPanel
{
  public:
    enum class Layer : uint8_t
    {
        Steps = 0, ///< step keys, or the keyboard when it is on
        Tracks,    ///< 12 track keys, then PLAY, PAGE, MUTE, KBD
        Patterns,  ///< 16 pattern slots: press loads, SHIFT + press saves
    };

    // The tracks layer's bottom row: four tracks, then four function keys.
    static constexpr int kKeyPlay = 12; ///< start/stop; with SHIFT, tap tempo
    static constexpr int kKeyPage = 13; ///< tap: next page; hold + key 1-4: that page
    static constexpr int kKeyMute = 14; ///< held: track keys mute/unmute
    static constexpr int kKeyKbd  = 15; ///< keyboard on/off; with SHIFT, REC

    void Init(Ui *ui, Machine *machine)
    {
        ui_      = ui;
        machine_ = machine;
        layer_   = Layer::Steps;
        shift_   = false;
        for(auto &r : role_)
            r = Role::None;
        for(auto &p : pots_)
            p = PotState{};
    }

    void SetTime(uint32_t ms)
    {
        now_ms_ = ms;
        ui_->SetTime(ms);
    }

    void SetShift(bool on)
    {
        if(on == shift_)
            return;
        shift_ = on;
        if(on)
            ui_->TransportPress(Ui::Key::Shift);
        else
            ui_->TransportRelease(Ui::Key::Shift);
    }

    /// Pattern mode is a held mode in `Ui`, so the PATTERN layer holds it for
    /// as long as the lever is down.
    void SetLayer(Layer l)
    {
        if(l == layer_)
            return;
        if(layer_ == Layer::Patterns)
            ui_->TransportRelease(Ui::Key::Patt);
        layer_ = l;
        if(layer_ == Layer::Patterns)
            ui_->TransportPress(Ui::Key::Patt);
    }

    void KeyDown(int k)
    {
        if(k < 0 || k >= kDevkitKeys)
            return;
        down_[k] = true;
        role_[k] = RoleFor(k);
        Press(k, role_[k]);
    }

    /// A key is released with the meaning it was **pressed** with, not what
    /// the current layer would give it. Otherwise flipping SW18 while holding
    /// a step would never release that step, and `Ui` would stay in lock mode
    /// with nothing held — every pot turn after that writes a p-lock.
    void KeyUp(int k)
    {
        if(k < 0 || k >= kDevkitKeys)
            return;
        down_[k] = false;
        const Role r = role_[k];
        role_[k]     = Role::None;
        switch(r)
        {
            case Role::Step: ui_->StepRelease(k); break;
            case Role::Mute: ui_->SetMuteHeld(false); break;
            case Role::Page:
                page_held_ = false;
                // A bare tap steps through the pages. Picking one by number
                // while holding is the other way in, and must not also step.
                if(!page_picked_)
                    NextPage();
                break;
            default: break;
        }
    }

    /// A pot's position, every control tick (1 kHz).
    ///
    /// Jump pickup: the first real movement after the pot's target changed
    /// sets the value to wherever the pot points. Two thresholds, because the
    /// two failure modes want opposite things. Engaging needs a deliberate
    /// move (kEngage), or ADC noise on an untouched pot would write the new
    /// track's parameter the moment you change track. Once engaged, a much
    /// smaller step (kStep) counts, or a slow turn would move in 2 % jumps.
    void Pot(int p, float v)
    {
        if(p < 0 || p >= kDevkitPots)
            return;
        PotState &s = pots_[p];

        // Snap the ends, or the hysteresis below stops a pot just short of
        // them and a parameter can never quite reach 0 or 1.
        const float q = v <= kEdge ? 0.f : (v >= 1.f - kEdge ? 1.f : v);

        const PotTarget t = TargetOf(p);
        if(!s.seeded || !(t == s.target))
        {
            // Re-anchor rather than write: what the pot points at changed, so
            // its position says nothing about the new target's value yet.
            s.seeded  = true;
            s.target  = t;
            s.anchor  = q;
            s.engaged = false;
            return;
        }

        // Idle long enough and it has to be engaged again, so a pot left
        // resting on a noisy boundary cannot keep writing. Judged by the clock
        // rather than by a quiet reading, so it holds however often this runs.
        if(s.engaged && now_ms_ - s.last_move_ms > kIdleMs)
            s.engaged = false;

        const float moved   = std::fabs(q - s.anchor);
        const bool  reached = (q == 0.f || q == 1.f) && q != s.anchor;
        if(!s.engaged)
        {
            if(moved < kEngage)
                return;
            s.engaged = true;
        }
        else if(moved < kStep && !reached)
            return;
        s.anchor       = q;
        s.last_move_ms = now_ms_;
        ui_->MacroSet(p, q);
    }

    /// Which pattern slots hold a save, as a bitmask over the 16 keys, and
    /// which slot was last loaded or saved (-1 for none). The main loop owns
    /// flash and tells the panel; the panel never reads it.
    void SetSlots(uint16_t occupied, int current)
    {
        occupied_slots_ = occupied;
        current_slot_   = current;
    }

    Layer layer() const { return layer_; }
    bool  shift() const { return shift_; }
    bool  page_held() const { return page_held_; }

    // ---- LEDs ---------------------------------------------------------------

    /// Perceived brightness 0..1 for each key's LED. Gamma is the driver's
    /// job, so these are levels to *look* evenly spaced, not PWM duty.
    ///
    /// One colour means the RGB language of docs/02-firmware.md §7 has to be
    /// carried by brightness and blinking instead. The rules that survive:
    /// the playhead is the brightest thing and always wins, velocity is
    /// brightness, a lock pulses, and the selected track is brightest.
    void Render(float *out) const
    {
        switch(layer_)
        {
            case Layer::Steps:
                if(ui_->mode() == Ui::Mode::Keyboard)
                    RenderKeyboard(out);
                else
                    RenderSteps(out);
                break;
            case Layer::Tracks: RenderTracks(out); break;
            case Layer::Patterns: RenderPatterns(out); break;
        }
    }

    // Brightness levels, named so the tests can assert the ordering they
    // depend on rather than magic numbers.
    static constexpr float kFull      = 1.0f;
    static constexpr float kStepLo    = 0.25f; ///< an active step at velocity 0
    static constexpr float kStepSpan  = 0.30f; ///< added at velocity 127
    static constexpr float kLockPulse = 0.25f;
    static constexpr float kContent   = 0.35f; ///< a track or slot with something in it
    static constexpr float kIdle      = 0.15f; ///< a key that exists but is not lit

  private:
    enum class Role : uint8_t
    {
        None = 0, Step, Track, PagePick, Play, Page, Mute, Kbd, Slot
    };

    /// What a pot is writing, in enough detail that any change means its
    /// position is stale: the layer it reaches, and which track and step.
    struct PotTarget
    {
        Ui::Target::Kind kind  = Ui::Target::Kind::None;
        uint8_t          index = 0;
        int8_t           track = 0;
        int8_t           step  = -1;

        bool operator==(const PotTarget &o) const
        {
            return kind == o.kind && index == o.index && track == o.track
                   && step == o.step;
        }
    };

    struct PotState
    {
        bool      seeded       = false;
        bool      engaged      = false;
        float     anchor       = 0.f;
        uint32_t  last_move_ms = 0;
        PotTarget target{};
    };

    static constexpr float    kEngage = 0.02f;        ///< deliberate move to take over
    static constexpr float    kStep   = 1.f / 256.f;  ///< resolution once engaged
    static constexpr float    kEdge   = 1.f / 256.f;  ///< snaps to 0 or 1 inside this
    static constexpr uint32_t kIdleMs = 300;

    PotTarget TargetOf(int p) const
    {
        const Ui::Target t = ui_->TargetFor(p);
        PotTarget        r;
        r.kind  = t.kind;
        r.index = t.index;
        r.track = static_cast<int8_t>(ui_->selected_track());
        r.step  = static_cast<int8_t>(ui_->held_step());
        return r;
    }

    Role RoleFor(int k) const
    {
        switch(layer_)
        {
            case Layer::Steps: return Role::Step;
            case Layer::Patterns: return Role::Slot;
            case Layer::Tracks: break;
        }
        if(page_held_ && k < PagePickCount())
            return Role::PagePick;
        if(k < kNumTracks)
            return Role::Track;
        switch(k)
        {
            case kKeyPlay: return Role::Play;
            case kKeyPage: return Role::Page;
            case kKeyMute: return Role::Mute;
            default: return Role::Kbd;
        }
    }

    /// With SHIFT the macros are on master FX, so PAGE picks the FX bank
    /// instead — the same split `Ui::NavTurn` makes for the page encoder.
    int PagePickCount() const
    {
        return shift_ ? kNumFxParams / kNumMacros : kNumPages;
    }

    void Press(int k, Role r)
    {
        switch(r)
        {
            // Steps and slots are the same call: `Ui` is in Pattern mode while
            // the PATTERN layer is selected, and turns a step press into a
            // storage request itself.
            case Role::Step:
            case Role::Slot: ui_->StepPress(k); break;

            case Role::Track:
                // Dev-kit only, and deliberately not a panel decision: which
                // gesture selects a machine on the real panel is still open
                // (see host/play.cpp). It is here so every machine can be
                // auditioned on hardware.
                if(shift_ && ui_->mode() != Ui::Mode::Mute)
                    NextMachine(k);
                ui_->TrackPress(k);
                break;

            case Role::PagePick:
                page_picked_ = true;
                if(shift_)
                    ui_->NavTurn(1, k - ui_->fx_bank());
                else
                    ui_->SetPage(k);
                break;

            case Role::Play:
                ui_->TransportPress(shift_ ? Ui::Key::Tap : Ui::Key::Play);
                break;

            case Role::Page:
                page_held_   = true;
                page_picked_ = false;
                break;

            case Role::Mute: ui_->SetMuteHeld(true); break;

            case Role::Kbd:
                if(shift_)
                    ui_->TransportPress(Ui::Key::Rec);
                else
                    ui_->SetKeyboard(!ui_->keyboard());
                break;

            case Role::None: break;
        }
    }

    /// Wraps, unlike the nav encoder it stands in for. With one key and no
    /// direction, clamping would leave the last page as a dead end.
    void NextPage()
    {
        if(shift_)
        {
            const int banks = kNumFxParams / kNumMacros;
            ui_->NavTurn(1, ui_->fx_bank() + 1 < banks ? 1 : -(banks - 1));
        }
        else
            ui_->SetPage((ui_->page() + 1) % kNumPages);
    }

    void NextMachine(int track)
    {
        const int n   = static_cast<int>(MachineId::Count);
        const int cur = static_cast<int>(machine_->machine(track));
        Command   c;
        c.type  = Command::Type::SetMachine;
        c.track = static_cast<uint8_t>(track);
        c.param = static_cast<uint8_t>((cur + 1) % n);
        machine_->Push(c);
    }

    // ---- rendering ----------------------------------------------------------

    bool Blink(uint32_t period_ms) const
    {
        return (now_ms_ % period_ms) < period_ms / 2;
    }

    /// 0..1..0 over `period_ms`, for the lock pulse.
    float Triangle(uint32_t period_ms) const
    {
        const float x = static_cast<float>(now_ms_ % period_ms) / period_ms;
        return x < 0.5f ? x * 2.f : (1.f - x) * 2.f;
    }

    void RenderSteps(float *out) const
    {
        const int    sel     = ui_->selected_track();
        const Track &t       = machine_->patch().pattern.tracks[sel];
        const auto  &st      = machine_->state();
        const bool   playing = st.playing.load(std::memory_order_relaxed);
        const int    head
            = static_cast<int>(st.position[sel].load(std::memory_order_relaxed));
        const float pulse = Triangle(1200);

        for(int i = 0; i < kDevkitKeys; ++i)
        {
            // A held step is the one you are editing, so it reads as such
            // rather than flickering with the playhead going past.
            if(i == ui_->held_step() || (playing && i == head))
            {
                out[i] = kFull;
                continue;
            }
            const Step &s = t.steps[i];
            if(!s.active())
            {
                out[i] = 0.f;
                continue;
            }
            // Never below kStepLo, so velocity 1 is still visibly on; never up
            // to kFull, so no step can be mistaken for the playhead.
            float b = kStepLo + kStepSpan * (s.velocity / 127.f);
            if(s.lock_count > 0)
                b += kLockPulse * pulse;
            out[i] = b;
        }
    }

    /// Keys are notes here, not steps, so the LEDs show the keyboard: the
    /// track's own root (every 12th key) brighter, so you can find your place.
    void RenderKeyboard(float *out) const
    {
        for(int i = 0; i < kDevkitKeys; ++i)
        {
            const int semi = ((ui_->KeyboardSemitone(i) % 12) + 12) % 12;
            out[i] = down_[i] ? kFull : (semi == 0 ? kContent : kIdle);
        }
    }

    void RenderTracks(float *out) const
    {
        const auto    &pattern = machine_->patch().pattern;
        const auto    &st      = machine_->state();
        const bool     playing = st.playing.load(std::memory_order_relaxed);
        const uint32_t tick    = st.tick.load(std::memory_order_relaxed);
        const bool     mute_mode = ui_->mode() == Ui::Mode::Mute;

        for(int t = 0; t < kNumTracks; ++t)
        {
            const bool muted = pattern.tracks[t].muted;
            if(mute_mode)
            {
                // While MUTE is held the question is only "is it playing", so
                // lit means audible — the Elektron convention.
                out[t] = muted ? 0.f : kContent;
                continue;
            }
            float b = t == ui_->selected_track()
                          ? kFull
                          : (HasContent(pattern.tracks[t]) ? kContent : kIdle);
            // Red means muted on the RGB panel; with one colour, a blink.
            if(muted)
                b = Blink(500) ? b : 0.f;
            out[t] = b;
        }

        // While PAGE is held the first keys show and pick the page (or the FX
        // bank, with SHIFT) — there is no screen to say which one you are on.
        if(page_held_)
        {
            const int n   = PagePickCount();
            const int cur = shift_ ? ui_->fx_bank() : ui_->page();
            for(int i = 0; i < kNumTracks; ++i)
                out[i] = i < n ? (i == cur ? kFull : kIdle) : 0.f;
        }

        // Flashes on the beat while playing, so tempo is visible.
        const bool on_beat = (tick % kPpqn) < kPpqn / 4;
        out[kKeyPlay] = playing ? (on_beat ? kFull : kContent) : kIdle;
        out[kKeyPage] = page_held_ ? kFull : kIdle;
        out[kKeyMute] = mute_mode ? kFull : kIdle;
        // REC armed is a fast blink, on or off the keyboard, because it changes
        // what the next key press does and is otherwise invisible.
        const float kbd = ui_->keyboard() ? kFull : kIdle;
        out[kKeyKbd]    = ui_->rec_armed() ? (Blink(250) ? kFull : 0.f) : kbd;
    }

    void RenderPatterns(float *out) const
    {
        for(int i = 0; i < kDevkitKeys; ++i)
        {
            const bool occupied = (occupied_slots_ >> i) & 1u;
            if(i == current_slot_)
                out[i] = kFull;
            else if(occupied)
                out[i] = kContent;
            // With SHIFT a press saves, so every slot is a target and the
            // empty ones glow too: the whole grid lit reads as "write mode".
            else
                out[i] = shift_ ? kIdle : 0.f;
        }
    }

    static bool HasContent(const Track &t)
    {
        for(int i = 0; i < t.length && i < kMaxSteps; ++i)
            if(t.steps[i].active())
                return true;
        return false;
    }

    Ui      *ui_      = nullptr;
    Machine *machine_ = nullptr;
    Layer    layer_   = Layer::Steps;
    bool     shift_   = false;
    bool     page_held_   = false;
    bool     page_picked_ = false;
    uint32_t now_ms_  = 0;
    uint16_t occupied_slots_ = 0;
    int      current_slot_   = -1;
    Role     role_[kDevkitKeys] = {};
    bool     down_[kDevkitKeys] = {};
    PotState pots_[kDevkitPots] = {};
};

} // namespace drom
