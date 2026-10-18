// On-target CPU profile: what each machine, the channel strip, the master FX
// and the whole Machine cost on the H750, in CPU cycles per sample.
//
//   make PROGRAM=bench && make PROGRAM=bench flash
//   then open the Seed's USB serial port; the run waits for it.
//
// This is the Phase 2 "know your CPU budget, measured on the H750, not on the
// host" item (docs/04-development-plan.md). The budget is the core clock over
// the sample rate: 480 MHz / 48 kHz = 10 000 cycles per sample, for
// everything, every sample.
//
// Cycles come from the DWT cycle counter, so they are exact and include cache
// effects — but each machine is measured alone and hot in cache, which is the
// best case. The full-Machine rows are the honest ones.

#include "daisy_seed.h"
#include "daisysp.h"

#include "machine.h"

using namespace daisy;
using namespace drom;

namespace {

DaisySeed hw;

Machine     g_machine;
MachineSlot g_slot;
ChannelStrip g_strip;

constexpr size_t  kDelayFrames = 2 * 48000; // as in main.cpp
float DSY_SDRAM_BSS g_delay[kDelayFrames * 2];
constexpr size_t    kReverbFloats = PlateReverb::BufferFloats(48000.f);
// AXI SRAM, not SDRAM: the plate reads 14 taps and 12 lines scattered over
// its buffer every sample, and from SDRAM those cache misses made it cost
// more than ReverbSc. ld/drom_sram.lds defines the section.
float __attribute__((section(".sram_bss"))) g_reverb[kReverbFloats];

constexpr float    kSampleRate = 48000.f;
constexpr uint32_t kBudget     = 480000000u / 48000u; // cycles per sample
constexpr int      kSamples    = 48000;               // one second of audio

/// Volatile so the compiler cannot discard a voice whose output is unused.
volatile float g_sink;

inline uint32_t Cycles() { return DWT->CYCCNT; }

void Row(const char *what, uint32_t cycles_per_sample)
{
    // Integer per-mille, so this does not depend on printf float support.
    const uint32_t pm = cycles_per_sample * 1000u / kBudget;
    hw.PrintLine("%-14s %6lu cyc/sample  %3lu.%lu %% of budget", what,
                 static_cast<unsigned long>(cycles_per_sample),
                 static_cast<unsigned long>(pm / 10),
                 static_cast<unsigned long>(pm % 10));
}

/// One voice alone for a second, two ways.
///
/// `sounding = false`: one hit at default DECAY, then mostly silence. Voices
/// that skip their work once silent look cheap here, so this is a *typical*
/// cost, not a bound — the first version of this bench reported only this,
/// and SD 808 at 688 cycles hid a voice that overruns the CPU at long DECAY.
///
/// `sounding = true`: DECAY at maximum and a retrigger every 100 ms, so the
/// voice never goes idle. That is the cost a budget has to be built on.
uint32_t BenchMachine(MachineId id, bool sounding)
{
    g_slot.Init(kSampleRate, id);
    IVoice *v = g_slot.voice();
    if(sounding)
        v->SetParam(ParamId::Decay, 1.f);
    // In 32-sample blocks through ProcessBlock, as the engine renders.
    constexpr int  kRetrigger = 4800; // a multiple of the block, so hits land on one
    float          buf[32];
    float          acc = 0.f;
    const uint32_t t0  = Cycles();
    for(int i = 0; i < kSamples; i += 32)
    {
        if(i == 0 || (sounding && i % kRetrigger == 0))
            v->Trigger(1.f);
        v->ProcessBlock(buf, 32);
        acc += buf[0];
    }
    const uint32_t dt = Cycles() - t0;
    g_sink            = acc;
    return dt / kSamples;
}

uint32_t BenchStrip()
{
    g_strip.Init(kSampleRate);
    MixBus         bus;
    const uint32_t t0 = Cycles();
    for(int i = 0; i < kSamples; ++i)
        g_strip.Process(0.25f, bus);
    const uint32_t dt = Cycles() - t0;
    g_sink            = bus.l;
    return dt / kSamples;
}

/// The whole machine as the audio callback runs it, in 32-frame blocks.
uint32_t BenchMachineBlocks(bool playing)
{
    Command c;
    c.type = playing ? Command::Type::Start : Command::Type::Stop;
    g_machine.Push(c);
    float          buf[64];
    const int      blocks = kSamples / 32;
    const uint32_t t0     = Cycles();
    for(int b = 0; b < blocks; ++b)
        g_machine.Process(buf, 32);
    const uint32_t dt = Cycles() - t0;
    g_sink            = buf[0];
    return dt / kSamples;
}

} // namespace

int main(void)
{
    hw.Init(true); // 480 MHz, as the instrument runs
    hw.StartLog(true);

    // The cycle counter is off out of reset.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    hw.PrintLine("drom-triks CPU profile, H750 @ %lu MHz, budget %lu cyc/sample",
                 static_cast<unsigned long>(System::GetSysClkFreq() / 1000000u),
                 static_cast<unsigned long>(kBudget));
    hw.PrintLine("");
    hw.PrintLine("one voice alone      one hit   sounding  sounding %% of budget");
    for(int m = 0; m < static_cast<int>(MachineId::Count); ++m)
    {
        const auto     id  = static_cast<MachineId>(m);
        const uint32_t hit = BenchMachine(id, false);
        const uint32_t snd = BenchMachine(id, true);
        const uint32_t pm  = snd * 1000u / kBudget;
        hw.PrintLine("%-14s %9lu %10lu  %3lu.%lu %%", MachineName(id),
                     static_cast<unsigned long>(hit), static_cast<unsigned long>(snd),
                     static_cast<unsigned long>(pm / 10), static_cast<unsigned long>(pm % 10));
    }

    hw.PrintLine("");
    Row("channel strip", BenchStrip());

    g_machine.Init(kSampleRate, g_delay, kDelayFrames, g_reverb, kReverbFloats);
    Patch &p = g_machine.mutable_patch();
    for(auto &t : p.pattern.tracks)
        for(int s = 0; s < 16; ++s)
            t.steps[s].flags = kStepActive; // every track, every step
    hw.PrintLine("");
    hw.PrintLine("whole Machine, default kit, 32-frame blocks:");
    // Two seconds first, so "stopped" measures the machine at rest — the
    // reverb runs until its own tail has settled (~0.9 s), then sleeps.
    BenchMachineBlocks(false);
    BenchMachineBlocks(false);
    Row("stopped", BenchMachineBlocks(false));
    Row("all 12 playing", BenchMachineBlocks(true));
    // The worst cases. Every track on every step with DECAY at its maximum,
    // so no voice ever sleeps; then the same with the reverb and compressor
    // engaged, since both sleep or bypass at their defaults and would
    // otherwise flatter the number.
    auto push = [](Command::Type type, int track, int param, float value) {
        Command c;
        c.type  = type;
        c.track = static_cast<uint8_t>(track);
        c.param = static_cast<uint8_t>(param);
        c.value = value;
        g_machine.Push(c);
    };
    for(int t = 0; t < kNumTracks; ++t)
        push(Command::Type::SetKitParam, t, static_cast<int>(ParamId::Decay), 1.f);
    Row("+ DECAY max", BenchMachineBlocks(true));
    for(int t = 0; t < kNumTracks; ++t)
    {
        push(Command::Type::SetKitParam, t, static_cast<int>(ParamId::ReverbSend), 0.5f);
        push(Command::Type::SetKitParam, t, static_cast<int>(ParamId::DelaySend), 0.3f);
    }
    push(Command::Type::SetFxParam, 0, static_cast<int>(FxId::ReverbLevel), 0.5f);
    push(Command::Type::SetFxParam, 0, static_cast<int>(FxId::CompRatio), 0.5f);
    push(Command::Type::SetFxParam, 0, static_cast<int>(FxId::CompThreshold), 0.5f);
    Row("+ verb, delay, comp", BenchMachineBlocks(true));

    // The last state runs forever, so OpenOCD can sample it:
    //
    //   openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
    //     -c "init" -c "profile 30 build/gmon.out" -c "resume" -c "shutdown"
    //   arm-none-eabi-gprof -b -p build/drom-triks-bench.elf build/gmon.out
    hw.PrintLine("");
    hw.PrintLine("done; now rendering the worst case forever, for a profiler");

    float buf[64];
    while(1)
    {
        g_machine.Process(buf, 32);
        g_sink = buf[0];
    }
}
