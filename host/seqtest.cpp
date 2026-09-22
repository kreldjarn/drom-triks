// Sequencer verification. Micro-timing and ratchets are the features most
// likely to be subtly wrong in a way you'd blame on the voices, so they get
// measured in samples rather than trusted.

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/seq/sequencer.h"

using namespace drom;

namespace {

constexpr float  kSr    = 48000.f;
constexpr size_t kBlock = 32;
int              failures = 0;

void Check(bool ok, const char *what, double got, double want, double tol)
{
    std::printf("  %-46s got %9.2f  want %9.2f  %s\n",
                what, got, want, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

/// Collects (track, absolute sample) for every event over `blocks` blocks.
std::vector<std::pair<int, long>> Run(Sequencer &s, int blocks)
{
    std::vector<std::pair<int, long>> hits;
    Sequencer::Event ev[64];
    for(int b = 0; b < blocks; ++b)
    {
        const size_t n = s.Process(kBlock, ev, 64);
        for(size_t i = 0; i < n; ++i)
            hits.push_back({ev[i].track, static_cast<long>(b) * kBlock + ev[i].offset});
    }
    return hits;
}

Pattern MakePattern()
{
    Pattern p;
    p.bpm_x10 = 1200; // 120 BPM -> 250 samples/tick, 6000 samples per 16th
    p.swing   = 50;
    for(auto &t : p.tracks)
    {
        t.length = 16;
        t.muted  = true;
    }
    return p;
}

} // namespace

int main()
{
    const double spt   = kSr * 60.0 / (120.0 * kPpqn); // samples per tick = 250
    const double sstep = spt * kTicksPerStep;          // samples per step  = 6000
    std::printf("120 BPM, %.0f samples/tick, %.0f samples/step\n\n", spt, sstep);

    // --- micro-timing ------------------------------------------------------
    std::printf("micro-timing (the headline feature):\n");
    for(int micro : {-23, -12, 0, 7, 23})
    {
        Pattern p  = MakePattern();
        p.tracks[0].muted = false;
        Step &st   = p.tracks[0].steps[4];
        st.flags   = kStepActive;
        st.micro   = static_cast<int8_t>(micro);

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();

        auto hits = Run(s, 400); // ~12800 samples, covers step 4 at 24000? no
        // step 4 nominally at 4*6000 = 24000 samples, so run long enough:
        (void)hits;
        Sequencer s2;
        s2.Init(kSr);
        s2.SetPattern(&p);
        s2.Start();
        auto h = Run(s2, 1000); // 32000 samples

        const double want = 4 * sstep + micro * spt;
        char label[64];
        std::snprintf(label, sizeof(label), "micro %+3d ticks (%+.1f ms)",
                      micro, micro * spt / kSr * 1000.0);
        if(h.empty())
            Check(false, label, -1, want, 1);
        else
            Check(std::fabs(h[0].second - want) <= 1.0, label,
                  static_cast<double>(h[0].second), want, 1.0);
    }

    // --- swing -------------------------------------------------------------
    std::printf("\nswing (delays odd steps only):\n");
    {
        Pattern p = MakePattern();
        p.swing   = 75;
        p.tracks[0].muted = false;
        for(int i = 0; i < 4; ++i)
            p.tracks[0].steps[i].flags = kStepActive;

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();
        auto h = Run(s, 800);

        const double swing_off = (75 - 50) * kTicksPerStep / 100 * spt;
        for(int i = 0; i < 4 && i < (int)h.size(); ++i)
        {
            const double want = i * sstep + ((i & 1) ? swing_off : 0);
            char label[64];
            std::snprintf(label, sizeof(label), "step %d %s", i, (i & 1) ? "(odd, swung)" : "(even, straight)");
            Check(std::fabs(h[i].second - want) <= 1.0, label,
                  static_cast<double>(h[i].second), want, 1.0);
        }
    }

    // --- ratchets ----------------------------------------------------------
    std::printf("\nratchets (subdivide the step, cross block boundaries):\n");
    {
        Pattern p = MakePattern();
        p.tracks[0].muted        = false;
        p.tracks[0].steps[0].flags   = kStepActive;
        p.tracks[0].steps[0].ratchet = 4;

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();
        auto h = Run(s, 200);

        Check(h.size() == 4, "4 hits emitted", static_cast<double>(h.size()), 4, 0);
        for(size_t i = 0; i < h.size() && i < 4; ++i)
        {
            const double want = i * (sstep / 4.0);
            char label[48];
            std::snprintf(label, sizeof(label), "ratchet %zu of 4", i + 1);
            Check(std::fabs(h[i].second - want) <= 2.0, label,
                  static_cast<double>(h[i].second), want, 2.0);
        }
    }

    // --- polymeter ---------------------------------------------------------
    std::printf("\npolymeter (7 against 16):\n");
    {
        Pattern p = MakePattern();
        p.tracks[0].muted  = false;
        p.tracks[0].length = 7;
        for(int i = 0; i < 7; ++i)
            p.tracks[0].steps[i].flags = kStepActive;

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();
        auto h = Run(s, 3000); // 96000 samples = 16 steps' worth

        Check(h.size() == 16, "16 hits in 16 steps of time",
              static_cast<double>(h.size()), 16, 0);
        if(h.size() > 7)
            Check(std::fabs(h[7].second - 7 * sstep) <= 1.0,
                  "8th hit wraps to step 0 of cycle 2",
                  static_cast<double>(h[7].second), 7 * sstep, 1.0);
    }

    // --- probability -------------------------------------------------------
    std::printf("\nprobability (50%% over 200 chances):\n");
    {
        Pattern p = MakePattern();
        p.tracks[0].muted = false;
        p.tracks[0].length = 1;
        p.tracks[0].steps[0].flags       = kStepActive;
        p.tracks[0].steps[0].probability = 50;

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();
        auto h = Run(s, 200 * 188); // ~200 steps

        const double frac = h.empty() ? 0 : h.size() / 200.0;
        Check(frac > 0.35 && frac < 0.65, "fires roughly half the time",
              frac * 100, 50, 15);
    }

    std::printf("\n%s\n", failures ? "SEQUENCER TESTS FAILED" : "all sequencer tests passed");
    return failures;
}
