#pragma once
#include <cmath>
#include <cstdint>

// One low-frequency oscillator per track.
//
// Deliberately knows nothing about ParamId: the destination is an opaque index
// the caller interprets. That keeps this header dependency-free — no DaisySP,
// no libDaisy — so it tests on its own.
//
// **Phase is runtime state and must never be saved.** Patch is memcpy'd to
// flash, so a stored phase would make every pattern load snap a free-running
// LFO to it, which is exactly what FREE mode means it must not do. The Kit
// stores the eight LFO *settings*; this object holds the phase, and it lives in
// Machine. See docs/02-firmware.md §5.

namespace drom {

class Lfo
{
  public:
    enum class Wave : uint8_t
    {
        Triangle = 0, Sine, Square, Saw, Ramp, Random, Count
    };

    enum class Mode : uint8_t
    {
        /// Never resets. The cheap mode, and the one with no timing subtlety:
        /// with no reset there is nothing to place accurately.
        Free = 0,
        /// Restarts from the start phase on every trigger.
        Trig,
        /// Runs free, but the output is sampled and held at each trigger.
        Hold,
        /// One cycle from the start phase, then holds its final value.
        OneShot,
        Count
    };

    /// Quantised from a normalised parameter. The quantisation is deliberate:
    /// these arrive as floats (Kit) or as a uint16 over 0..1 (ParamLock), and a
    /// mode that drifts to its neighbour through float comparison would be a
    /// miserable bug to find.
    template <typename E>
    static E Quantise(float v)
    {
        int n = static_cast<int>(v * static_cast<float>(E::Count));
        if(n < 0) n = 0;
        if(n >= static_cast<int>(E::Count))
            n = static_cast<int>(E::Count) - 1;
        return static_cast<E>(n);
    }

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        phase_ = 0.f;
        fade_pos_ = 1.f;
        held_ = 0.f;
        rng_ = 0x2545F491u;
        random_value_ = Random();
        Recalculate();
    }

    // ---- settings, all normalised 0..1 -------------------------------------

    void SetSpeed(float v) { speed_ = v; Recalculate(); }
    void SetMult(float v)  { mult_  = v; Recalculate(); }
    void SetWave(float v)  { wave_  = Quantise<Wave>(v); }
    void SetMode(float v)  { mode_  = Quantise<Mode>(v); }
    void SetDepth(float v) { depth_ = v; }
    void SetStartPhase(float v) { start_phase_ = v; }

    /// 0 disables the LFO; anything else is `destination index + 1`, so that
    /// "off" is representable without a sentinel in the destination space.
    void SetDest(float v)
    {
        int n = static_cast<int>(v * static_cast<float>(kDestSlots));
        if(n < 0) n = 0;
        if(n >= kDestSlots) n = kDestSlots - 1;
        dest_ = static_cast<uint8_t>(n);
    }

    /// 0 is no fade. Above that, a fade-in measured in beats.
    void SetFade(float v) { fade_beats_ = v * 8.f; Recalculate(); }

    /// LFO rate is tempo-relative, so the machine pushes tempo in.
    void SetTempo(float bpm) { bpm_ = bpm > 1.f ? bpm : 1.f; Recalculate(); }

    // ---- runtime -----------------------------------------------------------

    /// Called at the exact sample a step fires, not at a block boundary — the
    /// reset is part of the trigger, and block-quantising it would put
    /// +/-0.67 ms of phase jitter on every trig-synced LFO.
    void Trigger()
    {
        switch(mode_)
        {
            case Mode::Free: break; // the whole point: triggers do not touch it
            case Mode::Hold: held_ = Shape(phase_); break;
            case Mode::Trig:
            case Mode::OneShot:
                phase_     = start_phase_;
                fade_pos_  = fade_rate_ > 0.f ? 0.f : 1.f;
                finished_  = false;
                random_value_ = Random();
                break;
            default: break;
        }
    }

    void Advance(uint32_t samples)
    {
        if(finished_)
            return;
        const float step = phase_inc_ * static_cast<float>(samples);
        phase_ += step;
        while(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            random_value_ = Random();
            if(mode_ == Mode::OneShot)
            {
                // Stop at the end of the first cycle and hold the final value.
                finished_ = true;
                phase_    = 1.f - 1e-6f;
                break;
            }
        }
        if(fade_pos_ < 1.f)
        {
            fade_pos_ += fade_rate_ * static_cast<float>(samples);
            if(fade_pos_ > 1.f)
                fade_pos_ = 1.f;
        }
    }

    /// Bipolar, -1..1, with depth and fade already applied.
    float value() const
    {
        const float raw = mode_ == Mode::Hold ? held_ : Shape(phase_);
        return raw * depth_ * fade_pos_;
    }

    uint8_t dest() const { return dest_; }
    bool    active() const { return dest_ != 0 && depth_ > 0.f; }
    float   phase() const { return phase_; }
    Mode    mode() const { return mode_; }
    Wave    wave() const { return wave_; }

    /// Destination slots: "off" plus one per addressable parameter. Checked
    /// against ParamId::Count by a static_assert in voice.h, which cannot be
    /// done here because this header deliberately does not know about ParamId.
    static constexpr int kDestSlots = 33;

  private:
    void Recalculate()
    {
        // SPEED is cycles per beat, swept exponentially: a linear map crams
        // everything musically useful into the first few degrees of travel,
        // the same mistake docs/02-firmware.md §5 records for decay time.
        const float cycles_per_beat = 0.01f * std::pow(800.f, speed_);
        // MULT is a power-of-two multiplier, quantised so it lands on musical
        // ratios rather than somewhere between them.
        int m = static_cast<int>(mult_ * 8.f);
        if(m < 0) m = 0;
        if(m > 7) m = 7;
        const float multiplier = static_cast<float>(1 << m);

        const float beats_per_second = bpm_ / 60.f;
        phase_inc_ = cycles_per_beat * multiplier * beats_per_second / sample_rate_;

        const float fade_samples = fade_beats_ * sample_rate_ / beats_per_second;
        fade_rate_ = fade_samples > 1.f ? 1.f / fade_samples : 0.f;
        if(fade_rate_ <= 0.f)
            fade_pos_ = 1.f;
    }

    float Shape(float p) const
    {
        switch(wave_)
        {
            case Wave::Sine:   return std::sin(6.2831853f * p);
            case Wave::Square: return p < 0.5f ? 1.f : -1.f;
            case Wave::Saw:    return 1.f - 2.f * p;
            case Wave::Ramp:   return 2.f * p - 1.f;
            case Wave::Random: return random_value_;
            default: // Triangle
                return p < 0.5f ? (4.f * p - 1.f) : (3.f - 4.f * p);
        }
    }

    /// xorshift32. No allocation, no libc, deterministic per instance — which
    /// also means a Random LFO is reproducible in a test.
    float Random()
    {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (static_cast<float>(rng_ >> 8) / 8388608.f) - 1.f; // -1..1
    }

    float    sample_rate_ = 48000.f;
    float    bpm_         = 120.f;
    float    speed_ = 0.5f, mult_ = 0.f, depth_ = 0.f;
    float    start_phase_ = 0.f, fade_beats_ = 0.f;
    float    phase_ = 0.f, phase_inc_ = 0.f;
    float    fade_pos_ = 1.f, fade_rate_ = 0.f;
    float    held_ = 0.f, random_value_ = 0.f;
    uint32_t rng_ = 0x2545F491u;
    uint8_t  dest_ = 0;
    bool     finished_ = false;
    Wave     wave_ = Wave::Triangle;
    Mode     mode_ = Mode::Free;
};

} // namespace drom
