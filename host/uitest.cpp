// UI state machine and patch format verification.
//
// The interaction model is testable headless, which is worth exploiting: the
// awkward cases here (encoder acceleration, p-lock-vs-toggle on the same key) are the
// ones that feel wrong under the fingers and are tedious to diagnose on a
// panel with 30 keys.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/ui/ui.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

/// The UI emits commands; only the audio side applies them. Tests therefore
/// have to pump the machine, which is the real path and worth exercising.
void Pump(Machine &m)
{
    float buf[32];
    m.Process(buf, 32);
}

} // namespace

int main()
{
    std::printf("patch format:\n");
    {
        Patch p;
        InitPatch(p);
        Check(ValidatePatch(p), "a fresh patch validates");

        // Round trip through raw bytes, which is what QSPI persistence does.
        unsigned char buf[sizeof(Patch)];
        std::memcpy(buf, &p, sizeof(Patch));
        Patch back;
        std::memcpy(&back, buf, sizeof(Patch));
        Check(ValidatePatch(back), "survives a byte-for-byte round trip");

        Patch older = p;
        older.header.version = kPatchVersion - 1;
        Check(!ValidatePatch(older), "an older version is rejected, not reinterpreted");

        Patch resized = p;
        resized.header.payload_size = 1;
        Check(!ValidatePatch(resized), "a size mismatch is rejected");

        Patch garbage{};
        Check(!ValidatePatch(garbage), "uninitialised flash is rejected");
    }

    std::printf("\nstep editing:\n");
    {
        static Machine m; m.Init(48000.f);
        Ui ui; ui.Init(&m);

        Check(!ui.step_active(3), "step starts inactive");
        ui.StepPress(3); ui.StepRelease(3); Pump(m);
        Check(ui.step_active(3), "press+release toggles it on");
        ui.StepPress(3); ui.StepRelease(3); Pump(m);
        Check(!ui.step_active(3), "again toggles it off");
    }

    std::printf("\nmacro encoders (six knobs, twelve tracks):\n");
    {
        static Machine m; m.Init(48000.f);
        m.mutable_patch().kit.params[0][0] = 0.20f;   // track 0 TUNE
        m.mutable_patch().kit.params[1][0] = 0.80f;   // track 1 TUNE
        Ui ui; ui.Init(&m);
        const Patch &p = m.patch();

        ui.SetTime(1000);
        ui.EncoderTurn(0, +1); Pump(m);
        Check(p.kit.params[0][0] > 0.20f, "a detent moves the selected track's parameter");
        const float moved = p.kit.params[0][0];

        // The whole reason for encoders: no pickup sweep after a track change.
        ui.SetTime(2000);
        ui.TrackPress(1);
        ui.EncoderTurn(0, +1); Pump(m);
        Check(p.kit.params[1][0] > 0.80f, "the next detent addresses the new track immediately");
        Check(std::fabs(p.kit.params[0][0] - moved) < 1e-6f,
              "and the previous track's value is untouched");

        // An endless encoder has no end stop, so the range must provide one.
        ui.SetTime(3000);
        ui.EncoderTurn(0, +5000); Pump(m);
        Check(p.kit.params[1][0] <= 1.f, "turning past the top clamps rather than wrapping");
        ui.SetTime(4000);
        ui.EncoderTurn(0, -5000); Pump(m);
        Check(p.kit.params[1][0] >= 0.f, "and past the bottom too");
    }

    std::printf("\nencoder acceleration:\n");
    {
        static Machine m; m.Init(48000.f);
        Ui ui; ui.Init(&m);
        const Patch &p = m.patch();

        // Ten deliberate detents, 100 ms apart.
        m.mutable_patch().kit.params[0][0] = 0.50f;
        uint32_t t = 1000;
        for(int i = 0; i < 10; ++i) { ui.SetTime(t); ui.EncoderTurn(0, +1); t += 100; }
        Pump(m);
        const float slow = p.kit.params[0][0] - 0.50f;

        // The same ten detents as a spin.
        m.mutable_patch().kit.params[0][0] = 0.50f;
        Ui ui2; ui2.Init(&m);
        t = 20000;
        for(int i = 0; i < 10; ++i) { ui2.SetTime(t); ui2.EncoderTurn(0, +1); t += 2; }
        Pump(m);
        const float fast = p.kit.params[0][0] - 0.50f;

        std::printf("      10 detents slow %.4f, spun %.4f\n", slow, fast);
        Check(slow > 0.f, "a slow turn still moves the parameter");
        Check(fast > slow * 4.f,
              "a spin covers far more ground than the same detents deliberately");
    }

    std::printf("\nparameter locks from the panel:\n");
    {
        static Machine m; m.Init(48000.f);
        m.mutable_patch().kit.params[0][0] = 0.50f;
        Ui ui; ui.Init(&m);
        const Patch &p = m.patch();
        ui.SetTime(1000);

        ui.StepPress(4);
        ui.EncoderTurn(0, +8);           // hold step + nudge, no sweep needed
        ui.StepRelease(4); Pump(m);

        const Step &s = p.pattern.tracks[0].steps[4];
        Check(s.lock_count == 1, "holding a step and turning a macro writes one lock");
        Check(s.locks[0].as_float() > 0.50f, "nudged up from the track's value");
        const float first_lock = s.locks[0].as_float();
        Check(std::fabs(p.kit.params[0][0] - 0.50f) < 1e-5f,
              "and does NOT move the track's base value");
        Check(s.active(), "locking an inactive step activates it");

        // Writing a lock must not also toggle the step off on release.
        Check(s.active(), "releasing after a lock does not toggle the step off");

        // Same param again: updates in place, and resumes from the lock rather
        // than snapping back to the track's base first.
        ui.SetTime(3000);
        ui.StepPress(4); ui.EncoderTurn(0, -4); ui.StepRelease(4); Pump(m);
        Check(p.pattern.tracks[0].steps[4].lock_count == 1,
              "re-locking the same parameter updates in place");
        const float second_lock = p.pattern.tracks[0].steps[4].locks[0].as_float();
        Check(second_lock < first_lock && second_lock > 0.50f,
              "and continues from the existing lock, not from the track's base");

        // A different param appends.
        ui.SetTime(5000);
        ui.StepPress(4); ui.EncoderTurn(1, +2); ui.StepRelease(4); Pump(m);
        Check(p.pattern.tracks[0].steps[4].lock_count == 2, "a second parameter appends");

        // Clearing the step clears its locks.
        ui.StepPress(4); ui.StepRelease(4); Pump(m);   // toggles off
        Check(!p.pattern.tracks[0].steps[4].active(), "step toggled off");
        Check(p.pattern.tracks[0].steps[4].lock_count == 0,
              "clearing a step clears its locks rather than hiding them");
    }

    std::printf("\nmute mode:\n");
    {
        static Machine m; m.Init(48000.f);
        Ui ui; ui.Init(&m);
        ui.TrackPress(3);
        Check(ui.selected_track() == 3, "track keys select in Play mode");

        ui.SetMode(Ui::Mode::Mute);
        ui.TrackPress(3); Pump(m);
        Check(ui.track_muted(3), "track keys mute in Mute mode");
        Check(ui.selected_track() == 3, "and do not change the selection");
        ui.TrackPress(3); Pump(m);
        Check(!ui.track_muted(3), "pressing again unmutes");
    }

    std::printf("\n%s\n", failures ? "UI TESTS FAILED" : "all UI tests passed");
    return failures;
}
