#pragma once
#include <cstdint>
#include "../machine.h"
#include "ui.h"

namespace drom {

struct Rgb
{
    uint8_t r = 0, g = 0, b = 0;

    static Rgb Scale(Rgb c, float k)
    {
        if(k < 0.f) k = 0.f;
        if(k > 1.f) k = 1.f;
        return {static_cast<uint8_t>(c.r * k),
                static_cast<uint8_t>(c.g * k),
                static_cast<uint8_t>(c.b * k)};
    }

    /// Mix toward white. Used to mark locked steps: it keeps the track's hue
    /// readable while making the step obviously different.
    static Rgb Whiten(Rgb c, float k)
    {
        if(k < 0.f) k = 0.f;
        if(k > 1.f) k = 1.f;
        auto up = [&](uint8_t v) {
            return static_cast<uint8_t>(v + (255 - v) * k);
        };
        return {up(c.r), up(c.g), up(c.b)};
    }
};

inline constexpr int kNumTransportKeys = 6;
inline constexpr int kNumLeds = kNumStepKeys + kNumTracks + kNumTransportKeys; // 30

/// One hue per track, chosen to stay distinguishable at low brightness — which
/// is where they will actually live, since full brightness is unaffordable.
inline constexpr Rgb kTrackColour[kNumTracks] = {
    {255, 40, 30},   // BD  red
    {255, 130, 20},  // SD  orange
    {230, 220, 40},  // CH  yellow
    {60, 230, 80},   // OH  green
    {40, 220, 220},  // LT  cyan
    {60, 110, 255},  // CP  blue
    {160, 70, 255},  // RS  violet
    {255, 60, 190},  // FM  magenta
};

// --- current budget ---------------------------------------------------------
//
// 30 SK6812s at full white is 30 x 60 mA = 1.8 A, far past any USB supply. The
// renderer is the only place that knows what the whole frame looks like, so it
// is where the ceiling belongs — a per-LED clamp cannot see the total.

inline constexpr float kMaPerChannelFull = 20.f;  ///< one SK6812 channel, fully on
inline constexpr float kLedBudgetMa      = 400.f; ///< leaves headroom on a 500 mA USB port

inline float FrameCurrentMa(const Rgb *leds, int count)
{
    float total = 0.f;
    for(int i = 0; i < count; ++i)
        total += (leds[i].r + leds[i].g + leds[i].b) / 255.f * kMaPerChannelFull;
    return total;
}

/// Renders the whole panel. Pure function of machine + UI state plus a clock,
/// so every rule below is testable without a single LED attached.
class LedRenderer
{
  public:
    /// `now_ms` drives the playhead flash and the lock pulse.
    void Render(const Machine &m, const Ui &ui, uint32_t now_ms, Rgb *out) const
    {
        Rgb *step      = out;
        Rgb *track     = out + kNumStepKeys;
        Rgb *transport = out + kNumStepKeys + kNumTracks;

        const int  sel      = ui.selected_track();
        const Rgb  hue      = kTrackColour[sel];
        const auto &pattern = m.patch().pattern;
        const int  playhead = static_cast<int>(
            m.state().position[sel].load(std::memory_order_relaxed));
        const bool playing = m.state().playing.load(std::memory_order_relaxed);

        // Slow triangle, for the locked-step pulse.
        const float pulse = Triangle(now_ms, 1200);

        for(int i = 0; i < kNumStepKeys; ++i)
        {
            const Step &s = pattern.tracks[sel].steps[i];

            // Playhead wins over everything: it is the one thing you track with
            // your eyes while playing, so it must never be ambiguous.
            if(playing && i == playhead)
            {
                step[i] = {255, 255, 255};
                continue;
            }

            if(!s.active())
            {
                step[i] = {0, 0, 0};
                continue;
            }

            // Velocity maps to brightness, but not from zero — an active step
            // at velocity 1 still has to be visibly on.
            const float v = 0.25f + 0.75f * (s.velocity / 127.f);
            Rgb         c = Rgb::Scale(hue, v);
            if(s.lock_count > 0)
                c = Rgb::Whiten(c, 0.35f + 0.25f * pulse);
            step[i] = c;
        }

        for(int t = 0; t < kNumTracks; ++t)
        {
            if(pattern.tracks[t].muted)
            {
                track[t] = {255, 0, 0}; // red means muted, on every track
                continue;
            }
            const bool selected = (t == sel);
            const bool content  = HasContent(pattern.tracks[t]);
            float      k        = selected ? 1.0f : (content ? 0.35f : 0.10f);
            track[t]            = Rgb::Scale(kTrackColour[t], k);
        }

        transport[0] = playing ? Rgb{0, 255, 0} : Rgb{0, 40, 0};      // PLAY
        transport[1] = {40, 0, 0};                                     // REC
        transport[2] = ui.shift() ? Rgb{255, 255, 255} : Rgb{40, 40, 40}; // SHIFT
        transport[3] = {0, 40, 60};                                    // PATT
        transport[4] = {0, 40, 60};                                    // SONG
        transport[5] = m.state().external_sync.load(std::memory_order_relaxed)
                           ? Rgb{0, 120, 255}   // locked to an external clock
                           : Rgb{40, 30, 0};    // internal
        // Making the sync source visible is deliberate: silently switching
        // between internal and external is miserable to debug on stage.

        Clamp(out, kNumLeds);
    }

    /// Scales the whole frame down if it would exceed the supply budget.
    /// Global rather than per-LED, because only the whole frame knows the sum.
    static void Clamp(Rgb *leds, int count)
    {
        const float ma = FrameCurrentMa(leds, count);
        if(ma <= kLedBudgetMa)
            return;
        const float k = kLedBudgetMa / ma;
        for(int i = 0; i < count; ++i)
            leds[i] = Rgb::Scale(leds[i], k);
    }

  private:
    static bool HasContent(const Track &t)
    {
        for(int i = 0; i < t.length && i < kMaxSteps; ++i)
            if(t.steps[i].active())
                return true;
        return false;
    }

    /// 0..1..0 triangle over `period_ms`. Avoids a sine in a 1 kHz loop.
    static float Triangle(uint32_t now_ms, uint32_t period_ms)
    {
        const uint32_t p = now_ms % period_ms;
        const float    x = static_cast<float>(p) / period_ms;
        return x < 0.5f ? x * 2.f : (1.f - x) * 2.f;
    }
};

} // namespace drom
