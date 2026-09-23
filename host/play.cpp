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
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>

#include "../src/machine.h"
#include "../src/ui/display.h"
#include "../src/ui/led.h"
#include "wav.h"
#include <vector>

using namespace drom;

namespace {

/// Filled in from the device at startup rather than assumed. Another app
/// changing the output rate is the normal case on a Mac, not an edge case.
double g_sample_rate = 48000.0;

/// Sized from the unit's MaximumFramesPerSlice. Rendering must handle whatever
/// block size the device asks for, including a size that changes at runtime.
constexpr UInt32 kMaxChunk = 2048;

/// 0 = follow the system default. Set with --device to bypass a virtual
/// device without changing the system-wide setting.
AudioDeviceID g_device = 0;

// 15.5 kB — far too big for a stack frame, on a Mac as much as on an M7.
Machine         g_machine;
Ui              g_ui;
LedRenderer     g_leds;
DisplayRenderer g_display;

std::atomic<bool> g_quit{false};
std::atomic<unsigned long> g_frames_rendered{0};
std::atomic<unsigned>      g_callbacks{0};
int g_check_ms = 500;

// --- recording -------------------------------------------------------------
//
// Recording the machine itself, rather than routing its output through a
// virtual audio device, is both simpler and exact: no driver, no resampling,
// no second clock to disagree with. The buffer is preallocated and only ever
// appended to from the audio thread, so the callback still allocates nothing.
std::vector<float>    g_rec;
std::atomic<size_t>   g_rec_used{0};
std::atomic<bool>     g_recording{false};
constexpr size_t      kRecMinutes = 10;

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
    static float mono[kMaxChunk];

    float *l = static_cast<float *>(io->mBuffers[0].mData);
    float *r = io->mNumberBuffers > 1 ? static_cast<float *>(io->mBuffers[1].mData) : nullptr;

    // Render in chunks so ANY frame count is filled completely. Clamping the
    // request instead would leave the tail of the buffer holding whatever was
    // there before, which is heard as chopping — and it only shows up once
    // something else on the system pushes the device to a bigger block.
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    g_frames_rendered.fetch_add(frames, std::memory_order_relaxed);

    UInt32 done = 0;
    while(done < frames)
    {
        const UInt32 n = (frames - done) < kMaxChunk ? (frames - done) : kMaxChunk;
        g_machine.Process(mono, n);
        for(UInt32 i = 0; i < n; ++i)
        {
            l[done + i] = mono[i];
            if(r)
                r[done + i] = mono[i];
        }
        done += n;
    }

    if(g_recording.load(std::memory_order_relaxed))
    {
        const size_t used = g_rec_used.load(std::memory_order_relaxed);
        const size_t room = g_rec.size() - used;
        const size_t n    = frames * 2 < room ? frames * 2 : room;
        for(size_t i = 0; i + 1 < n; i += 2)
        {
            g_rec[used + i]     = l[i / 2];
            g_rec[used + i + 1] = l[i / 2];
        }
        g_rec_used.store(used + n, std::memory_order_relaxed);
        if(n < frames * 2)
            g_recording.store(false, std::memory_order_relaxed); // buffer full
    }
    return noErr;
}

/// Lists the output devices, so a misbehaving virtual device can be bypassed
/// and compared against real hardware without changing the system default.
void ListDevices()
{
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size);
    const UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> devs(count);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, devs.data());

    AudioDeviceID def = 0;
    size              = sizeof(def);
    a.mSelector       = kAudioHardwarePropertyDefaultOutputDevice;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &def);

    std::printf("output devices (pass --device <id>):\n");
    for(AudioDeviceID d : devs)
    {
        AudioObjectPropertyAddress p = {kAudioDevicePropertyStreamConfiguration,
                                        kAudioDevicePropertyScopeOutput,
                                        kAudioObjectPropertyElementMain};
        UInt32 s2 = 0;
        AudioObjectGetPropertyDataSize(d, &p, 0, nullptr, &s2);
        std::vector<char> buf(s2);
        auto             *bl = reinterpret_cast<AudioBufferList *>(buf.data());
        AudioObjectGetPropertyData(d, &p, 0, nullptr, &s2, bl);
        UInt32 ch = 0;
        for(UInt32 i = 0; i < bl->mNumberBuffers; ++i)
            ch += bl->mBuffers[i].mNumberChannels;
        if(ch == 0)
            continue;

        CFStringRef nm = nullptr;
        s2             = sizeof(nm);
        p.mSelector    = kAudioObjectPropertyName;
        p.mScope       = kAudioObjectPropertyScopeGlobal;
        AudioObjectGetPropertyData(d, &p, 0, nullptr, &s2, &nm);
        char name[128] = "?";
        if(nm)
        {
            CFStringGetCString(nm, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(nm);
        }
        Float64 sr = 0;
        s2         = sizeof(sr);
        p.mSelector = kAudioDevicePropertyNominalSampleRate;
        AudioObjectGetPropertyData(d, &p, 0, nullptr, &s2, &sr);
        std::printf("  %-4u %-32s %6.0f Hz %s\n", static_cast<unsigned>(d), name, sr,
                    d == def ? "(system default)" : "");
    }
}

/// Ask the output unit what format it actually wants, and match it.
///
/// Querying the *device* is not enough: a virtual driver — Background Music,
/// Loopback, BlackHole and friends, which is exactly what people route through
/// when recording — can report a nominal device rate that differs from the
/// format the default output unit presents. Feeding a rate it did not ask for
/// makes CoreAudio insert a sample-rate converter, and through a virtual
/// device that conversion is where the choppiness comes from.
///
/// So: read the unit's own default input-scope format, keep its rate, and only
/// override what we genuinely need (float, non-interleaved, stereo).
AudioStreamBasicDescription PreferredFormat(AudioUnit unit)
{
    AudioStreamBasicDescription fmt = {};
    UInt32                      size = sizeof(fmt);
    if(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Input, 0, &fmt, &size)
           != noErr
       || fmt.mSampleRate <= 0.0)
    {
        fmt.mSampleRate = 48000.0;
    }

    fmt.mFormatID    = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
                       | kAudioFormatFlagIsPacked;
    fmt.mFramesPerPacket  = 1;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel   = 32;
    fmt.mBytesPerFrame    = 4;
    fmt.mBytesPerPacket   = 4;
    return fmt;
}

AudioUnit StartAudio()
{
    AudioComponentDescription desc = {};
    desc.componentType             = kAudioUnitType_Output;
    desc.componentSubType          = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer     = kAudioUnitManufacturer_Apple;

    // AUHAL when a device is named, so we can talk to one device directly the
    // way a DAW does; the simpler DefaultOutput unit otherwise.
    if(g_device != 0)
        desc.componentSubType = kAudioUnitSubType_HALOutput;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if(!comp)
        return nullptr;

    AudioUnit unit = nullptr;
    if(AudioComponentInstanceNew(comp, &unit) != noErr)
        return nullptr;

    if(g_device != 0)
    {
        UInt32 on = 1;
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &on, sizeof(on));
        if(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                kAudioUnitScope_Global, 0, &g_device, sizeof(g_device))
           != noErr)
        {
            std::fprintf(stderr, "could not bind device %u\n", (unsigned)g_device);
            return nullptr;
        }
    }

    UInt32 max_frames = kMaxChunk;
    AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));

    // With AUHAL the device's own output format is authoritative; with
    // DefaultOutput the unit's input-scope default is.
    AudioStreamBasicDescription fmt = {};
    if(g_device != 0)
    {
        UInt32 size = sizeof(fmt);
        AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 0, &fmt, &size);
        fmt.mFormatID    = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
                           | kAudioFormatFlagIsPacked;
        fmt.mFramesPerPacket  = 1;
        fmt.mChannelsPerFrame = 2;
        fmt.mBitsPerChannel   = 32;
        fmt.mBytesPerFrame    = 4;
        fmt.mBytesPerPacket   = 4;
    }
    else
        fmt = PreferredFormat(unit);
    if(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Input, 0, &fmt, sizeof(fmt))
       != noErr)
    {
        std::fprintf(stderr, "the output unit rejected our stream format\n");
        return nullptr;
    }

    // Trust what is actually in effect, not what we asked for.
    UInt32 size = sizeof(fmt);
    AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                         &fmt, &size);
    g_sample_rate = fmt.mSampleRate;

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
    std::printf("\033[1m  drom-triks\033[0m   playground   \033[2m%.0f Hz\033[0m\n\n",
                g_sample_rate);

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
    if(g_recording.load())
        std::printf("  \033[1;31m* REC\033[0m  %.1f s\n",
                    g_rec_used.load() / 2.0 / g_sample_rate);
    else if(g_rec_used.load() > 0)
        std::printf("  \033[2mrecorded %.1f s - saved on quit\033[0m\n",
                    g_rec_used.load() / 2.0 / g_sample_rate);
    std::printf("\n  \033[2mspace play/stop   , . select pot   - = adjust   "
                "m mute mode\n"
                "  l lock a step     [ ] tempo      r record      esc quit\033[0m\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // --check opens the real audio device, runs briefly and reports, so the
    // CoreAudio path can be verified without a terminal or a listener.
    bool check           = false;
    bool start_recording = false;
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--check") == 0)
            check = true;
        else if(std::strcmp(argv[i], "--ms") == 0 && i + 1 < argc)
            g_check_ms = std::atoi(argv[++i]);
        else if(std::strcmp(argv[i], "--record") == 0)
            start_recording = true;
        else if(std::strcmp(argv[i], "--devices") == 0)
        {
            ListDevices();
            return 0;
        }
        else if(std::strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            g_device = static_cast<AudioDeviceID>(std::atoi(argv[++i]));
    }

    // The engine's rate comes from the device, so it has to be opened first.
    AudioUnit unit = StartAudio();
    if(!unit)
    {
        std::fprintf(stderr, "could not open the default audio output\n");
        return 1;
    }
    g_machine.Init(static_cast<float>(g_sample_rate));
    g_ui.Init(&g_machine);
    g_rec.assign(static_cast<size_t>(g_sample_rate) * 2 * 60 * kRecMinutes, 0.f);
    if(start_recording)
        g_recording = true;

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
        const auto t0 = std::chrono::steady_clock::now();
        usleep(g_check_ms * 1000);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        const bool playing = g_machine.state().playing.load();
        const uint32_t tick = g_machine.state().tick.load();
        std::printf("audio device   : opened at %.0f Hz\n", g_sample_rate);
        std::printf("transport      : %s\n", playing ? "playing" : "STOPPED");
        std::printf("ticks advanced : %u (expect ~%d after 0.5 s at 124 BPM)\n",
                    tick, static_cast<int>(124.0 / 60.0 * kPpqn * 0.5));
        std::printf("tempo          : %.1f BPM\n", g_machine.state().tempo.load());
        std::printf("elapsed        : %.3f s\n", el);
        std::printf("callbacks      : %u\n", g_callbacks.load());
        if(g_rec_used.load() > 0)
            std::printf("recorded       : %.2f s\n", g_rec_used.load() / 2.0 / g_sample_rate);
        std::printf("frames rendered: %lu of %.0f expected  (%.0f%% of real time)\n",
                    g_frames_rendered.load(), g_sample_rate * g_check_ms / 1000.0,
                    100.0 * g_frames_rendered.load() / (g_sample_rate * g_check_ms / 1000.0));
        std::printf("\none rendered frame:\n\n");
        g_ui.SetTime(400);
        Draw(400);
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        const size_t u = g_rec_used.load();
        if(u > 0)
        {
            std::vector<float> take(g_rec.begin(), g_rec.begin() + u);
            host::WriteWav("drom-triks-take.wav", take,
                           static_cast<uint32_t>(g_sample_rate));
            std::printf("wrote          : drom-triks-take.wav\n");
        }
        return (playing && tick > 20) ? 0 : 1;
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
                case 'r':
                    if(g_recording.load())
                    {
                        g_recording = false;
                    }
                    else
                    {
                        g_rec_used = 0;
                        g_recording = true;
                    }
                    break;
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
    RestoreTerminal();

    const size_t used = g_rec_used.load();
    if(used > 0)
    {
        std::vector<float> take(g_rec.begin(), g_rec.begin() + used);
        if(host::WriteWav("drom-triks-take.wav", take,
                          static_cast<uint32_t>(g_sample_rate)))
            std::printf("wrote drom-triks-take.wav - %.1f s at %.0f Hz\n",
                        used / 2.0 / g_sample_rate, g_sample_rate);
    }
    return 0;
}
