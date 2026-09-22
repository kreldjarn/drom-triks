#pragma once
#include "daisysp.h"
#include "../voice.h"

// The four voices DaisySP covers directly. These are mostly parameter mapping:
// the macro knobs are normalised 0..1 and each voice maps them onto its own
// ranges, chosen so the same knob does a musically similar thing everywhere.

namespace drom {

/// 808-style bass drum. SNAP drives the FM amount, which is what gives the
/// click/punch at the attack rather than a louder body.
class BassDrum : public VoiceBase
{
  public:
    void Init(float sr) override { d_.Init(sr); }

    void Trigger(float velocity) override
    {
        d_.SetAccent(velocity);
        pending_ = true;
    }

    float Process() override
    {
        const float s = d_.Process(pending_);
        pending_      = false;
        return Shape(s);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: d_.SetFreq(30.f + v * 70.f); break;
            case ParamId::Decay: d_.SetDecay(v); break;
            case ParamId::Tone: d_.SetTone(v); break;
            case ParamId::Snap:
                d_.SetAttackFmAmount(v);
                d_.SetSelfFmAmount(v * 0.8f);
                break;
            default: break;
        }
    }

  private:
    daisysp::AnalogBassDrum d_;
    bool                    pending_ = false;
};

/// 808-style snare. SNAP is the noise/body balance, the defining snare control.
class SnareDrum : public VoiceBase
{
  public:
    void Init(float sr) override { d_.Init(sr); }

    void Trigger(float velocity) override
    {
        d_.SetAccent(velocity);
        pending_ = true;
    }

    float Process() override
    {
        const float s = d_.Process(pending_);
        pending_      = false;
        return Shape(s);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: d_.SetFreq(120.f + v * 280.f); break;
            case ParamId::Decay: d_.SetDecay(v); break;
            case ParamId::Tone: d_.SetTone(v); break;
            case ParamId::Snap: d_.SetSnappy(v); break;
            default: break;
        }
    }

  private:
    daisysp::AnalogSnareDrum d_;
    bool                     pending_ = false;
};

/// Hi-hat, templated on the noise source so closed and open share one body of
/// code. CH uses the cheaper square-noise/linear-VCA pair; OH uses ring-mod
/// noise and the cymbal VCA, which rings longer and sounds less synthetic.
template <typename Noise, typename VCA>
class HiHatVoice : public VoiceBase
{
  public:
    void Init(float sr) override { d_.Init(sr); }

    void Trigger(float velocity) override
    {
        d_.SetAccent(velocity);
        pending_ = true;
    }

    float Process() override
    {
        const float s = d_.Process(pending_);
        pending_      = false;
        return Shape(s);
    }

  protected:
    void OnParam(ParamId id, float v) override
    {
        switch(id)
        {
            case ParamId::Tune: d_.SetFreq(3000.f + v * 9000.f); break;
            case ParamId::Decay: d_.SetDecay(v); break;
            case ParamId::Tone: d_.SetTone(v); break;
            case ParamId::Snap: d_.SetNoisiness(v); break;
            default: break;
        }
    }

  private:
    daisysp::HiHat<Noise, VCA> d_;
    bool                       pending_ = false;
};

using ClosedHat = HiHatVoice<daisysp::SquareNoise, daisysp::LinearVCA>;
using OpenHat   = HiHatVoice<daisysp::RingModNoise, daisysp::SwingVCA>;

} // namespace drom
