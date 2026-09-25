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
    for(auto &t : p.tracks)
    {
        t.length = 16;
        t.swing  = kSwingStraight;
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
        p.tracks[0].swing = 75;
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

    // Swing is per track: a swung hat over a straight kick is the whole reason
    // it does not live on the pattern.
    {
        Pattern p = MakePattern();
        p.tracks[0].swing = kSwingStraight; // straight
        p.tracks[1].swing = 75;             // swung
        p.tracks[0].muted = p.tracks[1].muted = false;
        for(int i = 0; i < 4; ++i)
            p.tracks[0].steps[i].flags = p.tracks[1].steps[i].flags = kStepActive;

        Sequencer s;
        s.Init(kSr);
        s.SetPattern(&p);
        s.Start();

        // Collect step 1 (the first odd step) for each track separately.
        double t0 = -1, t1 = -1;
        int64_t now = 0;
        Sequencer::Event ev[32];
        for(int b = 0; b < 400; ++b)
        {
            const size_t n = s.Process(32, ev, 32);
            for(size_t i = 0; i < n; ++i)
            {
                const double at = static_cast<double>(now + ev[i].offset);
                if(ev[i].track == 0 && at > sstep * 0.5 && at < sstep * 1.5 && t0 < 0) t0 = at;
                if(ev[i].track == 1 && at > sstep * 0.5 && at < sstep * 1.5 && t1 < 0) t1 = at;
            }
            now += 32;
        }
        const double swing_off = (75 - 50) * kTicksPerStep / 100 * spt;
        Check(std::fabs(t0 - sstep) <= 1.0, "track 0 step 1 lands straight", t0, sstep, 1.0);
        Check(std::fabs(t1 - (sstep + swing_off)) <= 1.0,
              "track 1 step 1 is swung, on the same pattern", t1, sstep + swing_off, 1.0);
    }

    // Micro is stored in ticks and capped at +/-23, which is "just under a step"
    // only while a step is 24 ticks. A track at double speed has 12 and at
    // quadruple speed 6, so the stored value can reach several positions — and
    // the tick window that finds a displaced step is only +/-1 wide.
    //
    // Unclamped this is not a rounding error: measured over a fixed span, a
    // speed-1 track at max micro and max swing played 4 of its 8 steps, and a
    // speed-2 track at max micro played **none at all**. It predates swing.
    std::printf("\na fast track with a big micro offset still plays:\n");
    {
        auto HitsAt = [](int8_t speed, int8_t micro, uint8_t swing) {
            static Pattern p;
            p = Pattern{};
            p.bpm_x10 = 1200;
            for(auto &tr : p.tracks) { tr.length = 16; tr.muted = true; }
            Track &tr = p.tracks[0];
            tr.muted = false; tr.speed = speed; tr.swing = swing;
            for(int i = 0; i < 16; ++i)
            {
                tr.steps[i].flags = kStepActive;
                tr.steps[i].micro = micro;
            }
            static Sequencer s;
            s.Init(kSr); s.SetPattern(&p); s.Start();
            Sequencer::Event ev[64];
            int n = 0;
            for(int b = 0; b < 2000; ++b) n += (int)s.Process(32, ev, 64);
            return n;
        };

        for(int8_t sp = 0; sp <= 2; ++sp)
        {
            const int plain   = HitsAt(sp, 0, kSwingStraight);
            const int nudged  = HitsAt(sp, kMicroRange, kSwingMax);
            char label[80];
            std::snprintf(label, sizeof(label),
                          "speed %d: max micro + max swing loses no steps", sp);
            Check(nudged == plain, label, static_cast<double>(nudged),
                  static_cast<double>(plain), 0.0);
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
