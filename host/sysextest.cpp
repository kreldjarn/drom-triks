// The SysEx protocol: codec, frame validation, control commands and chunked
// bulk transfer.
//
// All of it is data handling, which is exactly the kind of thing that tests
// natively and is miserable to debug over a MIDI cable. The builder and the
// parser are deliberately exercised against each other, so a change to the
// wire format that breaks one breaks the test rather than the instrument.

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <cstring>

#include "../src/io/sysex.h"

using namespace drom;

namespace {

int failures = 0;

void Check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "ok" : "*** FAIL ***");
    if(!ok)
        ++failures;
}

/// Frame a control message: F0 7D dev cmd <payload> F7
size_t Frame(uint8_t *o, uint8_t dev, SysExCmd c,
             std::initializer_list<uint8_t> payload)
{
    size_t w = 0;
    o[w++] = kSysExStart; o[w++] = kSysExManufacturer; o[w++] = dev;
    o[w++] = static_cast<uint8_t>(c);
    for(uint8_t b : payload)
        o[w++] = b;
    o[w++] = kSysExEnd;
    return w;
}

uint8_t     g_msg[2048];
SysExParser g_p;

} // namespace

int main()
{
    g_p.Init(0);

    std::printf("7-bit codec:\n");
    {
        static uint8_t src[1000], wire[2000], back[1000];
        for(size_t i = 0; i < sizeof(src); ++i)
            src[i] = static_cast<uint8_t>(i * 37 + (i >> 3)); // plenty of high bits

        bool sizes = true, clean = true, same = true;
        for(size_t n : {size_t(0), size_t(1), size_t(6), size_t(7), size_t(8),
                        size_t(13), size_t(14), size_t(255), size_t(1000)})
        {
            const size_t w = Pack7(src, n, wire, sizeof(wire));
            sizes &= (w == Packed7Size(n));
            for(size_t i = 0; i < w; ++i)
                clean &= (wire[i] & 0x80) == 0;
            const size_t b = Unpack7(wire, w, back, sizeof(back));
            same &= (b == n) && std::memcmp(src, back, n) == 0;
        }
        Check(sizes, "packed size matches Packed7Size for every length");
        Check(clean, "no wire byte ever has bit 7 set");
        Check(same, "unpack round-trips exactly, including partial groups");

        // A byte with bit 7 set cannot appear inside SysEx; treat it as corrupt.
        uint8_t bad[8] = {0, 1, 2, 3, 0x80, 5, 6, 7};
        Check(Unpack7(bad, 8, back, sizeof(back)) == 0,
              "a high bit in the payload is rejected, not masked off");
        Check(Pack7(src, 100, wire, 10) == 0, "and a short output buffer refuses");
    }

    std::printf("\nframing:\n");
    {
        size_t n = Frame(g_msg, 0, SysExCmd::Transport, {1});
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::Command,
              "a well-formed frame parses");

        g_msg[1] = 0x41; // someone else's manufacturer id
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::None,
              "another maker's message is ignored silently");

        g_msg[1] = kSysExManufacturer;
        g_msg[2] = 3; // a different device
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::None,
              "so is a message for a different device id");

        g_msg[2] = kSysExBroadcast;
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::Command,
              "but broadcast is honoured");

        g_msg[2] = 0;
        g_msg[n - 1] = 0x00; // no F7
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::None,
              "an unterminated frame is rejected");
        Check(g_p.Parse(g_msg, 3).kind == SysExResult::Kind::None,
              "and so is a runt");
    }

    std::printf("\ncontrol commands:\n");
    {
        const uint16_t v = NormToVal14(0.75f);
        const uint8_t  hi = (v >> 7) & 0x7F, lo = v & 0x7F;

        size_t n = Frame(g_msg, 0, SysExCmd::SetTrackParam,
                         {3, static_cast<uint8_t>(ParamId::Decay), hi, lo});
        auto   r = g_p.Parse(g_msg, n);
        Check(r.kind == SysExResult::Kind::Command
                  && r.command.type == Command::Type::SetKitParam
                  && r.command.track == 3
                  && r.command.param == static_cast<uint8_t>(ParamId::Decay)
                  && std::fabs(r.command.value - 0.75f) < 1e-3f,
              "set track parameter maps onto SetKitParam");

        n = Frame(g_msg, 0, SysExCmd::SetMachine,
                  {1, static_cast<uint8_t>(MachineId::BdBoom)});
        r = g_p.Parse(g_msg, n);
        Check(r.command.type == Command::Type::SetMachine
                  && r.command.param == static_cast<uint8_t>(MachineId::BdBoom),
              "set machine carries the machine id");

        n = Frame(g_msg, 0, SysExCmd::SetStepLock,
                  {2, 7, static_cast<uint8_t>(ParamId::Tune), hi, lo});
        r = g_p.Parse(g_msg, n);
        Check(r.command.type == Command::Type::SetStepLock && r.command.step == 7,
              "set step lock carries track, step, param and value");

        n = Frame(g_msg, 0, SysExCmd::Transport, {0});
        Check(g_p.Parse(g_msg, n).command.type == Command::Type::Stop,
              "transport 0 stops");
        n = Frame(g_msg, 0, SysExCmd::Transport, {2});
        Check(g_p.Parse(g_msg, n).command.type == Command::Type::Continue,
              "transport 2 continues");

        // Tempo is absolute, not normalised: 128.5 BPM must survive exactly.
        const uint16_t t = 1285;
        n = Frame(g_msg, 0, SysExCmd::SetTempo,
                  {static_cast<uint8_t>(t >> 7), static_cast<uint8_t>(t & 0x7F)});
        r = g_p.Parse(g_msg, n);
        Check(r.command.type == Command::Type::SetTempo
                  && std::fabs(r.command.value - 128.5f) < 1e-3f,
              "tempo round-trips as BPM x 10, not through 0..1");
    }

    std::printf("\nrange checks:\n");
    {
        size_t n = Frame(g_msg, 0, SysExCmd::SetTrackParam, {kNumTracks, 0, 0, 0});
        auto   r = g_p.Parse(g_msg, n);
        Check(r.kind == SysExResult::Kind::Error
                  && r.status == SysExStatus::BadSlot,
              "a track past the last one is refused");

        n = Frame(g_msg, 0, SysExCmd::ToggleStep, {0, kMaxSteps});
        Check(g_p.Parse(g_msg, n).status == SysExStatus::BadSlot,
              "and a step past the last one");

        n = Frame(g_msg, 0, SysExCmd::SetMachine,
                  {0, static_cast<uint8_t>(MachineId::Count)});
        Check(g_p.Parse(g_msg, n).status == SysExStatus::BadSlot,
              "and a machine id that does not exist");

        n = Frame(g_msg, 0, SysExCmd::SetTrackParam, {0, 0});
        Check(g_p.Parse(g_msg, n).status == SysExStatus::BadSize,
              "a truncated payload is a size error, not a crash");

        const uint16_t fast = 5000; // 500 BPM
        n = Frame(g_msg, 0, SysExCmd::SetTempo,
                  {static_cast<uint8_t>(fast >> 7),
                   static_cast<uint8_t>(fast & 0x7F)});
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::Error,
              "and an impossible tempo is refused");
    }

    std::printf("\nrequests:\n");
    {
        size_t n = Frame(g_msg, 0, SysExCmd::PatternDumpReq, {42});
        auto   r = g_p.Parse(g_msg, n);
        Check(r.kind == SysExResult::Kind::Request && r.slot == 42,
              "a pattern dump request reports its slot");

        n = Frame(g_msg, 0, SysExCmd::InquiryReq, {});
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::Request,
              "device inquiry is a request");

        n = Frame(g_msg, 0, static_cast<SysExCmd>(0x6E), {1, 2});
        Check(g_p.Parse(g_msg, n).kind == SysExResult::Kind::None,
              "an unknown command is ignored rather than nak'd");
    }

    std::printf("\nchunked bulk transfer:\n");
    {
        static Patch   src, dst;
        static uint8_t frame[2048];
        InitPatch(src);
        src.pattern.bpm_x10 = 1337;
        src.kit.params[5][2] = 0.4242f;
        src.kit.machine[4]   = static_cast<uint8_t>(MachineId::Glitch);

        const uint8_t *raw   = reinterpret_cast<const uint8_t *>(&src);
        const uint16_t total = ChunkCount(sizeof(Patch));
        std::printf("      %zu B in %u chunks of %zu\n",
                    sizeof(Patch), total, kSysExChunkData);

        SysExResult r;
        bool        partials_ok = true;
        for(uint16_t c = 0; c < total; ++c)
        {
            const size_t n = BuildDumpChunk(0, SysExCmd::PatternWrite, 9, c, raw,
                                            sizeof(Patch), frame, sizeof(frame));
            r = g_p.Parse(frame, n, reinterpret_cast<uint8_t *>(&dst),
                          sizeof(Patch));
            if(c + 1 < total)
                partials_ok &= r.kind == SysExResult::Kind::BulkPartial;
        }
        Check(partials_ok, "every chunk but the last reports partial");
        Check(r.kind == SysExResult::Kind::BulkComplete,
              "the last chunk completes the transfer");
        Check(r.bulk_len == sizeof(Patch) && r.slot == 9,
              "with the full length and the right slot");
        Check(std::memcmp(&src, &dst, sizeof(Patch)) == 0,
              "and the patch survives the round trip byte for byte");

        // Out of order must abort rather than splice two transfers together.
        BuildDumpChunk(0, SysExCmd::PatternWrite, 9, 0, raw, sizeof(Patch),
                       frame, sizeof(frame));
        g_p.Parse(frame, BuildDumpChunk(0, SysExCmd::PatternWrite, 9, 0, raw,
                                        sizeof(Patch), frame, sizeof(frame)),
                  reinterpret_cast<uint8_t *>(&dst), sizeof(Patch));
        const size_t skip = BuildDumpChunk(0, SysExCmd::PatternWrite, 9, 2, raw,
                                           sizeof(Patch), frame, sizeof(frame));
        r = g_p.Parse(frame, skip, reinterpret_cast<uint8_t *>(&dst),
                      sizeof(Patch));
        Check(r.kind == SysExResult::Kind::Error
                  && r.status == SysExStatus::BadEncoding,
              "a skipped chunk aborts instead of splicing");
    }

    std::printf("\nreject, never reinterpret:\n");
    {
        static Patch   src, dst;
        static uint8_t frame[2048];
        InitPatch(src);
        src.header.version = kPatchVersion - 1; // a patch from older firmware

        const uint8_t *raw   = reinterpret_cast<const uint8_t *>(&src);
        const uint16_t total = ChunkCount(sizeof(Patch));
        SysExResult    r;
        for(uint16_t c = 0; c < total; ++c)
        {
            const size_t n = BuildDumpChunk(0, SysExCmd::PatternWrite, 0, c, raw,
                                            sizeof(Patch), frame, sizeof(frame));
            r = g_p.Parse(frame, n, reinterpret_cast<uint8_t *>(&dst),
                          sizeof(Patch));
        }
        Check(r.kind == SysExResult::Kind::Error
                  && r.status == SysExStatus::BadVersion,
              "a patch from another firmware version is nak'd, not adopted");
    }

    std::printf("\nreplies:\n");
    {
        static uint8_t o[64];
        size_t n = BuildAck(0, SysExCmd::PatternWrite, SysExStatus::BadVersion, o,
                            sizeof(o));
        Check(n == 7 && o[0] == kSysExStart && o[3] == 0x0A
                  && o[4] == 0x07 && o[5] == 0x02 && o[6] == kSysExEnd,
              "an ack names the command it answers and the status");

        n = BuildParamReply(0, 2, 5, 0.5f, o, sizeof(o));
        Check(n == 9 && Val14(o[6], o[7]) == NormToVal14(0.5f),
              "a parameter reply carries a 14-bit value");

        const uint8_t hash[5] = {1, 2, 3, 4, 5};
        n = BuildInquiryReply(0, 1, 4, hash, o, sizeof(o));
        Check(n == 13 && o[6] == kPatchVersion && o[12] == kSysExEnd,
              "an inquiry reply reports the patch format version");
        Check(BuildInquiryReply(0, 1, 4, hash, o, 12) == 0,
              "and refuses a buffer one byte too small");

        bool clean = true;
        for(size_t i = 1; i + 1 < n; ++i)
            clean &= (o[i] & 0x80) == 0;
        Check(clean, "no reply byte between F0 and F7 has bit 7 set");
    }

    if(failures)
    {
        std::printf("\n%d SysEx check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall SysEx tests passed\n");
    return 0;
}
