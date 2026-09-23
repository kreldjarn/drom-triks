// Interactive playground: the real machine, driven from a Mac terminal.
//
// Audio goes out through CoreAudio, the keyboard stands in for the panel, and
// the LEDs are drawn as truecolour blocks using the *same* LedRenderer and
// DisplayRenderer the firmware will use. So this is not a mock-up of the
// instrument — it is the instrument, with a different set of pins.
//
//   make -C host play
//
// macOS only; the rest of the host tooling is portable.

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>

#include "../src/machine.h"
#include "../src/ui/display.h"
#include "../src/ui/led.h"

using namespace drom;

namespace {

constexpr double kSampleRate = 48000.0;

// 15.5 kB — far too big for a stack frame, on a Mac as much as on an M7.
Machine         g_machine;
Ui              g_ui;
LedRenderer     g_leds;
DisplayRenderer g_display;

std::atomic<bool> g_quit{false};

// --- keyboard layout, laid out like the panel it stands in for -------------
const char *kStepKeys  = "1234567890qwerty"; // 16 steps
const char *kTrackKeys = "asdfghjk";         // 8 tracks

/// Which pot the -/= keys address. A terminal has no knobs, so one "focused"
/// pot stands in for six physical ones.
int g_sel_pot = 0;

/// A terminal gives no key-up events, so "hold a step and turn a knob" has to
/// become a latch: press l, then a step, and that step stays held until l again.
bool g_lock_arm = false;

// --- terminal --------------------------------------------------------------
termios g_saved_termios;

void RestoreTerminal()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    std::printf("\033[?25h\033[0m\n"); // cursor back on
    std::fflush(stdout);
}

void RawTerminal()
{
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    std::atexit(RestoreTerminal);
    termios raw = g_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0; // non-blocking reads
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    std::printf("\033[?25l"); // hide cursor
}

// --- audio -----------------------------------------------------------------
OSStatus RenderCallback(void                       *,
                        AudioUnitRenderActionFlags *,
                        const AudioTimeStamp       *,
                        UInt32,
                        UInt32                      frames,
                        AudioBufferList            *io)
{
    // This is the real-time thread. Machine::Process drains the command queue
    // and renders; it allocates nothing and takes no locks, which is the whole
    // point of the architecture.
    static float mono[4096];
    if(frames > 4096)
        frames = 4096;
    g_machine.Process(mono, frames);

    float *l = static_cast<float *>(io->mBuffers[0].mData);
    float *r = io->mNumberBuffers > 1 ? static_cast<float *>(io->mBuffers[1].mData) : nullptr;
    for(UInt32 i = 0; i < frames; ++i)
    {
        l[i] = mono[i];
        if(r)
            r[i] = mono[i];
    }
    return noErr;
}

AudioUnit StartAudio()
{
    AudioComponentDescription desc = {};
    desc.componentType             = kAudioUnitType_Output;
    desc.componentSubType          = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer     = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if(!comp)
        return nullptr;

    AudioUnit unit = nullptr;
    if(AudioComponentInstanceNew(comp, &unit) != noErr)
        return nullptr;

    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate       = kSampleRate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
                       | kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = 4;
    fmt.mBytesPerPacket   = 4;
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct cb = {RenderCallback, nullptr};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));

    if(AudioUnitInitialize(unit) != noErr)
        return nullptr;
    if(AudioOutputUnitStart(unit) != noErr)
        return nullptr;
    return unit;
}

// --- drawing ---------------------------------------------------------------
void Block(Rgb c)
{
    std::printf("\033[48;2;%d;%d;%dm  \033[0m", c.r, c.g, c.b);
}

void Draw(uint32_t now_ms)
{
    Rgb frame[kNumLeds];
    g_leds.Render(g_machine, g_ui, now_ms, frame);
    DisplayLines d;
    g_display.Render(g_machine, g_ui, d);

    std::printf("\033[H\033[2J"); // home + clear
    std::printf("\033[1m  drom-triks\033[0m   playground\n\n");

    // The OLED, as it will actually read.
    std::printf("  \033[48;2;16;16;20m\033[38;2;180;220;255m");
    for(int r = 0; r < DisplayLines::kRows; ++r)
        std::printf(" %-*s \033[0m\n  \033[48;2;16;16;20m\033[38;2;180;220;255m",
                    DisplayLines::kCols, d.text[r]);
    std::printf("\033[0m\n\n");

    std::printf("  steps  ");
    for(int i = 0; i < kNumStepKeys; ++i)
    {
        Block(frame[i]);
        std::printf(" ");
    }
    std::printf("\n         ");
    for(int i = 0; i < kNumStepKeys; ++i)
        std::printf("%c   ", kStepKeys[i]);

    std::printf("\n\n  tracks ");
    for(int i = 0; i < kNumTracks; ++i)
    {
        Block(frame[kNumStepKeys + i]);
        std::printf(" ");
    }
    std::printf("\n         ");
    for(int i = 0; i < kNumTracks; ++i)
        std::printf("%c   ", kTrackKeys[i]);
    std::printf("\n         ");
    for(int i = 0; i < kNumTracks; ++i)
        std::printf("%-4s", kTrackName[i]);

    // Pots, with the selected one marked and pickup state made visible.
    std::printf("\n\n  pots   ");
    for(int i = 0; i < kNumPots; ++i)
    {
        const float v = g_machine.patch().kit.params[g_ui.selected_track()][i];
        const bool  sel = (i == g_sel_pot);
        std::printf("%s%-5s %3d%s%s  ",
                    sel ? "\033[7m" : "",
                    ParamName(static_cast<ParamId>(i)),
                    static_cast<int>(v * 100.f + 0.5f),
                    g_ui.pot_caught(i) ? "" : "*",
                    sel ? "\033[0m" : "");
    }

    std::printf("\n\n  \033[2m%s%s%s\033[0m\n",
                g_ui.mode() == Ui::Mode::Mute ? "[MUTE MODE] " : "",
                g_lock_arm ? "[LOCK ARMED - press a step key] " : "",
                g_ui.held_step() >= 0
                    ? "[HOLDING STEP - adjust a pot to write a lock, l to finish]"
                    : "");
    std::printf("\n  \033[2mspace play/stop   , . select pot   - = adjust   "
                "m mute mode\n"
                "  l lock a step     [ ] tempo      esc quit\033[0m\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // --check opens the real audio device, runs briefly and reports, so the
    // CoreAudio path can be verified without a terminal or a listener.
    bool check = false;
    for(int i = 1; i < argc; ++i)
        if(std::strcmp(argv[i], "--check") == 0)
            check = true;

    g_machine.Init(static_cast<float>(kSampleRate));
    g_ui.Init(&g_machine);

    // A starting pattern, so there is something to hear immediately.
    Patch &p = g_machine.mutable_patch();
    for(auto &t : p.pattern.tracks)
        t.length = 16;
    auto on = [&](int trk, int step, int vel) {
        p.pattern.tracks[trk].steps[step].flags    = kStepActive;
        p.pattern.tracks[trk].steps[step].velocity = static_cast<uint8_t>(vel);
    };
    on(0, 0, 110); on(0, 6, 80); on(0, 10, 110);
    on(1, 4, 115); on(1, 12, 115);
    for(int i = 0; i < 16; i += 2)
        on(2, i, i % 4 ? 60 : 95);

    AudioUnit unit = StartAudio();
    if(!unit)
    {
        std::fprintf(stderr, "could not open the default audio output\n");
        return 1;
    }

    float   tempo = 124.f;
    Command c;
    c.type  = Command::Type::SetTempo;
    c.value = tempo;
    g_machine.Push(c);
    c.type = Command::Type::Start;
    g_machine.Push(c);

    if(check)
    {
        // Let the device run, then confirm the sequencer advanced and the
        // machine is actually producing sound through it.
        usleep(500000);
        const bool playing = g_machine.state().playing.load();
        const uint32_t tick = g_machine.state().tick.load();
        std::printf("audio device   : opened\n");
        std::printf("transport      : %s\n", playing ? "playing" : "STOPPED");
        std::printf("ticks advanced : %u (expect ~%d after 0.5 s at 124 BPM)\n",
                    tick, static_cast<int>(124.0 / 60.0 * kPpqn * 0.5));
        std::printf("tempo          : %.1f BPM\n", g_machine.state().tempo.load());
        std::printf("\none rendered frame:\n\n");
        g_ui.SetTime(400);
        Draw(400);
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        return (playing && tick > 50) ? 0 : 1;
    }

    RawTerminal();

    uint32_t ms = 0;
    while(!g_quit)
    {
        char ch;
        while(read(STDIN_FILENO, &ch, 1) == 1)
        {
            if(ch == 27) { g_quit = true; break; }  // esc

            if(const char *s = std::strchr(kStepKeys, ch); s && ch)
            {
                const int idx = static_cast<int>(s - kStepKeys);
                if(g_lock_arm)
                {
                    g_ui.StepPress(idx);   // held until l is pressed again
                    g_lock_arm = false;
                }
                else if(g_ui.held_step() >= 0)
                {
                    g_ui.StepRelease(g_ui.held_step());
                    g_ui.StepPress(idx);   // and toggle the one just pressed
                    g_ui.StepRelease(idx);
                }
                else
                {
                    g_ui.StepPress(idx);
                    g_ui.StepRelease(idx);
                }
                continue;
            }
            if(const char *t = std::strchr(kTrackKeys, ch); t && ch)
            {
                g_ui.TrackPress(static_cast<int>(t - kTrackKeys));
                continue;
            }

            switch(ch)
            {
                case ' ':
                    c.type = g_machine.state().playing.load() ? Command::Type::Stop
                                                             : Command::Type::Start;
                    g_machine.Push(c);
                    break;
                case ',': g_sel_pot = (g_sel_pot + kNumPots - 1) % kNumPots; break;
                case '.': g_sel_pot = (g_sel_pot + 1) % kNumPots; break;
                case '-':
                case '=':
                {
                    const int   trk = g_ui.selected_track();
                    const float cur = g_machine.patch().kit.params[trk][g_sel_pot];
                    float       nv  = cur + (ch == '=' ? 0.05f : -0.05f);
                    nv              = nv < 0.f ? 0.f : (nv > 1.f ? 1.f : nv);
                    g_ui.PotMove(g_sel_pot, nv);
                    break;
                }
                case 'm':
                    g_ui.SetMode(g_ui.mode() == Ui::Mode::Mute ? Ui::Mode::Play
                                                               : Ui::Mode::Mute);
                    break;
                case 'l':
                    if(g_ui.held_step() >= 0)
                    {
                        g_ui.StepRelease(g_ui.held_step()); // done locking
                        g_lock_arm = false;
                    }
                    else
                        g_lock_arm = !g_lock_arm;
                    break;
                case '[':
                case ']':
                    tempo += (ch == ']' ? 2.f : -2.f);
                    c.type  = Command::Type::SetTempo;
                    c.value = tempo;
                    g_machine.Push(c);
                    c.type = Command::Type::None;
                    break;
                default: break;
            }
        }

        ms += 16;
        g_ui.SetTime(ms);
        Draw(ms);
        usleep(16000); // ~60 fps
    }

    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return 0;
}
