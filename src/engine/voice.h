#pragma once
#include <cmath>
#include <cstdint>
#include "lfo.h"

// The voice layer deliberately depends on DaisySP only — never on libDaisy.
// That is what lets this exact code build and run natively on a host machine
// (see host/), so voices and the sequencer can be developed and listened to
// without a board attached. Adding a libDaisy include here breaks that.

namespace drom {

/// Eight macro encoders x four pages. Every voice implements every parameter
/// on a page it uses, even where the mapping is a stretch: a knob that does
/// nothing on some tracks is worse than one that does something mild. See
/// docs/01-hardware.md §2.
///
/// The whole 32-entry space is declared now even though most of it is not
/// wired to anything yet, and that is deliberate. `Kit` is
/// `float params[kNumTracks][ParamId::Count]`, so Count is baked into
/// `sizeof(Patch)` and therefore into the QSPI slot stride and every pattern
/// in flash. Growing the enum later is a save-format break; declaring the
/// reserved slots now costs 4 bytes per track per slot and breaks nothing.
/// See docs/02-firmware.md §6.
enum class ParamId : uint8_t
{
    // --- Page 1: INST -------------------------------------------------------
    Tune = 0,
    Decay,
    Tone,
    Snap,
    Drive,
    Level,
    Pan,
    Note,       ///< semitone offset on top of TUNE, quantised

    // --- Page 2: FLTR -------------------------------------------------------
    // Two filters in series with a saturator driving them.
    SatDrive,
    Filter1Cutoff,
    Filter1Res,
    Filter1Mode,
    Filter2Cutoff,
    Filter2Res,
    Filter2Mode,
    FltrRsv1,

    // --- Page 3: FX ---------------------------------------------------------
    // One shared delay and one shared reverb; a track's controls here are
    // sends and placement, not per-voice effect instances. Per-voice delay
    // lines would be ~384 kB each and do not fit — docs/02-firmware.md §5.
    DelaySend,
    ReverbSend,
    FxRsv1,
    FxRsv2,
    FxRsv3,
    FxRsv4,
    FxRsv5,
    FxRsv6,

    // --- Page 4: LFO --------------------------------------------------------
    // Config only. The LFO *phase* is runtime state and must never live in
    // Patch: Patch is memcpy-saved, so a stored phase would make every load
    // snap a free-running LFO to it. See docs/02-firmware.md §5.
    LfoSpeed,
    LfoMult,
    LfoFade,
    LfoDest,
    LfoWave,
    LfoMode,
    LfoDepth,
    LfoStartPhase,

    Count
};

inline constexpr int kParamsPerPage = 8;
inline constexpr int kNumPages      = 4;

static_assert(static_cast<int>(ParamId::Count) == kNumPages * kParamsPerPage,
              "the ParamId space must be exactly the pages the panel can reach");

/// NOTE spans four octaves, +/-24 semitones about centre, and is **quantised**.
///
/// A pitch that lands between semitones is not a pitch anyone asked for, and
/// the whole point is to be able to say F# rather than 0.42. Same reasoning as
/// the filter mode and the LFO wave: these arrive as floats or as a uint16 over
/// 0..1 and the rounding has to be deliberate.
///
/// It offsets TUNE rather than replacing it — TUNE stays the voice's own base,
/// which is what docs/06-midi.md §5 already specifies for MIDI note input.
inline constexpr int kNoteRange = 24;

inline int NoteSemitones(float v)
{
    const float s = (v - 0.5f) * 2.f * kNoteRange;
    return static_cast<int>(s < 0.f ? s - 0.5f : s + 0.5f);
}

/// Power-on value for each parameter, indexed by ParamId.
///
/// Lives here rather than in params.h because VoiceBase needs it and params.h
/// includes this header, not the other way round. params.h's kParamInfo takes
/// its defaults from this array so there is one source of truth.
///
/// Reserved slots default to 0.5f rather than 0.0f on purpose: an unimplemented
/// parameter that later becomes a filter cutoff would otherwise power on fully
/// closed, i.e. silent, and the cause would not be obvious.
inline constexpr float kParamDefault[static_cast<int>(ParamId::Count)] = {
    // INST: Tune Decay Tone  Snap  Drive Level Pan   Note
    //   Note centres at 0.5, which is no offset.
    0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.8f, 0.5f, 0.5f,
    // FLTR: Sat  F1cut F1res F1mod F2cut F2res F2mod rsv
    //   Both cutoffs default wide open. A lowpass at cutoff 0 is silence, and
    //   a track that powers on mute with no obvious cause is exactly the trap
    //   the note above is about.
    0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f,
    // FX:   Dly  Rev   rsv   rsv   rsv   rsv   rsv   rsv
    0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    // LFO:  Spd  Mult  Fade  Dest  Wave  Mode  Depth Phase
    0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};

/// Selectable synthesis machines. A track's sound is not fixed to its legend:
/// any machine can go on any track, the way a Machinedrum works, so the panel
/// silkscreen is a default rather than a constraint. Worth knowing before that
/// legend is cut in metal — see docs/01-hardware.md §5.
///
/// The enum and its names live here, with ParamId, because the Kit stores a
/// machine per track and patch.h must stay free of DaisySP. Construction lives
/// in machines.h, which does not.
enum class MachineId : uint8_t
{
    Silent = 0, ///< an unpopulated cartridge slot, or a deliberately dead track

    BdAnalog,   ///< 808: long sine, pitch envelope, minimal click
    Bd909,      ///< 909: fast drop, short tail, beater click on top
    BdBoom,     ///< deep sine, long tail, slow sweep

    SdSynth,    ///< 909-style, balanced noise and body
    Sd808,      ///< the 808 model, with the envelope that makes DECAY work
    SdPunch,    ///< transient-forward crack over a fast-dropping body

    Glitch,     ///< bursts of crushed grains at unrelated pitches

    HatClosed,
    HatOpen,

    Tom,
    Clap,
    RimShot,
    FmPerc,
    Triangle,   ///< struck metal bar, modal

    Count
};

struct MachineInfo
{
    const char *name;   ///< for the screen, kept short
    const char *family; ///< groups the list; purely cosmetic
};

inline constexpr MachineInfo kMachineInfo[static_cast<int>(MachineId::Count)] = {
    {"SILENT",   "--"},
    {"BD 808",   "BD"},  {"BD 909",  "BD"},  {"BD BOOM", "BD"},
    {"SD 909",   "SD"},  {"SD 808",  "SD"},  {"SD PUNCH", "SD"},
    {"GLITCH",   "PERC"},
    {"CH",       "HAT"}, {"OH",      "HAT"},
    {"TOM",      "PERC"},{"CLAP",    "PERC"},
    {"RIM",      "PERC"},{"FM",      "PERC"}, {"TRI", "PERC"},
};

inline const char *MachineName(MachineId id)
{
    const int i = static_cast<int>(id);
    return (i >= 0 && i < static_cast<int>(MachineId::Count)) ? kMachineInfo[i].name
                                                             : "?";
}


/// A per-step parameter override. Value is 0..65535 mapping to the same 0..1
/// range SetParam takes — 16 bits so a lock is indistinguishable from a knob
/// position, and three bytes so a Step stays cache-friendly.
struct ParamLock
{
    uint8_t  param_id = 0;
    uint16_t value    = 0;

    float as_float() const { return value / 65535.f; }
};

/// Locks per step. Eight because four pages of eight parameters makes four
/// slots a rationing exercise rather than an expressive limit.
///
/// This is baked into sizeof(Step) -> sizeof(Patch) -> the QSPI slot stride,
/// so changing it invalidates every pattern in flash (SaveHeader rejects them
/// rather than reinterpreting, which is correct but means they are gone).
/// Pick it while flash is empty. Budget at 8: Patch ~30 kB, 32 kB slots,
/// 2.81 MB of QSPI still free, half the DTCM. See docs/02-firmware.md §6.
inline constexpr int kMaxLocks = 8;

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

/// The per-sample stereo bus, plus the two effect sends.
///
/// Sends are taken *after* pan, so a voice's placement carries into the delay —
/// which is what "stereo spread per voice" means in practice. Plain floats, no
/// DaisySP, so this header stays linkable without the DSP library.
struct MixBus
{
    float l = 0.f, r = 0.f;
    float delay_l = 0.f, delay_r = 0.f;
    float reverb_l = 0.f, reverb_r = 0.f;

    void Clear() { *this = MixBus{}; }
};

/// True for parameters owned by the channel strip rather than by the voice.
///
/// Pan, the sends, the saturator and the two filters are not sound generation,
/// so they must not go through IVoice — that seam is what keeps sample playback
/// and analog cartridges additions rather than rewrites. See
/// docs/02-firmware.md §5.
inline constexpr bool IsChannelParam(ParamId id)
{
    switch(id)
    {
        case ParamId::Pan:
        case ParamId::SatDrive:
        case ParamId::Filter1Cutoff:
        case ParamId::Filter1Res:
        case ParamId::Filter1Mode:
        case ParamId::Filter2Cutoff:
        case ParamId::Filter2Res:
        case ParamId::Filter2Mode:
        case ParamId::DelaySend:
        case ParamId::ReverbSend: return true;
        default: return false;
    }
}

/// True for the eight LFO settings. They configure the modulator rather than
/// being modulated, so they go to neither the voice nor the strip.
static_assert(Lfo::kDestSlots == static_cast<int>(ParamId::Count) + 1,
              "an LFO must be able to address every parameter, plus off");

inline constexpr bool IsLfoParam(ParamId id)
{
    const int i = static_cast<int>(id);
    return i >= static_cast<int>(ParamId::LfoSpeed)
           && i <= static_cast<int>(ParamId::LfoStartPhase);
}

/// The second seam, mirroring IVoice: a track's channel strip. The concrete
/// implementation needs DaisySP and lives in channel.h; this interface does
/// not, which is what lets the sequencer and p-lock tests link without it.
class IChannel
{
  public:
    virtual ~IChannel() = default;
    virtual void Init(float sample_rate) = 0;
    virtual void SetParam(ParamId id, float value) = 0;
    /// Adds this track's contribution to the bus.
    virtual void Process(float in, MixBus &bus) = 0;
};

/// A strip that only pans nothing and sums to both sides.
///
/// Exists for the same reason EmptySlot does: VoiceSlot touches the channel
/// every sample, and a real object costs less than a null check. Stateless, so
/// one shared instance is safe.
class DirectChannel : public IChannel
{
  public:
    void Init(float) override {}
    void SetParam(ParamId, float) override {}
    void Process(float in, MixBus &bus) override { bus.l += in; bus.r += in; }
};

inline DirectChannel &NullChannel()
{
    static DirectChannel c;
    return c;
}

/// Sample-accurate trigger scheduling.
///
/// The sequencer computes which sample *within* the current audio block a step
/// lands on and schedules it here, rather than firing at block boundaries.
/// Block-quantised triggers would put +/-0.67 ms of jitter on every hit at a
/// 32-sample block, which is audible smearing on hats and ruins flams.
class VoiceSlot
{
  public:
    void Init(IVoice *voice, float sample_rate, IChannel *channel = nullptr)
    {
        voice_   = voice;
        channel_ = channel ? channel : &NullChannel();
        voice_->Init(sample_rate);
        channel_->Init(sample_rate);
        lfo_.Init(sample_rate);
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
            Dispatch(id, value);
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

    /// Dry mono, voice only. Used by the p-lock tests and the WAV renderer,
    /// which both want the voice without a channel strip in the way.
    float Process()
    {
        if(delay_ == 0)
        {
            ApplyLocks();
            // Order matters: locks first so the LFO modulates this step's
            // locked value, then the reset, then push the modulated value so a
            // trig-synced LFO is already at its start phase when the voice
            // fires rather than a block later.
            lfo_.Trigger();
            ApplyLfo();
            voice_->Trigger(velocity_);
        }
        if(delay_ >= 0)
            --delay_;
        return voice_->Process();
    }

    /// Voice, then channel strip, accumulated into the bus.
    void Process(MixBus &bus) { channel_->Process(Process(), bus); }

    /// Advances the LFO one block and pushes the modulated value.
    ///
    /// Per block rather than per sample, deliberately: continuous modulation
    /// does not need sample accuracy, and pushing every parameter every sample
    /// would mean a virtual call per voice per sample for no audible gain. The
    /// *reset* is a different matter and happens in Process(), on the exact
    /// sample the trigger lands. See docs/02-firmware.md §5.
    void AdvanceLfo(uint32_t frames)
    {
        lfo_.Advance(frames);
        ApplyLfo();
    }

    void SetTempo(float bpm) { lfo_.SetTempo(bpm); }

    Lfo &lfo() { return lfo_; }

    IVoice   *voice() { return voice_; }
    IChannel *channel() { return channel_; }

    /// Point this slot at a different voice — after a machine change — and push
    /// every stored value into it. A freshly constructed machine starts at its
    /// own defaults and knows nothing of what the knobs currently say.
    void Rebind(IVoice *voice)
    {
        voice_ = voice;
        // The previous step's locks were applied to a voice that no longer
        // exists, so there is nothing to restore and the mask must not survive.
        locked_mask_   = 0;
        last_lfo_dest_ = -1;
        for(int i = 0; i < static_cast<int>(ParamId::Count); ++i)
            Dispatch(static_cast<ParamId>(i), base_[i]);
    }

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
                    Dispatch(static_cast<ParamId>(i), base_[i]);
            locked_mask_ = 0;
        }

        for(uint8_t i = 0; i < lock_count_ && locks_; ++i)
        {
            const int id = locks_[i].param_id;
            if(id >= static_cast<int>(ParamId::Count))
                continue;
            Dispatch(static_cast<ParamId>(id), locks_[i].as_float());
            locked_mask_ |= (1u << id);
        }
    }

    /// Routes a parameter to whichever object owns it. Both the lock path and
    /// the restore path go through here, so a channel parameter is p-lockable
    /// on exactly the same terms as a voice parameter.
    void Dispatch(ParamId id, float value)
    {
        if(IsLfoParam(id))
            SetLfoParam(id, value);
        else if(IsChannelParam(id))
            channel_->SetParam(id, value);
        else
            voice_->SetParam(id, value);
    }

    void SetLfoParam(ParamId id, float v)
    {
        switch(id)
        {
            // Speed changes the phase *increment* and never the accumulator,
            // so p-locking SPEED on a free-running LFO bends the rate instead
            // of clicking.
            case ParamId::LfoSpeed:      lfo_.SetSpeed(v); break;
            case ParamId::LfoMult:       lfo_.SetMult(v); break;
            case ParamId::LfoFade:       lfo_.SetFade(v); break;
            case ParamId::LfoDest:       lfo_.SetDest(v); break;
            case ParamId::LfoWave:       lfo_.SetWave(v); break;
            case ParamId::LfoMode:       lfo_.SetMode(v); break;
            case ParamId::LfoDepth:      lfo_.SetDepth(v); break;
            case ParamId::LfoStartPhase: lfo_.SetStartPhase(v); break;
            default: break;
        }
    }

    /// What a parameter would read without modulation: its base, unless this
    /// step locks it. Scanning the lock list beats shadowing every parameter,
    /// and it runs once per block rather than per sample.
    float PreModValue(int d) const
    {
        if(locked_mask_ & (1u << d))
            for(uint8_t i = 0; i < lock_count_ && locks_; ++i)
                if(locks_[i].param_id == d)
                    return locks_[i].as_float();
        return base_[d];
    }

    /// Writes base-or-lock plus modulation to the destination.
    ///
    /// The modulation is re-derived from PreModValue every time rather than
    /// accumulated, which is what stops an LFO walking the stored value —
    /// the same failure mode as a p-lock leaking into later steps.
    void ApplyLfo()
    {
        const int count = static_cast<int>(ParamId::Count);
        const int d     = lfo_.active() ? static_cast<int>(lfo_.dest()) - 1 : -1;

        // A destination that moves must not leave the old one stuck at
        // whatever the modulation last wrote.
        if(d != last_lfo_dest_)
        {
            if(last_lfo_dest_ >= 0 && last_lfo_dest_ < count)
                Dispatch(static_cast<ParamId>(last_lfo_dest_),
                         PreModValue(last_lfo_dest_));
            last_lfo_dest_ = d;
        }
        if(d < 0 || d >= count)
            return;

        float v = PreModValue(d) + lfo_.value();
        if(v < 0.f) v = 0.f;
        if(v > 1.f) v = 1.f;
        Dispatch(static_cast<ParamId>(d), v);
    }

    IVoice          *voice_    = nullptr;
    IChannel        *channel_  = nullptr;
    int32_t          delay_    = -1;
    float            velocity_ = 0.f;
    const ParamLock *locks_      = nullptr;
    uint8_t          lock_count_ = 0;
    /// One bit per ParamId. Must be at least Count bits wide — as a uint8_t
    /// this silently stopped restoring anything above index 7 the moment the
    /// parameter space grew past a single page.
    uint32_t         locked_mask_ = 0;
    Lfo              lfo_;
    int              last_lfo_dest_ = -1;
    float            base_[static_cast<int>(ParamId::Count)] = {};

    static_assert(static_cast<int>(ParamId::Count) <= 32,
                  "locked_mask_ has one bit per parameter");
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

        if(id == ParamId::Note)
        {
            pitch_mul_ = std::pow(2.f, NoteSemitones(value) / 12.f);
            // A voice works out its frequency when TUNE changes, so make it do
            // that again rather than making all thirteen of them watch two
            // parameters and remember to combine them the same way.
            OnParam(ParamId::Tune, params_[static_cast<int>(ParamId::Tune)]);
            return;
        }
        OnParam(id, value);
    }

  protected:
    VoiceBase()
    {
        for(int i = 0; i < static_cast<int>(ParamId::Count); ++i)
            params_[i] = kParamDefault[i];
    }

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

    /// Frequency multiplier from NOTE: 2^(semitones/12). Every voice's TUNE
    /// handler multiplies by this, which is what turns a per-step NOTE p-lock
    /// into a melody.
    float PitchMul() const { return pitch_mul_; }

    /// Soft saturation, then output gain. Drive at 0 is unity and clean.
    float Shape(float x) const
    {
        const float drive = 1.f + param(ParamId::Drive) * 9.f;
        const float d     = x * drive;
        // Cheap tanh-ish soft clip: monotonic, no branching in the hot path.
        const float y = d / (1.f + (d < 0.f ? -d : d));
        return y * param(ParamId::Level);
    }

    float params_[static_cast<int>(ParamId::Count)] = {};
    float pitch_mul_ = 1.f;
};

/// An analog cartridge slot with nothing plugged into it.
///
/// Every track holds an IVoice unconditionally, so an empty slot needs a real
/// object rather than a null pointer: VoiceSlot dereferences the voice every
/// sample, and a null check there would cost more than this does. It still
/// stores parameters, so knobs and p-locks behave identically whether or not
/// hardware is present — the values simply go nowhere until an AnalogVoice
/// takes this slot's place. See docs/05-analog-expansion.md §4.1.
class EmptySlot : public VoiceBase
{
  public:
    void  Init(float) override {}
    void  Trigger(float) override {}
    float Process() override { return 0.f; }
};

} // namespace drom
