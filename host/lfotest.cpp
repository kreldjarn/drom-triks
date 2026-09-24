// LFOs: waveforms, modes, and — the part that matters — how modulation layers
// onto the base/p-lock value chain.
//
// The failure modes here are all silent. An LFO that accumulates into the
// stored value walks the patch instead of modulating it. A destination change
// that does not clean up leaves the old parameter frozen wherever the
// modulation last put it. Neither is audible as "the LFO is broken"; both are
// audible as "this patch drifts".

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/engine/voice.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what, float got, float want)
{
    std::printf("  %-54s got %+.4f  want %+.4f  %s\n", what, got, want,
                ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

void Near(float got, float want, float tol, const char *what)
{
    Check(std::fabs(got - want) <= tol, what, got, want);
}

class SpyVoice : public IVoice
{
  public:
    void  Init(float) override {}
    void  Trigger(float) override {}
    float Process() override { return 0.f; }
    void  SetParam(ParamId id, float v) override { value[static_cast<int>(id)] = v; }
    float value[static_cast<int>(ParamId::Count)] = {};
    float get(ParamId id) const { return value[static_cast<int>(id)]; }
};

/// Normalised DEST that selects `id`. Slot 0 is "off", so a parameter sits at
/// its index plus one; the +0.5 aims at the middle of the quantisation step.
float DestFor(ParamId id)
{
    return (static_cast<float>(static_cast<int>(id)) + 1.5f) / Lfo::kDestSlots;
}

float WaveFor(Lfo::Wave w)
{
    return (static_cast<float>(static_cast<int>(w)) + 0.5f)
           / static_cast<float>(Lfo::Wave::Count);
}

float ModeFor(Lfo::Mode m)
{
    return (static_cast<float>(static_cast<int>(m)) + 0.5f)
           / static_cast<float>(Lfo::Mode::Count);
}

ParamLock Lock(ParamId id, float v)
{
    return ParamLock{static_cast<uint8_t>(id),
                     static_cast<uint16_t>(v * 65535.f + 0.5f)};
}

/// A slot with a square-wave LFO parked at phase 0, so its output is exactly
/// +depth and every assertion below can be an equality rather than a range.
void SetUpSquareLfo(VoiceSlot &slot, ParamId dest, float depth)
{
    slot.SetBase(ParamId::LfoWave, WaveFor(Lfo::Wave::Square));
    slot.SetBase(ParamId::LfoMode, ModeFor(Lfo::Mode::Trig));
    slot.SetBase(ParamId::LfoStartPhase, 0.f);
    slot.SetBase(ParamId::LfoSpeed, 0.f);   // slowest, so phase barely moves
    slot.SetBase(ParamId::LfoMult, 0.f);
    slot.SetBase(ParamId::LfoDepth, depth);
    slot.SetBase(ParamId::LfoDest, DestFor(dest));
}

} // namespace

int main()
{
    std::printf("waveforms:\n");
    {
        Lfo l;
        l.Init(48000.f);
        l.SetDepth(1.f);
        l.SetDest(DestFor(ParamId::Tune));
        l.SetMode(ModeFor(Lfo::Mode::Trig));
        l.SetStartPhase(0.f);

        l.SetWave(WaveFor(Lfo::Wave::Square));
        l.Trigger();
        Near(l.value(), 1.f, 1e-5f, "square starts high");

        l.SetWave(WaveFor(Lfo::Wave::Triangle));
        Near(l.value(), -1.f, 1e-5f, "triangle starts at its minimum");

        l.SetWave(WaveFor(Lfo::Wave::Ramp));
        Near(l.value(), -1.f, 1e-5f, "ramp starts low");

        l.SetWave(WaveFor(Lfo::Wave::Saw));
        Near(l.value(), 1.f, 1e-5f, "saw starts high");

        l.SetWave(WaveFor(Lfo::Wave::Sine));
        Near(l.value(), 0.f, 1e-5f, "sine starts at zero");
    }

    std::printf("\nmodes:\n");
    {
        Lfo l;
        l.Init(48000.f);
        l.SetDepth(1.f);
        l.SetDest(DestFor(ParamId::Tune));
        l.SetWave(WaveFor(Lfo::Wave::Ramp));
        l.SetSpeed(1.f);
        l.SetStartPhase(0.f);

        // FREE: a trigger must not disturb the phase. That is the whole mode.
        l.SetMode(ModeFor(Lfo::Mode::Free));
        l.Advance(1000);
        const float before = l.phase();
        l.Trigger();
        Near(l.phase(), before, 1e-9f, "FREE ignores triggers");

        // TRIG: back to the start phase every time.
        l.SetMode(ModeFor(Lfo::Mode::Trig));
        l.Advance(1000);
        l.Trigger();
        Near(l.phase(), 0.f, 1e-9f, "TRIG resets to the start phase");

        l.SetStartPhase(0.25f);
        l.Advance(1000);
        l.Trigger();
        Near(l.phase(), 0.25f, 1e-9f, "and honours a non-zero start phase");
    }
    {
        // HOLD: free-running phase, but the output is frozen between triggers.
        Lfo l;
        l.Init(48000.f);
        l.SetDepth(1.f);
        l.SetDest(DestFor(ParamId::Tune));
        l.SetWave(WaveFor(Lfo::Wave::Ramp));
        l.SetSpeed(1.f);
        l.SetMode(ModeFor(Lfo::Mode::Hold));

        l.Advance(500);
        l.Trigger();
        const float held = l.value();
        l.Advance(500);
        Near(l.value(), held, 1e-9f, "HOLD keeps its value between triggers");
        l.Trigger();
        Check(std::fabs(l.value() - held) > 1e-4f,
              "and samples a new one at the next trigger", l.value(), held);
    }
    {
        // ONE: a single cycle, then it stops rather than looping.
        Lfo l;
        l.Init(48000.f);
        l.SetDepth(1.f);
        l.SetDest(DestFor(ParamId::Tune));
        l.SetWave(WaveFor(Lfo::Wave::Ramp));
        l.SetSpeed(1.f);
        l.SetMode(ModeFor(Lfo::Mode::OneShot));
        l.Trigger();
        for(int i = 0; i < 200; ++i)
            l.Advance(64); // well past one cycle
        const float ended = l.phase();
        l.Advance(64);
        Near(l.phase(), ended, 1e-9f, "ONE stops after a single cycle");
    }

    std::printf("\nthe value chain:\n");
    {
        SpyVoice  v;
        VoiceSlot slot;
        slot.Init(&v, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        SetUpSquareLfo(slot, ParamId::Tune, 0.25f);

        slot.Schedule(0, 1.f);
        slot.Process();
        Near(v.get(ParamId::Tune), 0.75f, 1e-3f,
             "modulation is added to the base value");
        Near(slot.base(ParamId::Tune), 0.5f, 1e-6f,
             "and the stored base is untouched");

        // Re-derived every block, never accumulated — otherwise the value walks.
        for(int i = 0; i < 50; ++i)
            slot.AdvanceLfo(32);
        Near(v.get(ParamId::Tune), 0.75f, 1e-3f,
             "and does not accumulate over many blocks");
        Near(slot.base(ParamId::Tune), 0.5f, 1e-6f,
             "with the base still untouched after 50 blocks");
    }
    {
        // Modulation must sit on top of this step's lock, not the track value.
        SpyVoice  v;
        VoiceSlot slot;
        slot.Init(&v, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        SetUpSquareLfo(slot, ParamId::Tune, 0.25f);

        const ParamLock locked[1] = {Lock(ParamId::Tune, 0.60f)};
        slot.Schedule(0, 1.f, locked, 1);
        slot.Process();
        Near(v.get(ParamId::Tune), 0.85f, 2e-3f,
             "a locked step modulates the lock, not the base");

        slot.Schedule(0, 1.f); // an unlocked step
        slot.Process();
        Near(v.get(ParamId::Tune), 0.75f, 2e-3f,
             "and the next step is back to base plus modulation");
        Near(slot.base(ParamId::Tune), 0.5f, 1e-6f,
             "base survived a locked step under modulation");
    }
    {
        // A destination that moves must not strand the old one.
        SpyVoice  v;
        VoiceSlot slot;
        slot.Init(&v, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        slot.SetBase(ParamId::Decay, 0.3f);
        SetUpSquareLfo(slot, ParamId::Tune, 0.25f);

        slot.Schedule(0, 1.f);
        slot.Process();
        Near(v.get(ParamId::Tune), 0.75f, 1e-3f, "TUNE is being modulated");

        slot.SetBase(ParamId::LfoDest, DestFor(ParamId::Decay));
        slot.AdvanceLfo(32);
        Near(v.get(ParamId::Tune), 0.5f, 1e-3f,
             "moving DEST restores the parameter it left");
        Near(v.get(ParamId::Decay), 0.55f, 1e-3f, "and modulates the new one");

        slot.SetBase(ParamId::LfoDest, 0.f); // off
        slot.AdvanceLfo(32);
        Near(v.get(ParamId::Decay), 0.3f, 1e-3f,
             "and switching DEST off restores that one too");
    }
    {
        SpyVoice  v;
        VoiceSlot slot;
        slot.Init(&v, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        SetUpSquareLfo(slot, ParamId::Tune, 0.f); // depth zero
        slot.Schedule(0, 1.f);
        slot.Process();
        Near(v.get(ParamId::Tune), 0.5f, 1e-6f, "depth 0 modulates nothing");
    }

    std::printf("\np-locking the LFO itself:\n");
    {
        // A locked SPEED must bend the rate, not jump the phase — otherwise
        // every locked step clicks.
        SpyVoice  v;
        VoiceSlot slot;
        slot.Init(&v, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        SetUpSquareLfo(slot, ParamId::Tune, 0.25f);
        slot.SetBase(ParamId::LfoMode, ModeFor(Lfo::Mode::Free));
        slot.SetBase(ParamId::LfoSpeed, 0.5f);

        slot.AdvanceLfo(512);
        const float phase_before = slot.lfo().phase();

        const ParamLock locked[1] = {Lock(ParamId::LfoSpeed, 0.9f)};
        slot.Schedule(0, 1.f, locked, 1);
        slot.Process();
        Near(slot.lfo().phase(), phase_before, 1e-6f,
             "a locked SPEED leaves a free-running phase alone");
    }

    if(failures)
    {
        std::printf("\n%d LFO check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall LFO tests passed\n");
    return 0;
}
