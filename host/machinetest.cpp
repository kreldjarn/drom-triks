// Integration and threading-model verification.
//
// The queue is the load-bearing piece of the whole architecture: it is what
// lets the audio callback own every piece of mutable state and therefore run
// without a lock. If it is wrong, the symptom is a rare glitch under load,
// which is close to undiagnosable after the fact.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/io/storage_layout.h"
#include "../src/io/storage.h"
#include "../src/machine.h"
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

void Pump(Machine &m, int blocks = 1)
{
    float buf[64]; // 32 frames, interleaved stereo
    for(int i = 0; i < blocks; ++i)
        m.Process(buf, 32);
}

} // namespace

int main()
{
    std::printf("SPSC queue:\n");
    {
        SpscQueue<int, 8> q;
        Check(q.empty(), "starts empty");
        Check(q.capacity() == 7, "capacity is N-1, so full and empty differ");

        for(int i = 0; i < 7; ++i)
            q.Push(i);
        Check(q.size() == 7, "fills to capacity");
        Check(!q.Push(99), "rejects a push when full rather than overwriting");

        int v = -1, ok = 1;
        for(int i = 0; i < 7; ++i)
        {
            if(!q.Pop(v) || v != i)
                ok = 0;
        }
        Check(ok, "pops in order");
        Check(!q.Pop(v), "pop on empty returns false");

        // Wrap the indices well past the buffer length.
        int seen = 0;
        for(int i = 0; i < 1000; ++i)
        {
            q.Push(i);
            int got;
            if(q.Pop(got) && got == i)
                ++seen;
        }
        Check(seen == 1000, "survives index wrap-around");
    }

    std::printf("\nconcurrent producer/consumer (the real usage):\n");
    {
        // One writer, one reader, genuinely on different threads — the
        // arrangement the memory ordering exists for.
        SpscQueue<Command, 128> q;
        std::atomic<bool>       done{false};
        constexpr int           kTotal = 50000;

        std::thread producer([&] {
            for(int i = 0; i < kTotal;)
            {
                Command c;
                c.type  = Command::Type::SetKitParam;
                c.track = static_cast<uint8_t>(i & 7);
                c.value = static_cast<float>(i);
                if(q.Push(c))
                    ++i;
            }
            done = true;
        });

        long received = 0;
        bool ordered  = true;
        float expect  = 0.f;
        while(!done || !q.empty())
        {
            Command c;
            while(q.Pop(c))
            {
                if(c.value != expect)
                    ordered = false;
                expect += 1.f;
                ++received;
            }
        }
        producer.join();

        Check(received == kTotal, "every command arrives exactly once");
        Check(ordered, "and in order, with no tearing");
    }

    std::printf("\nQSPI storage layout:\n");
    {
        // The static_asserts in storage_layout.h are the real guard; this
        // prints the map so a struct change that moves everything is visible
        // in the test output rather than only at the next flash write.
        std::printf("      sector %u B, Patch %zu B -> stride %u B (%u sectors)\n",
                    kSectorBytes, sizeof(Patch), kPatternStride,
                    kPatternStride / kSectorBytes);
        std::printf("      settings 0x%06X  kits 0x%06X  patterns 0x%06X\n",
                    kSettingsBase, kKitBase, kPatternBase);
        std::printf("      songs    0x%06X  free 0x%06X  (chip 0x%06X)\n",
                    kSongBase, kFreeBase, kQspiBytes);

        Check(kPatternStride >= sizeof(Patch), "a pattern slot holds a whole Patch");
        Check(kPatternStride % kSectorBytes == 0,
              "slots are sector-aligned, so Erase cannot reach into the slot below");
        Check(kFreeBase <= kQspiBytes, "the whole layout fits the chip");
        Check(PatternAddr(kPatternSlots - 1) + kPatternStride == kSongBase,
              "the last pattern slot ends exactly where songs begin");
    }

    std::printf("\nUI -> queue -> audio:\n");
    {
        static Machine m; m.Init(48000.f);
        Ui ui; ui.Init(&m);

        const float before = m.patch().kit.params[0][0];
        ui.SetTime(1000);
        ui.EncoderTurn(0, +4);
        Check(std::fabs(m.patch().kit.params[0][0] - before) < 1e-5f,
              "a UI edit does NOT take effect before the audio side runs");
        Pump(m);
        Check(m.patch().kit.params[0][0] > before,
              "and does take effect once it drains the queue");
    }

    std::printf("\nthe machine actually plays:\n");
    {
        static Machine m; m.Init(48000.f);
        Patch &p = m.mutable_patch();
        p.pattern.tracks[0].steps[0].flags = kStepActive;
        p.pattern.tracks[0].steps[8].flags = kStepActive;
        p.pattern.tracks[0].length = 16;

        Command c; c.type = Command::Type::Start;
        m.Push(c);

        std::vector<float> audio;
        float buf[64]; // 32 frames, interleaved stereo
        for(int b = 0; b < 3000; ++b)
        {
            m.Process(buf, 32);
            audio.insert(audio.end(), buf, buf + 32);
        }

        float peak = 0.f;
        for(float s : audio)
            peak = std::fmax(peak, std::fabs(s));
        Check(peak > 0.01f, "produces audio");
        Check(m.state().playing.load(), "publishes playing state");
        Check(m.state().tick.load() > 0, "publishes an advancing tick");
        Check(std::fabs(m.state().tempo.load() - 120.f) < 1.f, "publishes tempo");
    }

    std::printf("\ntempo and transport through the queue:\n");
    {
        static Machine m; m.Init(48000.f);
        Command c;
        c.type = Command::Type::SetTempo; c.value = 145.f;
        m.Push(c);
        c.type = Command::Type::Start; m.Push(c);
        Pump(m);
        Check(std::fabs(m.state().tempo.load() - 145.f) < 0.5f, "tempo change applies");
        Check(m.state().playing.load(), "start applies");

        c.type = Command::Type::Stop; m.Push(c); Pump(m);
        Check(!m.state().playing.load(), "stop applies");
    }

    std::printf("\nexternal sync through the machine:\n");
    {
        static Machine m; m.Init(48000.f);
        Command c; c.type = Command::Type::Start; m.Push(c); Pump(m);

        const double spc = 48000.0 * 60.0 / (90.0 * 24.0);
        double next = spc;
        long   sample = 0;
        float  buf[32];
        for(int b = 0; b < 2000; ++b)
        {
            while(next < sample + 32)
            {
                m.OnMidiClock(static_cast<uint64_t>(next));
                next += spc;
            }
            m.Process(buf, 32);
            sample += 32;
        }
        Check(m.state().external_sync.load(), "reports external sync");
        Check(std::fabs(m.state().tempo.load() - 90.f) < 1.f,
              "follows the 90 BPM master through the machine");
    }

    std::printf("\nsnapshot for saving:\n");
    {
        // The main loop must never memcpy the patch itself — the audio side is
        // its only writer, and a torn copy passes every header check.
        static Machine m;
        static Patch   dst;
        m.Init(48000.f);

        Command c;
        c.type  = Command::Type::SetKitParam;
        c.track = 2;
        c.param = static_cast<uint8_t>(ParamId::Tune);
        c.value = 0.77f;
        m.Push(c);
        Pump(m);

        m.RequestSnapshot(&dst);
        Check(!m.SnapshotReady(), "not ready before the audio side has run");
        Pump(m);
        Check(m.SnapshotReady(), "ready once the command has been drained");
        Check(dst.kit.params[2][static_cast<int>(ParamId::Tune)] == 0.77f,
              "and the copy carries the edit that preceded it");
        Check(std::memcmp(&dst.pattern, &m.patch().pattern, sizeof(Pattern)) == 0,
              "the whole pattern matches the live one");

        // A snapshot taken into a Storage staging buffer is what actually gets
        // written, so prove that path end to end.
        static RamFlash<kQspiBytes> flash;
        static Storage              storage;
        storage.Init(&flash);
        m.RequestSnapshot(&storage.staging());
        Pump(m);
        Check(m.SnapshotReady(), "a snapshot straight into the staging buffer");
        Check(storage.SaveStaged(0) == Storage::Result::Ok, "saves from staging");

        static Patch back;
        Check(storage.LoadPatch(0, back) == Storage::Result::Ok, "and loads back");
        Check(back.kit.params[2][static_cast<int>(ParamId::Tune)] == 0.77f,
              "with the edit intact after a flash round trip");

        // The mirror: only the audio side may install a loaded patch.
        c.value = 0.11f;
        m.Push(c);
        Pump(m);
        Check(m.patch().kit.params[2][static_cast<int>(ParamId::Tune)] == 0.11f,
              "the live patch moves on");
        m.RequestLoad(&back);
        Pump(m);
        Check(m.LoadReady(), "a load request completes on the audio side");
        Check(m.patch().kit.params[2][static_cast<int>(ParamId::Tune)] == 0.77f,
              "and the loaded patch replaces the live one");
    }

    std::printf("\nselectable machines:\n");
    {
        static Machine m;
        m.Init(48000.f);

        bool defaults_ok = true;
        for(int i = 0; i < kNumTracks; ++i)
            defaults_ok &= m.machine(i) == kDefaultMachine[i];
        Check(defaults_ok, "every track powers on as its documented default");

        // Audio has to actually change, not just the label.
        auto Energy = [&](int track) {
            Command t;
            t.type = Command::Type::ToggleStep; t.track = (uint8_t)track; t.step = 0;
            m.Push(t); Pump(m);
            Command go; go.type = Command::Type::Start; m.Push(go);
            float buf[64], sum = 0.f;
            for(int b = 0; b < 200; ++b)
            {
                m.Process(buf, 32);
                for(int i = 0; i < 64; ++i) sum += buf[i] * buf[i];
            }
            Command stop; stop.type = Command::Type::Stop; m.Push(stop); Pump(m);
            m.Push(t); Pump(m); // toggle the step back off
            return sum;
        };

        const float e_808 = Energy(0);
        Command c;
        c.type = Command::Type::SetMachine;
        c.track = 0;
        c.param = static_cast<uint8_t>(MachineId::BdBoom);
        m.Push(c); Pump(m);
        Check(m.machine(0) == MachineId::BdBoom, "a track can be given another machine");
        const float e_boom = Energy(0);
        Check(e_808 > 0.f && e_boom > 0.f, "both machines make sound");
        Check(std::fabs(e_808 - e_boom) / (e_808 + e_boom) > 0.05f,
              "and they are audibly different, not just relabelled");

        // A new machine starts at its own defaults and knows nothing of the
        // knobs, so the slot has to push the kit back into it.
        Command k;
        k.type = Command::Type::SetKitParam;
        k.track = 1;
        k.param = static_cast<uint8_t>(ParamId::Decay);
        k.value = 0.83f;
        m.Push(k); Pump(m);
        c.track = 1;
        c.param = static_cast<uint8_t>(MachineId::SdPunch);
        m.Push(c); Pump(m);
        Check(m.patch().kit.params[1][static_cast<int>(ParamId::Decay)] == 0.83f,
              "the kit value survives a machine change");

        c.param = static_cast<uint8_t>(MachineId::Count); // out of range
        m.Push(c); Pump(m);
        Check(m.machine(1) == MachineId::SdPunch, "an invalid machine id is refused");

        // Machines are kit state, so they have to survive a flash round trip.
        static RamFlash<kQspiBytes> flash2;
        static Storage              st2;
        static Patch                back2;
        st2.Init(&flash2);
        m.RequestSnapshot(&st2.staging()); Pump(m);
        Check(st2.SaveStaged(7) == Storage::Result::Ok, "a patch with machines saves");

        c.track = 1; c.param = static_cast<uint8_t>(MachineId::Clap);
        m.Push(c); Pump(m);
        Check(m.machine(1) == MachineId::Clap, "the live machine moves on");

        Check(st2.LoadPatch(7, back2) == Storage::Result::Ok, "and loads back");
        m.RequestLoad(&back2); Pump(m);
        Check(m.machine(1) == MachineId::SdPunch,
              "loading restores the machine the patch was saved with");
    }

    std::printf("\nDECAY is authoritative on the wrapped 808 snare:\n");
    {
        // docs/02-firmware.md 5 measures the bare AnalogSnareDrum as ringing for
        // about a second at DECAY 0, never falling below -40 dB inside five
        // seconds, and not even monotonic. SnareDrum808 wraps it in an
        // amplitude envelope specifically to fix that, so the knob has to be
        // monotonic or the wrapper is not earning its place.
        auto Tail = [](MachineId id, float decay) {
            static MachineSlot slot;
            slot.Init(48000.f, id);
            IVoice *v = slot.voice();
            v->SetParam(ParamId::Decay, decay);
            v->SetParam(ParamId::Level, 0.8f);
            v->SetParam(ParamId::Drive, 0.f);
            v->Trigger(1.f);
            static float buf[48000 * 5];
            float        peak = 0.f;
            for(int i = 0; i < 48000 * 5; ++i)
            {
                buf[i] = v->Process();
                const float a = std::fabs(buf[i]);
                if(a > peak) peak = a;
            }
            for(int i = 48000 * 5 - 1; i >= 0; --i)
                if(std::fabs(buf[i]) > peak * 0.01f)
                    return i / 48.f; // ms
            return 0.f;
        };

        const float d[5] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
        float       ms[5];
        for(int i = 0; i < 5; ++i)
            ms[i] = Tail(MachineId::Sd808, d[i]);
        std::printf("      -40 dB ms at DECAY 0/.25/.5/.75/1: "
                    "%.0f %.0f %.0f %.0f %.0f\n",
                    ms[0], ms[1], ms[2], ms[3], ms[4]);

        bool monotonic = true;
        for(int i = 1; i < 5; ++i)
            monotonic &= ms[i] > ms[i - 1];
        Check(monotonic, "every step of the knob lengthens the tail");
        Check(ms[0] < 100.f, "and DECAY 0 is short rather than a second of ring");
        Check(ms[4] > 4.f * ms[0], "with real range end to end");
    }

    std::printf("\nNOTE transposes, per step:\n");
    {
        Check(NoteSemitones(0.5f) == 0, "centre is no transposition");
        Check(NoteSemitones(1.0f) == kNoteRange, "the top is +24 semitones");
        Check(NoteSemitones(0.0f) == -kNoteRange, "the bottom is -24");
        // Quantised on purpose: a pitch between semitones is not a pitch.
        const float octave_up = 0.5f + 12.f / (2.f * kNoteRange);
        Check(NoteSemitones(octave_up) == 12, "and an octave lands exactly on 12");

        // End to end: an octave up must actually double the pitch. Counted by
        // zero crossings, which is what a doubling looks like from outside.
        auto Crossings = [](float note) {
            static MachineSlot s;
            s.Init(48000.f, MachineId::Tom);
            IVoice *v = s.voice();
            v->SetParam(ParamId::Tune, 0.5f);
            v->SetParam(ParamId::Decay, 0.6f);
            v->SetParam(ParamId::Drive, 0.f);
            v->SetParam(ParamId::Level, 0.8f);
            v->SetParam(ParamId::Note, note);
            v->Trigger(1.f);
            int   zc = 0;
            float prev = 0.f;
            for(int i = 0; i < 24000; ++i)
            {
                const float y = v->Process();
                if(i > 2000 && (y >= 0.f) != (prev >= 0.f)) ++zc;
                prev = y;
            }
            return zc;
        };
        const int base = Crossings(0.5f);
        const int up   = Crossings(octave_up);
        std::printf("      %d crossings at centre, %d an octave up\n", base, up);
        Check(base > 0 && up > base * 1.7 && up < base * 2.3,
              "an octave up roughly doubles the pitch");
    }

    std::printf("\nthe triangle stays bounded:\n");
    {
        // Every other machine is an envelope times an oscillator and cannot run
        // away. This one is six near-unity-Q resonators, which is the shape that
        // actually can — so sweep the parameter space rather than spot-check it.
        auto Run = [](float tune, float dec, float tone, float snap,
                      float &peak, float &early, float &late) {
            static MachineSlot s;
            s.Init(48000.f, MachineId::Triangle);
            IVoice *v = s.voice();
            v->SetParam(ParamId::Tune, tune);
            v->SetParam(ParamId::Decay, dec);
            v->SetParam(ParamId::Tone, tone);
            v->SetParam(ParamId::Snap, snap);
            v->SetParam(ParamId::Drive, 0.f);
            v->SetParam(ParamId::Level, 0.8f);
            v->Trigger(1.f);
            peak = early = late = 0.f;
            const int n = 48000;
            for(int i = 0; i < n; ++i)
            {
                const float m = std::fabs(v->Process());
                if(m > peak) peak = m;
                if(i < n / 10 && m > early) early = m;
                if(i > n - n / 10 && m > late) late = m;
            }
        };

        int  unstable = 0, growing = 0;
        float worst = 0.f;
        for(int a = 0; a <= 2; ++a)
            for(int b = 0; b <= 2; ++b)
                for(int c = 0; c <= 2; ++c)
                    for(int d = 0; d <= 2; ++d)
                    {
                        float pk, e, l;
                        Run(a / 2.f, b / 2.f, c / 2.f, d / 2.f, pk, e, l);
                        if(!(pk < 2.f) || pk != pk) ++unstable;
                        if(l > e) ++growing;
                        if(pk > worst) worst = pk;
                    }
        std::printf("      81 settings, worst peak %.3f\n", worst);
        Check(unstable == 0, "no setting produces an unbounded or NaN output");
        Check(growing == 0, "and none grows louder over a second than it started");

        // The long inharmonic ring is the whole point; a short one is a rimshot.
        float pk, e, l;
        Run(0.4f, 0.2f, 0.5f, 0.3f, pk, e, l);
        const float shortish = l;
        Run(0.4f, 0.9f, 0.5f, 0.3f, pk, e, l);
        Check(l > shortish * 4.f, "DECAY buys a dramatically longer ring");
    }

    std::printf("\nqueue overflow is survivable:\n");
    {
        static Machine m; m.Init(48000.f);
        int accepted = 0;
        for(int i = 0; i < 500; ++i)
        {
            Command c; c.type = Command::Type::ToggleStep; c.step = 0;
            if(m.Push(c))
                ++accepted;
        }
        Check(accepted < 500, "the queue refuses rather than overruns");
        Pump(m);
        Check(true, "and the audio side keeps running afterwards");
    }

    std::printf("\n%s\n", failures ? "MACHINE TESTS FAILED" : "all machine tests passed");
    return failures;
}
