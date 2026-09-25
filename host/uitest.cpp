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
    float buf[64]; // 32 frames, interleaved stereo
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

    std::printf("\nnavigation encoders:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        ui.NavTurn(1, 1);
        Check(ui.page() == 1, "nav 1 moves to the next page");
        ui.NavTurn(1, 2);
        Check(ui.page() == 3, "and keeps going");
        ui.NavTurn(1, 5);
        Check(ui.page() == 3, "but stops at the last page rather than wrapping");
        ui.NavTurn(1, -99);
        Check(ui.page() == 0, "and at the first going the other way");

        const float before = m.state().tempo.load(std::memory_order_relaxed);
        ui.NavTurn(0, 5);
        Pump(m);
        Check(m.state().tempo.load(std::memory_order_relaxed) > before,
              "nav 0 changes the tempo");
    }

    std::printf("\nSHIFT puts the macros on master FX:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        ui.TransportPress(Ui::Key::Shift);
        ui.SetTime(1100);
        ui.EncoderTurn(1, 4); // bank 0, slot 1 = DelayFeedback
        Pump(m);
        Check(m.patch().kit.fx[static_cast<int>(FxId::DelayFeedback)] > 0.35f,
              "a macro writes a master FX parameter");
        Check(m.patch().kit.params[0][static_cast<int>(ParamId::Decay)] == 0.5f,
              "and leaves the per-track parameter alone");

        // The page encoder picks the FX bank while SHIFT is down.
        ui.NavTurn(1, 1);
        Check(ui.fx_bank() == 1, "nav 1 selects the second FX bank");
        Check(ui.page() == 0, "without moving the page underneath");
        ui.SetTime(1200);
        // Downward: CompThreshold defaults to 1.0, so turning up just clamps.
        ui.EncoderTurn(0, -4); // bank 1, slot 0 = CompThreshold
        Pump(m);
        Check(m.patch().kit.fx[static_cast<int>(FxId::CompThreshold)] != 1.0f,
              "and bank 1 reaches the compressor");

        ui.TransportRelease(Ui::Key::Shift);
        ui.SetTime(1300);
        ui.EncoderTurn(1, 4);
        Pump(m);
        Check(m.patch().kit.params[0][static_cast<int>(ParamId::Decay)] != 0.5f,
              "releasing SHIFT hands the macros back to the track");
    }

    std::printf("\nSHIFT + a held step is step detail:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        ui.StepPress(4);
        ui.TransportPress(Ui::Key::Shift);
        ui.SetTime(1100);
        ui.EncoderTurn(static_cast<int>(StepField::Probability), -20);
        Pump(m);
        Check(m.patch().pattern.tracks[0].steps[4].probability < 100,
              "a macro edits that step's probability");
        Check(m.patch().kit.fx[static_cast<int>(FxId::ReverbSize)] == kFxDefault[4],
              "and not master FX, even though SHIFT is held");

        // Step detail is not a p-lock, so releasing still toggles the step —
        // you held it to edit it, not to turn it off.
        ui.TransportRelease(Ui::Key::Shift);
        ui.StepRelease(4);
        Pump(m);
        Check(m.patch().pattern.tracks[0].steps[4].active(),
              "and the step still toggles on release");
    }

    std::printf("\npush to default:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        ui.EncoderTurn(0, 30); // TUNE well away from its default
        Pump(m);
        Check(m.patch().kit.params[0][0] != 0.5f, "a macro moves off the default");
        ui.EncoderPush(0);
        Pump(m);
        Check(m.patch().kit.params[0][0] == 0.5f, "and the push restores it");
    }

    std::printf("\ntransport:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        Check(!m.state().playing.load(std::memory_order_relaxed), "starts stopped");
        ui.TransportPress(Ui::Key::Play); Pump(m);
        Check(m.state().playing.load(std::memory_order_relaxed), "PLAY starts it");
        ui.TransportPress(Ui::Key::Play); Pump(m);
        Check(!m.state().playing.load(std::memory_order_relaxed), "and stops it again");

        Check(!ui.rec_armed(), "REC starts disarmed");
        ui.TransportPress(Ui::Key::Rec);
        Check(ui.rec_armed(), "and arms");
    }

    std::printf("\ntap tempo:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m);

        // Four taps 500 ms apart is 120 BPM.
        for(uint32_t i = 0; i < 4; ++i)
        {
            ui.SetTime(10000 + i * 500);
            ui.TransportPress(Ui::Key::Tap);
        }
        Pump(m);
        const float bpm = m.state().tempo.load(std::memory_order_relaxed);
        Check(std::fabs(bpm - 120.f) < 1.f, "four taps at 500 ms give 120 BPM");

        // A long pause restarts the average rather than folding the rest in.
        ui.SetTime(30000);
        ui.TransportPress(Ui::Key::Tap);
        Pump(m);
        Check(std::fabs(m.state().tempo.load(std::memory_order_relaxed) - bpm) < 0.01f,
              "and a long gap is discarded, not averaged in");
    }

    std::printf("\nPATTERN mode asks the main loop for flash work:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        Check(ui.TakeStorageRequest().type == Ui::StorageRequest::Type::None,
              "nothing pending to begin with");

        ui.TransportPress(Ui::Key::Patt);
        ui.StepPress(6);
        auto r = ui.TakeStorageRequest();
        Check(r.type == Ui::StorageRequest::Type::LoadPattern && r.slot == 6,
              "a step key loads that slot");
        Check(ui.TakeStorageRequest().type == Ui::StorageRequest::Type::None,
              "and taking it clears it");

        ui.TransportPress(Ui::Key::Shift);
        ui.StepPress(6);
        r = ui.TakeStorageRequest();
        Check(r.type == Ui::StorageRequest::Type::SavePattern,
              "with SHIFT it saves instead");
        ui.TransportRelease(Ui::Key::Shift);

        // 16 keys over 128 slots, so the value encoder banks them.
        ui.NavTurn(0, 3);
        Check(ui.pattern_bank() == 3, "nav 0 selects the pattern bank");
        ui.StepPress(2);
        r = ui.TakeStorageRequest();
        Check(r.slot == 3 * 16 + 2, "and the slot is bank * 16 + key");

        // A step key must not toggle a step while PATTERN is held.
        Check(!m.patch().pattern.tracks[0].steps[2].active(),
              "and no step was toggled");

        ui.TransportRelease(Ui::Key::Patt);
        Check(ui.mode() == Ui::Mode::Play, "releasing PATT returns to play");
    }

    std::printf("\nkeyboard mode:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);
        ui.TrackPress(4);

        ui.SetKeyboard(true);
        Check(ui.mode() == Ui::Mode::Keyboard, "the step keys become a keyboard");
        Check(ui.KeyboardSemitone(0) == 0 && ui.KeyboardSemitone(7) == 7,
              "key N is N semitones up");

        // REC off: play it. The track's own NOTE follows what you audition.
        ui.StepPress(5);
        Pump(m);
        Check(NoteSemitones(
                  m.patch().kit.params[4][static_cast<int>(ParamId::Note)]) == 5,
              "with REC off a key auditions and sets the track's note");
        Check(!m.patch().pattern.tracks[4].steps[0].active(),
              "and writes nothing to the pattern");

        // REC on: write it. Cursor advances so a line goes in as fast as you
        // can play it.
        ui.TransportPress(Ui::Key::Rec);
        Check(ui.rec_armed(), "REC arms");
        Check(ui.keyboard_cursor() == 0, "the cursor starts at step 0");

        const int line[4] = {0, 3, 7, 10};
        for(int i = 0; i < 4; ++i)
            ui.StepPress(line[i]);
        Pump(m);
        Check(ui.keyboard_cursor() == 4, "four notes advance the cursor four steps");

        bool wrote = true;
        for(int i = 0; i < 4; ++i)
        {
            const Step &s = m.patch().pattern.tracks[4].steps[i];
            wrote &= s.active();
            bool found = false;
            for(uint8_t k = 0; k < s.lock_count; ++k)
                if(s.locks[k].param_id == static_cast<uint8_t>(ParamId::Note)
                   && NoteSemitones(s.locks[k].as_float()) == line[i])
                    found = true;
            wrote &= found;
        }
        Check(wrote, "each step is on and carries the note that was played");

        // Writing to a step that is already on must not turn it off — which is
        // why this is SetStepActive rather than ToggleStep.
        ui.StepPress(2);
        Pump(m);
        Check(m.patch().pattern.tracks[4].steps[4].active(),
              "and a note over an existing step leaves it on");

        // The octave moves the window rather than stretching it.
        ui.NavTurn(0, -1);
        Check(ui.keyboard_octave() == -1 && ui.KeyboardSemitone(0) == -12,
              "nav 0 shifts the keyboard an octave");
        ui.NavTurn(0, -9);
        Check(ui.keyboard_octave() == -2, "and clamps at the bottom");

        ui.SetKeyboard(false);
        Check(ui.mode() == Ui::Mode::Play, "leaving returns to play");
    }

    std::printf("\nNOTE turns in semitones:\n");
    {
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);

        // One detent, one semitone. The default 1/256 step would be five clicks
        // per semitone, all of them silent.
        const int note = 7; // the eighth macro on the INST page
        ui.EncoderTurn(note, 1);
        Pump(m);
        const float after_one
            = m.patch().kit.params[0][static_cast<int>(ParamId::Note)];
        Check(NoteSemitones(after_one) == 1, "one detent moves one semitone up");

        ui.SetTime(1100);
        ui.EncoderTurn(note, 11);
        Pump(m);
        Check(NoteSemitones(
                  m.patch().kit.params[0][static_cast<int>(ParamId::Note)]) == 12,
              "and eleven more reaches the octave exactly");

        ui.SetTime(1200);
        ui.EncoderTurn(note, -24);
        Pump(m);
        Check(NoteSemitones(
                  m.patch().kit.params[0][static_cast<int>(ParamId::Note)]) == -12,
              "and it goes down as cleanly as it goes up");
    }

    std::printf("\nwhat a knob says it is doing:\n");
    {
        // Anything that draws must agree with what a turn would write. Naming a
        // macro from the page alone was wrong the moment SHIFT gained layers:
        // the screen would name one parameter while the knob moved another.
        static Machine m; static Ui ui;
        m.Init(48000.f); ui.Init(&m); ui.SetTime(1000);
        auto Eq = [](const char *a, const char *b) { return std::strcmp(a, b) == 0; };

        Check(Eq(ui.MacroContext(), "INST"), "defaults to the INST page");
        Check(Eq(ui.MacroName(0), "TUNE"), "macro 0 is TUNE there");
        Check(Eq(ui.MacroName(7), "NOTE"), "and the eighth is NOTE, beside TUNE");

        ui.SetPage(2); // FX: mostly reserved, so the inactive label still shows
        Check(Eq(ui.MacroName(7), "--"), "a genuinely reserved slot still says so");
        ui.SetPage(0);

        ui.SetPage(1);
        Check(Eq(ui.MacroContext(), "FLTR"), "the filter page names itself");
        Check(Eq(ui.MacroName(0), "SAT"), "and macro 0 is the saturator");

        ui.TransportPress(Ui::Key::Shift);
        Check(Eq(ui.MacroContext(), "MASTER FX 1"), "SHIFT moves to master FX");
        Check(Eq(ui.MacroName(0), "DLY TIME"), "with the delay first");
        ui.NavTurn(1, 1);
        Check(Eq(ui.MacroContext(), "MASTER FX 2"), "and the second bank");
        Check(Eq(ui.MacroName(0), "CMP THR"), "reaches the compressor");

        ui.StepPress(3);
        Check(Eq(ui.MacroContext(), "STEP"), "SHIFT over a held step is step detail");
        Check(Eq(ui.MacroName(0), "VEL"), "starting with velocity");
        Check(Eq(ui.MacroName(6), "--"), "and only four of the eight are live");

        // The value has to track the same target, not just the name.
        ui.TransportRelease(Ui::Key::Shift);
        ui.StepRelease(3);
        ui.SetPage(0);
        Check(std::fabs(ui.MacroValue(5) - 0.8f) < 1e-6f,
              "MacroValue reads the target the name refers to");
    }

    std::printf("\n%s\n", failures ? "UI TESTS FAILED" : "all UI tests passed");
    return failures;
}
