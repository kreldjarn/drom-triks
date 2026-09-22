// UI state machine and patch format verification.
//
// The interaction model is testable headless, which is worth exploiting: the
// awkward cases here (soft takeover, p-lock-vs-toggle on the same key) are the
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

Patch MakePatch()
{
    Patch p;
    InitPatch(p);
    for(auto &t : p.pattern.tracks)
        t.length = 16;
    return p;
}

} // namespace

int main()
{
    std::printf("patch format:\n");
    {
        Patch p = MakePatch();
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
        Patch p = MakePatch();
        Ui ui; ui.Init(&p);

        Check(!ui.step_active(3), "step starts inactive");
        ui.StepPress(3); ui.StepRelease(3);
        Check(ui.step_active(3), "press+release toggles it on");
        ui.StepPress(3); ui.StepRelease(3);
        Check(!ui.step_active(3), "again toggles it off");
    }

    std::printf("\nsoft takeover (six knobs, eight voices):\n");
    {
        Patch p = MakePatch();
        p.kit.params[0][0] = 0.20f;   // track 0 TUNE
        p.kit.params[1][0] = 0.80f;   // track 1 TUNE
        Ui ui; ui.Init(&p);

        // Knob physically at 0.20, matching track 0.
        ui.PotMove(0, 0.20f);
        Check(ui.pot_caught(0), "knob catches when it matches the stored value");
        ui.PotMove(0, 0.30f);
        Check(std::fabs(p.kit.params[0][0] - 0.30f) < 1e-5f, "and then follows the knob");

        ui.TrackPress(1);
        Check(!ui.pot_caught(0), "changing track releases every pot");

        // Knob is still at 0.30; track 1 holds 0.80. Moving below must not grab.
        ui.PotMove(0, 0.35f);
        Check(std::fabs(p.kit.params[1][0] - 0.80f) < 1e-5f,
              "moving the knob below the stored value changes nothing");
        ui.PotMove(0, 0.60f);
        Check(std::fabs(p.kit.params[1][0] - 0.80f) < 1e-5f, "still nothing at 0.60");

        ui.PotMove(0, 0.85f);   // crosses 0.80
        Check(ui.pot_caught(0), "catches on crossing the stored value");
        Check(std::fabs(p.kit.params[1][0] - 0.85f) < 1e-5f, "and takes over from there");

        Check(std::fabs(p.kit.params[0][0] - 0.30f) < 1e-5f,
              "track 0's value was never disturbed");
    }

    std::printf("\nparameter locks from the panel:\n");
    {
        Patch p = MakePatch();
        p.kit.params[0][0] = 0.50f;
        Ui ui; ui.Init(&p);
        ui.PotMove(0, 0.50f);            // catch the pot

        ui.StepPress(4);
        ui.PotMove(0, 0.90f);            // hold step + turn knob
        ui.StepRelease(4);

        const Step &s = p.pattern.tracks[0].steps[4];
        Check(s.lock_count == 1, "holding a step and turning a pot writes one lock");
        Check(std::fabs(s.locks[0].as_float() - 0.90f) < 0.001f, "with the knob's value");
        Check(std::fabs(p.kit.params[0][0] - 0.50f) < 1e-5f,
              "and does NOT move the track's base value");
        Check(s.active(), "locking an inactive step activates it");

        // Writing a lock must not also toggle the step off on release.
        Check(s.active(), "releasing after a lock does not toggle the step off");

        // Same param again should update in place, not append.
        ui.StepPress(4); ui.PotMove(0, 0.10f); ui.StepRelease(4);
        Check(p.pattern.tracks[0].steps[4].lock_count == 1,
              "re-locking the same parameter updates in place");

        // A different param appends.
        ui.StepPress(4); ui.PotMove(1, 0.5f); ui.PotMove(1, 0.7f); ui.StepRelease(4);
        Check(p.pattern.tracks[0].steps[4].lock_count == 2, "a second parameter appends");

        // Clearing the step clears its locks.
        ui.StepPress(4); ui.StepRelease(4);   // toggles off
        Check(!p.pattern.tracks[0].steps[4].active(), "step toggled off");
        Check(p.pattern.tracks[0].steps[4].lock_count == 0,
              "clearing a step clears its locks rather than hiding them");
    }

    std::printf("\nmute mode:\n");
    {
        Patch p = MakePatch();
        Ui ui; ui.Init(&p);
        ui.TrackPress(3);
        Check(ui.selected_track() == 3, "track keys select in Play mode");

        ui.SetMode(Ui::Mode::Mute);
        ui.TrackPress(3);
        Check(ui.track_muted(3), "track keys mute in Mute mode");
        Check(ui.selected_track() == 3, "and do not change the selection");
        ui.TrackPress(3);
        Check(!ui.track_muted(3), "pressing again unmutes");
    }

    std::printf("\n%s\n", failures ? "UI TESTS FAILED" : "all UI tests passed");
    return failures;
}
