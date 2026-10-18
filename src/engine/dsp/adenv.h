#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include "daisysp.h"

// DaisySP's AdEnv (Control/adenv.cpp), with its per-sample constants cached
// and its curve computed by recursion.
//
// The original recomputes, on every sample, values that only change when the
// segment, its time or the curve does: a float->int conversion of the segment
// length, an exp of the curve, and two divides. It also does all of that while
// idle. Most of our voices run two or three of these, which made
// AdEnv::Process 10 % of the worst-case CPU on the H750. Those are computed
// once per change here, and idle returns straight away.
//
// **The curve is a true exponential, and so not bit-identical to DaisySP.**
// The original evaluates 1 - expf_fast(x) with x stepping linearly, where
// expf_fast(x) = (1 + x/1024)^1024: ten multiplies a sample, and only an
// approximation of exp. With a real exponential, exp(x + step) =
// exp(x) * exp(step), so the curve advances by one multiply. The two differ by
// expf_fast's own error — under 1 % of full scale at the curve the voices use,
// and the segment ends on the same sample; host/dsptest.cpp holds it to that.
//
// Same API, and the same daisysp::AdEnvSegment values, so it is a drop-in.
//
// ---------------------------------------------------------------------------
// DaisySP, copyright (c) 2020 Electrosmith, Corp. MIT license — the full
// notice is in dsp/svf.h.
// ---------------------------------------------------------------------------

namespace drom {
namespace dsp {

class AdEnv
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_     = sample_rate;
        current_segment_ = daisysp::ADENV_SEG_IDLE;
        prev_segment_    = daisysp::ADENV_SEG_IDLE;
        curve_scalar_    = 0.0f; // full linear
        min_             = 0.0f;
        max_             = 1.0f;
        output_          = 0.0001f;
        e_               = 1.0;
        k_               = 1.0;
        retrig_val_      = 0.0f;
        trigger_         = false;
        for(auto &t : segment_time_)
            t = 0.05f;
        dirty_ = true;
    }

    void Trigger() { trigger_ = true; }

    void SetTime(uint8_t seg, float time)
    {
        segment_time_[seg] = time;
        dirty_             = true;
    }

    void SetCurve(float scalar)
    {
        curve_scalar_ = scalar;
        dirty_        = true;
    }

    void SetMin(float min) { min_ = min; }
    void SetMax(float max) { max_ = max; }

    float   GetValue() const { return (output_ * (max_ - min_)) + min_; }
    uint8_t GetCurrentSegment() const { return current_segment_; }
    bool IsRunning() const { return current_segment_ != daisysp::ADENV_SEG_IDLE; }

    // Forced inline: the voices call this two or three times a sample inside
    // their block loops, and as a call its state round-trips through memory.
    __attribute__((always_inline)) float Process()
    {
        // Idle, and was idle last sample: the original computes a whole
        // increment here and then discards it (its idle branch forces the
        // output to zero), so this returns the same value without the work.
        if(!trigger_ && current_segment_ == daisysp::ADENV_SEG_IDLE
           && prev_segment_ == daisysp::ADENV_SEG_IDLE)
        {
            output_ = 0.0f;
            return min_;
        }

        if(trigger_)
        {
            trigger_         = false;
            current_segment_ = daisysp::ADENV_SEG_ATTACK;
            e_               = 1.0; // exp(0): the curve restarts
            retrig_val_      = output_;
            dirty_           = true; // beg depends on retrig_val_
        }

        if(prev_segment_ != current_segment_)
        {
            // Reset at segment beginning
            e_ = 1.0;
            dirty_   = true;
        }
        if(dirty_)
            Recompute();

        float       val = output_;
        const float out = val;
        if(curve_scalar_ == 0.0f)
        {
            val += c_inc_;
        }
        else
        {
            // exp(x) for x stepping linearly, by recursion: e_ is exp(x) and
            // k_ is exp(step). The original's x advances before it is used,
            // so the first sample of a segment sees exp(step), as here.
            e_ *= k_;
            val = beg_ + c_inc_ * (1.0f - static_cast<float>(e_));
            if(val != val)
                val = 0.0f; // NaN check
        }

        // Update Segment
        prev_segment_ = current_segment_;
        if(current_segment_ != daisysp::ADENV_SEG_IDLE)
        {
            if((out >= 1.f && current_segment_ == daisysp::ADENV_SEG_ATTACK)
               || (out <= 0.f && current_segment_ == daisysp::ADENV_SEG_DECAY))
            {
                // Advance segment. The next call sees prev != current and
                // recomputes.
                current_segment_++;
                if(current_segment_ > daisysp::ADENV_SEG_DECAY)
                    current_segment_ = daisysp::ADENV_SEG_IDLE;
            }
        }
        float ret = out;
        if(current_segment_ == daisysp::ADENV_SEG_IDLE)
            val = ret = 0.0f;
        output_ = val;

        return ret * (max_ - min_) + min_;
    }

  private:
    /// Everything the original derives per sample from the segment, its time
    /// and the curve — the same expressions, so the same floats.
    void Recompute()
    {
        dirty_ = false;
        const uint32_t time_samps
            = static_cast<uint32_t>(segment_time_[current_segment_] * sample_rate_);

        float end;
        switch(current_segment_)
        {
            case daisysp::ADENV_SEG_ATTACK:
                beg_ = retrig_val_;
                end  = 1.0f;
                break;
            case daisysp::ADENV_SEG_DECAY:
                beg_ = 1.0f;
                end  = 0.0f;
                break;
            default:
                beg_ = 0;
                end  = 0;
                break;
        }

        if(curve_scalar_ == 0.0f)
            c_inc_ = (end - beg_) / time_samps;
        else
            c_inc_ = (end - beg_) / (1.0f - std::exp(curve_scalar_));

        if(c_inc_ >= 0.0f)
            c_inc_ = std::max(c_inc_, std::numeric_limits<float>::epsilon());
        else
            c_inc_ = std::min(c_inc_, -std::numeric_limits<float>::epsilon());

        // A zero-length segment gives exp(-inf) = 0: the curve lands on its
        // end value in one sample, rather than DaisySP's inf.
        k_ = std::exp(static_cast<double>(curve_scalar_) / time_samps);
    }

    uint8_t current_segment_, prev_segment_;
    float   segment_time_[daisysp::ADENV_SEG_LAST];
    float   sample_rate_, min_, max_, output_, curve_scalar_;
    /// exp(x) and exp(step), in double. A segment can run tens of thousands
    /// of samples, and in float the recursion's rounding accumulates enough to
    /// move the zero crossing — and with it the end of the sound — by several
    /// samples. The M7's FPU does double in hardware; one multiply is still
    /// far cheaper than the original's ten.
    double  e_, k_;
    float   retrig_val_;
    bool    trigger_;

    // Cached per segment/setting; see Recompute().
    bool  dirty_;
    float beg_, c_inc_;
};

} // namespace dsp
} // namespace drom
