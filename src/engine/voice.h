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

/// A per-step parameter override. Value is 0..65535 mapping to the same 0..1
/// range SetParam takes — 16 bits so a lock is indistinguishable from a knob
/// position, and three bytes so a Step stays cache-friendly.
struct ParamLock
{
    uint8_t  param_id = 0;
    uint16_t value    = 0;

    float as_float() const { return value / 65535.f; }
};

inline constexpr int kMaxLocks = 4;

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
        for(int i = 0; i < static_cast<int>(ParamId::Count); ++i)
            base_[i] = 0.5f;
    }

    /// The pattern-level value for a parameter — what the knob says. Locks
    /// override this for one step and then it is restored.
    void SetBase(ParamId id, float value)
    {
        base_[static_cast<int>(id)] = value;
        // Only push through if this parameter is not currently locked, or the
        // knob move would be overwritten by the restore and appear to do
        // nothing until the next unlocked step.
        if((locked_mask_ & (1u << static_cast<int>(id))) == 0)
            voice_->SetParam(id, value);
    }

    float base(ParamId id) const { return base_[static_cast<int>(id)]; }

    /// delay_samples may exceed the current block; it simply counts down.
    /// `locks` are applied at the moment the voice fires, not at schedule time.
    void Schedule(int32_t   delay_samples,
                  float     velocity,
                  const ParamLock *locks = nullptr,
                  uint8_t   lock_count   = 0)
    {
        delay_      = delay_samples;
        velocity_   = velocity;
        locks_      = locks;
        lock_count_ = lock_count;
    }

    float Process()
    {
        if(delay_ == 0)
        {
            ApplyLocks();
            voice_->Trigger(velocity_);
        }
        if(delay_ >= 0)
            --delay_;
        return voice_->Process();
    }

    IVoice *voice() { return voice_; }

  private:
    /// Restore whatever the previous step locked, then apply this step's locks.
    ///
    /// Restoring first is what stops a lock leaking into every later step —
    /// the bug that makes a p-lock feel like it permanently moved the knob.
    /// Only previously-locked parameters are touched, so an unlocked step
    /// costs nothing.
    void ApplyLocks()
    {
        if(locked_mask_)
        {
            for(int i = 0; i < static_cast<int>(ParamId::Count); ++i)
                if(locked_mask_ & (1u << i))
                    voice_->SetParam(static_cast<ParamId>(i), base_[i]);
            locked_mask_ = 0;
        }

        for(uint8_t i = 0; i < lock_count_ && locks_; ++i)
        {
            const int id = locks_[i].param_id;
            if(id >= static_cast<int>(ParamId::Count))
                continue;
            voice_->SetParam(static_cast<ParamId>(id), locks_[i].as_float());
            locked_mask_ |= (1u << id);
        }
    }

    IVoice          *voice_    = nullptr;
    int32_t          delay_    = -1;
    float            velocity_ = 0.f;
    const ParamLock *locks_      = nullptr;
    uint8_t          lock_count_ = 0;
    uint8_t          locked_mask_ = 0;
    float            base_[static_cast<int>(ParamId::Count)] = {};
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
