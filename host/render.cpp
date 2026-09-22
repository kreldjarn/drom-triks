// Host renderer: runs the real voice engine natively and writes a WAV.
//
// This is how Phases 2 and 3 get built without a board. The engine code under
// src/engine/ is the same code the firmware compiles; only the audio sink
// differs — a WAV file here, the codec there. Voices can be tuned against
// reference records, and sequencer timing inspected sample by sample, months
// before hardware exists.
//
//   make -C host && host/build/render out.wav && afplay out.wav

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>

#include "../src/engine/voices/drums.h"
#include "../src/engine/voices/synth.h"
#include "wav.h"

using namespace drom;

namespace {

constexpr float kSampleRate = 48000.f;
constexpr int   kSteps      = 16;
constexpr float kBpm        = 124.f;
constexpr int   kBars       = 2;

enum Track
{
    BD = 0,
    SD,
    CH,
    OH,
    LT,
    CP,
    RS,
    FM,
    kNumTracks
};

// A plain 16-step pattern, velocities 0 = rest. Deliberately boring: the point
// is to hear whether the engine is right, not whether the beat is good.
const float kPattern[kNumTracks][kSteps] = {
    /* BD */ {1.0f, 0, 0, 0, 0, 0, 0.7f, 0, 0, 0, 1.0f, 0, 0, 0, 0, 0},
    /* SD */ {0, 0, 0, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 1.0f, 0, 0, 0.5f},
    /* CH */ {0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0},
    /* OH */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.7f, 0},
    /* LT */ {0, 0, 0, 0, 0, 0, 0, 0, 0.8f, 0, 0, 0, 0, 0, 0, 0},
    /* CP */ {0, 0, 0, 0, 0.9f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* RS */ {0, 0, 0, 0.6f, 0, 0, 0, 0.6f, 0, 0, 0, 0.6f, 0, 0, 0, 0},
    /* FM */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.7f, 0, 0},
};

} // namespace

namespace {

/// Regression check: a voice that has never been triggered must be silent.
///
/// This is not hypothetical — DaisySP's AdEnv swells to ~0.49 over its first
/// ~8000 idle samples, so every envelope-based voice thumps at power-on unless
/// it gates on envelope activity. Cheap to check, unpleasant to rediscover
/// through a speaker.
int SelfTest()
{
    BassDrum bd; SnareDrum sd; ClosedHat ch; OpenHat oh;
    Tom lt; Clap cp; RimShot rs; FmVoice fm;
    IVoice *voices[] = {&bd, &sd, &ch, &oh, &lt, &cp, &rs, &fm};
    const char *names[] = {"BD", "SD", "CH", "OH", "LT", "CP", "RS", "FM"};

    int failures = 0;
    for(int v = 0; v < 8; ++v)
    {
        voices[v]->Init(kSampleRate);
        for(int pp = 0; pp < static_cast<int>(ParamId::Count); ++pp)
            voices[v]->SetParam(static_cast<ParamId>(pp), 0.5f);

        float peak = 0.f;
        for(int i = 0; i < 48000; ++i)
        {
            const float a = std::fabs(voices[v]->Process());
            if(a > peak)
                peak = a;
        }
        const bool ok = peak < 1e-6f;
        std::printf("  %-3s untriggered peak %.8f  %s\n", names[v], peak,
                    ok ? "silent" : "*** DRONES ***");
        if(!ok)
            ++failures;
    }
    std::printf("%s\n", failures ? "SELFTEST FAILED" : "selftest passed");
    return failures;
}

} // namespace

int main(int argc, char **argv)
{
    for(int i = 1; i < argc; ++i)
        if(std::strcmp(argv[i], "--selftest") == 0)
            return SelfTest();

    const char *out   = (argc > 1) ? argv[1] : "drom-triks.wav";
    // --trace prints the exact sample each step fires on. Audio onset detection
    // is far too blunt to verify sub-millisecond scheduling; this is exact.
    bool        trace = false;
    // --solo N renders one track alone, which is how you actually tune a voice:
    // in a mix everything sounds fine until it doesn't.
    int         solo  = -1;
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--trace") == 0)
            trace = true;
        else if(std::strcmp(argv[i], "--solo") == 0 && i + 1 < argc)
            solo = std::atoi(argv[++i]);
    }

    BassDrum  bd;
    SnareDrum sd;
    ClosedHat ch;
    OpenHat   oh;
    Tom       lt;
    Clap      cp;
    RimShot   rs;
    FmVoice   fm;

    IVoice *voices[kNumTracks] = {&bd, &sd, &ch, &oh, &lt, &cp, &rs, &fm};
    VoiceSlot slots[kNumTracks];
    for(int i = 0; i < kNumTracks; ++i)
        slots[i].Init(voices[i], kSampleRate);

    // Macro settings. These are the numbers to fiddle with while listening.
    auto set = [&](int t, ParamId p, float v) { voices[t]->SetParam(p, v); };
    set(BD, ParamId::Tune, 0.20f); set(BD, ParamId::Decay, 0.65f);
    set(BD, ParamId::Tone, 0.35f); set(BD, ParamId::Snap,  0.55f);
    set(BD, ParamId::Drive, 0.30f); set(BD, ParamId::Level, 0.90f);

    set(SD, ParamId::Tune, 0.35f); set(SD, ParamId::Decay, 0.40f);
    set(SD, ParamId::Tone, 0.55f); set(SD, ParamId::Snap,  0.60f);
    set(SD, ParamId::Drive, 0.20f); set(SD, ParamId::Level, 0.70f);

    set(CH, ParamId::Tune, 0.55f); set(CH, ParamId::Decay, 0.12f);
    set(CH, ParamId::Tone, 0.70f); set(CH, ParamId::Snap,  0.50f);
    set(CH, ParamId::Drive, 0.0f); set(CH, ParamId::Level, 0.45f);

    set(OH, ParamId::Tune, 0.50f); set(OH, ParamId::Decay, 0.55f);
    set(OH, ParamId::Tone, 0.65f); set(OH, ParamId::Snap,  0.55f);
    set(OH, ParamId::Drive, 0.0f); set(OH, ParamId::Level, 0.40f);

    set(LT, ParamId::Tune, 0.25f); set(LT, ParamId::Decay, 0.45f);
    set(LT, ParamId::Tone, 0.55f); set(LT, ParamId::Snap,  0.50f);
    set(LT, ParamId::Drive, 0.25f); set(LT, ParamId::Level, 0.65f);

    set(CP, ParamId::Tune, 0.45f); set(CP, ParamId::Decay, 0.35f);
    set(CP, ParamId::Tone, 0.55f); set(CP, ParamId::Snap,  0.35f);
    set(CP, ParamId::Drive, 0.15f); set(CP, ParamId::Level, 0.55f);

    set(RS, ParamId::Tune, 0.40f); set(RS, ParamId::Decay, 0.15f);
    set(RS, ParamId::Tone, 0.50f); set(RS, ParamId::Snap,  0.45f);
    set(RS, ParamId::Drive, 0.10f); set(RS, ParamId::Level, 0.40f);

    set(FM, ParamId::Tune, 0.30f); set(FM, ParamId::Decay, 0.30f);
    set(FM, ParamId::Tone, 0.62f); set(FM, ParamId::Snap,  0.45f);
    set(FM, ParamId::Drive, 0.10f); set(FM, ParamId::Level, 0.45f);

    // 16th notes. Kept as a float so a later swing offset lands sub-sample and
    // gets rounded once, rather than accumulating error step by step.
    const double samples_per_step = (60.0 / kBpm) * kSampleRate / 4.0;
    const int    total_steps      = kSteps * kBars;
    const int    total_samples
        = static_cast<int>(samples_per_step * total_steps) + static_cast<int>(kSampleRate);

    std::vector<float> audio;
    audio.reserve(static_cast<size_t>(total_samples) * 2);

    int next_step = 0;
    for(int n = 0; n < total_samples; ++n)
    {
        // Schedule on the exact sample the step falls on.
        if(next_step < total_steps
           && n >= static_cast<int>(samples_per_step * next_step))
        {
            const int s = next_step % kSteps;
            for(int t = 0; t < kNumTracks; ++t)
            {
                if(solo >= 0 && t != solo)
                    continue;
                if(kPattern[t][s] > 0.f)
                    slots[t].Schedule(0, kPattern[t][s]);
            }
            if(trace)
            {
                const double want = samples_per_step * next_step;
                std::printf("step %3d  fired@%8d  want %10.2f  err %+.2f smp (%+.4f ms)\n",
                            next_step, n, want, n - want, (n - want) / kSampleRate * 1000.0);
            }
            ++next_step;
        }

        float mix = 0.f;
        for(int t = 0; t < kNumTracks; ++t)
            mix += slots[t].Process();

        mix *= 0.35f; // headroom; the real mixer lives in engine/mixer.cpp later
        audio.push_back(mix);
        audio.push_back(mix);
    }

    if(!host::WriteWav(out, audio, static_cast<uint32_t>(kSampleRate)))
    {
        std::fprintf(stderr, "could not write %s\n", out);
        return 1;
    }

    static const char *kNames[kNumTracks]
        = {"BD", "SD", "CH", "OH", "LT", "CP", "RS", "FM"};
    if(solo >= 0 && solo < kNumTracks)
        std::printf("solo: %s\n", kNames[solo]);

    std::printf("wrote %s — %.1f s, %d steps @ %.0f BPM\n",
                out,
                static_cast<double>(total_samples) / kSampleRate,
                total_steps,
                static_cast<double>(kBpm));
    return 0;
}
