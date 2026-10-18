#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// The master reverb: a Dattorro plate.
//
// Replaces DaisySP's ReverbSc, for three reasons recorded in
// docs/02-firmware.md §10. It was the largest single cost on the H750 once the
// voices were fixed (~1 200 cycles a sample); it is LGPL, which carries a
// relinking obligation on every unit shipped; and its 395 kB held by value
// forced SDRAM, which a board without it could not provide. This needs ~165 kB
// of caller-provided memory at 48 kHz — small enough for AXI SRAM — and no
// licence beyond our own.
//
// The topology is the plate from Jon Dattorro, "Effect Design, Part 1: Reverb
// and Other Filters", JAES 45(9), 1997: a band-limited, pre-delayed mono input
// through four diffusing allpasses into a figure-of-eight tank of two
// cross-fed halves. Each half is a slowly modulated allpass, a delay, a
// damping lowpass, a second allpass and a second delay. Stereo comes from
// seven output taps per side spread across the whole tank. The modulation is
// what keeps a plate from ringing metallically on drums, where an
// unmodulated comb design (Freeverb) audibly does.
//
// All delay lengths are Dattorro's, specified at his 29 761 Hz and scaled to
// the running rate, so the room is the same size at any sample rate.

namespace drom {

class PlateReverb
{
  public:
    /// Longest pre-delay, and so the size of its line.
    static constexpr float kMaxPreDelaySeconds = 0.1f;

    /// Floats of memory Init needs at `sample_rate`. ~41 000 at 48 kHz.
    /// constexpr, so a board can size a static array with it.
    static constexpr size_t BufferFloats(float sample_rate)
    {
        size_t n = PreDelayLength(sample_rate);
        for(int i = 0; i < kNumLines; ++i)
            n += LineLength(i, sample_rate);
        return n;
    }

    /// `buffer` must hold BufferFloats(sample_rate) floats and outlive this.
    /// Returns false — and leaves the reverb silent — if it is too small, so a
    /// board that under-provisions it gets a dry mix rather than a fault.
    bool Init(float sample_rate, float *buffer, size_t floats)
    {
        sr_    = sample_rate;
        ready_ = buffer && floats >= BufferFloats(sample_rate);
        if(!ready_)
            return false;

        float *p = buffer;
        pre_.Init(p, PreDelayLength(sample_rate));
        p += pre_.len;
        for(int i = 0; i < kNumLines; ++i)
        {
            line_[i].Init(p, LineLength(i, sample_rate));
            p += line_[i].len;
        }

        const float s = Scale();
        for(int i = 0; i < 7; ++i)
        {
            tap_l_[i] = static_cast<uint32_t>(kTapL[i].at * s);
            tap_r_[i] = static_cast<uint32_t>(kTapR[i].at * s);
        }
        excursion_ = kExcursion * s;
        tank_ap_len_[0] = static_cast<float>(line_[kTankAp1L].len) - excursion_ - 2.f;
        tank_ap_len_[1] = static_cast<float>(line_[kTankAp1R].len) - excursion_ - 2.f;

        // Quadrature LFO: one half of the tank on sine, the other on cosine,
        // so the two never move together.
        const float w = 2.f * 3.14159265f * kLfoHz / sample_rate;
        lfo_rot_c_    = std::cos(w);
        lfo_rot_s_    = std::sin(w);
        lfo_x_        = 1.f;
        lfo_y_        = 0.f;

        bandwidth_lp_ = 0.f;
        damp_lp_[0] = damp_lp_[1] = 0.f;
        tank_out_[0] = tank_out_[1] = 0.f;

        SetDecayTime(1.5f);
        SetDamping(9000.f);
        SetPreDelay(0.f);
        return true;
    }

    /// RT60 in seconds: how long the tail takes to fall 60 dB.
    ///
    /// The decay gain is applied twice per half-tank, so four times per trip
    /// round the figure of eight. Spreading the 60 dB over those applications
    /// gives the gain per application directly, which is what makes SIZE a
    /// time rather than an unlabelled feedback amount.
    void SetDecayTime(float rt60)
    {
        if(rt60 < 0.05f)
            rt60 = 0.05f;
        const float loop_s = static_cast<float>(TankLoopSamples()) / sr_;
        decay_ = std::pow(0.001f, (loop_s / 4.f) / rt60);
        if(decay_ > 0.9999f)
            decay_ = 0.9999f; // an infinite tail is a stuck note, not a reverb
    }

    /// Cutoff of the lowpass inside the tank, in Hz: higher is brighter.
    void SetDamping(float hz)
    {
        if(hz < 20.f)
            hz = 20.f;
        // One-pole: y += (1 - d)(x - y), with d = exp(-2 pi fc / fs).
        damp_ = std::exp(-2.f * 3.14159265f * hz / sr_);
    }

    void SetPreDelay(float seconds)
    {
        if(seconds < 0.f)
            seconds = 0.f;
        if(seconds > kMaxPreDelaySeconds)
            seconds = kMaxPreDelaySeconds;
        pre_delay_ = static_cast<uint32_t>(seconds * sr_);
        if(pre_delay_ >= pre_.len)
            pre_delay_ = pre_.len - 1;
    }

    /// Longest path a sound can take before it next reaches an output tap,
    /// bounded generously by one full trip round the tank plus the input
    /// stage. MasterFx sleeps the reverb only after silence longer than this.
    uint32_t LongestPathSamples() const
    {
        return TankLoopSamples() + pre_.len + line_[kIn1].len + line_[kIn2].len
               + line_[kIn3].len + line_[kIn4].len;
    }

    void Process(float in_l, float in_r, float &out_l, float &out_r)
    {
        if(!ready_)
        {
            out_l = out_r = 0.f;
            return;
        }

        // Input: mono, pre-delayed, band-limited.
        pre_.Write(0.5f * (in_l + in_r));
        const float x = pre_.Read(pre_delay_);
        bandwidth_lp_ += kBandwidth * (x - bandwidth_lp_);

        // Four input diffusers.
        float d = bandwidth_lp_;
        d = line_[kIn1].Allpass(d, kInputDiffusion1);
        d = line_[kIn2].Allpass(d, kInputDiffusion1);
        d = line_[kIn3].Allpass(d, kInputDiffusion2);
        d = line_[kIn4].Allpass(d, kInputDiffusion2);

        // Advance the LFO: a rotating phasor, two multiplies a sample and no
        // trig. Renormalising every sample would cost a divide; the error of
        // a unit rotation grows by ~1e-7 a sample, so it is corrected rarely.
        const float nx = lfo_x_ * lfo_rot_c_ - lfo_y_ * lfo_rot_s_;
        lfo_y_         = lfo_x_ * lfo_rot_s_ + lfo_y_ * lfo_rot_c_;
        lfo_x_         = nx;
        if(++lfo_renorm_ >= 4096)
        {
            lfo_renorm_   = 0;
            const float m = 1.f / std::sqrt(lfo_x_ * lfo_x_ + lfo_y_ * lfo_y_);
            lfo_x_ *= m;
            lfo_y_ *= m;
        }

        // Each half of the tank is fed by the input and by the other half's
        // output from the previous sample — the figure of eight.
        const float left_in  = d + decay_ * tank_out_[1];
        const float right_in = d + decay_ * tank_out_[0];
        tank_out_[0] = TankHalf(0, left_in, tank_ap_len_[0] + excursion_ * lfo_x_);
        tank_out_[1] = TankHalf(1, right_in, tank_ap_len_[1] + excursion_ * lfo_y_);

        // Seven taps a side, signs and positions per Dattorro's Table 2.
        float l = 0.f, r = 0.f;
        for(int i = 0; i < 7; ++i)
        {
            l += kTapL[i].sign * line_[kTapL[i].line].Read(tap_l_[i]);
            r += kTapR[i].sign * line_[kTapR[i].line].Read(tap_r_[i]);
        }
        out_l = l * kOutputGain;
        out_r = r * kOutputGain;
    }

  private:
    // ---- Dattorro's constants, at his 29 761 Hz -----------------------------

    static constexpr float kDattorroRate     = 29761.f;
    static constexpr float kBandwidth        = 0.9995f;
    static constexpr float kInputDiffusion1  = 0.75f;
    static constexpr float kInputDiffusion2  = 0.625f;
    static constexpr float kDecayDiffusion1  = 0.70f;
    static constexpr float kDecayDiffusion2  = 0.50f;
    static constexpr float kExcursion        = 16.f; ///< samples, peak
    static constexpr float kLfoHz            = 1.0f;
    static constexpr float kOutputGain       = 0.6f;

    /// The lines, in the order they are carved out of the buffer.
    enum Line
    {
        kIn1, kIn2, kIn3, kIn4,          // input diffusers
        kTankAp1L, kTankDel1L, kTankAp2L, kTankDel2L,
        kTankAp1R, kTankDel1R, kTankAp2R, kTankDel2R,
        kNumLines
    };

    /// Lengths at 29 761 Hz. The modulated allpasses get room for their
    /// excursion on top, plus two samples for interpolation.
    static constexpr float kLength[kNumLines] = {
        142.f, 107.f, 379.f, 277.f,
        672.f + kExcursion + 2.f, 4453.f, 1800.f, 3720.f,
        908.f + kExcursion + 2.f, 4217.f, 2656.f, 3163.f,
    };

    struct Tap
    {
        int   line;
        float at;   ///< samples at 29 761 Hz
        float sign;
    };

    // Table 2 of the paper. Left reads mostly the right half of the tank and
    // vice versa, which is what decorrelates the two sides.
    static constexpr Tap kTapL[7] = {
        {kTankDel1R, 266.f, +1.f},  {kTankDel1R, 2974.f, +1.f},
        {kTankAp2R, 1913.f, -1.f},  {kTankDel2R, 1996.f, +1.f},
        {kTankDel1L, 1990.f, -1.f}, {kTankAp2L, 187.f, -1.f},
        {kTankDel2L, 1066.f, -1.f},
    };
    static constexpr Tap kTapR[7] = {
        {kTankDel1L, 353.f, +1.f},  {kTankDel1L, 3627.f, +1.f},
        {kTankAp2L, 1228.f, -1.f},  {kTankDel2L, 2673.f, +1.f},
        {kTankDel1R, 2111.f, -1.f}, {kTankAp2R, 335.f, -1.f},
        {kTankDel2R, 121.f, -1.f},
    };

    // ---- a ring-buffer delay line ------------------------------------------

    struct Delay
    {
        float   *buf = nullptr;
        uint32_t len = 0;
        uint32_t w   = 0; ///< next write position

        void Init(float *b, uint32_t n)
        {
            buf = b;
            len = n;
            w   = 0;
            for(uint32_t i = 0; i < n; ++i)
                buf[i] = 0.f;
        }

        void Write(float x)
        {
            buf[w] = x;
            if(++w >= len)
                w = 0;
        }

        /// A plain delay of exactly `len`: read the oldest sample, then
        /// overwrite it.
        float Tick(float x)
        {
            const float out = buf[w];
            Write(x);
            return out;
        }

        /// The sample written `d` writes ago; d = 0 is the most recent.
        float Read(uint32_t d) const
        {
            const uint32_t last = w == 0 ? len - 1 : w - 1;
            return buf[last >= d ? last - d : last + len - d];
        }

        /// Fractional read, linearly interpolated.
        float ReadFrac(float d) const
        {
            const uint32_t i = static_cast<uint32_t>(d);
            const float    f = d - static_cast<float>(i);
            const float    a = Read(i), b = Read(i + 1);
            return a + f * (b - a);
        }

        /// Schroeder allpass over the full line length: (g + z^-N)/(1 + g z^-N).
        float Allpass(float x, float g)
        {
            const float delayed = buf[w]; // oldest sample: exactly len back
            const float v       = x - g * delayed;
            Write(v);
            return delayed + g * v;
        }

        /// The same allpass, but with a fractional, moving length.
        float AllpassFrac(float x, float g, float d)
        {
            const float delayed = ReadFrac(d - 1.f);
            const float v       = x - g * delayed;
            Write(v);
            return delayed + g * v;
        }
    };

    float TankHalf(int side, float x, float mod_len)
    {
        const int base = side == 0 ? kTankAp1L : kTankAp1R;
        // Decay diffusion 1 is negated in the tank, per the paper.
        float y = line_[base].AllpassFrac(x, -kDecayDiffusion1, mod_len);
        y = line_[base + 1].Tick(y);
        damp_lp_[side] += (1.f - damp_) * (y - damp_lp_[side]);
        y = decay_ * damp_lp_[side];
        y = line_[base + 2].Allpass(y, kDecayDiffusion2);
        return line_[base + 3].Tick(y);
    }

    float Scale() const { return sr_ / kDattorroRate; }

    uint32_t TankLoopSamples() const
    {
        uint32_t n = 0;
        for(int i = kTankAp1L; i < kNumLines; ++i)
            n += line_[i].len;
        return n;
    }

    static constexpr uint32_t LineLength(int i, float sample_rate)
    {
        return static_cast<uint32_t>(kLength[i] * sample_rate / kDattorroRate) + 1;
    }

    static constexpr uint32_t PreDelayLength(float sample_rate)
    {
        return static_cast<uint32_t>(kMaxPreDelaySeconds * sample_rate) + 1;
    }

    float    sr_    = 48000.f;
    bool     ready_ = false;
    Delay    pre_;
    Delay    line_[kNumLines];
    uint32_t tap_l_[7] = {}, tap_r_[7] = {};
    uint32_t pre_delay_ = 0;
    float    tank_ap_len_[2] = {};
    float    excursion_      = 0.f;
    float    decay_ = 0.5f, damp_ = 0.f;
    float    bandwidth_lp_ = 0.f;
    float    damp_lp_[2]   = {};
    float    tank_out_[2]  = {};
    float    lfo_x_ = 1.f, lfo_y_ = 0.f, lfo_rot_c_ = 1.f, lfo_rot_s_ = 0.f;
    uint32_t lfo_renorm_ = 0;
};

} // namespace drom
