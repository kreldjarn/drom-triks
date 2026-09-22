#pragma once
#include <cstdint>

// The voice layer deliberately depends on DaisySP only — never on libDaisy.
// That is what lets this exact code build and run natively on a host machine
// (see host/), so voices and the sequencer can be developed and listened to
// without a board attached. Adding a libDaisy include here breaks that.

namespace drom {

/// The six macro parameters. Every voice implements all six, even where the
/// mapping is a stretch: a knob that does nothing on some tracks is worse than
/// one that does something mild. See docs/01-hardware.md §2.
enum class ParamId : uint8_t
{
    Tune = 0,
    Decay,
    Tone,
    Snap,
    Drive,
    Level,
    Count
};

/// The seam. Sample playback and analog voices implement this same interface,
/// so they become additions rather than rewrites of the sequencer, mixer and
/// UI. Defined before any concrete voice, deliberately.
class IVoice
{
  public:
    virtual ~IVoice() = default;

    virtual void Init(float sample_rate) = 0;

    /// velocity is 0..1.
    virtual void Trigger(float velocity) = 0;

    /// All parameters are normalised 0..1; the voice maps to its own ranges.
    virtual void SetParam(ParamId id, float value) = 0;

    virtual float Process() = 0;
};

/// Sample-accurate trigger scheduling.
///
/// The sequencer computes which sample *within* the current audio block a step
/// lands on and schedules it here, rather than firing at block boundaries.
/// Block-quantised triggers would put +/-0.67 ms of jitter on every hit at a
/// 32-sample block, which is audible smearing on hats and ruins flams.
class VoiceSlot
{
  public:
    void Init(IVoice *voice, float sample_rate)
    {
        voice_ = voice;
        voice_->Init(sample_rate);
        delay_ = -1;
    }

    /// delay_samples may exceed the current block; it simply counts down.
    void Schedule(int32_t delay_samples, float velocity)
    {
        delay_    = delay_samples;
        velocity_ = velocity;
    }

    float Process()
    {
        if(delay_ == 0)
            voice_->Trigger(velocity_);
        if(delay_ >= 0)
            --delay_;
        return voice_->Process();
    }

    IVoice *voice() { return voice_; }

  private:
    IVoice *voice_    = nullptr;
    int32_t delay_    = -1;
    float   velocity_ = 0.f;
};

/// Shared drive + level tail, since DaisySP's drum models have neither and all
/// eight voices need both.
class VoiceBase : public IVoice
{
  public:
    void SetParam(ParamId id, float value) override
    {
        if(value < 0.f) value = 0.f;
        if(value > 1.f) value = 1.f;
        params_[static_cast<int>(id)] = value;
        OnParam(id, value);
    }

  protected:
    virtual void OnParam(ParamId, float) {}

    /// Voices that use AdEnv must gate on this.
    ///
    /// A freshly Init'd AdEnv does not rest at zero: it starts at 0.0001 and,
    /// because the idle increment is clamped to +epsilon rather than 0, ramps
    /// to ~0.49 over the first ~8000 samples before settling. Left ungated
    /// that is an audible swell out of every voice at power-on. Gating also
    /// skips the DSP for idle voices, which is most of them most of the time.
    bool  active_ = false;

    float param(ParamId id) const { return params_[static_cast<int>(id)]; }

    /// Soft saturation, then output gain. Drive at 0 is unity and clean.
    float Shape(float x) const
    {
        const float drive = 1.f + param(ParamId::Drive) * 9.f;
        const float d     = x * drive;
        // Cheap tanh-ish soft clip: monotonic, no branching in the hot path.
        const float y = d / (1.f + (d < 0.f ? -d : d));
        return y * param(ParamId::Level);
    }

    float params_[static_cast<int>(ParamId::Count)] = {0.5f, 0.5f, 0.5f, 0.5f, 0.f, 0.8f};
};

} // namespace drom
