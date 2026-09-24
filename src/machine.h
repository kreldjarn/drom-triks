#pragma once
#include <atomic>
#include <cstring>
#include "engine/channel.h"
#include "engine/fx.h"
#include "engine/voices/drums.h"
#include "engine/voices/synth.h"
#include "io/patch.h"
#include "seq/clock.h"
#include "seq/sequencer.h"
#include "util/spsc.h"

namespace drom {

/// Every edit the UI can make, as data.
///
/// Routing edits through a queue rather than letting the UI touch the pattern
/// is what lets the audio side run without a single lock: it is the only
/// writer of sequencer, voice and pattern state.
struct Command
{
    enum class Type : uint8_t
    {
        None = 0,
        SetKitParam,   ///< track, param, value
        SetFxParam,    ///< param = FxId, value — master delay/reverb/compressor
        Snapshot,      ///< copy the patch to snapshot_dst_ for the main loop
        ToggleStep,    ///< track, step
        SetStepLock,   ///< track, step, param, value
        SetStepField,  ///< track, step, param = StepField, value 0..1
        ClearStepLocks,///< track, step
        SetTrackMute,  ///< track, value != 0
        SetTempo,      ///< value = BPM
        Start,
        Stop,
        Continue,
    };

    Type    type  = Type::None;
    uint8_t track = 0;
    uint8_t step  = 0;
    uint8_t param = 0;
    float   value = 0.f;
};

/// Published by the audio side for the UI to read. Relaxed atomics: stale by a
/// block is fine for LEDs and a screen, and the alternative costs a barrier in
/// the callback for no musical benefit.
struct AudioState
{
    std::atomic<uint32_t> position[kNumTracks];
    std::atomic<uint32_t> tick{0};
    std::atomic<bool>     playing{false};
    std::atomic<bool>     external_sync{false};
    std::atomic<float>    tempo{120.f};
};

/// Owns the whole audio-side machine. One writer, no locks, no allocation.
class Machine
{
  public:
    /// `delay_buf` holds `2 * delay_frames` floats and `reverb` is borrowed;
    /// both are supplied by the caller because where they live is a board
    /// decision. ReverbSc alone is 395 kB, so it cannot be a member here —
    /// Machine stays ~21.5 kB, which CLAUDE.md treats as a hard rule. Passing
    /// nothing simply leaves the master effects silent.
    void Init(float              sample_rate,
              float             *delay_buf    = nullptr,
              size_t             delay_frames = 0,
              daisysp::ReverbSc *reverb       = nullptr)
    {
        sample_rate_ = sample_rate;
        fx_.Init(sample_rate, delay_buf, delay_frames, reverb);

        voices_[0] = &bd_; voices_[1] = &sd_; voices_[2] = &ch_; voices_[3] = &oh_;
        voices_[4] = &lt_; voices_[5] = &cp_; voices_[6] = &rs_; voices_[7] = &fm_;
        for(int i = 0; i < kNumCartridgeSlots; ++i)
            voices_[kNumDigitalVoices + i] = &cart_[i];
        for(int i = 0; i < kNumTracks; ++i)
            slots_[i].Init(voices_[i], sample_rate, &strips_[i]);

        InitPatch(patch_);
        for(auto &t : patch_.pattern.tracks)
            t.length = 16;

        seq_.Init(sample_rate);
        seq_.SetPattern(&patch_.pattern);
        pll_.Init(sample_rate);

        ApplyKit();
    }

    // ---- main-loop side ----------------------------------------------------

    /// Returns false if the queue is full; the caller should not retry from a
    /// real-time context, but the UI runs at 1 kHz and can.
    bool Push(const Command &c) { return queue_.Push(c); }

    const AudioState &state() const { return state_; }

    /// Ask the audio side for a coherent copy of the patch, for saving.
    ///
    /// The main loop must not memcpy the patch itself. The audio side is its
    /// only writer (§4), so a copy taken while a step is being edited or a
    /// p-lock written can tear — and a torn Patch passes every header check,
    /// because the magic, version and size are all still correct. It surfaces
    /// later as one wrong step, which is exactly the "sounds like corruption"
    /// failure SaveHeader exists to prevent and cannot catch.
    ///
    /// `dst` must outlive the request. Poll SnapshotReady() before using it.
    /// The copy costs ~30 kB of memcpy inside one audio block — roughly 4 % of
    /// a 667 us block, once, on an explicit user action.
    void RequestSnapshot(Patch *dst)
    {
        snapshot_dst_ = dst;
        snapshot_done_.store(false, std::memory_order_relaxed);
        Command c;
        c.type = Command::Type::Snapshot;
        Push(c);
    }

    bool SnapshotReady() const
    {
        return snapshot_done_.load(std::memory_order_acquire);
    }

    /// Read-only view for drawing. May be one block stale, and a concurrent
    /// edit can tear a field — which costs at worst one frame of wrong
    /// brightness, and never a wrong note, because the audio side is the only
    /// writer.
    const Patch &patch() const { return patch_; }

    /// Only safe before Process() is running, or from the audio side.
    Patch &mutable_patch() { return patch_; }

    /// Timestamped MIDI clock byte, from the UART interrupt on hardware.
    void OnMidiClock(uint64_t sample_time) { pll_.OnClock(sample_time); }

    ClockPll  &pll() { return pll_; }
    Sequencer &sequencer() { return seq_; }

    // ---- audio side --------------------------------------------------------

    /// Renders one block of **interleaved stereo** — `out` holds `2 * frames`
    /// samples. Everything below this line runs in the callback.
    void Process(float *out, size_t frames)
    {
        DrainCommands();

        pll_.Advance(static_cast<uint32_t>(frames));
        if(pll_.external())
            seq_.SetTickRateQ16(pll_.samples_per_tick_q16());

        Sequencer::Event ev[32];
        const size_t     n = seq_.Process(frames, ev, 32);
        for(size_t i = 0; i < n; ++i)
            slots_[ev[i].track].Schedule(ev[i].offset,
                                         ev[i].velocity,
                                         ev[i].step ? ev[i].step->locks : nullptr,
                                         ev[i].step ? ev[i].step->lock_count : 0);

        // LFO rate is tempo-relative, but Recalculate() costs a pow and two
        // divides per track, so push it only when the tempo actually moves.
        const float bpm = pll_.external() ? pll_.bpm() : seq_.tempo();
        if(bpm != last_bpm_)
        {
            last_bpm_ = bpm;
            for(int t = 0; t < kNumTracks; ++t)
                slots_[t].SetTempo(bpm);
        }
        for(int t = 0; t < kNumTracks; ++t)
            slots_[t].AdvanceLfo(static_cast<uint32_t>(frames));

        for(size_t f = 0; f < frames; ++f)
        {
            MixBus bus;
            for(int t = 0; t < kNumTracks; ++t)
                slots_[t].Process(bus);

            float l = 0.f, r = 0.f;
            fx_.Process(bus, l, r);
            out[2 * f]     = l * master_;
            out[2 * f + 1] = r * master_;
        }

        Publish();
    }

  private:
    void DrainCommands()
    {
        Command c;
        while(queue_.Pop(c))
            Apply(c);
    }

    void Apply(const Command &c)
    {
        const int t = c.track < kNumTracks ? c.track : 0;
        switch(c.type)
        {
            case Command::Type::Snapshot:
                // snapshot_dst_ was published by the queue's release/acquire
                // pair, so it is visible here without a separate barrier.
                if(snapshot_dst_)
                    *snapshot_dst_ = patch_;
                snapshot_done_.store(true, std::memory_order_release);
                break;

            case Command::Type::SetFxParam:
                if(c.param < kNumFxParams)
                {
                    patch_.kit.fx[c.param] = c.value;
                    fx_.SetParam(static_cast<FxId>(c.param), c.value);
                }
                break;

            case Command::Type::SetKitParam:
                if(c.param < static_cast<int>(ParamId::Count))
                {
                    patch_.kit.params[t][c.param] = c.value;
                    slots_[t].SetBase(static_cast<ParamId>(c.param), c.value);
                }
                break;

            case Command::Type::ToggleStep:
                if(c.step < kMaxSteps)
                {
                    Step &s = patch_.pattern.tracks[t].steps[c.step];
                    if(s.active())
                    {
                        s.flags &= static_cast<uint8_t>(~kStepActive);
                        s.lock_count = 0;
                    }
                    else
                        s.flags |= kStepActive;
                }
                break;

            case Command::Type::SetStepLock:
                if(c.step < kMaxSteps && c.param < static_cast<int>(ParamId::Count))
                    WriteLock(patch_.pattern.tracks[t].steps[c.step], c.param, c.value);
                break;

            case Command::Type::SetStepField:
                if(c.step < kMaxSteps && c.param < static_cast<uint8_t>(StepField::Count))
                {
                    Step &s = patch_.pattern.tracks[t].steps[c.step];
                    switch(static_cast<StepField>(c.param))
                    {
                        case StepField::Velocity:
                            s.velocity = static_cast<uint8_t>(c.value * 127.f + 0.5f);
                            break;
                        // Bipolar: 0.5 is dead on the grid. kMicroRange is just
                        // under one step, so the ends stay unambiguous — see §6.
                        case StepField::Micro:
                            s.micro = static_cast<int8_t>((c.value * 2.f - 1.f)
                                                          * kMicroRange);
                            break;
                        case StepField::Probability:
                            s.probability = static_cast<uint8_t>(c.value * 100.f + 0.5f);
                            break;
                        case StepField::Ratchet:
                            s.ratchet = static_cast<uint8_t>(1.f + c.value * 7.f + 0.5f);
                            break;
                        default: break;
                    }
                }
                break;

            case Command::Type::ClearStepLocks:
                if(c.step < kMaxSteps)
                    patch_.pattern.tracks[t].steps[c.step].lock_count = 0;
                break;

            case Command::Type::SetTrackMute:
                patch_.pattern.tracks[t].muted = c.value != 0.f;
                break;

            case Command::Type::SetTempo:
                patch_.pattern.bpm_x10 = static_cast<uint16_t>(c.value * 10.f);
                seq_.SetTempo(c.value);
                pll_.SetInternalTempo(c.value);
                break;

            case Command::Type::Start: seq_.Start(); break;
            case Command::Type::Stop: seq_.Stop(); break;
            case Command::Type::Continue: seq_.Continue(); break;
            case Command::Type::None: break;
        }
    }

    static void WriteLock(Step &s, uint8_t param, float value)
    {
        s.flags |= kStepActive;
        const uint16_t v = static_cast<uint16_t>(value * 65535.f + 0.5f);
        for(uint8_t i = 0; i < s.lock_count; ++i)
            if(s.locks[i].param_id == param)
            {
                s.locks[i].value = v;
                return;
            }
        if(s.lock_count < kMaxLocks)
        {
            s.locks[s.lock_count].param_id = param;
            s.locks[s.lock_count].value    = v;
            ++s.lock_count;
        }
    }

    void ApplyKit()
    {
        for(int t = 0; t < kNumTracks; ++t)
            for(int p = 0; p < static_cast<int>(ParamId::Count); ++p)
                slots_[t].SetBase(static_cast<ParamId>(p), patch_.kit.params[t][p]);
        for(int i = 0; i < kNumFxParams; ++i)
            fx_.SetParam(static_cast<FxId>(i), patch_.kit.fx[i]);
    }

    void Publish()
    {
        for(int t = 0; t < kNumTracks; ++t)
            state_.position[t].store(static_cast<uint32_t>(seq_.position(t)),
                                     std::memory_order_relaxed);
        state_.tick.store(static_cast<uint32_t>(seq_.tick()), std::memory_order_relaxed);
        state_.playing.store(seq_.playing(), std::memory_order_relaxed);
        state_.external_sync.store(pll_.external(), std::memory_order_relaxed);
        state_.tempo.store(pll_.external() ? pll_.bpm() : seq_.tempo(),
                           std::memory_order_relaxed);
    }

    BassDrum bd_; SnareDrum sd_; ClosedHat ch_; OpenHat oh_;
    Tom lt_; Clap cp_; RimShot rs_; FmVoice fm_;

    /// Tracks 9–12. An AnalogVoice replaces one of these per occupied slot once
    /// a carrier exists; until then the tracks sequence and p-lock normally and
    /// simply sound nothing.
    EmptySlot cart_[kNumCartridgeSlots];

    IVoice      *voices_[kNumTracks] = {};
    VoiceSlot    slots_[kNumTracks];
    ChannelStrip strips_[kNumTracks];
    Patch     patch_;
    Sequencer seq_;
    ClockPll  pll_;

    MasterFx                fx_;
    Patch                   *snapshot_dst_ = nullptr;
    std::atomic<bool>        snapshot_done_{false};
    SpscQueue<Command, 128> queue_;
    AudioState              state_;

    float sample_rate_ = 48000.f;
    float master_      = 0.35f;
    float last_bpm_    = 0.f;
};

} // namespace drom
