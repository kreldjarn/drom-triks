#pragma once
#include <cmath>
#include "daisysp.h"

// A copy of DaisySP's Svf (Filters/svf.cpp), plus one fast path.
//
// The arithmetic is the original's, statement for statement, so a filter set
// up through SetFreq/SetRes produces the same samples. What differs is who
// calls it: the forked drums in drum_models.h call SetFreq/SetRes when a
// parameter changes, where DaisySP's drums call them on every sample — each
// call is a sinf and a powf, which made them the largest cost on the H750.
// See docs/02-firmware.md §10.
//
// ---------------------------------------------------------------------------
// DaisySP, copyright (c) 2020 Electrosmith, Corp. Published under the MIT
// license:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ---------------------------------------------------------------------------

namespace drom {
namespace dsp {

class Svf
{
  public:
    void Init(float sample_rate)
    {
        sr_        = sample_rate;
        fc_        = 200.0f;
        res_       = 0.5f;
        drive_     = 0.5f;
        pre_drive_ = 0.5f;
        freq_      = 0.25f;
        damp_      = 0.0f;
        notch_ = low_ = high_ = band_ = 0.0f;
        input_     = 0.0f;
        out_notch_ = out_low_ = out_high_ = out_peak_ = out_band_ = 0.0f;
        fc_max_    = sr_ / 3.f;
    }

    void SetFreq(float f)
    {
        fc_   = daisysp::fclamp(f, 1.0e-6f, fc_max_);
        // fs*2 because double sampled
        freq_ = 2.0f * ::sinf(PI_F * Min(0.25f, fc_ / (sr_ * 2.0f)));
        damp_ = Min(2.0f * (1.0f - ::powf(res_, 0.25f)),
                    Min(2.0f, 2.0f / freq_ - freq_ * 0.5f));
    }

    void SetRes(float r)
    {
        res_   = daisysp::fclamp(r, 0.f, 1.f);
        damp_  = Min(2.0f * (1.0f - ::powf(res_, 0.25f)),
                    Min(2.0f, 2.0f / freq_ - freq_ * 0.5f));
        drive_ = pre_drive_ * res_;
    }

    void SetDrive(float d)
    {
        pre_drive_ = daisysp::fclamp(d * 0.1f, 0.f, 1.f);
        drive_     = pre_drive_ * res_;
    }

    /// SetFreq then SetRes, for a cutoff that moves every sample — the one
    /// case hoisting cannot remove (AnalogBassDrum's FM'd resonator).
    ///
    /// **Not bit-exact**, deliberately. sinf becomes a polynomial, exact to
    /// float precision on the [0, pi/4] this argument is clamped to (the next
    /// Taylor term is ~2e-9). powf(r, 0.25) becomes sqrtf(sqrtf(r)), which is
    /// the same value to within an ulp and is two hardware instructions on the
    /// M7. And damp is computed once rather than twice, since SetFreq's damp
    /// is immediately overwritten by SetRes's.
    void SetFreqResFast(float f, float r)
    {
        fc_          = daisysp::fclamp(f, 1.0e-6f, fc_max_);
        const float x = PI_F * Min(0.25f, fc_ / (sr_ * 2.0f));
        freq_        = 2.0f * SinQuarter(x);
        res_         = daisysp::fclamp(r, 0.f, 1.f);
        damp_        = Min(2.0f * (1.0f - ::sqrtf(::sqrtf(res_))),
                    Min(2.0f, 2.0f / freq_ - freq_ * 0.5f));
        drive_       = pre_drive_ * res_;
    }

    void Process(float in)
    {
        input_ = in;
        // first pass
        notch_ = input_ - damp_ * band_;
        low_   = low_ + freq_ * band_;
        high_  = notch_ - low_;
        band_  = freq_ * high_ + band_ - drive_ * band_ * band_ * band_;
        // take first sample of output
        out_low_   = 0.5f * low_;
        out_high_  = 0.5f * high_;
        out_band_  = 0.5f * band_;
        out_peak_  = 0.5f * (low_ - high_);
        out_notch_ = 0.5f * notch_;
        // second pass
        notch_ = input_ - damp_ * band_;
        low_   = low_ + freq_ * band_;
        high_  = notch_ - low_;
        band_  = freq_ * high_ + band_ - drive_ * band_ * band_ * band_;
        // average second pass outputs
        out_low_ += 0.5f * low_;
        out_high_ += 0.5f * high_;
        out_band_ += 0.5f * band_;
        out_peak_ += 0.5f * (low_ - high_);
        out_notch_ += 0.5f * notch_;
    }

    float Low() const { return out_low_; }
    float High() const { return out_high_; }
    float Band() const { return out_band_; }
    float Notch() const { return out_notch_; }
    float Peak() const { return out_peak_; }

  private:
    static float Min(float x, float y) { return x < y ? x : y; }

    /// sin(x) for x in [0, pi/4], Taylor to x^9 in Horner form.
    static float SinQuarter(float x)
    {
        const float x2 = x * x;
        return x
               * (1.f
                  + x2
                        * (-1.f / 6.f
                           + x2
                                 * (1.f / 120.f
                                    + x2 * (-1.f / 5040.f + x2 * (1.f / 362880.f)))));
    }

    float sr_, fc_, res_, drive_, freq_, damp_;
    float notch_, low_, high_, band_;
    float input_;
    float out_low_, out_high_, out_band_, out_peak_, out_notch_;
    float pre_drive_, fc_max_;
};

} // namespace dsp
} // namespace drom
