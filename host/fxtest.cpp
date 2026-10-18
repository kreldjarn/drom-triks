// Master effects: delay, reverb, compressor, and the routing between them.
//
// The things worth pinning are the ones that are silent when wrong: a send at
// zero that still bleeds, a delay whose feedback never decays, a compressor
// that pumps the stereo image because its detector is not linked.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "../src/engine/fx.h"

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

constexpr size_t kDelayFrames = 24000; // 0.5 s at 48 kHz
float            g_delay[kDelayFrames * 2];
constexpr size_t kReverbFloats = PlateReverb::BufferFloats(48000.f);
float            g_reverb[kReverbFloats]; // ~165 kB — a static, never a member

MixBus Send(float dry, float delay, float reverb)
{
    MixBus b;
    b.l = b.r = dry;
    b.delay_l = b.delay_r = delay;
    b.reverb_l = b.reverb_r = reverb;
    return b;
}

} // namespace

int main()
{
    std::printf("stereo delay:\n");
    {
        StereoDelay d;
        d.Init(48000.f, g_delay, kDelayFrames);
        d.SetParam(FxId::DelayFeedback, 0.f);
        d.SetParam(FxId::DelayWidth, 0.f);
        d.SetParam(FxId::DelayTone, 1.f); // no damping, so the tap is clean
        d.SetParam(FxId::DelayTime, 0.3f);

        const size_t tap = d.time_samples();
        Check(tap > 1 && tap < kDelayFrames, "delay time lands inside the buffer",
              static_cast<float>(tap), static_cast<float>(tap));

        float l = 0.f, r = 0.f, peak_before = 0.f, at_tap = 0.f;
        for(size_t i = 0; i <= tap + 1; ++i)
        {
            const float in = (i == 0) ? 1.f : 0.f; // one impulse
            d.Process(in, in, l, r);
            if(i == tap)
                at_tap = l;
            else if(std::fabs(l) > peak_before)
                peak_before = std::fabs(l);
        }
        Near(at_tap, 1.f, 1e-3f, "the impulse comes back at exactly that time");
        Near(peak_before, 0.f, 1e-6f, "and nothing arrives before it");
    }

    {
        // Feedback must decay. Unity feedback is not "infinite repeats", it is
        // a slow overflow, so the parameter is capped below 1.
        StereoDelay d;
        d.Init(48000.f, g_delay, kDelayFrames);
        d.SetParam(FxId::DelayTime, 0.f);   // shortest tap
        d.SetParam(FxId::DelayFeedback, 1.f); // maximum the knob allows
        d.SetParam(FxId::DelayTone, 1.f);
        d.SetParam(FxId::DelayWidth, 0.f);

        float l = 0.f, r = 0.f, first = 0.f, last = 0.f;
        const size_t tap = d.time_samples();
        for(size_t i = 0; i < tap * 40; ++i)
        {
            d.Process(i == 0 ? 1.f : 0.f, i == 0 ? 1.f : 0.f, l, r);
            if(i == tap) first = std::fabs(l);
            if(i == tap * 30) last = std::fabs(l);
        }
        Check(last < first, "repeats decay even at maximum feedback", last, 0.f);
        Check(last < 1.f, "and never exceed the original hit", last, 0.f);
    }

    {
        // Width cross-feeds the loop, which is where stereo bounce comes from.
        StereoDelay d;
        d.Init(48000.f, g_delay, kDelayFrames);
        d.SetParam(FxId::DelayTime, 0.f);
        d.SetParam(FxId::DelayFeedback, 0.8f);
        d.SetParam(FxId::DelayTone, 1.f);
        d.SetParam(FxId::DelayWidth, 1.f);

        const size_t tap = d.time_samples();
        float l = 0.f, r = 0.f, second_l = 0.f, second_r = 0.f;
        for(size_t i = 0; i <= tap * 2 + 1; ++i)
        {
            d.Process(i == 0 ? 1.f : 0.f, 0.f, l, r); // left only
            if(i == tap * 2) { second_l = std::fabs(l); second_r = std::fabs(r); }
        }
        Check(second_r > second_l, "a left-only hit bounces to the right",
              second_r, second_l);
    }

    std::printf("\nreverb:\n");
    {
        MasterFx fx;
        fx.Init(48000.f, g_delay, kDelayFrames, g_reverb, kReverbFloats);
        fx.SetParam(FxId::ReverbLevel, 0.f);
        fx.SetParam(FxId::DelayFeedback, 0.f);

        float l = 0.f, r = 0.f;
        MixBus b = Send(0.5f, 0.f, 1.f); // reverb send up, level down
        for(int i = 0; i < 64; ++i)
            fx.Process(b, l, r);
        Near(l, 0.5f, 1e-3f, "level at 0 means no reverb reaches the output");
    }
    {
        MasterFx fx;
        fx.Init(48000.f, g_delay, kDelayFrames, g_reverb, kReverbFloats);
        fx.SetParam(FxId::ReverbLevel, 1.f);
        fx.SetParam(FxId::ReverbSize, 0.8f);
        fx.SetParam(FxId::DelayFeedback, 0.f);

        float l = 0.f, r = 0.f, tail = 0.f;
        MixBus wet = Send(0.f, 0.f, 1.f);
        for(int i = 0; i < 4800; ++i) // 100 ms of excitation
            fx.Process(wet, l, r);
        MixBus silence;
        for(int i = 0; i < 4800; ++i) // then 100 ms of nothing
        {
            fx.Process(silence, l, r);
            if(std::fabs(l) > tail)
                tail = std::fabs(l);
        }
        Check(tail > 1e-4f, "a tail continues after the input stops", tail, 1.f);
    }

    std::printf("\nplate reverb:\n");
    {
        // SIZE is a decay time, so measure one. Excite with a short burst,
        // then time the tail's fall from -5 dB to -25 dB below its peak (20
        // ms RMS windows) and extrapolate to 60 dB — the early-decay method,
        // since a full 60 dB fall runs into float noise on long settings.
        static PlateReverb rv;
        for(float target : {0.5f, 3.0f})
        {
            rv.Init(48000.f, g_reverb, kReverbFloats);
            rv.SetDecayTime(target);
            rv.SetDamping(18000.f);
            std::vector<float> env;
            float              acc = 0.f;
            uint32_t           rng = 1;
            const int          n   = static_cast<int>(48000 * target * 1.5f);
            for(int i = 0; i < n; ++i)
            {
                rng            = rng * 1664525u + 1013904223u;
                const float in = i < 480 ? (static_cast<float>(rng >> 9) / 4194304.f - 1.f) : 0.f;
                float       l, r;
                rv.Process(in, in, l, r);
                acc += l * l + r * r;
                if(i % 960 == 959)
                {
                    env.push_back(10.f * std::log10(acc / 960.f + 1e-30f));
                    acc = 0.f;
                }
            }
            const float peak = *std::max_element(env.begin(), env.end());
            int         a = -1, b = -1;
            for(size_t i = 0; i < env.size(); ++i)
            {
                if(a < 0 && env[i] <= peak - 5.f && i > 0 && env[i - 1] > env[i])
                    a = static_cast<int>(i);
                if(a >= 0 && b < 0 && env[i] <= peak - 25.f)
                    b = static_cast<int>(i);
            }
            const float rt60 = (b - a) * 0.02f * 3.f;
            char        what[64];
            std::snprintf(what, sizeof(what), "SIZE sets RT60: %.1f s asked", target);
            Near(rt60, target, target * 0.25f, what);
        }
    }
    {
        // Pre-delay: nothing out before it, something soon after.
        static PlateReverb rv;
        rv.Init(48000.f, g_reverb, kReverbFloats);
        rv.SetPreDelay(0.05f); // 2400 samples
        float before = 0.f, after = 0.f;
        for(int i = 0; i < 4800; ++i)
        {
            float l, r;
            rv.Process(i == 0 ? 1.f : 0.f, i == 0 ? 1.f : 0.f, l, r);
            (i < 2400 ? before : after) += std::fabs(l) + std::fabs(r);
        }
        Check(before == 0.f, "nothing comes out before the pre-delay", before, 0.f);
        Check(after > 0.f, "and the tail arrives after it", after, 1.f);
    }
    {
        // Stereo: the two outputs read different parts of the tank, so they
        // must not be a copy of each other.
        static PlateReverb rv;
        rv.Init(48000.f, g_reverb, kReverbFloats);
        double ll = 0, rr = 0, lr = 0;
        for(int i = 0; i < 48000; ++i)
        {
            float l, r;
            rv.Process(i < 48 ? 1.f : 0.f, i < 48 ? 1.f : 0.f, l, r);
            ll += l * l;
            rr += r * r;
            lr += l * r;
        }
        const float corr = static_cast<float>(lr / std::sqrt(ll * rr));
        Check(std::fabs(corr) < 0.5f, "left and right are decorrelated", corr, 0.f);
    }
    {
        // Longest SIZE, fed continuously for ten seconds: bounded, not
        // building up. A reverb that blows up does so in exactly this case.
        static PlateReverb rv;
        rv.Init(48000.f, g_reverb, kReverbFloats);
        rv.SetDecayTime(10.f);
        rv.SetDamping(18000.f);
        float    peak = 0.f;
        uint32_t rng  = 7;
        for(int i = 0; i < 480000; ++i)
        {
            rng            = rng * 1664525u + 1013904223u;
            const float in = static_cast<float>(rng >> 9) / 4194304.f - 1.f;
            float       l, r;
            rv.Process(in * 0.5f, in * 0.5f, l, r);
            peak = std::fmax(peak, std::fmax(std::fabs(l), std::fabs(r)));
        }
        Check(peak < 20.f && peak == peak, "10 s of input at the longest SIZE stays bounded", peak, 20.f);
    }
    {
        // An undersized buffer is a silent reverb, never an overrun.
        static PlateReverb rv;
        Check(!rv.Init(48000.f, g_reverb, kReverbFloats / 2),
              "an undersized buffer is refused", 1.f, 0.f);
        float l = 1.f, r = 1.f;
        rv.Process(1.f, 1.f, l, r);
        Check(l == 0.f && r == 0.f, "and the reverb is silent", l, 0.f);
    }

    std::printf("\ncompressor:\n");
    {
        MasterFx fx;
        fx.Init(48000.f, g_delay, kDelayFrames, g_reverb, kReverbFloats);
        fx.SetParam(FxId::ReverbLevel, 0.f);
        fx.SetParam(FxId::DelayFeedback, 0.f);
        fx.SetParam(FxId::CompThreshold, 0.5f); // -30 dB
        fx.SetParam(FxId::CompRatio, 1.f);      // 20:1
        fx.SetParam(FxId::CompAttack, 0.f);     // fastest
        fx.SetParam(FxId::CompMakeup, 0.f);

        float l = 0.f, r = 0.f;
        MixBus loud = Send(0.9f, 0.f, 0.f);
        for(int i = 0; i < 48000; ++i) // a second, well past the attack
            fx.Process(loud, l, r);
        Check(l < 0.9f, "a signal above threshold is attenuated", l, 0.f);

        // Linked detector: both sides must take the same gain, or the image
        // walks sideways whenever one channel is louder.
        MixBus lopsided;
        lopsided.l = 0.9f;
        lopsided.r = 0.45f;
        for(int i = 0; i < 48000; ++i)
            fx.Process(lopsided, l, r);
        Near(l / r, 2.f, 0.02f, "and the L/R ratio is preserved, not squashed");
    }

    std::printf("\nrouting:\n");
    {
        MasterFx fx;
        fx.Init(48000.f, g_delay, kDelayFrames, g_reverb, kReverbFloats);
        fx.SetParam(FxId::ReverbLevel, 1.f);

        float l = 0.f, r = 0.f;
        MixBus dry_only = Send(0.4f, 0.f, 0.f);
        for(int i = 0; i < 256; ++i)
            fx.Process(dry_only, l, r);
        Near(l, 0.4f, 1e-3f, "sends at 0 pass the dry signal untouched");
    }
    {
        // No reverb supplied at all: the machine must still make sound.
        //
        // Settled over a few hundred samples rather than measured on the first
        // one — the compressor's detector starts at zero, so sample 0 of any
        // signal is read as a transient and gets a gain that has not caught up
        // yet. That is correct behaviour, not transparency failing.
        MasterFx fx;
        fx.Init(48000.f, nullptr, 0, nullptr, 0);
        float  l = 0.f, r = 0.f;
        MixBus b = Send(0.3f, 1.f, 1.f);
        for(int i = 0; i < 256; ++i)
            fx.Process(b, l, r);
        Near(l, 0.3f, 1e-3f, "and with no FX memory at all, dry still passes");
    }

    if(failures)
    {
        std::printf("\n%d master-FX check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall master-FX tests passed\n");
    return 0;
}
