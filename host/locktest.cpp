// Parameter-lock verification.
//
// The failure mode that matters is a lock *leaking*: a locked value that is
// never restored makes the p-lock feel like it permanently moved the knob,
// and it is invisible until you listen to the step after the locked one. So
// these tests watch every SetParam call rather than the audio.

#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/seq/sequencer.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what, float got, float want)
{
    std::printf("  %-52s got %.3f  want %.3f  %s\n", what, got, want,
                ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

/// Records what the sequencer actually does to a voice.
class MockVoice : public IVoice
{
  public:
    void  Init(float) override {}
    void  Trigger(float v) override { triggers.push_back({v, current[0]}); }
    float Process() override { return 0.f; }
    void  SetParam(ParamId id, float v) override
    {
        current[static_cast<int>(id)] = v;
        ++set_calls;
    }

    struct Fired { float velocity; float tune_at_trigger; };
    std::vector<Fired> triggers;
    float current[static_cast<int>(ParamId::Count)] = {};
    int   set_calls = 0;
};

ParamLock Lock(ParamId id, float v)
{
    return ParamLock{static_cast<uint8_t>(id),
                     static_cast<uint16_t>(v * 65535.f + 0.5f)};
}

} // namespace

int main()
{
    std::printf("parameter locks:\n");

    // --- a lock applies, and does not leak into the next step --------------
    {
        MockVoice mv;
        VoiceSlot slot;
        slot.Init(&mv, 48000.f);
        slot.SetBase(ParamId::Tune, 0.25f);

        const ParamLock locked[] = {Lock(ParamId::Tune, 0.90f)};

        slot.Schedule(0, 1.f);                 // step A: no locks
        slot.Process();
        slot.Schedule(0, 1.f, locked, 1);      // step B: TUNE locked high
        slot.Process();
        slot.Schedule(0, 1.f);                 // step C: no locks again
        slot.Process();

        Check(mv.triggers.size() == 3, "three triggers fired",
              static_cast<float>(mv.triggers.size()), 3);
        if(mv.triggers.size() == 3)
        {
            Check(mv.triggers[0].tune_at_trigger == 0.25f,
                  "step A uses the base value", mv.triggers[0].tune_at_trigger, 0.25f);
            Check(std::abs(mv.triggers[1].tune_at_trigger - 0.90f) < 0.001f,
                  "step B uses the locked value", mv.triggers[1].tune_at_trigger, 0.90f);
            Check(mv.triggers[2].tune_at_trigger == 0.25f,
                  "step C is restored, the lock did not leak",
                  mv.triggers[2].tune_at_trigger, 0.25f);
        }
    }

    // --- unlocked steps must not cost any SetParam work -------------------
    {
        MockVoice mv;
        VoiceSlot slot;
        slot.Init(&mv, 48000.f);
        slot.SetBase(ParamId::Tune, 0.5f);
        const int after_setup = mv.set_calls;

        for(int i = 0; i < 10; ++i)
        {
            slot.Schedule(0, 1.f);
            slot.Process();
        }
        Check(mv.set_calls == after_setup, "10 unlocked steps cause no param writes",
              static_cast<float>(mv.set_calls - after_setup), 0);
    }

    // --- a knob move while a lock is held must survive the restore --------
    {
        MockVoice mv;
        VoiceSlot slot;
        slot.Init(&mv, 48000.f);
        slot.SetBase(ParamId::Decay, 0.2f);

        const ParamLock locked[] = {Lock(ParamId::Decay, 0.95f)};
        slot.Schedule(0, 1.f, locked, 1);
        slot.Process();                        // Decay now locked at 0.95

        slot.SetBase(ParamId::Decay, 0.7f);    // user turns the knob meanwhile
        Check(std::abs(mv.current[static_cast<int>(ParamId::Decay)] - 0.95f) < 0.001f,
              "knob move does not fight the held lock",
              mv.current[static_cast<int>(ParamId::Decay)], 0.95f);

        slot.Schedule(0, 1.f);                 // next unlocked step
        slot.Process();
        Check(std::abs(mv.current[static_cast<int>(ParamId::Decay)] - 0.7f) < 0.001f,
              "restore uses the NEW base, not the old one",
              mv.current[static_cast<int>(ParamId::Decay)], 0.7f);
    }

    // --- multiple locks on one step ---------------------------------------
    {
        MockVoice mv;
        VoiceSlot slot;
        slot.Init(&mv, 48000.f);
        slot.SetBase(ParamId::Tune, 0.1f);
        slot.SetBase(ParamId::Decay, 0.2f);
        slot.SetBase(ParamId::Tone, 0.3f);

        const ParamLock locks[] = {Lock(ParamId::Tune, 0.8f),
                                   Lock(ParamId::Decay, 0.9f),
                                   Lock(ParamId::Tone, 1.0f)};
        slot.Schedule(0, 1.f, locks, 3);
        slot.Process();
        Check(std::abs(mv.current[0] - 0.8f) < 0.001f, "three locks applied (Tune)",
              mv.current[0], 0.8f);

        slot.Schedule(0, 1.f);
        slot.Process();
        const bool all_back = std::abs(mv.current[0] - 0.1f) < 0.001f
                              && std::abs(mv.current[1] - 0.2f) < 0.001f
                              && std::abs(mv.current[2] - 0.3f) < 0.001f;
        Check(all_back, "all three restored together", all_back ? 1.f : 0.f, 1.f);
    }

    // --- locks reach the voice through the sequencer, end to end ----------
    {
        Pattern p;
        for(auto &t : p.tracks)
        {
            t.muted  = true;
            t.length = 16;
        }
        p.tracks[0].muted = false;
        p.tracks[0].length = 2;
        Step &a = p.tracks[0].steps[0];
        Step &b = p.tracks[0].steps[1];
        a.flags = kStepActive;
        b.flags = kStepActive;
        b.lock_count = 1;
        b.locks[0]   = Lock(ParamId::Tune, 0.77f);

        Sequencer seq;
        seq.Init(48000.f);
        seq.SetPattern(&p);
        seq.Start();

        MockVoice mv;
        VoiceSlot slot;
        slot.Init(&mv, 48000.f);
        slot.SetBase(ParamId::Tune, 0.11f);

        Sequencer::Event ev[16];
        for(int blk = 0; blk < 800; ++blk)
        {
            const size_t n = seq.Process(32, ev, 16);
            for(size_t i = 0; i < n; ++i)
                slot.Schedule(ev[i].offset, ev[i].velocity,
                              ev[i].step ? ev[i].step->locks : nullptr,
                              ev[i].step ? ev[i].step->lock_count : 0);
            for(int s = 0; s < 32; ++s)
                slot.Process();
        }

        bool alternates = mv.triggers.size() >= 4;
        for(size_t i = 0; i < mv.triggers.size() && alternates; ++i)
        {
            const float want = (i % 2 == 0) ? 0.11f : 0.77f;
            if(std::abs(mv.triggers[i].tune_at_trigger - want) > 0.001f)
                alternates = false;
        }
        Check(alternates, "sequencer alternates base / locked across steps",
              alternates ? 1.f : 0.f, 1.f);
    }

    std::printf("\n%s\n", failures ? "LOCK TESTS FAILED" : "all lock tests passed");
    return failures;
}
