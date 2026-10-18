#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "params.h"
#include "reverb.h"
#include "voice.h"

// Master effects: one stereo delay, one reverb, one compressor.
//
// The delay and reverb are ours; both take caller-provided buffers, because
// where ~165 kB (reverb) and ~768 kB (delay) live is a board decision, not an
// engine one. The reverb is a Dattorro plate (reverb.h). It replaced DaisySP's
// ReverbSc, which was the largest cost on the H750, LGPL, and 395 kB held by
// value — see docs/02-firmware.md §10.
//
// The compressor is still DaisySP's, from **DaisySP-LGPL** — a separate
// library under LGPL-2.1, where DaisySP proper is MIT. Licensing bites only on
// distribution: shipping units means the licence text, an acknowledgement and,
// because this is a static link, our object files for relinking. It does not
// require opening our firmware. docs/11-production.md §3.

namespace drom {

// -----------------------------------------------------------------------------

/// A stereo delay over a caller-provided buffer of `2 * frames` floats.
///
/// Feedback is cross-fed by WIDTH: at 0 the two sides are independent, at 1 a
/// hit bounces left-right. That, plus the per-voice pan the sends are taken
/// after, is where "stereo spread per voice" actually comes from.
class StereoDelay
{
  public:
    void Init(float sample_rate, float *buffer, size_t frames)
    {
        sample_rate_ = sample_rate;
        buf_         = buffer;
        cap_         = frames;
        write_       = 0;
        lp_l_ = lp_r_ = 0.f;
        if(buf_)
            for(size_t i = 0; i < cap_ * 2; ++i)
                buf_[i] = 0.f;
        SetParam(FxId::DelayTime, kFxDefault[0]);
        SetParam(FxId::DelayFeedback, kFxDefault[1]);
        SetParam(FxId::DelayWidth, kFxDefault[2]);
        SetParam(FxId::DelayTone, kFxDefault[3]);
    }

    void SetParam(FxId id, float v)
    {
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;
        switch(id)
        {
            case FxId::DelayTime:
            {
                // 20 ms to the whole buffer, exponential.
                const float max_s = cap_ ? static_cast<float>(cap_) / sample_rate_ : 0.f;
                const float min_s = 0.020f;
                float       s     = max_s > min_s
                                        ? min_s * std::pow(max_s / min_s, v)
                                        : max_s;
                size_t      n     = static_cast<size_t>(s * sample_rate_);
                if(n < 1) n = 1;
                if(cap_ && n > cap_ - 1) n = cap_ - 1;
                time_ = n;
                break;
            }
            // Capped below 1 so the tail decays. Unity feedback in a delay is
            // not "infinite repeats", it is a slow-motion overflow.
            case FxId::DelayFeedback: fb_ = v * 0.95f; break;
            case FxId::DelayWidth:    width_ = v; break;
            // One-pole lowpass inside the loop: repeats get darker, which is
            // what stops a long feedback setting turning into noise.
            case FxId::DelayTone:     tone_ = 0.05f + v * 0.94f; break;
            default: break;
        }
    }

    void Process(float in_l, float in_r, float &out_l, float &out_r)
    {
        if(!buf_ || !cap_)
        {
            out_l = out_r = 0.f;
            return;
        }
        const size_t read = (write_ + cap_ - time_) % cap_;
        out_l = buf_[read * 2];
        out_r = buf_[read * 2 + 1];

        lp_l_ += tone_ * (out_l - lp_l_);
        lp_r_ += tone_ * (out_r - lp_r_);

        // Cross-feed: width 0 keeps the sides apart, width 1 swaps them.
        const float fb_l = lp_l_ * (1.f - width_) + lp_r_ * width_;
        const float fb_r = lp_r_ * (1.f - width_) + lp_l_ * width_;

        buf_[write_ * 2]     = in_l + fb_l * fb_;
        buf_[write_ * 2 + 1] = in_r + fb_r * fb_;
        write_ = (write_ + 1) % cap_;
    }

    size_t time_samples() const { return time_; }

  private:
    float *buf_ = nullptr;
    size_t cap_ = 0, write_ = 0, time_ = 1;
    float  sample_rate_ = 48000.f;
    float  fb_ = 0.f, width_ = 0.f, tone_ = 1.f;
    float  lp_l_ = 0.f, lp_r_ = 0.f;
};

// -----------------------------------------------------------------------------

/// The master bus: the two sends into their effects, summed back with the dry
/// signal, then compressed.
///
/// Both buffers are borrowed, not owned — see the note at the top of this file.
/// A null or undersized reverb buffer leaves the reverb silent.
class MasterFx
{
  public:
    void Init(float sample_rate,
              float             *delay_buf,
              size_t             delay_frames,
              float             *reverb_buf,
              size_t             reverb_floats)
    {
        delay_.Init(sample_rate, delay_buf, delay_frames);
        has_reverb_  = reverb_.Init(sample_rate, reverb_buf, reverb_floats);
        reverb_sleep_ = reverb_.LongestPathSamples();
        comp_.Init(sample_rate);
        comp_.AutoMakeup(false);
        for(int i = 0; i < kNumFxParams; ++i)
            SetParam(static_cast<FxId>(i), kFxDefault[i]);
    }

    void SetParam(FxId id, float v)
    {
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;
        params_[static_cast<int>(id)] = v;
        switch(id)
        {
            case FxId::DelayTime:
            case FxId::DelayFeedback:
            case FxId::DelayWidth:
            case FxId::DelayTone: delay_.SetParam(id, v); break;

            // A decay time, 0.3 s to 10 s, exponential: every step of the knob
            // is the same ratio longer, which is how decay is heard.
            case FxId::ReverbSize: reverb_.SetDecayTime(0.3f * std::pow(100.f / 3.f, v)); break;
            // More damping means a lower cutoff, so the knob reads as "darker".
            case FxId::ReverbDamp: reverb_.SetDamping(18000.f - v * 16500.f); break;
            case FxId::ReverbLevel: reverb_level_ = v; break;
            case FxId::ReverbPreDelay:
                reverb_.SetPreDelay(v * PlateReverb::kMaxPreDelaySeconds);
                break;

            // DaisySP takes real units, so the normalised knobs are mapped here
            // rather than in the caller. Times sweep exponentially: a linear map
            // crams every useful attack into the first few degrees of travel.
            case FxId::CompThreshold: comp_.SetThreshold(-60.f + v * 60.f); break;
            case FxId::CompRatio:     comp_.SetRatio(1.f + v * 19.f); break;
            case FxId::CompAttack:    comp_.SetAttack(0.001f * std::pow(100.f, v)); break;
            case FxId::CompRelease:   comp_.SetRelease(0.010f * std::pow(100.f, v)); break;
            case FxId::CompMakeup:    comp_.SetMakeup(v * 12.f); break;
            default: break;
        }
        comp_unity_ = params_[static_cast<int>(FxId::CompRatio)] <= 0.f
                      && params_[static_cast<int>(FxId::CompMakeup)] <= 0.f;
    }

    float param(FxId id) const { return params_[static_cast<int>(id)]; }

    /// Consumes the bus's sends, adds the wet signal to the dry, compresses.
    void Process(const MixBus &bus, float &out_l, float &out_r)
    {
        float dl = 0.f, dr = 0.f;
        delay_.Process(bus.delay_l, bus.delay_r, dl, dr);

        float rl = 0.f, rr = 0.f;
        if(has_reverb_)
        {
            // The delay feeds the reverb as well as the output, so a delayed
            // hit picks up the same space as the dry one.
            const float in_l = bus.reverb_l + dl, in_r = bus.reverb_r + dr;
            const bool  silent_in = in_l == 0.f && in_r == 0.f;

            // The sends default to zero, so the reverb usually has nothing to
            // do. It sleeps once its input is silent and its own output has
            // been below -120 dB for the longest path through it (~0.9 s),
            // so nothing audible can still be circulating — every tank line
            // reaches an output tap far sooner than that. It wakes on the
            // first non-zero input sample.
            //
            // Not skipped merely because REV LVL is 0: the delay lines would
            // freeze holding an old tail, and turning the level up would
            // replay it.
            if(silent_in && reverb_quiet_ >= reverb_sleep_)
            {
                rl = rr = 0.f;
            }
            else
            {
                reverb_.Process(in_l, in_r, rl, rr);
                const bool quiet_out
                    = std::fabs(rl) < kSilence && std::fabs(rr) < kSilence;
                reverb_quiet_ = silent_in && quiet_out ? reverb_quiet_ + 1 : 0;
            }
        }

        out_l = bus.l + dl + rl * reverb_level_;
        out_r = bus.r + dr + rr * reverb_level_;

        // At ratio 1:1 with no makeup the compressor's gain is exactly 1.0
        // once its gain-reduction state has decayed (it can only shrink at
        // 1:1, and pow10f(0) is exactly 1), so processing it changes nothing
        // but costs a log and an exp per call. Apply(1) reads that gain
        // exactly. The detector does not run while bypassed, so raising the
        // ratio later starts it from a stale envelope; it settles within the
        // attack time.
        if(comp_unity_ && comp_.Apply(1.f) == 1.f)
            return;

        // Linked stereo: one detector driving both sides. Independent detectors
        // pull the image toward whichever side is loud, which on a drum bus
        // reads as the mix lurching sideways on every kick.
        const float al = std::fabs(out_l), ar = std::fabs(out_r);
        const float key = al > ar ? al : ar;
        out_l = comp_.Process(out_l, key);
        out_r = comp_.Process(out_r, key);
    }

    daisysp::Compressor &compressor() { return comp_; }

  private:
    /// -120 dBFS: far below the 24-bit codec's own floor.
    static constexpr float kSilence = 1e-6f;
    StereoDelay          delay_;
    PlateReverb          reverb_;
    bool                 has_reverb_   = false;
    uint32_t             reverb_sleep_ = 0;
    daisysp::Compressor  comp_;
    float                reverb_level_ = 0.f;
    uint32_t             reverb_quiet_ = 0;
    bool                 comp_unity_   = false;
    float                params_[kNumFxParams] = {};
};

} // namespace drom
