#pragma once
#include <cmath>
#include "daisysp.h"
#include "voice.h"

// A track's channel strip: saturator -> two filters in series -> pan -> bus.
//
// Needs DaisySP, which is why it is not in voice.h — the sequencer and p-lock
// tests link without the DSP library and must keep doing so. Still no libDaisy,
// so this builds on the host like the rest of src/engine/ (see CLAUDE.md).

namespace drom {

/// Filter response, quantised from the normalised MODE parameter.
///
/// The quantisation is deliberate rather than incidental: MODE arrives as a
/// float (Kit) or a uint16 over 0..1 (ParamLock), and a mode that drifts to its
/// neighbour through float comparison would be a miserable bug to find. See
/// docs/02-firmware.md §5.
enum class FilterMode : uint8_t
{
    LowPass = 0,
    BandPass,
    HighPass,
    Count
};

inline FilterMode ModeFromNorm(float v)
{
    int m = static_cast<int>(v * static_cast<float>(FilterMode::Count));
    if(m < 0) m = 0;
    if(m >= static_cast<int>(FilterMode::Count))
        m = static_cast<int>(FilterMode::Count) - 1;
    return static_cast<FilterMode>(m);
}

class ChannelStrip : public IChannel
{
  public:
    void Init(float sample_rate) override
    {
        sample_rate_ = sample_rate;
        for(int i = 0; i < 2; ++i)
        {
            f_[i].Init(sample_rate);
            // SetFreq before SetRes: both recompute `damp`, and SetRes reads
            // the frequency term while doing it.
            f_[i].SetFreq(kMaxHz);
            f_[i].SetRes(0.f);
            cutoff_[i] = 1.f;
            res_[i]    = 0.f;
            mode_[i]   = FilterMode::LowPass;
        }
        SetParam(ParamId::Pan, 0.5f);
        UpdateBypass();
    }

    void SetParam(ParamId id, float v) override
    {
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;
        switch(id)
        {
            case ParamId::SatDrive: sat_ = v; break;

            case ParamId::Filter1Cutoff: SetCutoff(0, v); break;
            case ParamId::Filter1Res:    SetRes(0, v);    break;
            case ParamId::Filter1Mode:   mode_[0] = ModeFromNorm(v); break;
            case ParamId::Filter2Cutoff: SetCutoff(1, v); break;
            case ParamId::Filter2Res:    SetRes(1, v);    break;
            case ParamId::Filter2Mode:   mode_[1] = ModeFromNorm(v); break;

            // Equal-power pan: constant perceived loudness across the sweep,
            // where a linear law dips ~3 dB in the middle.
            case ParamId::Pan:
            {
                const float a = v * 1.57079633f; // pi/2
                gain_l_ = std::cos(a);
                gain_r_ = std::sin(a);
                break;
            }

            case ParamId::DelaySend:  delay_send_  = v; break;
            case ParamId::ReverbSend: reverb_send_ = v; break;
            default: return; // not ours; IsChannelParam should have caught it
        }
        UpdateBypass();
    }

    void Process(float in, MixBus &bus) override
    {
        float x = Saturate(in);
        if(!bypass_[0]) x = RunFilter(0, x);
        if(!bypass_[1]) x = RunFilter(1, x);

        const float l = x * gain_l_;
        const float r = x * gain_r_;
        bus.l += l;
        bus.r += r;

        // Sends are post-pan, so a voice's placement carries into the delay.
        bus.delay_l  += l * delay_send_;
        bus.delay_r  += r * delay_send_;
        bus.reverb_l += l * reverb_send_;
        bus.reverb_r += r * reverb_send_;
    }

  private:
    /// 20 Hz to 18 kHz, exponential — a linear cutoff map puts everything
    /// musically useful in the first few degrees of travel, the same mistake
    /// docs/02-firmware.md §5 records for decay time.
    static constexpr float kMinHz = 20.f;
    static constexpr float kMaxHz = 18000.f;

    /// Svf sets damp = 2*(1 - res^0.25), so res = 1 means zero damping and the
    /// filter self-oscillates. Stop short of it.
    static constexpr float kMaxRes = 0.95f;

    void SetCutoff(int i, float v)
    {
        cutoff_[i] = v;
        f_[i].SetFreq(kMinHz * std::pow(kMaxHz / kMinHz, v));
    }

    void SetRes(int i, float v)
    {
        res_[i] = v;
        f_[i].SetRes(v * kMaxRes);
    }

    /// Unity at drive 0 and fixed points at +/-1, so turning SAT up adds grit
    /// without changing level — a saturator that also acts as a volume control
    /// is impossible to set by ear.
    float Saturate(float x) const
    {
        if(sat_ <= 0.f)
            return x;
        const float k = sat_ * 9.f;
        return x * (1.f + k) / (1.f + k * std::fabs(x));
    }

    float RunFilter(int i, float x)
    {
        f_[i].Process(x);
        switch(mode_[i])
        {
            case FilterMode::BandPass: return f_[i].Band();
            case FilterMode::HighPass: return f_[i].High();
            default:                   return f_[i].Low();
        }
    }

    /// A wide-open lowpass with no resonance is transparent, so skip it. Most
    /// tracks will never touch the filter page and this makes that free rather
    /// than merely cheap.
    void UpdateBypass()
    {
        for(int i = 0; i < 2; ++i)
            bypass_[i] = mode_[i] == FilterMode::LowPass && cutoff_[i] >= 0.999f
                         && res_[i] <= 0.001f;
    }

    daisysp::Svf f_[2];
    float        cutoff_[2] = {1.f, 1.f};
    float        res_[2]    = {0.f, 0.f};
    FilterMode   mode_[2]   = {FilterMode::LowPass, FilterMode::LowPass};
    bool         bypass_[2] = {true, true};

    float sat_          = 0.f;
    float gain_l_       = 0.70710678f;
    float gain_r_       = 0.70710678f;
    float delay_send_   = 0.f;
    float reverb_send_  = 0.f;
    float sample_rate_  = 48000.f;
};

} // namespace drom
