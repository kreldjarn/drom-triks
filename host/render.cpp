// Host renderer: the real engine — sequencer and voices — rendered to a WAV.
//
// Same code the firmware compiles; only the audio sink differs. Processing
// happens in 32-sample blocks exactly as the audio callback will, so timing
// behaviour here is the timing behaviour on hardware.
//
//   make -C host run && afplay host/build/out.wav
//   host/build/render out.wav --solo 2      one voice alone, for tuning
//   host/build/render out.wav --trace       every event, with its sample
//   host/build/render --selftest            voices silent until triggered

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/engine/voices/drums.h"
#include "../src/engine/voices/synth.h"
#include "../src/seq/sequencer.h"
#include "wav.h"

using namespace drom;

namespace {

constexpr float  kSampleRate = 48000.f;
constexpr size_t kBlock      = 32; // matches the firmware's audio block
constexpr float  kBpm        = 124.f;
constexpr int    kBars       = 2;

// Named TrackId, not Track: drom::Track is the pattern's track struct.
enum TrackId { BD = 0, SD, CH, OH, LT, CP, RS, FM };

const char *kNames[kNumTracks]
    = {"BD", "SD", "CH", "OH", "LT", "CP", "RS", "FM", "C1", "C2", "C3", "C4"};

struct Lk
{
    ParamId id;
    float   value; ///< 0..1, same range the knobs use
};

/// velocity 0 = rest. micro is in ticks at 96 PPQN: 24 ticks is one step, so
/// a few ticks is the few-milliseconds nudge that makes a groove sit.
struct Hit
{
    int             step;
    int             velocity;
    int             micro;
    std::vector<Lk> locks = {};
};

struct TrackDef
{
    int              track;
    int              length;
    std::vector<Hit> hits;
};

// The micro offsets here are the point. The snare drags a little behind the
// grid, the offbeat hats push slightly ahead of it: the classic "laid back
// backbeat over an urgent hat" feel, which is unreachable with a plain grid.
const std::vector<TrackDef> kSong = {
    {BD, 16, {{0, 110, 0}, {6, 80, 0}, {10, 110, 0}}},
    {SD, 16, {{4, 115, +3}, {12, 115, +3}, {15, 60, +3}}},
    {CH, 16, {{0, 95, 0}, {2, 65, -2}, {4, 95, 0}, {6, 65, -2},
              {8, 95, 0}, {10, 65, -2}, {12, 95, 0}, {14, 65, -2}}},
    {OH, 16, {{14, 90, 0}}},
    {LT, 16, {{8, 100, 0}}},
    {CP, 16, {{4, 110, +3}}},
    // A 7-step rim against everything else's 16: polymeter for free.
    {RS, 7, {{3, 70, 0}}},
    // Parameter locks doing the thing they exist for: one EFM voice covering
    // four different metallic percussion sounds, because TONE (ratio) and SNAP
    // (index) are locked per step. Without locks this needs four tracks.
    {FM, 16, {{1,  85, 0, {{ParamId::Tune, 0.30f}, {ParamId::Tone, 0.62f}}},
              {5,  70, 0, {{ParamId::Tune, 0.52f}, {ParamId::Tone, 0.88f},
                           {ParamId::Snap, 0.90f}}},
              {9,  85, 0, {{ParamId::Tune, 0.30f}, {ParamId::Tone, 0.62f}}},
              {13, 98, 0, {{ParamId::Tune, 0.20f}, {ParamId::Tone, 0.44f},
                           {ParamId::Snap, 0.80f}, {ParamId::Decay, 0.55f}}}}},
};

Pattern BuildPattern()
{
    Pattern p;
    p.bpm_x10 = static_cast<uint16_t>(kBpm * 10.f);
    p.swing   = 50;

    for(auto &t : p.tracks)
    {
        t.muted  = true;
        t.length = 16;
    }

    for(const auto &def : kSong)
    {
        Track &t = p.tracks[def.track];
        t.muted  = false;
        t.length = static_cast<uint8_t>(def.length);
        for(const Hit &h : def.hits)
        {
            Step &s      = t.steps[h.step];
            s.flags      = kStepActive;
            s.velocity   = static_cast<uint8_t>(h.velocity);
            s.micro      = static_cast<int8_t>(h.micro);
            s.ratchet    = 1;
            s.probability = 100;

            s.lock_count = 0;
            for(const Lk &l : h.locks)
            {
                if(s.lock_count >= kMaxLocks)
                    break;
                s.locks[s.lock_count].param_id = static_cast<uint8_t>(l.id);
                s.locks[s.lock_count].value
                    = static_cast<uint16_t>(l.value * 65535.f + 0.5f);
                ++s.lock_count;
            }
        }
    }
    return p;
}

int SelfTest()
{
    BassDrum bd; SnareDrum sd; ClosedHat ch; OpenHat oh;
    Tom lt; Clap cp; RimShot rs; FmVoice fm;
    IVoice *voices[] = {&bd, &sd, &ch, &oh, &lt, &cp, &rs, &fm};

    // Digital voices only: an empty cartridge slot is silent by
    // construction, so there is nothing here for it to fail.
    int failures = 0;
    for(int v = 0; v < kNumDigitalVoices; ++v)
    {
        voices[v]->Init(kSampleRate);
        for(int pp = 0; pp < static_cast<int>(ParamId::Count); ++pp)
            voices[v]->SetParam(static_cast<ParamId>(pp), 0.5f);

        float peak = 0.f;
        for(int i = 0; i < 48000; ++i)
            peak = std::fmax(peak, std::fabs(voices[v]->Process()));

        const bool ok = peak < 1e-6f;
        std::printf("  %-3s untriggered peak %.8f  %s\n", kNames[v], peak,
                    ok ? "silent" : "*** DRONES ***");
        failures += ok ? 0 : 1;
    }
    std::printf("%s\n", failures ? "SELFTEST FAILED" : "selftest passed");
    return failures;
}

} // namespace

int main(int argc, char **argv)
{
    const char *out   = "drom-triks.wav";
    bool        trace = false;
    int         solo  = -1;

    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--selftest") == 0)
            return SelfTest();
        else if(std::strcmp(argv[i], "--trace") == 0)
            trace = true;
        else if(std::strcmp(argv[i], "--solo") == 0 && i + 1 < argc)
            solo = std::atoi(argv[++i]);
        else if(argv[i][0] != '-')
            out = argv[i];
    }

    BassDrum bd; SnareDrum sd; ClosedHat ch; OpenHat oh;
    Tom lt; Clap cp; RimShot rs; FmVoice fm;
    // Twelve slots, matching the firmware: the upper four are cartridge
    // slots with nothing plugged in. Rendering the same shape the hardware
    // runs is the point of this tool.
    EmptySlot cart[kNumCartridgeSlots];
    IVoice   *voices[kNumTracks] = {&bd, &sd, &ch, &oh, &lt, &cp, &rs, &fm};
    for(int i = 0; i < kNumCartridgeSlots; ++i)
        voices[kNumDigitalVoices + i] = &cart[i];
    VoiceSlot slots[kNumTracks];
    for(int i = 0; i < kNumTracks; ++i)
        slots[i].Init(voices[i], kSampleRate);

    // Through the slot, not the voice: the slot owns the base value a lock
    // restores to. Writing the voice directly would make locks restore to
    // whatever was set at Init.
    auto set = [&](int t, ParamId p, float v) { slots[t].SetBase(p, v); };
    set(BD, ParamId::Tune, 0.20f); set(BD, ParamId::Decay, 0.65f);
    set(BD, ParamId::Tone, 0.35f); set(BD, ParamId::Snap,  0.55f);
    set(BD, ParamId::Drive, 0.30f); set(BD, ParamId::Level, 0.90f);

    set(SD, ParamId::Tune, 0.35f); set(SD, ParamId::Decay, 0.40f);
    set(SD, ParamId::Tone, 0.55f); set(SD, ParamId::Snap,  0.60f);
    set(SD, ParamId::Drive, 0.20f); set(SD, ParamId::Level, 0.70f);

    set(CH, ParamId::Tune, 0.55f); set(CH, ParamId::Decay, 0.12f);
    set(CH, ParamId::Tone, 0.70f); set(CH, ParamId::Snap,  0.50f);
    set(CH, ParamId::Drive, 0.0f); set(CH, ParamId::Level, 0.60f);

    set(OH, ParamId::Tune, 0.50f); set(OH, ParamId::Decay, 0.55f);
    set(OH, ParamId::Tone, 0.65f); set(OH, ParamId::Snap,  0.55f);
    set(OH, ParamId::Drive, 0.0f); set(OH, ParamId::Level, 0.45f);

    set(LT, ParamId::Tune, 0.25f); set(LT, ParamId::Decay, 0.62f);
    set(LT, ParamId::Tone, 0.55f); set(LT, ParamId::Snap,  0.50f);
    set(LT, ParamId::Drive, 0.25f); set(LT, ParamId::Level, 0.65f);

    set(CP, ParamId::Tune, 0.45f); set(CP, ParamId::Decay, 0.66f);
    set(CP, ParamId::Tone, 0.55f); set(CP, ParamId::Snap,  0.35f);
    set(CP, ParamId::Drive, 0.15f); set(CP, ParamId::Level, 0.55f);

    set(RS, ParamId::Tune, 0.40f); set(RS, ParamId::Decay, 0.15f);
    set(RS, ParamId::Tone, 0.50f); set(RS, ParamId::Snap,  0.45f);
    set(RS, ParamId::Drive, 0.10f); set(RS, ParamId::Level, 0.45f);

    // EFM territory: inharmonic ratio, high index that collapses fast, and
    // enough DRIVE for operator feedback plus a little bit reduction.
    set(FM, ParamId::Tune, 0.30f); set(FM, ParamId::Decay, 0.32f);
    set(FM, ParamId::Tone, 0.62f); set(FM, ParamId::Snap,  0.78f);
    set(FM, ParamId::Drive, 0.38f); set(FM, ParamId::Level, 0.55f);

    Pattern   pattern = BuildPattern();
    Sequencer seq;
    seq.Init(kSampleRate);
    seq.SetPattern(&pattern);
    seq.Start();

    const double samples_per_step = (60.0 / kBpm) * kSampleRate / 4.0;
    const int    total_samples
        = static_cast<int>(samples_per_step * 16 * kBars) + static_cast<int>(kSampleRate);
    const int total_blocks = total_samples / static_cast<int>(kBlock);

    std::vector<float> audio;
    audio.reserve(static_cast<size_t>(total_blocks) * kBlock * 2);

    Sequencer::Event events[32];
    for(int b = 0; b < total_blocks; ++b)
    {
        const size_t n = seq.Process(kBlock, events, 32);
        for(size_t e = 0; e < n; ++e)
        {
            const Sequencer::Event &ev = events[e];
            if(solo >= 0 && ev.track != solo)
                continue;
            slots[ev.track].Schedule(ev.offset, ev.velocity,
                                     ev.step ? ev.step->locks : nullptr,
                                     ev.step ? ev.step->lock_count : 0);
            if(trace)
            {
                const long abs = static_cast<long>(b) * kBlock + ev.offset;
                std::printf("%8ld  %-3s vel %.2f  micro %+3d  locks %d\n",
                            abs, kNames[ev.track], ev.velocity,
                            ev.step ? ev.step->micro : 0,
                            ev.step ? ev.step->lock_count : 0);
            }
        }

        for(size_t i = 0; i < kBlock; ++i)
        {
            float mix = 0.f;
            for(int t = 0; t < kNumTracks; ++t)
                mix += slots[t].Process();
            mix *= 0.35f;
            audio.push_back(mix);
            audio.push_back(mix);
        }
    }

    if(solo >= 0 && solo < kNumTracks)
        std::printf("solo: %s\n", kNames[solo]);
    if(!host::WriteWav(out, audio, static_cast<uint32_t>(kSampleRate)))
    {
        std::fprintf(stderr, "could not write %s\n", out);
        return 1;
    }
    std::printf("wrote %s — %.1f s @ %.0f BPM, %zu-sample blocks\n",
                out, static_cast<double>(audio.size() / 2) / kSampleRate,
                static_cast<double>(kBpm), kBlock);
    return 0;
}
