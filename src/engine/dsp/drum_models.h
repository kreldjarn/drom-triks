#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include "daisysp.h"
#include "svf.h"

// DaisySP's four drum models, with their per-sample constants hoisted.
//
// Measured on the H750 (docs/02-firmware.md §10), these four classes were most
// of the CPU: AnalogSnareDrum alone was 72 % while sounding. Not because the
// models are expensive, but because every sample they recompute values that
// depend only on parameters — decay coefficients through powf, and filter
// coefficients through Svf::SetFreq/SetRes (a sinf and a powf each). The
// AnalogSnareDrum's six filters made that 5 sinf and ~14 powf per sample.
//
// Each fork keeps the original's sample loop and moves everything derived
// only from parameters into Update(), run when a parameter changes. Since the
// hoisted values are the same expressions, **three of the four are
// bit-identical** to DaisySP; host/dsptest.cpp checks them sample for sample.
// The exception is AnalogBassDrum, whose resonator frequency is FM'd every
// sample and so cannot be hoisted; it uses Svf::SetFreqResFast instead, and
// the test holds it to a tolerance.
//
// Removed, because no voice uses them: sustain mode, and Trig() (our voices
// pass the trigger to Process). The ports' structure and comments are kept so
// they can be compared against DaisySP and against Plaits, which they all
// derive from.
//
// ---------------------------------------------------------------------------
// DaisySP, copyright (c) 2020 Electrosmith, Corp, and Plaits, copyright 2016
// Emilie Gillet. Both MIT license — the full notice is in dsp/svf.h.
// ---------------------------------------------------------------------------

namespace drom {
namespace dsp {

using daisysp::fclamp;
using daisysp::fonepole;
using daisysp::kOneTwelfth;
using daisysp::kRandFrac;

// ---- noise ----------------------------------------------------------------------

/// A distinct seed per noise source, so two instances never produce the same
/// noise — two snares hit together would otherwise sum correlated, +6 dB
/// where independent noise gives +3.
///
/// The counter is a variable rather than a function-local static so a test
/// can reset it, and render the same voice twice from the same seed.
inline uint32_t g_noise_seed_count = 0;

inline uint32_t NextNoiseSeed()
{
    return 0x9E3779B9u * ++g_noise_seed_count + 0x7F4A7C15u;
}

/// The engine's noise: one generator per voice instance.
///
/// DaisySP's models call the C library's rand(), which is one generator shared
/// by every voice. That coupled them: a voice sleeping, or the engine changing
/// the order it renders tracks in, shifted every other voice's noise. Per
/// instance, each voice's noise depends only on that voice. It is also a
/// multiply-add, where newlib's rand() goes through a reentrancy struct.
///
/// Draw() returns a float on rand()'s own scale, [0, RAND_MAX], so every
/// expression the models wrap around it is DaisySP's unchanged.
struct LcgRand
{
    uint32_t state = 1;
    void     Seed(uint32_t s) { state = s; }
    float    Draw()
    {
        state = state * 1664525u + 1013904223u; // Numerical Recipes LCG
        // Bits 1..31: RAND_MAX is 2^31-1 on newlib and macOS alike. Float keeps
        // the top 24, so the LCG's weak low bits never reach the output.
        return static_cast<float>(state >> 1);
    }
};

/// The C library's rand(), exactly as DaisySP draws it. Only host/dsptest.cpp
/// uses this, to prove the forks bit-identical to the originals.
struct StdRand
{
    void  Seed(uint32_t) {}
    float Draw() { return static_cast<float>(std::rand()); }
};

// ---- AnalogBassDrum -----------------------------------------------------------

/// 808 bass drum model. Not bit-exact: see the file comment.
class AnalogBassDrum
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        pulse_remaining_samples_    = 0;
        fm_pulse_remaining_samples_ = 0;
        pulse_ = pulse_height_ = pulse_lp_ = fm_pulse_lp_ = 0.0f;
        retrig_pulse_ = lp_out_ = tone_lp_ = 0.0f;

        // Rate-dependent constants, computed by the original on every sample.
        trigger_pulse_duration_ = static_cast<int>(1.0e-3f * sample_rate_);
        fm_pulse_duration_      = static_cast<int>(6.0e-3f * sample_rate_);
        const float pulse_decay_time    = 0.2e-3f * sample_rate_;
        const float pulse_filter_time   = 0.1e-3f * sample_rate_;
        const float retrig_pulse_length = 0.05f * sample_rate_;
        pulse_decay_      = 1.0f - 1.0f / pulse_decay_time;
        pulse_filter_     = 1.0f / pulse_filter_time;
        retrig_decay_     = 1.0f - 1.0f / retrig_pulse_length;

        accent_ = fclamp(.1f, 0.f, 1.f);
        f0_     = fclamp(50.f / sample_rate_, 0.f, .5f);
        tone_   = fclamp(.1f, 0.f, 1.f);
        // Two statements, as SetDecay has them: one expression could be fused
        // into an FMA and round differently.
        decay_ = .3f * .1f;
        decay_ -= .1f;
        self_fm_amount_   = 1.f * 50.f;
        attack_fm_amount_ = .5f * 50.f;

        resonator_.Init(sample_rate_);
        Update();
    }

    void SetAccent(float accent) { accent_ = fclamp(accent, 0.f, 1.f); }

    void SetFreq(float f0)
    {
        f0 /= sample_rate_;
        f0_ = fclamp(f0, 0.f, .5f);
        Update();
    }

    void SetTone(float tone)
    {
        tone_ = fclamp(tone, 0.f, 1.f);
        Update();
    }

    void SetDecay(float decay)
    {
        decay_ = decay * .1f;
        decay_ -= .1f;
        Update();
    }

    void SetAttackFmAmount(float a) { attack_fm_amount_ = a * 50.f; }
    void SetSelfFmAmount(float a) { self_fm_amount_ = a * 50.f; }

    float Process(bool trigger)
    {
        if(trigger)
        {
            pulse_remaining_samples_    = trigger_pulse_duration_;
            fm_pulse_remaining_samples_ = fm_pulse_duration_;
            pulse_height_               = 3.0f + 7.0f * accent_;
            lp_out_                     = 0.0f;
        }

        // Q39 / Q40
        float pulse = 0.0f;
        if(pulse_remaining_samples_)
        {
            --pulse_remaining_samples_;
            pulse  = pulse_remaining_samples_ ? pulse_height_ : pulse_height_ - 1.0f;
            pulse_ = pulse;
        }
        else
        {
            pulse_ *= pulse_decay_;
            pulse = pulse_;
        }

        // C40 / R163 / R162 / D83
        fonepole(pulse_lp_, pulse, pulse_filter_);
        pulse = Diode((pulse - pulse_lp_) + pulse * 0.044f);

        // Q41 / Q42
        float fm_pulse = 0.0f;
        if(fm_pulse_remaining_samples_)
        {
            --fm_pulse_remaining_samples_;
            fm_pulse = 1.0f;
            // C39 / C52
            retrig_pulse_ = fm_pulse_remaining_samples_ ? 0.0f : -0.8f;
        }
        else
        {
            // C39 / R161
            retrig_pulse_ *= retrig_decay_;
        }
        fonepole(fm_pulse_lp_, fm_pulse, pulse_filter_);

        // Q43 and R170 leakage
        const float punch = 0.7f + Diode(10.0f * lp_out_ - 1.0f);

        // Q43 / R165
        const float attack_fm = fm_pulse_lp_ * 1.7f * attack_fm_amount_;
        const float self_fm   = punch * 0.08f * self_fm_amount_;
        float       f         = f0_ * (1.0f + attack_fm + self_fm);
        f                     = fclamp(f, 0.0f, 0.4f);

        // The one per-sample coefficient update that has to stay: f is FM'd.
        resonator_.SetFreqResFast(f * sample_rate_, .4f * q_ * f);
        resonator_.Process((pulse - retrig_pulse_ * 0.2f) * scale_);
        const float resonator_out = resonator_.Band();
        lp_out_                   = resonator_.Low();

        fonepole(tone_lp_, pulse * exciter_leak_ + resonator_out, tone_f_);
        return tone_lp_;
    }

  private:
    static float Diode(float x)
    {
        if(x >= 0.0f)
            return x;
        x *= 2.0f;
        return 0.7f * x / (1.0f + std::fabs(x));
    }

    void Update()
    {
        scale_  = 0.001f / f0_;
        q_      = 1500.0f * ::powf(2.f, kOneTwelfth * decay_ * 80.0f);
        tone_f_ = std::fmin(4.0f * f0_ * ::powf(2.f, kOneTwelfth * tone_ * 108.0f), 1.0f);
        exciter_leak_ = 0.08f * (tone_ + 0.25f);
    }

    float sample_rate_;
    float accent_, f0_, tone_, decay_;
    float attack_fm_amount_, self_fm_amount_;

    int   pulse_remaining_samples_, fm_pulse_remaining_samples_;
    float pulse_, pulse_height_, pulse_lp_, fm_pulse_lp_;
    float retrig_pulse_, lp_out_, tone_lp_;

    // Hoisted.
    int   trigger_pulse_duration_, fm_pulse_duration_;
    float pulse_decay_, pulse_filter_, retrig_decay_;
    float scale_, q_, tone_f_, exciter_leak_;

    Svf resonator_;
};

// ---- SyntheticSnareDrum -------------------------------------------------------

/// 909 snare model. Bit-identical to daisysp::SyntheticSnareDrum, with one
/// deliberate fix: the original alternates an `even` flag that is a *file-scope
/// global*, so two snares on two tracks would each see it flip twice per sample
/// and stop alternating. Here it is per instance, which is what it meant.
template <typename Rng = LcgRand>
class SyntheticSnareDrum
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_     = sample_rate;
        phase_[0]        = 0.0f;
        phase_[1]        = 0.0f;
        drum_amplitude_  = 0.0f;
        snare_amplitude_ = 0.0f;
        fm_              = 0.0f;
        hold_counter_    = 0;
        even_            = true;
        rng_.Seed(NextNoiseSeed());

        accent_    = fclamp(.6f, 0.f, 1.f);
        f0_        = fclamp(200.f / sample_rate_, 0.f, 1.f);
        fm_amount_ = .1f * .1f;
        decay_     = std::fmax(.3f, 0.f);
        snappy_    = fclamp(.7f, 0.f, 1.f);

        drum_lp_.Init(sample_rate_);
        snare_hp_.Init(sample_rate_);
        snare_lp_.Init(sample_rate_);
        fm_decay_ = 1.0f - 1.0f / (0.007f * sample_rate_);
        Update();
    }

    void SetAccent(float accent) { accent_ = fclamp(accent, 0.f, 1.f); }

    void SetFreq(float f0)
    {
        f0 /= sample_rate_;
        f0_ = fclamp(f0, 0.f, 1.f);
        Update();
    }

    void SetFmAmount(float fm_amount)
    {
        fm_amount  = fclamp(fm_amount, 0.f, 1.f);
        fm_amount_ = fm_amount * fm_amount;
        Update();
    }

    void SetDecay(float decay)
    {
        decay_ = std::fmax(decay, 0.f);
        Update();
    }

    void SetSnappy(float snappy)
    {
        snappy_ = fclamp(snappy, 0.f, 1.f);
        Update();
    }

    float Process(bool trigger)
    {
        if(trigger)
        {
            snare_amplitude_ = drum_amplitude_ = 0.3f + 0.7f * accent_;
            fm_                                = 1.0f;
            phase_[0] = phase_[1] = 0.0f;
            hold_counter_
                = static_cast<int>((0.04f + decay_ * 0.03f) * sample_rate_);
        }

        even_ = !even_;
        // Compute all D envelopes.
        // The envelope for the drum has a very long tail.
        // The envelope for the snare has a "hold" stage which lasts between
        // 40 and 70 ms
        drum_amplitude_ *= (drum_amplitude_ > 0.03f || even_) ? drum_decay_ : 1.0f;
        if(hold_counter_)
            --hold_counter_;
        else
            snare_amplitude_ *= snare_decay_;
        fm_ *= fm_decay_;

        // The 909 circuit has a funny kind of oscillator coupling - the signal
        // leaving Q40's collector and resetting all oscillators allow some
        // intermodulation.
        float reset_noise = 0.0f;
        reset_noise += phase_[0] > 0.5f ? -1.0f : 1.0f;
        reset_noise += phase_[1] > 0.5f ? -1.0f : 1.0f;
        reset_noise *= reset_noise_amount_ * 0.025f;

        const float f = f0_ * (1.0f + fm_amount_ * (4.0f * fm_));
        phase_[0] += f;
        phase_[1] += f * 1.47f;
        if(reset_noise_amount_ > 0.1f)
        {
            if(phase_[0] >= 1.0f + reset_noise)
                phase_[0] = 1.0f - phase_[0];
            if(phase_[1] >= 1.0f + reset_noise)
                phase_[1] = 1.0f - phase_[1];
        }
        else
        {
            if(phase_[0] >= 1.0f)
                phase_[0] -= 1.0f;
            if(phase_[1] >= 1.0f)
                phase_[1] -= 1.0f;
        }

        float drum = -0.1f;
        drum += DistortedSine(phase_[0]) * 0.60f;
        drum += DistortedSine(phase_[1]) * 0.25f;
        drum *= drum_amplitude_ * drum_level_;

        drum_lp_.Process(drum);
        drum = drum_lp_.Low();

        const float noise = rng_.Draw() * kRandFrac;
        snare_lp_.Process(noise);
        float snare = snare_lp_.Low();
        snare_hp_.Process(snare);
        snare = snare_hp_.High();
        snare = (snare + 0.1f) * (snare_amplitude_ + fm_) * snare_level_;

        return snare + drum; // It's a snare, it's a drum, it's a snare drum.
    }

  private:
    static float DistortedSine(float phase)
    {
        const float triangle = (phase < 0.5f ? phase : 1.0f - phase) * 4.0f - 1.3f;
        return 2.0f * triangle / (1.0f + std::fabs(triangle));
    }

    /// The original's per-sample preamble, run on a parameter change. The
    /// filter calls are in the original's order, which matters: SetFreq's
    /// damp reads res_, and SetRes overwrites it.
    void Update()
    {
        const float decay_xt = decay_ * (1.0f + decay_ * (decay_ - 1.0f));
        drum_decay_
            = 1.0f
              - 1.0f / (0.015f * sample_rate_)
                    * ::powf(2.f,
                             kOneTwelfth
                                 * (-decay_xt * 72.0f - fm_amount_ * 12.0f
                                    + snappy_ * 7.0f));
        snare_decay_
            = 1.0f
              - 1.0f / (0.01f * sample_rate_)
                    * ::powf(2.f, kOneTwelfth * (-decay_ * 60.0f - snappy_ * 7.0f));

        float snappy = snappy_ * 1.1f - 0.05f;
        snappy       = fclamp(snappy, 0.0f, 1.0f);
        drum_level_  = ::sqrtf(1.0f - snappy);
        snare_level_ = ::sqrtf(snappy);

        const float snare_f_min = std::fmin(10.0f * f0_, 0.5f);
        const float snare_f_max = std::fmin(35.0f * f0_, 0.5f);
        snare_hp_.SetFreq(snare_f_min * sample_rate_);
        snare_lp_.SetFreq(snare_f_max * sample_rate_);
        snare_lp_.SetRes(0.5f + 2.0f * snappy);
        drum_lp_.SetFreq(3.0f * f0_ * sample_rate_);

        float amount = (0.125f - f0_) * 8.0f;
        amount       = fclamp(amount, 0.0f, 1.0f);
        amount *= amount;
        amount *= fm_amount_;
        reset_noise_amount_ = amount;
    }

    float sample_rate_;
    float accent_, f0_, fm_amount_, decay_, snappy_;

    float phase_[2];
    float drum_amplitude_, snare_amplitude_, fm_;
    int   hold_counter_;
    bool  even_;

    // Hoisted.
    float drum_decay_, snare_decay_, fm_decay_;
    float drum_level_, snare_level_, reset_noise_amount_;

    Svf drum_lp_, snare_hp_, snare_lp_;
    Rng rng_;
};

// ---- AnalogSnareDrum ------------------------------------------------------------

/// 808 snare model. Bit-identical to daisysp::AnalogSnareDrum, including two
/// oddities kept on purpose because changing them changes the sound: the
/// noise filter's frequency clamp is computed and discarded, and pulse_lp_ is
/// updated with fclamp where a one-pole was surely meant.
template <typename Rng = LcgRand>
class AnalogSnareDrum
{
  public:
    static constexpr int kNumModes = 5;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;

        pulse_remaining_samples_ = 0;
        pulse_ = pulse_height_ = pulse_lp_ = noise_envelope_ = 0.0f;

        accent_ = fclamp(.6f, 0.f, 1.f);
        f0_     = fclamp(200.f / sample_rate_, 0.f, .4f);
        decay_  = .3f;
        snappy_ = fclamp(.7f, 0.f, 1.f);
        tone_   = fclamp(.5f, 0.f, 1.f) * 2.f;

        for(auto &r : resonator_)
            r.Init(sample_rate_);
        noise_filter_.Init(sample_rate_);
        rng_.Seed(NextNoiseSeed());

        // The original computes these in double, then narrows. Kept.
        trigger_pulse_duration_ = static_cast<int>(1.0e-3 * sample_rate_);
        const float pulse_decay_time = static_cast<float>(0.1e-3 * sample_rate_);
        pulse_decay_ = 1.0f - 1.0f / pulse_decay_time;
        Update();
    }

    void SetAccent(float accent) { accent_ = fclamp(accent, 0.f, 1.f); }

    void SetFreq(float f0)
    {
        f0  = f0 / sample_rate_;
        f0_ = fclamp(f0, 0.f, .4f);
        Update();
    }

    void SetTone(float tone)
    {
        tone_ = fclamp(tone, 0.f, 1.f);
        tone_ *= 2.f;
        Update();
    }

    void SetDecay(float decay)
    {
        decay_ = decay;
        Update();
    }

    void SetSnappy(float snappy)
    {
        snappy_ = fclamp(snappy, 0.f, 1.f);
        Update();
    }

    float Process(bool trigger)
    {
        if(trigger)
        {
            pulse_remaining_samples_ = trigger_pulse_duration_;
            pulse_height_            = 3.0f + 7.0f * accent_;
            noise_envelope_          = 2.0f;
        }

        // Q45 / Q46
        float pulse = 0.0f;
        if(pulse_remaining_samples_)
        {
            --pulse_remaining_samples_;
            pulse  = pulse_remaining_samples_ ? pulse_height_ : pulse_height_ - 1.0f;
            pulse_ = pulse;
        }
        else
        {
            pulse_ *= pulse_decay_;
            pulse = pulse_;
        }

        // R189 / C57 / R190 + C58 / C59 / R197 / R196 / IC14
        pulse_lp_ = fclamp(pulse_lp_, pulse, 0.75f);

        float shell = 0.0f;
        for(int i = 0; i < kNumModes; ++i)
        {
            const float excitation
                = i == 0 ? (pulse - pulse_lp_) + 0.006f * pulse : 0.026f * pulse;
            resonator_[i].Process(excitation);
            shell += gain_[i] * (resonator_[i].Band() + excitation * exciter_leak_);
        }
        shell = SoftClip(shell);

        // C56 / R194 / Q48 / C54 / R188 / D54
        float noise = 2.0f * rng_.Draw() * kRandFrac - 1.0f;
        if(noise < 0.0f)
            noise = 0.0f;
        noise_envelope_ *= noise_envelope_decay_;
        noise *= noise_envelope_ * snappy_scaled_ * 2.0f;

        // C66 / R201 / C67 / R202 / R203 / Q49
        noise_filter_.Process(noise);
        noise = noise_filter_.Band();

        // IC13
        return noise + shell * (1.0f - snappy_scaled_);
    }

  private:
    static float SoftLimit(float x) { return x * (27.0f + x * x) / (27.0f + 9.0f * x * x); }

    static float SoftClip(float x)
    {
        if(x < -3.0f)
            return -1.0f;
        if(x > 3.0f)
            return 1.0f;
        return SoftLimit(x);
    }

    void Update()
    {
        const float decay_xt = decay_ * (1.0f + decay_ * (decay_ - 1.0f));
        const float q = 2000.0f * ::powf(2.f, kOneTwelfth * decay_xt * 84.0f);
        noise_envelope_decay_
            = 1.0f
              - 0.0017f
                    * ::powf(2.f, kOneTwelfth * (-decay_ * (50.0f + snappy_ * 10.0f)));
        exciter_leak_ = snappy_ * (2.0f - snappy_) * 0.1f;

        float snappy   = snappy_ * 1.1f - 0.05f;
        snappy_scaled_ = fclamp(snappy, 0.0f, 1.0f);

        static constexpr float kModeFrequencies[kNumModes]
            = {1.00f, 2.00f, 3.18f, 4.16f, 5.62f};
        for(int i = 0; i < kNumModes; ++i)
        {
            const float f = std::fmin(f0_ * kModeFrequencies[i], 0.499f);
            resonator_[i].SetFreq(f * sample_rate_);
            // `* .2` is a double multiply in the original; kept, as it rounds.
            resonator_[i].SetRes((f * (i == 0 ? q : q * 0.25f)) * .2);
        }

        float tone = tone_;
        if(tone < 0.666667f)
        {
            // 808-style (2 modes)
            tone *= 1.5f;
            gain_[0] = 1.5f + (1.0f - tone) * (1.0f - tone) * 4.5f;
            gain_[1] = 2.0f * tone + 0.15f;
            for(int i = 2; i < kNumModes; i++)
                gain_[i] = 0.f;
        }
        else
        {
            // What the 808 could have been if there were extra modes!
            tone     = (tone - 0.666667f) * 3.0f;
            gain_[0] = 1.5f - tone * 0.5f;
            gain_[1] = 2.15f - tone * 0.7f;
            for(int i = 2; i < kNumModes; ++i)
            {
                gain_[i] = tone;
                tone *= tone;
            }
        }

        // The original clamps f_noise and discards the result; not clamped.
        const float f_noise = f0_ * 16.0f;
        noise_filter_.SetFreq(f_noise * sample_rate_);
        noise_filter_.SetRes(f_noise * 1.5f);
    }

    float sample_rate_;
    float f0_, tone_, accent_, snappy_, decay_;

    int   pulse_remaining_samples_;
    float pulse_, pulse_height_, pulse_lp_, noise_envelope_;

    // Hoisted.
    int   trigger_pulse_duration_;
    float pulse_decay_, noise_envelope_decay_, exciter_leak_, snappy_scaled_;
    float gain_[kNumModes];

    Svf resonator_[kNumModes];
    Svf noise_filter_;
    Rng rng_;
};

// ---- Hi-hat and its metallic noise sources -----------------------------------

/// 808 metallic noise: six square oscillators. Frequency set once per change
/// rather than passed in per sample. Bit-identical.
class SquareNoise
{
  public:
    void Init(float) { for(auto &p : phase_) p = 0; }

    void SetF0(float f0)
    {
        static constexpr float kRatios[6] // Nominal f0: 414 Hz
            = {1.0f, 1.304f, 1.466f, 1.787f, 1.932f, 2.536f};
        for(int i = 0; i < 6; ++i)
        {
            float f = f0 * kRatios[i];
            if(f >= 0.499f)
                f = 0.499f;
            increment_[i] = static_cast<uint32_t>(f * 4294967296.0f);
        }
    }

    float Process()
    {
        uint32_t noise = 0;
        for(int i = 0; i < 6; ++i)
        {
            phase_[i] += increment_[i];
            noise += phase_[i] >> 31;
        }
        return 0.33f * static_cast<float>(noise) - 1.0f;
    }

  private:
    uint32_t phase_[6];
    uint32_t increment_[6] = {};
};

/// Ring-mod metallic noise: three square x saw pairs. Bit-identical.
///
/// The original drives six daisysp::Oscillators and re-sets each one's
/// waveform and frequency on every sample. Here the frequencies are set on a
/// change, and the oscillators are reduced to what a square and a saw need:
/// Oscillator::Process also runs a waveform switch and end-of-cycle/rise
/// bookkeeping that nothing reads, which made it 8 % of the worst case. The
/// arithmetic that reaches the output is the original's, operation for
/// operation, including its amplitude of 0.5 and its `phase > 1` wrap.
class RingModNoise
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        sr_recip_    = 1.0f / sample_rate;
        for(auto &p : phase_)
            p = 0.0f;
    }

    void SetF0(float f0)
    {
        const float ratio = f0 / (0.01f + f0);
        const float f[6]  = {200.0f / sample_rate_ * ratio,
                            7530.0f / sample_rate_ * ratio,
                            510.0f / sample_rate_ * ratio,
                            8075.0f / sample_rate_ * ratio,
                            730.0f / sample_rate_ * ratio,
                            10500.0f / sample_rate_ * ratio};
        // As Oscillator::SetFreq(f * sr) computes it: (f * sr) * (1 / sr).
        for(int i = 0; i < 6; ++i)
            inc_[i] = (f[i] * sample_rate_) * sr_recip_;
    }

    float Process()
    {
        float out = Square(0) * Saw(1);
        out += Square(2) * Saw(3);
        out += Square(4) * Saw(5);
        return out;
    }

  private:
    static constexpr float kAmp = 0.5f; // Oscillator::Init's default amplitude
    static constexpr float kPw  = 0.5f; // and pulse width

    float Square(int i)
    {
        const float out = phase_[i] < kPw ? (1.0f) : -1.0f;
        Advance(i);
        return out * kAmp;
    }

    float Saw(int i)
    {
        const float out = -1.0f * (((phase_[i] * 2.0f)) - 1.0f);
        Advance(i);
        return out * kAmp;
    }

    void Advance(int i)
    {
        phase_[i] += inc_[i];
        if(phase_[i] > 1.0f)
            phase_[i] -= 1.0f;
    }

    float phase_[6];
    float inc_[6] = {};
    float sample_rate_, sr_recip_;
};

/// 808 hi-hat. Bit-identical to daisysp::HiHat with the matching noise source.
template <typename MetallicNoiseSource,
          typename VCA,
          bool resonance = true,
          typename Rng   = LcgRand>
class HiHat
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_  = sample_rate;
        envelope_     = 0.0f;
        noise_clock_  = 0.0f;
        noise_sample_ = 0.0f;

        f0_        = fclamp(3000.f / sample_rate_, 0.f, 1.f);
        tone_      = fclamp(.5f, 0.f, 1.f);
        decay_     = std::fmax(.2f, 0.f); // as SetDecay(.2f), statement for statement
        decay_ *= 1.7;
        decay_ -= 1.2;
        noisiness_ = fclamp(.8f, 0.f, 1.f);
        noisiness_ *= noisiness_;
        accent_ = fclamp(.8f, 0.f, 1.f);

        metallic_noise_.Init(sample_rate_);
        rng_.Seed(NextNoiseSeed());
        noise_coloration_svf_.Init(sample_rate_);
        hpf_.Init(sample_rate_);
        Update();
    }

    void SetAccent(float accent) { accent_ = fclamp(accent, 0.f, 1.f); }

    void SetFreq(float f0)
    {
        f0 /= sample_rate_;
        f0_ = fclamp(f0, 0.f, 1.f);
        Update();
    }

    void SetTone(float tone)
    {
        tone_ = fclamp(tone, 0.f, 1.f);
        Update();
    }

    /// The original does this in double (`* 1.7`, `- 1.2`); kept, as it rounds.
    void SetDecay(float decay)
    {
        decay_ = std::fmax(decay, 0.f);
        decay_ *= 1.7;
        decay_ -= 1.2;
        Update();
    }

    void SetNoisiness(float noisiness)
    {
        noisiness_ = fclamp(noisiness, 0.f, 1.f);
        noisiness_ *= noisiness_;
        Update();
    }

    float Process(bool trigger)
    {
        if(trigger)
            envelope_ = (1.5f + 0.5f * (1.0f - decay_)) * (0.3f + 0.7f * accent_);

        // Process the metallic noise.
        float out = metallic_noise_.Process();

        // Apply BPF on the metallic noise.
        noise_coloration_svf_.Process(out);
        out = noise_coloration_svf_.Band();

        // This is not at all part of the 808 circuit! But to add more variety, we
        // add a variable amount of clocked noise to the output of the 6 schmitt
        // trigger oscillators.
        noise_clock_ += noise_f_;
        if(noise_clock_ >= 1.0f)
        {
            noise_clock_ -= 1.0f;
            noise_sample_ = rng_.Draw() * kRandFrac - 0.5f;
        }
        out += noisiness_ * (noise_sample_ - out);

        // Apply VCA.
        VCA vca;
        envelope_ *= envelope_ > 0.5f ? envelope_decay_ : cut_decay_;
        out = vca(out, envelope_);

        hpf_.Process(out);
        return hpf_.High();
    }

  private:
    static float SemitonesToRatio(float in) { return ::powf(2.f, in * kOneTwelfth); }

    void Update()
    {
        envelope_decay_ = 1.0f - 0.003f * SemitonesToRatio(-decay_ * 84.0f);
        cut_decay_      = 1.0f - 0.0025f * SemitonesToRatio(-decay_ * 36.0f);

        metallic_noise_.SetF0(2.0f * f0_);

        float cutoff = 150.0f / sample_rate_ * SemitonesToRatio(tone_ * 72.0f);
        cutoff       = fclamp(cutoff, 0.0f, 16000.0f / sample_rate_);
        noise_coloration_svf_.SetFreq(cutoff * sample_rate_);
        noise_coloration_svf_.SetRes(resonance ? 3.0f + 6.0f * tone_ : 1.0f);

        float noise_f = f0_ * (16.0f + 16.0f * (1.0f - noisiness_));
        noise_f_      = fclamp(noise_f, 0.0f, 0.5f);

        hpf_.SetFreq(cutoff * sample_rate_);
        hpf_.SetRes(.5f);
    }

    float sample_rate_;
    float accent_, f0_, tone_, decay_, noisiness_;
    float envelope_, noise_clock_, noise_sample_;

    // Hoisted.
    float envelope_decay_, cut_decay_, noise_f_;

    MetallicNoiseSource metallic_noise_;
    Svf                 noise_coloration_svf_;
    Svf                 hpf_;
    Rng                 rng_;
};

} // namespace dsp
} // namespace drom
