// Desktop Dev Kit mapping verification.
//
// The kit folds 34 keys and 10 encoders onto 16 keys, 8 pots and two toggles,
// so most of what can go wrong is in the folding: a key released on a
// different layer than it was pressed on, a pot writing to a track it was
// never turned on, a held mode forgetting the keyboard. All of those feel like
// a flaky panel on hardware and are deterministic here.

#include <cmath>
#include <cstdio>

#include "../src/ui/devkit_panel.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-62s %s\n", what, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

void Pump(Machine &m)
{
    float buf[64];
    m.Process(buf, 32);
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

using Layer = DevkitPanel::Layer;

// ~21.5 kB each, so statics — the same shape the firmware uses.
Machine     g_m;
Ui          g_ui;
DevkitPanel g_p;
uint32_t    g_ms = 0;

/// Fresh machine, UI and panel, with every pot seeded at `pot_pos`.
void Reset(float pot_pos = 0.5f)
{
    g_m.Init(48000.f);
    g_ui.Init(&g_m);
    g_p.Init(&g_ui, &g_m);
    g_ms = 0;
    g_p.SetTime(g_ms);
    for(int i = 0; i < kDevkitPots; ++i)
        g_p.Pot(i, pot_pos);
}

void Tick(int ms = 1)
{
    g_ms += static_cast<uint32_t>(ms);
    g_p.SetTime(g_ms);
}

void Tap(int key)
{
    g_p.KeyDown(key);
    g_p.KeyUp(key);
    Pump(g_m);
}

float Param(int track, ParamId id)
{
    return g_m.patch().kit.params[track][static_cast<int>(id)];
}

const Step &StepOf(int track, int step)
{
    return g_m.patch().pattern.tracks[track].steps[step];
}

} // namespace

int main()
{
    std::printf("steps layer:\n");
    {
        Reset();
        Tap(5);
        Check(StepOf(0, 5).active(), "a tap toggles the step on");
        Tap(5);
        Check(!StepOf(0, 5).active(), "and off again");

        // Hold a step and turn: a lock, and the step must not toggle on release.
        // The firmware polls every pot each millisecond, so the hold is seen at
        // the resting position before any turn — which is what re-anchors it.
        g_p.KeyDown(9);
        g_p.Pot(0, 0.5f);
        Tick();
        g_p.Pot(0, 0.9f);
        Pump(g_m);
        g_p.KeyUp(9);
        Pump(g_m);
        const Step &s = StepOf(0, 9);
        Check(s.active() && s.lock_count == 1, "hold + pot writes a lock and keeps the step on");
        Check(Near(s.locks[0].as_float(), 0.9f), "the lock is where the pot points (jump)");
        Check(!Near(Param(0, ParamId::Tune), 0.9f), "the track's own value is untouched");
    }

    std::printf("pots:\n");
    {
        Reset(0.5f);
        const float before = Param(0, ParamId::Decay);
        g_p.Pot(1, 0.51f); // 1 %: under the engage threshold
        Pump(g_m);
        Check(Near(Param(0, ParamId::Decay), before), "a small wobble on a resting pot writes nothing");

        g_p.Pot(1, 0.7f);
        Pump(g_m);
        Check(Near(Param(0, ParamId::Decay), 0.7f), "a deliberate move jumps to the pot");

        g_p.Pot(1, 0.702f); // under one step once engaged: ignored
        g_p.Pot(1, 0.71f);
        Pump(g_m);
        Check(Near(Param(0, ParamId::Decay), 0.71f), "once engaged it follows at fine resolution");

        g_p.Pot(1, 0.999f);
        Pump(g_m);
        Check(Near(Param(0, ParamId::Decay), 1.f), "the end of travel snaps to exactly 1");

        // Change track: the pot's position says nothing about track 3's value.
        g_p.SetLayer(Layer::Tracks);
        Tap(3);
        const float t3 = Param(3, ParamId::Decay);
        Tick();
        g_p.Pot(1, 0.995f); // noise on the resting pot
        Pump(g_m);
        Check(Near(Param(3, ParamId::Decay), t3), "a track change does not write the old position");
        Check(Near(Param(0, ParamId::Decay), 1.f), "nor touch the previous track");

        g_p.Pot(1, 0.6f);
        Pump(g_m);
        Check(Near(Param(3, ParamId::Decay), 0.6f), "moving it after the change takes over");

        // Idle: re-arm the engage threshold.
        Tick(1000);
        g_p.Pot(1, 0.608f); // > one step but < engage, after a long rest
        Pump(g_m);
        Check(Near(Param(3, ParamId::Decay), 0.6f), "after resting, a wobble needs a real move again");
    }

    std::printf("layer changes:\n");
    {
        Reset();
        g_p.KeyDown(4); // hold step 5
        Check(g_ui.held_step() == 4, "the step is held");
        g_p.SetLayer(Layer::Tracks);
        g_p.KeyUp(4);   // released on the tracks layer
        Pump(g_m);
        Check(g_ui.held_step() == -1, "released with the meaning it was pressed with");
        Check(StepOf(0, 4).active(), "and so it toggled as a step, not selected a track");
        Check(g_ui.selected_track() == 0, "track selection is untouched");
    }

    std::printf("tracks layer:\n");
    {
        Reset();
        g_p.SetLayer(Layer::Tracks);
        Tap(10);
        Check(g_ui.selected_track() == 10, "key 11 selects track 11 (C3)");

        const MachineId was = g_m.machine(2);
        g_p.SetShift(true);
        Tap(2);
        g_p.SetShift(false);
        Check(g_m.machine(2) != was, "SHIFT + track key changes that track's machine");
        Check(g_ui.selected_track() == 2, "and selects it, so the pots follow");

        Tap(DevkitPanel::kKeyPlay);
        Check(g_m.state().playing.load(), "PLAY starts");
        Tap(DevkitPanel::kKeyPlay);
        Check(!g_m.state().playing.load(), "and stops");

        // PAGE: tap steps and wraps, hold + key picks.
        for(int i = 0; i < kNumPages; ++i)
            Tap(DevkitPanel::kKeyPage);
        Check(g_ui.page() == 0, "tapping PAGE four times wraps back to INST");
        g_p.KeyDown(DevkitPanel::kKeyPage);
        Tap(2);
        g_p.KeyUp(DevkitPanel::kKeyPage);
        Check(g_ui.page() == 2, "hold PAGE + key 3 jumps to FX, and the release does not step on");
        Check(g_ui.selected_track() == 2, "keys under a held PAGE are pages, not tracks");

        g_p.SetShift(true);
        Tap(DevkitPanel::kKeyPage);
        Check(g_ui.fx_bank() == 1, "with SHIFT, PAGE steps the master FX bank");
        Tap(DevkitPanel::kKeyPage);
        Check(g_ui.fx_bank() == 0, "and wraps");
        g_p.SetShift(false);
        Check(g_ui.page() == 2, "without disturbing the page");

        // MUTE is held.
        g_p.KeyDown(DevkitPanel::kKeyMute);
        Tap(1);
        g_p.KeyUp(DevkitPanel::kKeyMute);
        Pump(g_m);
        Check(g_m.patch().pattern.tracks[1].muted, "hold MUTE + track key mutes it");
        Check(g_ui.selected_track() == 2, "without selecting it");
    }

    std::printf("keyboard and held modes:\n");
    {
        Reset();
        g_p.SetLayer(Layer::Tracks);
        Tap(DevkitPanel::kKeyKbd);
        Check(g_ui.mode() == Ui::Mode::Keyboard, "KBD turns the step keys into a keyboard");

        g_p.KeyDown(DevkitPanel::kKeyMute);
        Check(g_ui.mode() == Ui::Mode::Mute, "holding MUTE is mute mode");
        g_p.KeyUp(DevkitPanel::kKeyMute);
        Check(g_ui.mode() == Ui::Mode::Keyboard, "releasing MUTE returns to the keyboard, not to steps");

        g_p.SetLayer(Layer::Patterns);
        g_p.SetLayer(Layer::Steps);
        Check(g_ui.mode() == Ui::Mode::Keyboard, "so does leaving the pattern layer");

        g_p.SetLayer(Layer::Tracks);
        g_p.SetShift(true);
        Tap(DevkitPanel::kKeyKbd);
        g_p.SetShift(false);
        Check(g_ui.rec_armed(), "SHIFT + KBD arms REC");
        Check(g_ui.mode() == Ui::Mode::Keyboard, "without turning the keyboard off");
    }

    std::printf("pattern layer:\n");
    {
        Reset();
        g_p.SetLayer(Layer::Patterns);
        Check(g_ui.mode() == Ui::Mode::Pattern, "the layer holds pattern mode");
        Tap(6);
        auto r = g_ui.TakeStorageRequest();
        Check(r.type == Ui::StorageRequest::Type::LoadPattern && r.slot == 6,
              "a key loads its slot");
        g_p.SetShift(true);
        Tap(6);
        r = g_ui.TakeStorageRequest();
        Check(r.type == Ui::StorageRequest::Type::SavePattern && r.slot == 6,
              "SHIFT + key saves to it");
        g_p.SetShift(false);
        g_p.SetLayer(Layer::Steps);
        Check(g_ui.mode() == Ui::Mode::Play, "leaving the layer releases pattern mode");
    }

    std::printf("LEDs:\n");
    {
        Reset();
        float led[kDevkitKeys];
        Patch &p = g_m.mutable_patch();
        p.pattern.tracks[0].steps[2].flags    = kStepActive;
        p.pattern.tracks[0].steps[2].velocity = 1;
        p.pattern.tracks[0].steps[3].flags    = kStepActive;
        p.pattern.tracks[0].steps[3].velocity = 127;
        g_p.Render(led);
        Check(led[0] == 0.f, "an inactive step is dark");
        Check(led[2] >= DevkitPanel::kStepLo, "velocity 1 is still visibly on");
        Check(led[3] > led[2], "velocity is brightness");
        Check(led[3] < DevkitPanel::kFull, "no step is as bright as the playhead");

        g_p.KeyDown(3);
        g_p.Render(led);
        Check(led[3] == DevkitPanel::kFull, "a held step is fully lit");
        g_p.KeyUp(3);

        g_p.SetLayer(Layer::Tracks);
        g_p.Render(led);
        Check(led[0] == DevkitPanel::kFull, "the selected track is brightest");
        Check(led[1] < led[0], "an empty track is dimmer");
        Check(led[1] > 0.f, "but still lit, so the key reads as live");

        g_p.KeyDown(DevkitPanel::kKeyPage);
        g_p.Render(led);
        Check(led[0] == DevkitPanel::kFull && led[1] < led[0] && led[4] == 0.f,
              "holding PAGE shows the current page on the first four keys");
        g_p.KeyUp(DevkitPanel::kKeyPage);

        g_p.SetLayer(Layer::Patterns);
        g_p.SetSlots(0b101, 2);
        g_p.Render(led);
        Check(led[2] == DevkitPanel::kFull, "the current slot is brightest");
        Check(led[0] > 0.f && led[0] < DevkitPanel::kFull, "an occupied slot is lit");
        Check(led[1] == 0.f, "an empty slot is dark");
        g_p.SetShift(true);
        g_p.Render(led);
        Check(led[1] > 0.f, "with SHIFT (save) every slot glows as a target");
    }

    if(failures)
    {
        std::printf("\n%d FAILED\n", failures);
        return 1;
    }
    std::printf("\nall devkit tests passed\n");
    return 0;
}
