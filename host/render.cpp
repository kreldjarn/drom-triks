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
#include <cstring>
#include <vector>

#include "../src/engine/voices/drums.h"
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
    kNumTracks
};

// A plain 16-step pattern, velocities 0 = rest. Deliberately boring: the point
// is to hear whether the engine is right, not whether the beat is good.
const float kPattern[kNumTracks][kSteps] = {
    /* BD */ {1.0f, 0, 0, 0, 0, 0, 0.7f, 0, 0, 0, 1.0f, 0, 0, 0, 0, 0},
    /* SD */ {0, 0, 0, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 1.0f, 0, 0, 0.5f},
    /* CH */ {0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0, 0.8f, 0, 0.5f, 0},
    /* OH */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.7f, 0},
};

} // namespace

int main(int argc, char **argv)
{
    const char *out   = (argc > 1) ? argv[1] : "drom-triks.wav";
    // --trace prints the exact sample each step fires on. Audio onset detection
    // is far too blunt to verify sub-millisecond scheduling; this is exact.
    bool        trace = false;
    for(int i = 1; i < argc; ++i)
        if(std::strcmp(argv[i], "--trace") == 0)
            trace = true;

    BassDrum  bd;
    SnareDrum sd;
    ClosedHat ch;
    OpenHat   oh;

    IVoice   *voices[kNumTracks] = {&bd, &sd, &ch, &oh};
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
                if(kPattern[t][s] > 0.f)
                    slots[t].Schedule(0, kPattern[t][s]);
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

        mix *= 0.5f; // headroom; the real mixer lives in engine/mixer.cpp later
        audio.push_back(mix);
        audio.push_back(mix);
    }

    if(!host::WriteWav(out, audio, static_cast<uint32_t>(kSampleRate)))
    {
        std::fprintf(stderr, "could not write %s\n", out);
        return 1;
    }

    std::printf("wrote %s — %.1f s, %d steps @ %.0f BPM\n",
                out,
                static_cast<double>(total_samples) / kSampleRate,
                total_steps,
                static_cast<double>(kBpm));
    return 0;
}
