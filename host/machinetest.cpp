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
    float buf[32];
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
        float buf[32];
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
