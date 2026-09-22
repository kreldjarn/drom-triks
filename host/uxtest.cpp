// LED and display rendering.
//
// Both are pure functions of machine + UI state, which is worth exploiting:
// the LED language is a set of rules a person has to read at a glance while
// playing, and "does the playhead always win" is much easier to settle here
// than by staring at a panel.
//
// The current budget is the one with teeth. Thirty SK6812s at full white draw
// 1.8 A, and the renderer is the only place that sees a whole frame, so it is
// the only place that can enforce a ceiling.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/ui/display.h"
#include "../src/ui/led.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

void Pump(Machine &m, int blocks = 1)
{
    float buf[32];
    for(int i = 0; i < blocks; ++i)
        m.Process(buf, 32);
}

bool IsOff(Rgb c) { return c.r == 0 && c.g == 0 && c.b == 0; }
int  Sum(Rgb c) { return c.r + c.g + c.b; }
/// How close to white a colour is: 0 = fully saturated hue, 1 = white.
float Whiteness(Rgb c)
{
    const int lo = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    const int hi = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    return hi > 0 ? static_cast<float>(lo) / hi : 0.f;
}
bool Whiteish(Rgb c) { return Whiteness(c) > 0.8f; }

} // namespace

int main()
{
    static Machine m;
    LedRenderer     leds;
    DisplayRenderer disp;
    Rgb             frame[kNumLeds];

    std::printf("LED language:\n");
    {
        m.Init(48000.f);
        Patch &p = m.mutable_patch();
        p.pattern.tracks[0].length = 16;
        p.pattern.tracks[0].steps[2].flags    = kStepActive;
        p.pattern.tracks[0].steps[2].velocity = 127;
        p.pattern.tracks[0].steps[5].flags    = kStepActive;
        p.pattern.tracks[0].steps[5].velocity = 20;
        // Same velocity as step 2, so the only difference is the lock.
        p.pattern.tracks[0].steps[9].flags      = kStepActive;
        p.pattern.tracks[0].steps[9].velocity   = 127;
        p.pattern.tracks[0].steps[9].lock_count = 1;

        Ui ui; ui.Init(&m);
        leds.Render(m, ui, 0, frame);

        Check(IsOff(frame[0]), "an inactive step is off");
        Check(!IsOff(frame[2]), "an active step is lit");
        Check(Sum(frame[2]) > Sum(frame[5]),
              "a loud step is brighter than a quiet one");
        Check(Sum(frame[5]) > 0, "but a quiet step is still visibly on");
        const float plain  = Whiteness(frame[2]);
        const float locked = Whiteness(frame[9]);
        std::printf("      plain step whiteness %.2f, locked %.2f\n", plain, locked);
        Check(locked > plain + 0.25f,
              "a locked step is clearly whiter than a plain one at equal velocity");
    }

    std::printf("\nplayhead:\n");
    {
        m.Init(48000.f);
        Patch &p = m.mutable_patch();
        p.pattern.tracks[0].length = 16;
        for(int i = 0; i < 16; ++i)
            p.pattern.tracks[0].steps[i].flags = kStepActive;

        Ui ui; ui.Init(&m);
        Command c; c.type = Command::Type::Start; m.Push(c);
        Pump(m, 50);

        leds.Render(m, ui, 0, frame);
        const int head = static_cast<int>(m.state().position[0].load());
        Check(Whiteish(frame[head]) && Sum(frame[head]) > 400,
              "the playhead is white and brightest");

        // It must win even over a locked step, or it becomes ambiguous exactly
        // when you are relying on it.
        p.pattern.tracks[0].steps[head].lock_count = 1;
        leds.Render(m, ui, 0, frame);
        Check(Whiteish(frame[head]), "and wins over a locked step");
    }

    std::printf("\ntrack keys:\n");
    {
        m.Init(48000.f);
        Patch &p = m.mutable_patch();
        p.pattern.tracks[1].steps[0].flags = kStepActive;  // track 1 has content
        Ui ui; ui.Init(&m);

        leds.Render(m, ui, 0, frame);
        Rgb *tk = frame + kNumStepKeys;
        Check(Sum(tk[0]) > Sum(tk[1]), "the selected track is brightest");
        Check(Sum(tk[1]) > Sum(tk[2]), "a track with content outranks an empty one");

        Command c; c.type = Command::Type::SetTrackMute; c.track = 3; c.value = 1.f;
        m.Push(c); Pump(m);
        leds.Render(m, ui, 0, frame);
        Check(tk[3].r > 200 && tk[3].g < 40 && tk[3].b < 40, "a muted track is red");
    }

    std::printf("\ncurrent budget (the one that protects the hardware):\n");
    {
        m.Init(48000.f);
        Patch &p = m.mutable_patch();
        for(int t = 0; t < kNumTracks; ++t)
        {
            p.pattern.tracks[t].length = 16;
            for(int i = 0; i < 16; ++i)
            {
                p.pattern.tracks[t].steps[i].flags    = kStepActive;
                p.pattern.tracks[t].steps[i].velocity = 127;
                p.pattern.tracks[t].steps[i].lock_count = 1;  // whitened = worst case
            }
        }
        Ui ui; ui.Init(&m);
        leds.Render(m, ui, 600, frame);   // mid-pulse

        const float ma = FrameCurrentMa(frame, kNumLeds);
        std::printf("  worst-case frame draws %.0f mA (budget %.0f mA)\n", ma, kLedBudgetMa);
        Check(ma <= kLedBudgetMa + 1.f, "a full bright frame stays inside the budget");

        // And the unclamped ideal really would have blown it, or the test proves nothing.
        Rgb white[kNumLeds];
        for(auto &c : white) c = Rgb{255, 255, 255};
        std::printf("  unclamped all-white would draw %.0f mA\n",
                    FrameCurrentMa(white, kNumLeds));
        Check(FrameCurrentMa(white, kNumLeds) > kLedBudgetMa * 3.f,
              "and the clamp is doing real work");
    }

    std::printf("\ndisplay:\n");
    {
        m.Init(48000.f);
        m.mutable_patch().kit.params[0][0] = 0.50f;
        Ui ui; ui.Init(&m);
        DisplayLines d;

        ui.SetTime(1000);
        disp.Render(m, ui, d);
        Check(std::strstr(d.text[0], "BD") != nullptr, "overview names the track");
        Check(std::strstr(d.text[1], "BPM") != nullptr, "and shows tempo");
        Check(std::strstr(d.text[1], "INT") != nullptr, "and the sync source");

        ui.PotMove(0, 0.50f);          // catch, then move
        ui.PotMove(0, 0.62f); Pump(m);
        disp.Render(m, ui, d);
        Check(std::strstr(d.text[0], "TUNE") != nullptr,
              "touching a knob shows that parameter's name");

        ui.SetTime(1000 + DisplayRenderer::kParamHoldMs + 1);
        disp.Render(m, ui, d);
        Check(std::strstr(d.text[0], "TUNE") == nullptr,
              "and reverts to the overview once you let go");
    }

    std::printf("\nsoft takeover, explained on screen:\n");
    {
        m.Init(48000.f);
        m.mutable_patch().kit.params[0][0] = 0.20f;
        m.mutable_patch().kit.params[1][0] = 0.90f;
        Ui ui; ui.Init(&m);
        DisplayLines d;

        ui.SetTime(500);
        ui.PotMove(0, 0.20f); Pump(m);     // caught on track 0
        ui.TrackPress(1);                  // switch: pot is now stale
        ui.PotMove(0, 0.25f); Pump(m);

        disp.Render(m, ui, d);
        const bool shows_both = std::strstr(d.text[2], "25") && std::strstr(d.text[2], "90");
        Check(!ui.pot_caught(0), "the pot is released after a track change");
        Check(shows_both, "the screen shows knob position AND stored value");
        Check(std::strstr(d.text[3], "pick up") != nullptr,
              "and says what to do about it");
        std::printf("      | %s\n      | %s\n", d.text[2], d.text[3]);
    }

    std::printf("\n%s\n", failures ? "UX TESTS FAILED" : "all UX tests passed");
    return failures;
}
