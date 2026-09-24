// Channel strip: pan law, saturator, filters, sends — and the routing that
// decides whether a parameter reaches the voice or the strip.
//
// The routing test is the one that matters. Channel parameters travel the same
// SetBase/p-lock path as voice parameters, so if Dispatch sends one to the
// wrong object it fails silently: the knob moves, the patch stores the value,
// and nothing happens to the audio.

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/engine/channel.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what, float got, float want)
{
    std::printf("  %-52s got %+.4f  want %+.4f  %s\n", what, got, want,
                ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

void Near(float got, float want, float tol, const char *what)
{
    Check(std::fabs(got - want) <= tol, what, got, want);
}

/// Counts which object a parameter actually reached.
class SpyVoice : public IVoice
{
  public:
    void  Init(float) override {}
    void  Trigger(float) override {}
    float Process() override { return 1.f; }
    void  SetParam(ParamId id, float v) override
    {
        seen.push_back(id);
        value[static_cast<int>(id)] = v;
    }
    std::vector<ParamId> seen;
    float value[static_cast<int>(ParamId::Count)] = {};
};

class SpyChannel : public IChannel
{
  public:
    void Init(float) override {}
    void SetParam(ParamId id, float v) override
    {
        seen.push_back(id);
        value[static_cast<int>(id)] = v;
    }
    void Process(float in, MixBus &bus) override { bus.l += in; bus.r += in; }
    std::vector<ParamId> seen;
    float value[static_cast<int>(ParamId::Count)] = {};
};

bool Saw(const std::vector<ParamId> &v, ParamId id)
{
    for(ParamId x : v)
        if(x == id)
            return true;
    return false;
}

ParamLock Lock(ParamId id, float v)
{
    return ParamLock{static_cast<uint8_t>(id),
                     static_cast<uint16_t>(v * 65535.f + 0.5f)};
}

/// Gain the strip applies to a sine at `hz`, as a ratio.
///
/// Normalised against the *sampled* peak of the same sine rather than against
/// 1.0, because a discretely sampled sine rarely lands on its own crest: at
/// 48 kHz an 8 kHz tone is six samples per cycle, so its sampled peak is
/// sin(120 deg) = 0.866. Comparing raw peaks against 1.0 measures the sample
/// grid, not the filter.
float GainAt(ChannelStrip &s, float hz, float sr = 48000.f)
{
    const int n = 4096;
    float     out_peak = 0.f, in_peak = 0.f;
    for(int i = 0; i < n; ++i)
    {
        const float x = std::sin(6.2831853f * hz * static_cast<float>(i) / sr);
        MixBus      bus;
        s.Process(x, bus);
        if(i > n / 2) // skip the settling half
        {
            if(std::fabs(bus.l) > out_peak) out_peak = std::fabs(bus.l);
            if(std::fabs(x) > in_peak)      in_peak  = std::fabs(x);
        }
    }
    return in_peak > 0.f ? out_peak / in_peak : 0.f;
}

} // namespace

int main()
{
    std::printf("parameter routing:\n");
    {
        SpyVoice   v;
        SpyChannel c;
        VoiceSlot  slot;
        slot.Init(&v, 48000.f, &c);

        slot.SetBase(ParamId::Tune, 0.3f);   // a voice parameter
        slot.SetBase(ParamId::Pan, 0.9f);    // a channel parameter

        Check(Saw(v.seen, ParamId::Tune), "TUNE reaches the voice", 1, 1);
        Check(!Saw(c.seen, ParamId::Tune), "TUNE does not reach the strip", 1, 1);
        Check(Saw(c.seen, ParamId::Pan), "PAN reaches the strip", 1, 1);
        Check(!Saw(v.seen, ParamId::Pan), "PAN does not reach the voice", 1, 1);
        Near(c.value[static_cast<int>(ParamId::Pan)], 0.9f, 1e-4f,
             "and arrives with the right value");
    }

    // A channel parameter must be p-lockable on exactly the same terms as a
    // voice parameter, restore included — otherwise a locked PAN would stick.
    {
        SpyVoice   v;
        SpyChannel c;
        VoiceSlot  slot;
        slot.Init(&v, 48000.f, &c);
        slot.SetBase(ParamId::Pan, 0.25f);

        const ParamLock locked[1] = {Lock(ParamId::Pan, 0.80f)};
        slot.Schedule(0, 1.f, locked, 1);
        slot.Process();
        Near(c.value[static_cast<int>(ParamId::Pan)], 0.80f, 1e-3f,
             "a locked PAN applies on the locked step");

        slot.Schedule(0, 1.f); // next step carries no lock
        slot.Process();
        Near(c.value[static_cast<int>(ParamId::Pan)], 0.25f, 1e-3f,
             "and is restored on the step after, not left stuck");
    }

    std::printf("\npan law:\n");
    {
        ChannelStrip s;
        s.Init(48000.f);
        MixBus bus;

        s.SetParam(ParamId::Pan, 0.f);
        bus.Clear(); s.Process(1.f, bus);
        Near(bus.l, 1.f, 1e-4f, "hard left puts everything in L");
        Near(bus.r, 0.f, 1e-4f, "and nothing in R");

        s.SetParam(ParamId::Pan, 1.f);
        bus.Clear(); s.Process(1.f, bus);
        Near(bus.l, 0.f, 1e-4f, "hard right puts nothing in L");
        Near(bus.r, 1.f, 1e-4f, "and everything in R");

        s.SetParam(ParamId::Pan, 0.5f);
        bus.Clear(); s.Process(1.f, bus);
        Near(bus.l, bus.r, 1e-5f, "centre is equal on both sides");
        Near(bus.l * bus.l + bus.r * bus.r, 1.f, 1e-4f,
             "and equal-power, so the centre does not dip");
    }

    std::printf("\nsaturator:\n");
    {
        ChannelStrip s;
        s.Init(48000.f);
        s.SetParam(ParamId::Pan, 0.f); // all signal into L, easier to read
        MixBus bus;

        s.SetParam(ParamId::SatDrive, 0.f);
        bus.Clear(); s.Process(0.3f, bus);
        Near(bus.l, 0.3f, 1e-6f, "SAT at 0 is an exact bypass");

        s.SetParam(ParamId::SatDrive, 1.f);
        bus.Clear(); s.Process(0.3f, bus);
        Check(bus.l > 0.3f, "SAT adds gain in the middle of the range", bus.l, 0.3f);

        bus.Clear(); s.Process(1.f, bus);
        Near(bus.l, 1.f, 1e-4f, "but full scale stays full scale, not louder");
    }

    std::printf("\nfilters:\n");
    {
        ChannelStrip s;
        s.Init(48000.f);
        s.SetParam(ParamId::Pan, 0.f);

        Near(GainAt(s, 8000.f), 1.f, 1e-4f,
             "wide-open lowpass is transparent (bypassed)");

        // Lowpass at roughly 200 Hz: 100 Hz passes, 8 kHz does not.
        s.SetParam(ParamId::Filter1Mode, 0.f);
        s.SetParam(ParamId::Filter1Cutoff, 0.34f);
        const float low  = GainAt(s, 100.f);
        const float high = GainAt(s, 8000.f);
        Check(low > 0.7f, "lowpass passes 100 Hz", low, 1.f);
        Check(high < 0.1f, "and stops 8 kHz", high, 0.f);

        // Highpass at the same cutoff does the opposite.
        s.SetParam(ParamId::Filter1Mode, 0.9f);
        const float hp_high = GainAt(s, 8000.f);
        const float hp_low  = GainAt(s, 100.f);
        Check(hp_high > 0.7f, "highpass passes 8 kHz", hp_high, 1.f);
        Check(hp_low < 0.3f, "and stops 100 Hz", hp_low, 0.f);
    }

    std::printf("\nmode quantisation:\n");
    {
        Check(ModeFromNorm(0.0f) == FilterMode::LowPass,  "0.0 is lowpass", 1, 1);
        Check(ModeFromNorm(0.5f) == FilterMode::BandPass, "0.5 is bandpass", 1, 1);
        Check(ModeFromNorm(1.0f) == FilterMode::HighPass, "1.0 is highpass", 1, 1);
        // The clamp matters: 1.0 * 3 == 3 indexes off the end of the enum.
        Check(ModeFromNorm(1.5f) == FilterMode::HighPass, "and above 1.0 clamps", 1, 1);
    }

    std::printf("\nsends:\n");
    {
        ChannelStrip s;
        s.Init(48000.f);
        s.SetParam(ParamId::Pan, 0.f);   // hard left
        s.SetParam(ParamId::DelaySend, 0.5f);
        s.SetParam(ParamId::ReverbSend, 0.f);

        MixBus bus;
        s.Process(1.f, bus);
        Near(bus.delay_l, 0.5f, 1e-4f, "delay send scales the signal");
        Near(bus.delay_r, 0.f, 1e-4f, "and is post-pan, so a hard-left voice stays left");
        Near(bus.reverb_l, 0.f, 1e-6f, "a send at 0 contributes nothing");
    }

    if(failures)
    {
        std::printf("\n%d channel-strip check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall channel-strip tests passed\n");
    return 0;
}
