// MIDI clock recovery verification.
//
// Every number here is a property you'd otherwise only discover on stage:
// does the tempo estimate converge, does it stay steady under a jittery
// source, does it survive a dropped byte, and does it fall back gracefully
// when the master goes away.

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "../src/seq/clock.h"
#include "../src/seq/sequencer.h"

using namespace drom;

namespace {

constexpr double kSr = 48000.0;
int failures = 0;

void Check(bool ok, const char *what, double got, double want, const char *unit = "")
{
    std::printf("  %-50s %8.3f%-4s (want %.3f%s)  %s\n", what, got, unit, want, unit,
                ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

double SamplesPerClock(double bpm) { return kSr * 60.0 / (bpm * 24.0); }

/// Deterministic pseudo-random in [-1,1], so runs are reproducible.
double Rand(uint32_t &s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (static_cast<double>(s % 20001) - 10000.0) / 10000.0;
}

struct Result { double mean_bpm; double bpm_spread; double max_abs_phase; };

/// Drives the PLL with `count` clocks, jittered by `jitter_samples`, and
/// reports the tempo estimate's behaviour over the final quarter of the run.
Result Drive(ClockPll &pll, double bpm, int count, double jitter_samples,
             bool quantise_1ms = false, double ramp_to_bpm = 0.0)
{
    uint32_t seed = 12345;
    double   t    = 10000.0;
    double   lo = 1e9, hi = -1e9, sum = 0, maxph = 0;
    int      n = 0;

    for(int i = 0; i < count; ++i)
    {
        const double frac    = static_cast<double>(i) / count;
        const double cur_bpm = ramp_to_bpm > 0.0 ? bpm + (ramp_to_bpm - bpm) * frac : bpm;
        double       stamp   = t + Rand(seed) * jitter_samples;

        // USB MIDI is delivered on 1 ms frame boundaries, so its timestamps
        // are quantised no matter how accurate the sender is.
        if(quantise_1ms)
        {
            const double frame = kSr / 1000.0;
            stamp = std::floor(stamp / frame) * frame;
        }

        pll.OnClock(static_cast<uint64_t>(stamp < 0 ? 0 : stamp));
        pll.Advance(static_cast<uint32_t>(SamplesPerClock(cur_bpm)));
        t += SamplesPerClock(cur_bpm);

        if(frac > 0.75)
        {
            const double b = pll.bpm();
            lo = std::fmin(lo, b); hi = std::fmax(hi, b); sum += b; ++n;
            maxph = std::fmax(maxph, std::fabs(pll.phase_error_samples()));
        }
    }
    return {n ? sum / n : 0.0, hi - lo, maxph};
}

} // namespace

int main()
{
    std::printf("MIDI clock recovery (24 PPQN in, 96 PPQN out)\n");
    std::printf("at 120 BPM: %.0f samples per clock byte, %.0f per internal tick\n\n",
                SamplesPerClock(120), SamplesPerClock(120) / 4);

    // --- convergence on a perfect source -----------------------------------
    std::printf("clean source:\n");
    {
        ClockPll p; p.Init(kSr);
        Result r = Drive(p, 120.0, 200, 0.0);
        Check(std::fabs(r.mean_bpm - 120.0) < 0.05, "tracks 120 BPM", r.mean_bpm, 120.0, " BPM");
        Check(p.locked(), "reports locked", p.locked() ? 1 : 0, 1);
        Check(p.external(), "reports external sync", p.external() ? 1 : 0, 1);

        ClockPll q; q.Init(kSr);
        Result r2 = Drive(q, 174.0, 200, 0.0);
        Check(std::fabs(r2.mean_bpm - 174.0) < 0.05, "tracks 174 BPM", r2.mean_bpm, 174.0, " BPM");
    }

    // --- the headline: jitter rejection ------------------------------------
    std::printf("\njittery source (+/-1 ms, i.e. +/-48 samples):\n");
    double tight_spread = 0, smooth_spread = 0;
    {
        ClockPll t; t.Init(kSr); t.SetTightness(ClockPll::Tightness::Tight);
        Result rt = Drive(t, 120.0, 400, 48.0);
        tight_spread = rt.bpm_spread;
        Check(std::fabs(rt.mean_bpm - 120.0) < 1.0, "Tight  holds 120 BPM", rt.mean_bpm, 120.0, " BPM");

        ClockPll s; s.Init(kSr); s.SetTightness(ClockPll::Tightness::Smooth);
        Result rs = Drive(s, 120.0, 400, 48.0);
        smooth_spread = rs.bpm_spread;
        Check(std::fabs(rs.mean_bpm - 120.0) < 1.0, "Smooth holds 120 BPM", rs.mean_bpm, 120.0, " BPM");

        std::printf("  %-50s %8.3f BPM\n", "Tight  tempo spread under jitter", tight_spread);
        std::printf("  %-50s %8.3f BPM\n", "Smooth tempo spread under jitter", smooth_spread);
        Check(smooth_spread < tight_spread, "Smooth rejects more jitter than Tight",
              smooth_spread, tight_spread, " BPM");
        Check(smooth_spread < 0.5, "Smooth spread stays musically inaudible",
              smooth_spread, 0.5, " BPM");
    }

    // --- USB frame quantisation --------------------------------------------
    std::printf("\nUSB source (timestamps quantised to 1 ms frames):\n");
    {
        ClockPll s; s.Init(kSr); s.SetTightness(ClockPll::Tightness::Smooth);
        Result r = Drive(s, 120.0, 400, 0.0, /*quantise*/ true);
        Check(std::fabs(r.mean_bpm - 120.0) < 1.0, "holds tempo despite frame quantisation",
              r.mean_bpm, 120.0, " BPM");
        Check(r.bpm_spread < 1.0, "spread stays under 1 BPM", r.bpm_spread, 1.0, " BPM");
    }

    // --- following a tempo ramp --------------------------------------------
    std::printf("\ntempo automation (120 -> 140 over 400 clocks):\n");
    {
        ClockPll t; t.Init(kSr); t.SetTightness(ClockPll::Tightness::Tight);
        Result rt = Drive(t, 120.0, 400, 0.0, false, 140.0);
        ClockPll s; s.Init(kSr); s.SetTightness(ClockPll::Tightness::Smooth);
        Result rs = Drive(s, 120.0, 400, 0.0, false, 140.0);
        std::printf("  %-50s %8.3f BPM\n", "Tight  reaches", rt.mean_bpm);
        std::printf("  %-50s %8.3f BPM\n", "Smooth reaches", rs.mean_bpm);
        Check(rt.mean_bpm > rs.mean_bpm, "Tight follows a ramp more closely than Smooth",
              rt.mean_bpm - rs.mean_bpm, 0.0, " BPM");
    }

    // --- a dropped byte ----------------------------------------------------
    std::printf("\ncorrupted stream (one clock byte dropped):\n");
    {
        ClockPll p; p.Init(kSr);
        const double spc = SamplesPerClock(120.0);
        double t = 10000.0;
        for(int i = 0; i < 100; ++i) { p.OnClock((uint64_t)t); t += spc; }
        const double before = p.bpm();
        t += spc;                                   // drop one: double interval
        p.OnClock((uint64_t)t); t += spc;
        for(int i = 0; i < 100; ++i) { p.OnClock((uint64_t)t); t += spc; }

        Check(p.resyncs() >= 1, "detects the gap and resyncs instead of filtering it",
              p.resyncs(), 1);
        Check(std::fabs(p.bpm() - 120.0) < 0.5, "tempo survives the glitch", p.bpm(), 120.0, " BPM");
        Check(std::fabs(before - 120.0) < 0.5, "and was correct beforehand", before, 120.0, " BPM");
    }

    // --- master goes away --------------------------------------------------
    std::printf("\nmaster stops sending:\n");
    {
        ClockPll p; p.Init(kSr);
        const double spc = SamplesPerClock(120.0);
        double t = 10000.0;
        for(int i = 0; i < 100; ++i) { p.OnClock((uint64_t)t); p.Advance((uint32_t)spc); t += spc; }
        Check(p.external(), "external while clocks arrive", p.external() ? 1 : 0, 1);

        for(int i = 0; i < 40; ++i) p.Advance(1000);   // ~0.83 s of silence
        Check(!p.external(), "falls back to internal after ~0.5 s",
              p.external() ? 1 : 0, 0);
        Check(std::fabs(p.bpm() - 120.0) < 1.0, "keeps the last known tempo rather than stalling",
              p.bpm(), 120.0, " BPM");
    }

    // --- output rate feeds the sequencer -----------------------------------
    std::printf("\nrate handed to the sequencer:\n");
    {
        ClockPll p; p.Init(kSr);
        Drive(p, 120.0, 200, 0.0);
        const double spt = p.samples_per_tick_q16() / 65536.0;
        Check(std::fabs(spt - 250.0) < 0.5, "250 samples per 96 PPQN tick at 120 BPM",
              spt, 250.0, " smp");
    }

    // --- acquisition speed --------------------------------------------------
    std::printf("\nacquisition (how fast an unknown tempo is found):\n");
    {
        ClockPll p; p.Init(kSr);            // starts assuming 120
        const double spc = SamplesPerClock(174.0);
        double t = 10000.0;
        int    n = 0;
        while(n < 100 && std::fabs(p.bpm() - 174.0) > 1.0)
        {
            p.OnClock(static_cast<uint64_t>(t));
            t += spc;
            ++n;
        }
        Check(n <= 8, "within 1 BPM of a 174 BPM source", n, 8, " clocks");
        std::printf("  %-50s %8.3f beats\n", "which is", n / 24.0);
    }

    // --- the sequencer actually follows -------------------------------------
    std::printf("\nsequencer driven by recovered clock:\n");
    {
        Pattern pat;
        for(auto &tr : pat.tracks) { tr.muted = true; tr.length = 16; }
        pat.tracks[0].muted = false;
        pat.tracks[0].length = 1;
        pat.tracks[0].steps[0].flags = kStepActive;

        ClockPll  pll; pll.Init(kSr);
        Sequencer seq; seq.Init(kSr); seq.SetPattern(&pat); seq.Start();

        // 90 BPM master: a 16th step should come every 8000 samples.
        const double spc = SamplesPerClock(90.0);
        double next_clock = spc;
        long   sample = 0;
        std::vector<long> hits;
        Sequencer::Event ev[8];

        for(int blk = 0; blk < 10000; ++blk)
        {
            while(next_clock < sample + 32)
            {
                pll.OnClock(static_cast<uint64_t>(next_clock));
                next_clock += spc;
            }
            pll.Advance(32);
            if(pll.external())
                seq.SetTickRateQ16(pll.samples_per_tick_q16());

            const size_t n = seq.Process(32, ev, 8);
            for(size_t i = 0; i < n; ++i)
                hits.push_back(sample + ev[i].offset);
            sample += 32;
        }

        const double want = kSr * 60.0 / 90.0 / 4.0;   // 8000 samples per 16th at 90 BPM

        // The sequencer starts on its internal 120 BPM and is pulled to the
        // master's 90 as the PLL acquires, so the first steps are legitimately
        // mis-spaced. Steady-state accuracy is what matters; acquisition is
        // measured separately above.
        double worst_steady = 0, worst_acquire = 0;
        for(size_t i = 1; i < hits.size(); ++i)
        {
            const double d = std::fabs((hits[i] - hits[i - 1]) - want);
            if(i <= 4)
                worst_acquire = std::fmax(worst_acquire, d);
            else
                worst_steady = std::fmax(worst_steady, d);
        }
        Check(hits.size() > 25, "sequencer produced steps", (double)hits.size(), 25);
        Check(worst_steady < 8.0, "steady-state step spacing matches the master",
              worst_steady, 8.0, " smp");
        std::printf("  %-50s %8.1f smp (%.3f ms)\n", "worst steady-state deviation",
                    worst_steady, worst_steady / kSr * 1000.0);
        std::printf("  %-50s %8.1f smp (%.2f ms)\n", "worst during acquisition (first 4 steps)",
                    worst_acquire, worst_acquire / kSr * 1000.0);
    }

    std::printf("\n%s\n", failures ? "CLOCK TESTS FAILED" : "all clock tests passed");
    return failures;
}
