#pragma once
#include <cstdint>
#include "pattern.h"

namespace drom {

/// MIDI clock recovery.
///
/// Slaving naively — advancing a step on every Timing Clock byte — imports the
/// source's jitter straight into the groove, and the jitter is real: USB MIDI
/// is quantised to 1 ms USB frames, so a USB clock source can never be tighter
/// than that. Against a sequencer built for sample-accurate triggers, that
/// throws the precision away.
///
/// So clock bytes are timestamped against the audio sample counter (in the
/// UART interrupt on hardware — timestamping in a 1 kHz main loop has already
/// cost ±1 ms) and fed to this PI loop. The sequencer runs from the smoothed
/// estimate, never from raw edges.
///
/// Phase is established by Start/Continue/Song Position, not by this loop. Its
/// job is to keep the *rate* right so phase does not then drift — a phase error
/// that persists is a rate error, which the integral term removes.
class ClockPll
{
  public:
    /// MIDI clock is 24 PPQN; the internal clock is 96. Four internal ticks
    /// per clock byte, which is why 96 was chosen.
    static constexpr int kMidiPpqn        = 24;
    static constexpr int kTicksPerMidiClk = kPpqn / kMidiPpqn;

    /// How hard to chase the incoming clock. This is a musical choice, not a
    /// technical one, so it is exposed to the user rather than tuned once.
    enum class Tightness : uint8_t
    {
        /// Short time constant. Follows tempo automation and live tempo
        /// changes closely, at the cost of passing more jitter through.
        Tight = 0,
        /// Long time constant, high inertia. Rejects USB frame jitter and
        /// sloppy senders. The right default for steady-tempo DAW sync.
        Smooth,
    };

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        SetInternalTempo(120.f);
        SetTightness(Tightness::Smooth);
        Reset();
    }

    void Reset()
    {
        have_last_      = false;
        clocks_         = 0;
        last_t_         = 0.0;
        since_clock_    = 0;
        external_       = false;
        phase_error_    = 0.0;
        spc_            = internal_spc_;
        next_expected_  = 0.0;
    }

    void SetTightness(Tightness t)
    {
        tightness_ = t;
        if(t == Tightness::Tight)
        {
            kp_ = 0.30;
            ki_ = 0.020;
        }
        else
        {
            // Deliberately sluggish: at these gains a ±1 ms USB jitter moves
            // the tempo estimate by well under a BPM.
            kp_ = 0.06;
            ki_ = 0.0015;
        }
    }

    Tightness tightness() const { return tightness_; }

    /// Tempo used when no external clock is present.
    void SetInternalTempo(float bpm)
    {
        if(bpm < 20.f) bpm = 20.f;
        if(bpm > 999.f) bpm = 999.f;
        internal_bpm_ = bpm;
        internal_spc_ = SamplesPerClock(bpm);
        if(!external_)
            spc_ = internal_spc_;
    }

    /// Nudges the recovered phase to compensate for a DAW's own output
    /// latency. Set once per rig; it does not change the recovered tempo.
    void SetLatencyOffsetMs(float ms) { latency_samples_ = ms * sample_rate_ / 1000.f; }

    /// Timestamp of an incoming Timing Clock byte, in absolute audio samples.
    void OnClock(uint64_t sample_time)
    {
        const double t = static_cast<double>(sample_time) - latency_samples_;
        ++clocks_;
        since_clock_ = 0;

        if(!have_last_)
        {
            have_last_     = true;
            last_t_        = t;
            next_expected_ = t + spc_;
            return;
        }

        const double interval = t - last_t_;
        last_t_               = t;

        // Acquisition. The loop gains are sized to reject jitter once locked,
        // which makes them far too sluggish to *find* an unknown tempo: from a
        // 120 BPM start, Smooth gains take thousands of clocks to reach 174.
        // So seed the period straight from the measured interval first, then
        // hand over to the loop. Costs a sixth of a beat.
        if(clocks_ <= kAcquireClocks)
        {
            if(interval > 40.0 && interval < 200000.0)
                spc_ = (clocks_ == 2) ? interval : (spc_ + interval) * 0.5;
            next_expected_ = t + spc_;
            external_      = true;
            return;
        }

        const double err = t - next_expected_;

        // A dropped or doubled byte must not be filtered as if it were tempo
        // information — one glitch would drag the estimate for seconds. Treat
        // anything wildly off as a resync instead.
        if(err > spc_ * 0.5 || err < -spc_ * 0.5)
        {
            next_expected_ = t + spc_;
            phase_error_   = 0.0;
            resyncs_       = resyncs_ + 1;
            return;
        }

        phase_error_ = err;

        // Integral term carries the tempo estimate; proportional term pulls
        // the schedule toward the observed edge without chasing it fully.
        spc_ += ki_ * err;
        if(spc_ < 8.0)
            spc_ = 8.0;
        next_expected_ += spc_ + kp_ * err;

        external_ = true;
    }

    /// Called once per audio block. Drives the fallback to internal timing.
    void Advance(uint32_t samples)
    {
        if(since_clock_ > kTimeoutSamplesCap)
            return;
        since_clock_ += samples;

        // Half a second of silence: the master stopped or the cable went. Keep
        // playing at the last known tempo rather than stalling, but say so —
        // silently switching sync source is miserable to debug on stage.
        if(external_ && since_clock_ > static_cast<uint32_t>(sample_rate_ * 0.5f))
        {
            external_  = false;
            have_last_ = false;
        }
    }

    /// True while an external clock is driving the tempo.
    bool external() const { return external_; }

    /// True once enough clocks have arrived for the estimate to be meaningful.
    bool locked() const { return external_ && clocks_ >= 8; }

    float bpm() const
    {
        return static_cast<float>(sample_rate_ * 60.0
                                  / (spc_ * kMidiPpqn));
    }

    /// Samples per internal 96 PPQN tick, Q16 — what the sequencer consumes.
    uint32_t samples_per_tick_q16() const
    {
        return static_cast<uint32_t>(spc_ / kTicksPerMidiClk * 65536.0 + 0.5);
    }

    /// Residual phase error in samples at the last clock; positive means the
    /// incoming clock arrived later than predicted. Useful on the display.
    float phase_error_samples() const { return static_cast<float>(phase_error_); }

    uint32_t clocks_received() const { return clocks_; }
    uint32_t resyncs() const { return resyncs_; }

  private:
    static constexpr uint32_t kTimeoutSamplesCap = 1u << 30;
    /// Clocks spent seeding the period before the loop takes over.
    static constexpr uint32_t kAcquireClocks      = 4;

    double SamplesPerClock(float bpm) const
    {
        return sample_rate_ * 60.0 / (static_cast<double>(bpm) * kMidiPpqn);
    }

    double    sample_rate_     = 48000.0;
    double    spc_             = 1000.0;
    double    internal_spc_    = 1000.0;
    double    next_expected_   = 0.0;
    double    last_t_          = 0.0;
    double    phase_error_     = 0.0;
    double    kp_              = 0.06;
    double    ki_              = 0.0015;
    float     internal_bpm_    = 120.f;
    float     latency_samples_ = 0.f;
    uint32_t  clocks_          = 0;
    uint32_t  since_clock_     = 0;
    uint32_t  resyncs_         = 0;
    bool      have_last_       = false;
    bool      external_        = false;
    Tightness tightness_       = Tightness::Smooth;
};

} // namespace drom
