// The forked DSP in src/engine/dsp/ against the DaisySP originals it replaces.
//
// The forks exist to be cheaper (docs/02-firmware.md §10), and the claim that
// they sound the same is checked here rather than trusted: each original and
// its fork run the same script — triggers, retriggers while ringing,
// parameter changes mid-sound, extremes — and every sample is compared.
//
// The drum models are expected to be **bit-identical**, except AnalogBassDrum,
// whose FM'd resonator needs a per-sample coefficient update that uses fast
// maths instead of sinf/powf; it is held to -100 dB. AdEnv's curve is a true
// exponential where DaisySP's is an approximation, so it is held to 1 %.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#include "daisysp.h"
#include "../src/engine/dsp/adenv.h"
#include "../src/engine/dsp/drum_models.h"
#include "../src/engine/machines.h"

// DaisySP's SyntheticSnareDrum alternates a file-scope global (the fork makes
// it a member). It has to be reset before each run of the original or the two
// start out of phase.
extern bool even;

namespace {

int failures = 0;

constexpr float kSr      = 48000.f;
constexpr int   kSamples = 4 * 48000; // four seconds per scenario

/// One step of a script: at sample `at`, call `act`.
template <typename T>
struct Event
{
    int                    at;
    std::function<void(T &)> act;
};

/// Runs `voice` through its script, triggering wherever `trig` says, and
/// returns every sample. rand() is re-seeded so noise sources match.
template <typename T>
std::vector<float> Run(T &voice, const std::vector<Event<T>> &script,
                       const std::vector<int> &trigs, bool reset_even = false)
{
    std::srand(12345);
    if(reset_even)
        even = true;
    voice.Init(kSr);
    std::vector<float> out(kSamples);
    size_t             e = 0, t = 0;
    for(int n = 0; n < kSamples; ++n)
    {
        while(e < script.size() && script[e].at == n)
            script[e++].act(voice);
        bool trig = false;
        while(t < trigs.size() && trigs[t] == n)
        {
            trig = true;
            ++t;
        }
        out[n] = voice.Process(trig);
    }
    return out;
}

struct Diff
{
    int   mismatches = 0;
    float max_abs    = 0.f;
    float peak       = 0.f;
};

Diff Compare(const std::vector<float> &a, const std::vector<float> &b)
{
    Diff d;
    for(size_t i = 0; i < a.size(); ++i)
    {
        if(a[i] != b[i])
            ++d.mismatches;
        d.max_abs = std::fmax(d.max_abs, std::fabs(a[i] - b[i]));
        d.peak    = std::fmax(d.peak, std::fabs(a[i]));
    }
    return d;
}

void Report(const char *what, const Diff &d, bool exact)
{
    const float db = d.max_abs > 0.f && d.peak > 0.f
                         ? 20.f * std::log10(d.max_abs / d.peak)
                         : -999.f;
    const bool ok = exact ? d.mismatches == 0 : db < -100.f;
    std::printf("  %-44s %7d differ, worst %7.1f dB  %s\n", what, d.mismatches,
                db, ok ? "ok" : "*** FAIL ***");
    if(d.peak == 0.f)
    {
        std::printf("  %-44s silent — the scenario tests nothing  *** FAIL ***\n", what);
        ++failures;
    }
    if(!ok)
        ++failures;
}

/// Triggers every `period` samples, plus a retrigger soon after some hits so
/// a voice is hit again while still ringing.
std::vector<int> Hits(int period)
{
    std::vector<int> t;
    for(int n = 100; n < kSamples; n += period)
    {
        t.push_back(n);
        if((n / period) % 3 == 1)
            t.push_back(n + period / 7);
    }
    return t;
}

/// Same script against an original and its fork. The setters share names, so
/// one lambda drives both; `exact` picks bit-identity or the -100 dB bound.
template <typename Orig, typename Fork, typename Script>
void Pair(const char *what, Script script, int period, bool exact, bool reset_even = false)
{
    static Orig o;
    static Fork f;
    std::vector<Event<Orig>> so;
    std::vector<Event<Fork>> sf;
    script(so);
    script(sf);
    const auto hits = Hits(period);
    const auto a    = Run(o, so, hits, reset_even);
    const auto b    = Run(f, sf, hits, reset_even);
    Report(what, Compare(a, b), exact);
}

} // namespace

int main()
{
    std::printf("drum models (bit-identical unless noted):\n");

    using drom::dsp::StdRand; // the forks draw from rand() exactly as DaisySP does, for this test
    Pair<daisysp::SyntheticSnareDrum, drom::dsp::SyntheticSnareDrum<StdRand>>(
        "SyntheticSnareDrum, defaults",
        [](auto &) {}, 12000, true, true);
    Pair<daisysp::SyntheticSnareDrum, drom::dsp::SyntheticSnareDrum<StdRand>>(
        "SyntheticSnareDrum, swept mid-sound",
        [](auto &s) {
            using E = typename std::remove_reference_t<decltype(s)>::value_type;
            s.push_back(E{0, [](auto &v) { v.SetAccent(0.9f); v.SetDecay(1.f); }});
            for(int k = 1; k < 16; ++k)
            {
                const float x = k / 15.f;
                s.push_back(E{k * 11000, [x](auto &v) {
                                  v.SetFreq(80.f + x * 400.f);
                                  v.SetFmAmount(1.f - x);
                                  v.SetSnappy(x);
                                  v.SetDecay(x);
                                  v.SetAccent(0.3f + 0.7f * x);
                              }});
            }
        },
        9000, true, true);

    Pair<daisysp::AnalogSnareDrum, drom::dsp::AnalogSnareDrum<StdRand>>(
        "AnalogSnareDrum, defaults", [](auto &) {}, 12000, true);
    Pair<daisysp::AnalogSnareDrum, drom::dsp::AnalogSnareDrum<StdRand>>(
        "AnalogSnareDrum, swept mid-sound",
        [](auto &s) {
            using E = typename std::remove_reference_t<decltype(s)>::value_type;
            s.push_back(E{0, [](auto &v) { v.SetDecay(1.f); }});
            for(int k = 1; k < 16; ++k)
            {
                const float x = k / 15.f;
                s.push_back(E{k * 11000, [x](auto &v) {
                                  v.SetFreq(100.f + x * 400.f);
                                  v.SetTone(x);
                                  v.SetSnappy(1.f - x);
                                  v.SetDecay(x);
                                  v.SetAccent(x);
                              }});
            }
        },
        9000, true);

    using OrigCH = daisysp::HiHat<daisysp::SquareNoise, daisysp::LinearVCA>;
    using ForkCH = drom::dsp::HiHat<drom::dsp::SquareNoise, daisysp::LinearVCA, true, StdRand>;
    using OrigOH = daisysp::HiHat<daisysp::RingModNoise, daisysp::SwingVCA>;
    using ForkOH = drom::dsp::HiHat<drom::dsp::RingModNoise, daisysp::SwingVCA, true, StdRand>;
    auto hat_sweep = [](auto &s) {
        using E = typename std::remove_reference_t<decltype(s)>::value_type;
        s.push_back(E{0, [](auto &v) { v.SetDecay(1.f); }});
        for(int k = 1; k < 16; ++k)
        {
            const float x = k / 15.f;
            s.push_back(E{k * 11000, [x](auto &v) {
                              v.SetFreq(3000.f + x * 9000.f);
                              v.SetTone(x);
                              v.SetNoisiness(1.f - x);
                              v.SetDecay(x);
                              v.SetAccent(x);
                          }});
        }
    };
    Pair<OrigCH, ForkCH>("HiHat (square noise), defaults", [](auto &) {}, 6000, true);
    Pair<OrigCH, ForkCH>("HiHat (square noise), swept mid-sound", hat_sweep, 5000, true);
    Pair<OrigOH, ForkOH>("HiHat (ring-mod noise), defaults", [](auto &) {}, 6000, true);
    Pair<OrigOH, ForkOH>("HiHat (ring-mod noise), swept mid-sound", hat_sweep, 5000, true);

    auto bd_sweep = [](auto &s) {
        using E = typename std::remove_reference_t<decltype(s)>::value_type;
        s.push_back(E{0, [](auto &v) { v.SetDecay(1.f); v.SetAccent(1.f); }});
        for(int k = 1; k < 16; ++k)
        {
            const float x = k / 15.f;
            s.push_back(E{k * 11000, [x](auto &v) {
                              v.SetFreq(30.f + x * 70.f);
                              v.SetTone(x);
                              v.SetDecay(x);
                              v.SetAttackFmAmount(x);
                              v.SetSelfFmAmount(x * 0.8f);
                              v.SetAccent(x);
                          }});
        }
    };
    Pair<daisysp::AnalogBassDrum, drom::dsp::AnalogBassDrum>(
        "AnalogBassDrum, defaults (fast maths: -100 dB)", [](auto &) {}, 12000, false);
    Pair<daisysp::AnalogBassDrum, drom::dsp::AnalogBassDrum>(
        "AnalogBassDrum, swept (fast maths: -100 dB)", bd_sweep, 9000, false);

    std::printf("\nenvelope:\n");
    {
        // AdEnv takes no trigger argument, so it gets its own harness: random
        // times, curves and retriggers, including SetTime mid-segment.
        //
        // The fork's curve is a true exponential where DaisySP's is
        // expf_fast, so only the linear curve (0) is bit-identical. At -4,
        // kPercCurve — the only curve any voice uses — it is held to 1 % of
        // full scale, and every segment boundary to within one sample (a decay
        // that ends late is a longer sound). Steeper curves are reported but
        // not failed: their deviation is expf_fast's own error, which grows
        // with |x|, so there the fork is the closer of the two to exp.
        static daisysp::AdEnv  o;
        static drom::dsp::AdEnv f;
        const float curves[] = {0.f, -4.f, -15.f, -50.f, 3.f, -1.f};
        for(float curve : curves)
        {
            std::srand(7);
            o.Init(kSr);
            f.Init(kSr);
            std::vector<float> a(kSamples), b(kSamples);
            std::vector<int>   edges_a, edges_b; // samples where the segment changed
            uint8_t            seg_a = 255, seg_b = 255;
            for(int n = 0; n < kSamples; ++n)
            {
                if(n % 9000 == 0 || n % 9000 == 2500)
                {
                    const float at = 0.0005f + (std::rand() % 100) * 0.0001f;
                    const float dt = 0.01f + (std::rand() % 100) * 0.004f;
                    o.SetTime(daisysp::ADENV_SEG_ATTACK, at);
                    o.SetTime(daisysp::ADENV_SEG_DECAY, dt);
                    f.SetTime(daisysp::ADENV_SEG_ATTACK, at);
                    f.SetTime(daisysp::ADENV_SEG_DECAY, dt);
                    o.SetCurve(curve);
                    f.SetCurve(curve);
                    o.SetMax(0.8f);
                    f.SetMax(0.8f);
                }
                if(n % 4500 == 10 || n % 13000 == 300) // includes retrigger mid-decay
                {
                    o.Trigger();
                    f.Trigger();
                }
                a[n] = o.Process();
                b[n] = f.Process();
                if(o.GetCurrentSegment() != seg_a)
                {
                    seg_a = o.GetCurrentSegment();
                    edges_a.push_back(n);
                }
                if(f.GetCurrentSegment() != seg_b)
                {
                    seg_b = f.GetCurrentSegment();
                    edges_b.push_back(n);
                }
            }
            char what[64];
            const Diff d = Compare(a, b);
            if(curve == 0.f)
            {
                std::snprintf(what, sizeof(what), "AdEnv, linear (bit-identical)");
                Report(what, d, true);
                continue;
            }
            int worst_edge = edges_a.size() == edges_b.size() ? 0 : 1 << 30;
            for(size_t i = 0; i < edges_a.size() && i < edges_b.size(); ++i)
                worst_edge = std::max(worst_edge, std::abs(edges_a[i] - edges_b[i]));
            const float pct     = 100.f * d.max_abs / 0.8f; // SetMax(0.8)
            const bool  checked = curve == -4.f;
            const bool  ok      = !checked || (pct < 1.f && worst_edge <= 1);
            std::printf("  AdEnv, curve %+3.0f: worst %.2f %% of full scale, %zu "
                        "segment edges, worst %d samples apart  %s\n",
                        curve, pct, edges_a.size(), worst_edge,
                        !checked ? "(reported)" : ok ? "ok" : "*** FAIL ***");
            if(!ok)
                ++failures;
        }
    }

    std::printf("\nblock processing (bit-identical to per-sample):\n");
    {
        // The audio path renders in blocks (VoiceSlot::RenderBlock); the
        // per-sample path (VoiceSlot::Process) is the reference. Every machine,
        // through both, with triggers at arbitrary offsets inside a block,
        // p-locks, a parameter change between blocks, and block sizes that do
        // not divide anything evenly. Long enough for voices to fall asleep
        // and wake, which is where an off-by-one would hide.
        static drom::MachineSlot machine;
        static drom::VoiceSlot   slot;
        static drom::ParamLock   locks[2];
        locks[0].param_id = static_cast<uint8_t>(drom::ParamId::Decay);
        locks[0].value    = 52000;
        locks[1].param_id = static_cast<uint8_t>(drom::ParamId::Tune);
        locks[1].value    = 20000;
        const size_t sizes[] = {7, 32, 48, 100, 1};
        constexpr int kLen    = 3 * 48000;
        constexpr int kKnobAt = 60000;

        auto render = [&](drom::MachineId id, bool blocks) {
            drom::dsp::g_noise_seed_count = 0; // same seeds both times
            machine.Init(kSr, id);
            slot.Init(machine.voice(), kSr);
            std::vector<float> out(kLen);
            int                n = 0, k = 0, hit = 0;
            while(n < kLen)
            {
                const int len = blocks ? static_cast<int>(sizes[k++ % 5]) : 1;
                int       end = n + len > kLen ? kLen : n + len;
                if(n < kKnobAt && end > kKnobAt)
                    end = kKnobAt; // a block boundary exactly where the knob moves
                // Hits every 7919 samples (prime, so they land everywhere in a
                // block), scheduled at the block that contains them.
                const int next = 300 + hit * 7919;
                if(next >= n && next < end)
                {
                    const bool locked = hit % 3 == 1;
                    slot.Schedule(next - n, 0.5f + 0.1f * (hit % 5),
                                  locked ? locks : nullptr, locked ? 2 : 0);
                    ++hit;
                }
                if(n == kKnobAt) // a knob move, between blocks, on the same sample both ways
                    slot.SetBase(drom::ParamId::Tone, 0.8f);
                if(blocks)
                    slot.RenderBlock(out.data() + n, static_cast<size_t>(end - n));
                else
                    out[n] = slot.Process();
                n = end;
            }
            return out;
        };
        for(int m = 0; m < static_cast<int>(drom::MachineId::Count); ++m)
        {
            const auto id = static_cast<drom::MachineId>(m);
            const auto a  = render(id, false);
            const auto b  = render(id, true);
            Diff d = Compare(a, b);
            char what[64];
            std::snprintf(what, sizeof(what), "%s", drom::MachineName(id));
            if(id == drom::MachineId::Silent)
                d.peak = 1.f; // silent by design; only the equality matters
            Report(what, d, true);
        }
    }

    if(failures)
    {
        std::printf("\n%d DSP check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall DSP equivalence tests passed\n");
    return 0;
}
