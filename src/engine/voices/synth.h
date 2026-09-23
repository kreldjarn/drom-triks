#pragma once
#include "daisysp.h"
#include "../voice.h"

// The four voices DaisySP doesn't cover. Each is built from primitives, and
// each maps the same six macros as the DaisySP-backed voices so muscle memory
// transfers between tracks (docs/01-hardware.md §2).

namespace drom {

/// Tom. A sine with a fast downward pitch sweep — the sweep is what separates a
/// tom from a beep, and TONE controls how far it falls.
class Tom : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        osc_.Init(sr);
        osc_.SetWaveform(daisysp::Oscillator::WAVE_SIN);
        amp_.Init(sr);
        amp_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.001f);
        amp_.SetMax(1.f);
        amp_.SetMin(0.f);
        pitch_.Init(sr);
        pitch_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        pitch_.SetMax(1.f);
        pitch_.SetMin(0.f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_    = velocity;
        active_ = true;
        amp_.Trigger();
        pitch_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        // Pitch envelope is exponential in frequency, so the sweep sounds even
        // across the tuning range rather than collapsing at low base notes.
        const float sweep = pitch_.Process() * sweep_amt_;
        osc_.SetFreq(base_hz_ * (1.f + sweep * 3.f));
        const float e = amp_.Process();
        if(!amp_.IsRunning())
            active_ = false;
        return Shape(osc_.Process() * e * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: base_hz_ = 60.f + v * 240.f; break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone: sweep_amt_ = v; break;
            // SNAP shortens the pitch sweep, turning a soft tom into a hard hit.
            case ParamId::Snap: Retime(); break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, 0.05f + param(ParamId::Decay) * 1.2f);
        pitch_.SetTime(daisysp::ADENV_SEG_DECAY,
                       0.01f + (1.f - param(ParamId::Snap)) * 0.12f);
    }

    daisysp::Oscillator osc_;
    daisysp::AdEnv      amp_, pitch_;
    float               sr_ = 48000.f, base_hz_ = 120.f, sweep_amt_ = 0.5f, vel_ = 1.f;
};

/// Clap. Three closely spaced noise bursts then a longer tail, all through a
/// bandpass — the burst spacing is the whole character, and SNAP controls it.
class Clap : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        noise_.Init();
        bpf_.Init(sr);
        bpf_.SetRes(0.35f);
        burst_.Init(sr);
        burst_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        burst_.SetTime(daisysp::ADENV_SEG_DECAY, 0.008f);
        burst_.SetMax(1.f);
        burst_.SetMin(0.f);
        tail_.Init(sr);
        tail_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.002f);
        tail_.SetMax(1.f);
        tail_.SetMin(0.f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_          = velocity;
        active_       = true;
        bursts_left_  = 3;
        countdown_    = 0;
        tail_started_ = false;
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        // Fire the remaining bursts on a countdown, then start the tail. This
        // is what a real 808 clap does with a delay line and a retrigger.
        if(bursts_left_ > 0 && countdown_ <= 0)
        {
            burst_.Trigger();
            --bursts_left_;
            countdown_ = static_cast<int>(spread_s_ * sr_);
        }
        else if(bursts_left_ == 0 && !tail_started_)
        {
            tail_.Trigger();
            tail_started_ = true;
        }
        --countdown_;

        const float n = noise_.Process();
        bpf_.Process(n);
        const float env = burst_.Process() + tail_.Process() * 0.6f;
        if(bursts_left_ == 0 && tail_started_ && !burst_.IsRunning() && !tail_.IsRunning())
            active_ = false;
        return Shape(bpf_.Band() * env * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: bpf_.SetFreq(600.f + v * 1800.f); break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone: bpf_.SetRes(0.1f + v * 0.7f); break;
            // 5–25 ms between bursts; past ~30 ms it stops fusing into one clap.
            case ParamId::Snap: spread_s_ = 0.005f + v * 0.020f; break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        tail_.SetTime(daisysp::ADENV_SEG_DECAY, 0.06f + param(ParamId::Decay) * 0.5f);
    }

    daisysp::WhiteNoise noise_;
    daisysp::Svf        bpf_;
    daisysp::AdEnv      burst_, tail_;
    float               sr_ = 48000.f, spread_s_ = 0.012f, vel_ = 1.f;
    int                 bursts_left_ = 0, countdown_ = 0;
    bool                tail_started_ = true;
};

/// Rimshot. Two detuned squares through a sharp resonant bandpass with a very
/// short decay — the detuning is what gives the metallic beat in the attack.
class RimShot : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        a_.Init(sr);
        b_.Init(sr);
        a_.SetWaveform(daisysp::Oscillator::WAVE_SQUARE);
        b_.SetWaveform(daisysp::Oscillator::WAVE_SQUARE);
        bpf_.Init(sr);
        bpf_.SetRes(0.8f);
        env_.Init(sr);
        env_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        env_.SetMax(1.f);
        env_.SetMin(0.f);
        Retune();
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_    = velocity;
        active_ = true;
        env_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float mix = a_.Process() * 0.6f + b_.Process() * 0.4f;
        bpf_.Process(mix);
        const float e = env_.Process();
        if(!env_.IsRunning())
            active_ = false;
        return Shape(bpf_.Band() * e * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: base_hz_ = 200.f + v * 600.f; Retune(); break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone: bpf_.SetFreq(400.f + v * 3000.f); break;
            case ParamId::Snap: detune_ = 1.1f + v * 0.9f; Retune(); break;
            default: break;
        }
    }

  private:
    void Retune()
    {
        a_.SetFreq(base_hz_);
        b_.SetFreq(base_hz_ * detune_);
    }
    void Retime()
    {
        // Deliberately short even at full DECAY: a long rimshot is a woodblock.
        env_.SetTime(daisysp::ADENV_SEG_DECAY, 0.01f + param(ParamId::Decay) * 0.12f);
    }

    daisysp::Oscillator a_, b_;
    daisysp::Svf        bpf_;
    daisysp::AdEnv      env_;
    float               base_hz_ = 400.f, detune_ = 1.4f, vel_ = 1.f;
};

/// Cheap sine over a phase that need not be wrapped by the caller.
///
/// Two evaluations per sample per voice makes sinf worth avoiding; this is the
/// standard parabolic approximation with one refinement pass, accurate to
/// roughly 0.1%. That error is orders of magnitude below the grit this voice
/// adds deliberately.
inline float FastSin(float phase)
{
    phase = phase - static_cast<float>(static_cast<int>(phase));
    if(phase < 0.f)
        phase += 1.f;
    const float t = phase * 2.f - 1.f;          // [-1, 1) ~ [-pi, pi)
    const float a = t < 0.f ? -t : t;
    float       y = 4.f * t * (1.f - a);
    const float b = y < 0.f ? -y : y;
    return 0.225f * (y * b - y) + y;
}

/// Two-operator FM with operator feedback — the Machinedrum EFM shape.
///
/// Three things separate percussive FM from a bell, and only the first is
/// obvious:
///
///  1. **The modulation index must decay**, and faster than the amplitude. A
///     static index gives an organ or a bell; an index that collapses in a few
///     milliseconds gives a transient with a body behind it. This is most of
///     the "punch".
///  2. **Operator feedback** — the modulator folded back into its own phase.
///     Past roughly 0.6 it breaks into noise, which is exactly what makes
///     metallic percussion read as metal rather than as a tuned tone.
///  3. **Bit and rate reduction.** The hardware this imitates ran 12-bit
///     converters, and that grit is part of the sound rather than a flaw.
///
/// Covers cowbell, metallic percussion, sharp blips and sub depending mostly
/// on where TONE (ratio) and SNAP (index) sit.
class FmVoice : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        inv_sr_ = 1.f / sr;
        amp_.Init(sr);
        amp_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0004f);
        amp_.SetMax(1.f);
        amp_.SetMin(0.f);
        idx_.Init(sr);
        idx_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        idx_.SetMax(1.f);
        idx_.SetMin(0.f);
        pitch_.Init(sr);
        pitch_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        pitch_.SetTime(daisysp::ADENV_SEG_DECAY, 0.02f);
        pitch_.SetMax(1.f);
        pitch_.SetMin(0.f);
        crush_.Init();
        crush_.SetDownsampleFactor(0.f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_     = velocity;
        active_  = true;
        cphase_  = 0.f;
        mphase_  = 0.f;
        fb_      = 0.f;
        amp_.Trigger();
        idx_.Trigger();
        pitch_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;

        const float ae = amp_.Process();
        if(!amp_.IsRunning())
            active_ = false;
        const float ie = idx_.Process();
        const float pe = pitch_.Process();

        // A short pitch blip on top of the FM, which is what stops high-ratio
        // settings sounding static.
        const float hz   = base_hz_ * (1.f + pe * 0.6f);
        const float minc = hz * ratio_ * inv_sr_;
        const float cinc = hz * inv_sr_;

        const float m = FastSin(mphase_ + fb_ * feedback_);
        fb_           = m;
        mphase_ += minc;
        if(mphase_ >= 1.f)
            mphase_ -= 1.f;

        const float c = FastSin(cphase_ + m * index_ * ie);
        cphase_ += cinc;
        if(cphase_ >= 1.f)
            cphase_ -= 1.f;

        float out = c * ae * vel_;
        if(bits_ > 0)
            out = crush_.Process(out);
        return Shape(out);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: base_hz_ = 40.f + v * 760.f; break;
            case ParamId::Decay: Retime(); break;
            // Integer ratios stay harmonic; the space between them is where
            // the metallic, inharmonic tones live. The useful range spans both.
            case ParamId::Tone: ratio_ = 0.5f + v * 11.5f; break;
            // SNAP sets how much modulation there is *and* how fast it
            // collapses. Turning it up makes the hit sharper, not just brighter.
            case ParamId::Snap:
                index_ = v * 9.f;
                Retime();
                break;
            // DRIVE buys grit: feedback first, then bit reduction on top.
            case ParamId::Drive:
                feedback_ = v * 0.85f;
                bits_     = static_cast<uint8_t>(v * 7.f);
                crush_.SetBitsToCrush(bits_);
                break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, 0.015f + param(ParamId::Decay) * 1.4f);
        // The index envelope is always shorter than the amplitude envelope —
        // that ordering is what makes it read as a transient.
        idx_.SetTime(daisysp::ADENV_SEG_DECAY,
                     0.004f + (1.f - param(ParamId::Snap)) * 0.10f);
    }

    daisysp::AdEnv     amp_, idx_, pitch_;
    daisysp::Decimator crush_;
    float              inv_sr_   = 1.f / 48000.f;
    float              cphase_ = 0.f, mphase_ = 0.f, fb_ = 0.f;
    float              base_hz_  = 200.f;
    float              ratio_    = 3.5f;
    float              index_    = 4.f;
    float              feedback_ = 0.f;
    float              vel_      = 1.f;
    uint8_t            bits_     = 0;
};

} // namespace drom
