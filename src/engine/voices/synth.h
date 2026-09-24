#pragma once
#include "daisysp.h"
#include "../voice.h"

// The four voices DaisySP doesn't cover. Each is built from primitives, and
// each maps the same INST-page macros as the DaisySP-backed voices so muscle memory
// transfers between tracks (docs/01-hardware.md §2).

namespace drom {

/// AdEnv defaults to a LINEAR decay, which is wrong for percussion: it holds
/// near full level and then drops, so a hit reads as a sustained tone that
/// cuts off rather than as a strike. Real percussion decays roughly
/// exponentially. Negative curve values give that; around -4 puts the envelope
/// at ~35% a quarter of the way through its decay, which is what makes a hit
/// sound struck.
inline constexpr float kPercCurve = -4.f;

/// Decay times spread exponentially, not linearly.
///
/// A linear map from 15 ms to 1.4 s puts the knob's midpoint at 715 ms, so
/// most of its travel is "long" and the short, percussive settings are all
/// crammed into the first few degrees. Exponential spacing matches how decay
/// time is actually heard: 20 ms at zero, ~155 ms at halfway, 1.2 s at full.
inline float DecayTime(float v)
{
    return 0.02f * powf(60.f, v);
}

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
        amp_.SetCurve(kPercCurve);
        amp_.SetMax(1.f);
        amp_.SetMin(0.f);
        pitch_.Init(sr);
        pitch_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        pitch_.SetCurve(kPercCurve);
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
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, DecayTime(param(ParamId::Decay)));
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
        burst_.SetCurve(kPercCurve);
        burst_.SetMax(1.f);
        burst_.SetMin(0.f);
        tail_.Init(sr);
        tail_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.002f);
        tail_.SetCurve(kPercCurve);
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
        tail_.SetTime(daisysp::ADENV_SEG_DECAY, DecayTime(param(ParamId::Decay)) * 0.6f);
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
        env_.SetCurve(kPercCurve);
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

/// Three-operator FM with operator feedback — the Machinedrum EFM shape.
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
/// op2 and op3 modulate the carrier in parallel, at ratios chosen to be
/// incommensurate so their sidebands never line up. op3 only engages in the
/// upper half of SNAP and rides a squared copy of the index envelope, so it
/// decays faster than op2 — an attack thickener rather than a drone, and one
/// knob from a clean two-op tone to a dense metallic crash.
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
        amp_.SetCurve(kPercCurve);
        amp_.SetMax(1.f);
        amp_.SetMin(0.f);
        idx_.Init(sr);
        idx_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        idx_.SetCurve(kPercCurve);
        idx_.SetMax(1.f);
        // The index floor is what stops the tail collapsing to a bare carrier
        // sine. Without it a hit is a noise attack followed by a beep, which
        // is the single most un-percussive thing this voice can do.
        idx_.SetMin(0.12f);
        pitch_.Init(sr);
        pitch_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        pitch_.SetTime(daisysp::ADENV_SEG_DECAY, 0.02f);
        pitch_.SetCurve(kPercCurve);
        pitch_.SetMax(1.f);
        pitch_.SetMin(0.f);
        crush_.Init();
        crush_.SetDownsampleFactor(0.f); // 0 = sample-accurate, no hold
        crush_.SetBitsToCrush(0);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_     = velocity;
        active_  = true;
        cphase_  = 0.f;
        mphase_  = 0.f;
        m3phase_ = 0.f;
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

        // op3 modulates the carrier in PARALLEL with op2, not stacked into it.
        // Stacking was tried and measured: once op2 is deep enough to sound
        // metallic it is already near-chaotic, so a third operator feeding it
        // just adds more of the same and changes the character barely at all.
        // In parallel, at a ratio deliberately incommensurate with op2's, it
        // contributes its own sideband family instead.
        float m3 = 0.f;
        if(stack_ > 0.f)
        {
            m3 = FastSin(m3phase_) * stack_ * ie * ie;
            m3phase_ += hz * ratio3_ * inv_sr_;
            if(m3phase_ >= 1.f)
                m3phase_ -= 1.f;
        }

        const float m = FastSin(mphase_ + fb_ * feedback_);
        fb_           = m;
        mphase_ += minc;
        if(mphase_ >= 1.f)
            mphase_ -= 1.f;

        const float c = FastSin(cphase_ + m * index_ * ie + m3);
        cphase_ += cinc;
        if(cphase_ >= 1.f)
            cphase_ -= 1.f;

        float out = c * ae * vel_;
        if(gritty_)
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
            case ParamId::Tone:
                ratio_ = 0.5f + v * 11.5f;
                // Incommensurate with op2 by an irrational factor, so op3's
                // sidebands never line up with op2's and the two families stay
                // audibly separate. That separation is the metallic character.
                ratio3_ = ratio_ * 0.6180f + 1.37f;
                break;
            // SNAP sets modulation depth, how fast it collapses, and — past
            // halfway — how much of op3 joins in. One knob from clean to
            // aggressive, which is how the hardware behaves.
            case ParamId::Snap:
                // Index is in TURNS, not radians, because FastSin takes a 0..1
                // phase. Textbook FM indices of 0-9 are radians; the same
                // numbers here would be nine whole cycles of phase modulation,
                // which is noise at every setting rather than a tone. 1.4 turns
                // is about 8.8 rad — deep, but still recognisably FM.
                index_ = v * 1.4f;
                stack_ = Ramp(v, 0.45f, 1.0f) * 0.9f;
                Retime();
                break;
            // DRIVE is a layered grit control rather than one effect: operator
            // feedback first, bit reduction over the top, then sample-rate
            // reduction at the extreme. There is no spare macro for these
            // separately, and stacking them this way gives one usable sweep
            // from clean to destroyed.
            case ParamId::Drive:
            {
                feedback_ = Ramp(v, 0.0f, 0.45f) * 0.85f;
                const uint8_t bits
                    = static_cast<uint8_t>(Ramp(v, 0.30f, 0.80f) * 6.f);
                // Only the bottom third of the factor is musical: it maps to a
                // hold of up to 96 samples, which past ~0.3 is a buzz, not a
                // drum.
                const float ds = Ramp(v, 0.60f, 1.0f) * 0.30f;
                crush_.SetBitsToCrush(bits);
                crush_.SetDownsampleFactor(ds);
                gritty_ = bits > 0 || ds > 0.f;
                break;
            }
            default: break;
        }
    }

  private:
    /// 0 below `lo`, rising linearly to 1 at `hi`. Used to layer several
    /// effects onto one knob without steps at the hand-over points.
    static float Ramp(float v, float lo, float hi)
    {
        if(v <= lo)
            return 0.f;
        if(v >= hi)
            return 1.f;
        return (v - lo) / (hi - lo);
    }

    void Retime()
    {
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, DecayTime(param(ParamId::Decay)));
        // The index envelope is always shorter than the amplitude envelope —
        // that ordering is what makes it read as a transient.
        idx_.SetTime(daisysp::ADENV_SEG_DECAY,
                     0.004f + (1.f - param(ParamId::Snap)) * 0.10f);
    }

    daisysp::AdEnv     amp_, idx_, pitch_;
    daisysp::Decimator crush_;
    float              inv_sr_   = 1.f / 48000.f;
    float              cphase_ = 0.f, mphase_ = 0.f, m3phase_ = 0.f, fb_ = 0.f;
    float              base_hz_  = 200.f;
    float              ratio_    = 3.5f;
    float              ratio3_   = 3.5f * 2.37f;
    float              index_    = 4.f;
    float              stack_    = 0.f; ///< op3 -> op2 depth
    float              feedback_ = 0.f;
    float              vel_      = 1.f;
    bool               gritty_   = false;
};

/// Boom kick. A deep sine with a long pitch sweep and a soft-clipped tail —
/// the opposite end of the range from the 808 model, which is tuned for punch.
///
/// The character comes from the *ratio* of sweep to decay rather than from
/// either alone: a long amplitude tail under a pitch drop that finishes early
/// reads as "boom", while the same drop under a short tail is just a click.
/// SNAP sets the sweep depth, so it moves between a sub thud and a laser.
class BassDrumBoom : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        amp_.Init(sr);
        amp_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.001f);
        amp_.SetCurve(kPercCurve);
        amp_.SetMax(1.f);
        amp_.SetMin(0.f);
        phase_ = 0.f;
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_    = velocity;
        phase_  = 0.f;
        sweep_  = 1.f;
        active_ = true;
        amp_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float env = amp_.Process();
        if(!amp_.IsRunning() && env <= 0.f)
        {
            active_ = false;
            return 0.f;
        }

        // The pitch envelope is its own exponential decay, deliberately much
        // faster than the amplitude one.
        sweep_ *= sweep_coeff_;
        const float f = base_hz_ * (1.f + sweep_ * depth_);
        phase_ += f / sr_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;

        // Soft clip before the shared drive stage: a sine this loud with no
        // harmonics disappears on small speakers, and a little fold puts a
        // second harmonic back without making it buzz.
        const float s    = FastSin(phase_);
        const float warm = s * (1.f + tone_ * 1.5f);
        const float sat  = warm / (1.f + (warm < 0.f ? -warm : warm) * tone_);
        return Shape(sat * env * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune:  base_hz_ = 25.f + v * 45.f; break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone:  tone_ = v; break;
            // Up to four octaves of drop. Past that it stops reading as a kick.
            case ParamId::Snap:  depth_ = v * 15.f; break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        // Longer than the other kick on purpose: this machine exists for tails.
        const float d = DecayTime(param(ParamId::Decay)) * 1.8f;
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, d);
        // Sweep finishes in roughly the first eighth of the tail.
        const float sweep_s = d * 0.125f;
        sweep_coeff_        = expf(-1.f / (sweep_s * sr_));
    }

    daisysp::AdEnv amp_;
    float sr_ = 48000.f, phase_ = 0.f, base_hz_ = 45.f;
    float sweep_ = 0.f, sweep_coeff_ = 0.999f, depth_ = 6.f;
    float tone_ = 0.5f, vel_ = 1.f;
};

/// Punch snare. Transient-forward: a very short, bright noise crack over a body
/// tone that drops fast, rather than the balanced noise/body of the 909 model.
///
/// Two noise envelopes rather than one — a few-millisecond crack and a longer
/// rattle — because a single envelope can be sharp or sustained but not both,
/// and a snare needs the attack to outrun its own tail.
class SnareDrumPunch : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        noise_.Init();
        crack_.Init(sr);
        crack_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        crack_.SetTime(daisysp::ADENV_SEG_DECAY, 0.012f);
        crack_.SetCurve(kPercCurve);
        crack_.SetMax(1.f);
        crack_.SetMin(0.f);
        rattle_.Init(sr);
        rattle_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.001f);
        rattle_.SetCurve(kPercCurve);
        rattle_.SetMax(1.f);
        rattle_.SetMin(0.f);
        body_.Init(sr);
        body_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        body_.SetCurve(kPercCurve);
        body_.SetMax(1.f);
        body_.SetMin(0.f);
        hp_.Init(sr);
        hp_.SetRes(0.15f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_     = velocity;
        phase_   = 0.f;
        drop_    = 1.f;
        active_  = true;
        crack_.Trigger();
        rattle_.Trigger();
        body_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float ce = crack_.Process();
        const float re = rattle_.Process();
        const float be = body_.Process();
        if(!crack_.IsRunning() && !rattle_.IsRunning() && !body_.IsRunning()
           && ce <= 0.f && re <= 0.f && be <= 0.f)
        {
            active_ = false;
            return 0.f;
        }

        const float n = noise_.Process();
        hp_.Process(n);
        const float noise = hp_.High() * (ce * 1.4f + re * 0.6f);

        // A body pitch that falls fast is most of what "punch" means here.
        drop_ *= drop_coeff_;
        phase_ += (base_hz_ * (1.f + drop_ * 1.6f)) / sr_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        const float body = FastSin(phase_) * be;

        return Shape((noise * snap_ + body * (1.f - snap_ * 0.6f)) * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune:  base_hz_ = 140.f + v * 260.f; break;
            case ParamId::Decay: Retime(); break;
            // Cutoff of the noise highpass: low is a fat snare, high is a crack.
            case ParamId::Tone:  hp_.SetFreq(600.f + v * 6000.f); break;
            case ParamId::Snap:  snap_ = 0.25f + v * 0.7f; break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        const float d = DecayTime(param(ParamId::Decay)) * 0.5f;
        rattle_.SetTime(daisysp::ADENV_SEG_DECAY, d);
        body_.SetTime(daisysp::ADENV_SEG_DECAY, d * 0.35f);
        drop_coeff_ = expf(-1.f / (d * 0.08f * sr_ + 1.f));
    }

    daisysp::WhiteNoise noise_;
    daisysp::AdEnv      crack_, rattle_, body_;
    daisysp::Svf        hp_;
    float sr_ = 48000.f, phase_ = 0.f, base_hz_ = 200.f;
    float drop_ = 0.f, drop_coeff_ = 0.99f;
    float snap_ = 0.6f, vel_ = 1.f;
};

/// 909-style kick. A fast pitch drop under a short tail, with a click on top —
/// where the 808 model is a long sine, this is a beater hitting a head.
///
/// The click is a separate few-millisecond noise burst through a highpass
/// rather than FM on the body: FM brightens the whole hit, while a 909 click is
/// a distinct transient that stops before the body has finished moving.
class BassDrum909 : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        noise_.Init();
        amp_.Init(sr);
        amp_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        amp_.SetCurve(kPercCurve);
        amp_.SetMax(1.f); amp_.SetMin(0.f);
        click_.Init(sr);
        click_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0002f);
        click_.SetTime(daisysp::ADENV_SEG_DECAY, 0.004f);
        click_.SetCurve(kPercCurve);
        click_.SetMax(1.f); click_.SetMin(0.f);
        hp_.Init(sr);
        hp_.SetFreq(1800.f);
        hp_.SetRes(0.2f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_    = velocity;
        phase_  = 0.f;
        drop_   = 1.f;
        active_ = true;
        amp_.Trigger();
        click_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float ae = amp_.Process();
        const float ce = click_.Process();
        if(!amp_.IsRunning() && ae <= 0.f)
        {
            active_ = false;
            return 0.f;
        }

        drop_ *= drop_coeff_;
        phase_ += (base_hz_ * (1.f + drop_ * depth_)) / sr_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;

        hp_.Process(noise_.Process());
        const float body = FastSin(phase_) * ae;
        return Shape((body + hp_.High() * ce * click_amt_) * vel_);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune:  base_hz_ = 40.f + v * 50.f; break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone:  click_amt_ = v * 0.8f; break;
            // Shallower and faster than the boom machine: a 909 drop is over
            // before you can hear it as a sweep.
            case ParamId::Snap:  depth_ = 1.f + v * 6.f; Retime(); break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        const float d = DecayTime(param(ParamId::Decay)) * 0.7f;
        amp_.SetTime(daisysp::ADENV_SEG_DECAY, d);
        drop_coeff_ = expf(-1.f / (0.012f * sr_));
    }

    daisysp::WhiteNoise noise_;
    daisysp::AdEnv      amp_, click_;
    daisysp::Svf        hp_;
    float sr_ = 48000.f, phase_ = 0.f, base_hz_ = 55.f;
    float drop_ = 0.f, drop_coeff_ = 0.99f, depth_ = 4.f;
    float click_amt_ = 0.4f, vel_ = 1.f;
};

/// 808-style snare: DaisySP's AnalogSnareDrum with an amplitude envelope
/// wrapped around it.
///
/// The wrapper is not decoration. That model gives its body resonators a Q of
/// 2000 x 2^(decay x 7), so even at DECAY 0 they ring for about a second, and
/// measured across the whole range the tail never fell below -40 dB inside five
/// seconds and was not monotonic. Its own DECAY is therefore unusable as a knob.
/// Holding it at a fixed value and gating the output with our own envelope
/// keeps the 808 character and makes DECAY mean what it says - which is exactly
/// the fix docs/02-firmware.md 5 records but never applied.
class SnareDrum808 : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        d_.Init(sr);
        // Fixed, mid-range: this is now a timbre control, not a time one.
        d_.SetDecay(0.4f);
        amp_.Init(sr);
        amp_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0005f);
        amp_.SetCurve(kPercCurve);
        amp_.SetMax(1.f); amp_.SetMin(0.f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        d_.SetAccent(velocity);
        pending_ = true;
        active_  = true;
        amp_.Trigger();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float e = amp_.Process();
        const float s = d_.Process(pending_);
        pending_      = false;
        if(!amp_.IsRunning() && e <= 0.f)
        {
            active_ = false;
            return 0.f;
        }
        return Shape(s * e);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune:  d_.SetFreq(150.f + v * 300.f); break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone:  d_.SetTone(v); break;
            case ParamId::Snap:  d_.SetSnappy(v); break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        amp_.SetTime(daisysp::ADENV_SEG_DECAY,
                     DecayTime(param(ParamId::Decay)) * 0.6f);
    }

    daisysp::AnalogSnareDrum d_;
    daisysp::AdEnv           amp_;
    bool                     pending_ = false;
};

/// Glitch percussion. A burst of very short grains at unrelated pitches, run
/// through bit and rate reduction.
///
/// SNAP is the chaos control and it does something unusual: at zero the seed is
/// reset on every trigger, so a hit is **identical every time** and a pattern is
/// reproducible. Above zero the seed advances per hit, so no two are the same.
/// That is deliberate - a machine that never repeats is fun and impossible to
/// arrange with, so the knob has to reach both.
class GlitchPerc : public VoiceBase
{
  public:
    void Init(float sr) override
    {
        sr_ = sr;
        crush_.Init();
        grain_env_.Init(sr);
        grain_env_.SetTime(daisysp::ADENV_SEG_ATTACK, 0.0003f);
        grain_env_.SetCurve(kPercCurve);
        grain_env_.SetMax(1.f); grain_env_.SetMin(0.f);
        Retime();
    }

    void Trigger(float velocity) override
    {
        vel_ = velocity;
        // Reproducible at SNAP 0, different every hit above it.
        if(chaos_ <= 0.f)
            rng_ = 0x9E3779B9u;
        grains_left_ = 1 + static_cast<int>(chaos_ * 7.f);
        active_      = true;
        NextGrain();
    }

    float Process() override
    {
        if(!active_)
            return 0.f;
        const float e = grain_env_.Process();
        if(!grain_env_.IsRunning() && e <= 0.f)
        {
            if(--grains_left_ <= 0)
            {
                active_ = false;
                return 0.f;
            }
            NextGrain();
        }

        phase_ += hz_ / sr_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        // Square rather than sine: the crusher has something to bite on, and a
        // glitch is meant to be edgy rather than round.
        const float raw = (phase_ < duty_ ? 1.f : -1.f) * e;
        return Shape(crush_.Process(raw) * vel_ * 0.5f);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune:  base_hz_ = 80.f + v * 1600.f; break;
            case ParamId::Decay: Retime(); break;
            case ParamId::Tone:
                crush_.SetBitcrushFactor(0.15f + v * 0.85f);
                crush_.SetDownsampleFactor(v * 0.6f);
                break;
            case ParamId::Snap:  chaos_ = v; break;
            default: break;
        }
    }

  private:
    void Retime()
    {
        grain_s_ = 0.004f + DecayTime(param(ParamId::Decay)) * 0.12f;
        grain_env_.SetTime(daisysp::ADENV_SEG_DECAY, grain_s_);
    }

    uint32_t Rand()
    {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return rng_;
    }

    void NextGrain()
    {
        // Ratios deliberately not harmonic: harmonic grains fuse into one tone,
        // and the point is that they do not.
        static constexpr float kRatio[8]
            = {1.f, 1.47f, 2.09f, 0.63f, 3.17f, 4.41f, 0.41f, 6.73f};
        const uint32_t r = Rand();
        hz_    = base_hz_ * kRatio[r & 7u];
        duty_  = 0.15f + ((r >> 8) & 0xFFu) / 255.f * 0.7f;
        phase_ = 0.f;
        grain_env_.Trigger();
    }

    daisysp::Decimator crush_;
    daisysp::AdEnv     grain_env_;
    uint32_t rng_ = 0x9E3779B9u;
    float sr_ = 48000.f, phase_ = 0.f, hz_ = 440.f, duty_ = 0.5f;
    float base_hz_ = 400.f, grain_s_ = 0.02f, chaos_ = 0.5f, vel_ = 1.f;
    int   grains_left_ = 0;
};

} // namespace drom
