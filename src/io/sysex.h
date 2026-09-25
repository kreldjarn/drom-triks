#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "command.h"
#include "patch.h"
#include "settings.h"

// The SysEx protocol from docs/06-midi.md §8: backup, restore and control.
//
// Free of DaisySP and libDaisy on purpose, so the whole protocol - the codec,
// the frame validation, the range checks and the chunk reassembly - tests
// natively. Every bug this can have is a data bug, and data bugs are exactly
// what a host test catches and a board does not.
//
// **Parse in the main loop, never in the MIDI interrupt.** The command queue is
// single-producer/single-consumer and docs/02-firmware.md §4 is explicit that
// the ISR never pushes commands. A parser in the UART interrupt makes it
// two-producer, which does not crash - it corrupts a pattern occasionally.

namespace drom {

inline constexpr uint8_t kSysExStart        = 0xF0;
inline constexpr uint8_t kSysExEnd          = 0xF7;
inline constexpr uint8_t kSysExManufacturer = 0x7D; ///< non-commercial ID
inline constexpr uint8_t kSysExBroadcast    = 0x7F;

enum class SysExCmd : uint8_t
{
    // librarian
    PatternDumpReq  = 0x01, PatternData   = 0x02,
    KitDumpReq      = 0x03, KitData       = 0x04,
    SettingsDumpReq = 0x05, SettingsData  = 0x06,
    PatternWrite    = 0x07, KitWrite      = 0x08, SettingsWrite = 0x09,
    Ack             = 0x0A,
    InquiryReq      = 0x0F, InquiryReply  = 0x10,

    // control
    SetTrackParam = 0x20, SetFxParam     = 0x21, SetMachine   = 0x22,
    ToggleStep    = 0x23, SetStepLock    = 0x24, ClearLocks   = 0x25,
    SetStepField  = 0x26, SetTrackMute   = 0x27, SetTempo     = 0x28,
    Transport     = 0x29,

    // query
    QueryParamReq = 0x30, QueryParamReply = 0x31,
    QueryStateReq = 0x32, QueryStateReply = 0x33,
};

enum class SysExStatus : uint8_t
{
    Ok = 0, BadSlot = 1, BadVersion = 2, BadSize = 3, BadEncoding = 4, Busy = 5
};

// --- 7-bit codec -------------------------------------------------------------
//
// Seven data bytes become eight on the wire: one byte carrying the seven
// stripped high bits, then the seven stripped bytes. Payloads grow by 8/7.

inline constexpr size_t Packed7Size(size_t n) { return n + (n + 6) / 7; }

inline constexpr size_t Unpacked7Size(size_t n)
{
    return (n / 8) * 7 + (n % 8 ? n % 8 - 1 : 0);
}

/// Returns bytes written, or 0 if `cap` is too small.
inline size_t Pack7(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    if(Packed7Size(n) > cap)
        return 0;
    size_t o = 0;
    for(size_t i = 0; i < n; i += 7)
    {
        const size_t g = (n - i) < 7 ? (n - i) : 7;
        uint8_t      m = 0;
        for(size_t k = 0; k < g; ++k)
            m |= static_cast<uint8_t>(((src[i + k] >> 7) & 1u) << k);
        dst[o++] = m;
        for(size_t k = 0; k < g; ++k)
            dst[o++] = src[i + k] & 0x7F;
    }
    return o;
}

/// Returns bytes written, or 0 on a short buffer or a wire byte with bit 7 set
/// — which cannot legally appear inside a SysEx message and means the frame is
/// corrupt rather than merely unexpected.
inline size_t Unpack7(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    if(Unpacked7Size(n) > cap)
        return 0;
    size_t o = 0;
    for(size_t i = 0; i < n; i += 8)
    {
        const size_t g = (n - i) < 8 ? (n - i) : 8;
        if(src[i] & 0x80)
            return 0;
        for(size_t k = 1; k < g; ++k)
        {
            if(src[i + k] & 0x80)
                return 0;
            dst[o++] = static_cast<uint8_t>(src[i + k]
                                            | (((src[i] >> (k - 1)) & 1u) << 7));
        }
    }
    return o;
}

// --- 14-bit values -----------------------------------------------------------

inline constexpr uint16_t kVal14Max = 16383;

inline uint16_t Val14(uint8_t msb, uint8_t lsb)
{
    return static_cast<uint16_t>((msb & 0x7F) << 7 | (lsb & 0x7F));
}

inline float Val14ToNorm(uint16_t v) { return static_cast<float>(v) / kVal14Max; }

inline uint16_t NormToVal14(float f)
{
    if(f < 0.f) f = 0.f;
    if(f > 1.f) f = 1.f;
    return static_cast<uint16_t>(f * kVal14Max + 0.5f);
}

// --- chunking ----------------------------------------------------------------

/// Data bytes per chunk. A multiple of seven so each chunk packs to a whole
/// number of wire groups — 896 in, 1024 out, which is the frame size §8 asks
/// for. A whole 30 kB pattern in one message is legal and many hosts hate it.
inline constexpr size_t kSysExChunkData = 896;

inline constexpr uint16_t ChunkCount(size_t n)
{
    return static_cast<uint16_t>((n + kSysExChunkData - 1) / kSysExChunkData);
}

// --- parsing -----------------------------------------------------------------

struct SysExResult
{
    enum class Kind : uint8_t
    {
        None = 0,     ///< not for us: wrong manufacturer or device id
        Command,      ///< push `command` onto the queue
        Request,      ///< the caller should reply — see `cmd` and `slot`
        BulkPartial,  ///< a chunk landed; more expected
        BulkComplete, ///< `bulk_len` bytes are valid in the caller's buffer
        Error,        ///< reply with an Ack carrying `status`
    };

    Kind        kind     = Kind::None;
    SysExCmd    cmd      = SysExCmd::Ack;
    Command     command  = {};
    uint8_t     slot     = 0;
    size_t      bulk_len = 0;
    SysExStatus status   = SysExStatus::Ok;
};

class SysExParser
{
  public:
    void Init(uint8_t device_id = 0)
    {
        dev_ = device_id;
        Reset();
    }

    void Reset()
    {
        bulk_active_ = false;
        bulk_next_   = 0;
        bulk_total_  = 0;
        bulk_len_    = 0;
    }

    /// `msg` is one complete frame, F0 through F7. Bulk payloads are decoded
    /// into `bulk` — in the firmware that is Storage::staging(), so a SysEx
    /// load and a flash load end up being the same operation and the protocol
    /// costs no RAM of its own.
    SysExResult Parse(const uint8_t *msg,
                      size_t         len,
                      uint8_t       *bulk     = nullptr,
                      size_t         bulk_cap = 0)
    {
        SysExResult r;
        // F0 <mfr> <dev> <cmd> ... F7
        if(!msg || len < 5 || msg[0] != kSysExStart || msg[len - 1] != kSysExEnd)
            return r;
        if(msg[1] != kSysExManufacturer)
            return r; // another maker's message; silence is the correct reply
        if(msg[2] != dev_ && msg[2] != kSysExBroadcast)
            return r;

        const SysExCmd cmd = static_cast<SysExCmd>(msg[3]);
        const uint8_t *d   = msg + 4;
        const size_t   n   = len - 5; // payload, excluding F7

        r.cmd = cmd;
        switch(cmd)
        {
            // ---- control ----------------------------------------------------
            case SysExCmd::SetTrackParam:
                if(n < 4) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks
                   || d[1] >= static_cast<uint8_t>(ParamId::Count))
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetKitParam, d[0], 0, d[1],
                           Val14ToNorm(Val14(d[2], d[3])));

            case SysExCmd::SetFxParam:
                if(n < 3) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumFxParams) return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetFxParam, 0, 0, d[0],
                           Val14ToNorm(Val14(d[1], d[2])));

            case SysExCmd::SetMachine:
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks
                   || d[1] >= static_cast<uint8_t>(MachineId::Count))
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetMachine, d[0], 0, d[1], 0.f);

            case SysExCmd::ToggleStep:
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks || d[1] >= kMaxSteps)
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::ToggleStep, d[0], d[1], 0, 0.f);

            case SysExCmd::SetStepLock:
                if(n < 5) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks || d[1] >= kMaxSteps
                   || d[2] >= static_cast<uint8_t>(ParamId::Count))
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetStepLock, d[0], d[1], d[2],
                           Val14ToNorm(Val14(d[3], d[4])));

            case SysExCmd::ClearLocks:
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks || d[1] >= kMaxSteps)
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::ClearStepLocks, d[0], d[1], 0, 0.f);

            case SysExCmd::SetStepField:
                if(n < 5) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks || d[1] >= kMaxSteps
                   || d[2] >= static_cast<uint8_t>(StepField::Count))
                    return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetStepField, d[0], d[1], d[2],
                           Val14ToNorm(Val14(d[3], d[4])));

            case SysExCmd::SetTrackMute:
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks) return Bad(r, SysExStatus::BadSlot);
                return Cmd(r, Command::Type::SetTrackMute, d[0], 0, 0,
                           d[1] ? 1.f : 0.f);

            // Tempo is BPM x 10 rather than normalised: a tempo is an absolute
            // quantity and round-tripping it through 0..1 loses the exact value
            // a host asked for.
            case SysExCmd::SetTempo:
            {
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                const float bpm = static_cast<float>(Val14(d[0], d[1])) * 0.1f;
                if(bpm < 20.f || bpm > 300.f) return Bad(r, SysExStatus::BadSize);
                return Cmd(r, Command::Type::SetTempo, 0, 0, 0, bpm);
            }

            case SysExCmd::Transport:
                if(n < 1) return Bad(r, SysExStatus::BadSize);
                switch(d[0])
                {
                    case 0: return Cmd(r, Command::Type::Stop, 0, 0, 0, 0.f);
                    case 1: return Cmd(r, Command::Type::Start, 0, 0, 0, 0.f);
                    case 2: return Cmd(r, Command::Type::Continue, 0, 0, 0, 0.f);
                    default: return Bad(r, SysExStatus::BadSize);
                }

            // ---- requests the caller answers --------------------------------
            case SysExCmd::PatternDumpReq:
            case SysExCmd::KitDumpReq:
                if(n < 1) return Bad(r, SysExStatus::BadSize);
                r.slot = d[0];
                r.kind = SysExResult::Kind::Request;
                return r;

            case SysExCmd::SettingsDumpReq:
            case SysExCmd::InquiryReq:
            case SysExCmd::QueryStateReq:
                r.kind = SysExResult::Kind::Request;
                return r;

            case SysExCmd::QueryParamReq:
                if(n < 2) return Bad(r, SysExStatus::BadSize);
                if(d[0] >= kNumTracks
                   || d[1] >= static_cast<uint8_t>(ParamId::Count))
                    return Bad(r, SysExStatus::BadSlot);
                r.command.track = d[0];
                r.command.param = d[1];
                r.kind          = SysExResult::Kind::Request;
                return r;

            // ---- bulk writes ------------------------------------------------
            case SysExCmd::PatternWrite:
            case SysExCmd::KitWrite:
            case SysExCmd::SettingsWrite:
                return Bulk(r, cmd, d, n, bulk, bulk_cap);

            default:
                return r; // unknown command: ignore rather than nak
        }
    }

  private:
    static SysExResult &Bad(SysExResult &r, SysExStatus s)
    {
        r.kind   = SysExResult::Kind::Error;
        r.status = s;
        return r;
    }

    static SysExResult &Cmd(SysExResult &r,
                            Command::Type t,
                            uint8_t       track,
                            uint8_t       step,
                            uint8_t       param,
                            float         value)
    {
        r.command.type  = t;
        r.command.track = track;
        r.command.step  = step;
        r.command.param = param;
        r.command.value = value;
        r.kind          = SysExResult::Kind::Command;
        return r;
    }

    /// `<slot> <chunk14> <total14> <wire…>`, reassembled into `bulk`.
    SysExResult &Bulk(SysExResult   &r,
                      SysExCmd       cmd,
                      const uint8_t *d,
                      size_t         n,
                      uint8_t       *bulk,
                      size_t         bulk_cap)
    {
        if(n < 5 || !bulk)
            return Bad(r, SysExStatus::BadSize);
        const uint8_t  slot  = d[0];
        const uint16_t chunk = Val14(d[1], d[2]);
        const uint16_t total = Val14(d[3], d[4]);
        if(total == 0)
            return Bad(r, SysExStatus::BadSize);

        // Chunk 0 restarts the transfer. Anything else must continue the one in
        // progress, or a dropped frame would silently splice two transfers.
        if(chunk == 0)
        {
            bulk_active_ = true;
            bulk_cmd_    = cmd;
            bulk_slot_   = slot;
            bulk_total_  = total;
            bulk_next_   = 0;
            bulk_len_    = 0;
        }
        else if(!bulk_active_ || cmd != bulk_cmd_ || slot != bulk_slot_
                || chunk != bulk_next_ || total != bulk_total_)
        {
            Reset();
            return Bad(r, SysExStatus::BadEncoding);
        }

        const size_t got
            = Unpack7(d + 5, n - 5, bulk + bulk_len_, bulk_cap - bulk_len_);
        if(got == 0 && n - 5 > 0)
        {
            Reset();
            return Bad(r, SysExStatus::BadEncoding);
        }
        bulk_len_ += got;
        ++bulk_next_;

        r.slot     = slot;
        r.bulk_len = bulk_len_;
        if(bulk_next_ < bulk_total_)
        {
            r.kind = SysExResult::Kind::BulkPartial;
            return r;
        }

        // Last chunk: the payload has to pass the same header check a flash
        // load does. A host sending a v3 pattern to a v4 device is rejected,
        // never reinterpreted — see docs/06-midi.md §8.5.
        const SysExStatus st = ValidateBulk(cmd, bulk, bulk_len_);
        Reset();
        if(st != SysExStatus::Ok)
            return Bad(r, st);
        r.kind = SysExResult::Kind::BulkComplete;
        return r;
    }

    static SysExStatus ValidateBulk(SysExCmd cmd, const uint8_t *b, size_t n)
    {
        size_t want_total = 0, want_payload = 0;
        switch(cmd)
        {
            case SysExCmd::PatternWrite:
                want_total   = sizeof(Patch);
                want_payload = sizeof(Pattern) + sizeof(Kit);
                break;
            case SysExCmd::KitWrite:
                want_total   = sizeof(SaveHeader) + sizeof(Kit);
                want_payload = sizeof(Kit);
                break;
            case SysExCmd::SettingsWrite:
                // Settings carries its own magic and version rather than a
                // SaveHeader, so only the length is checkable here.
                return n == sizeof(Settings) ? SysExStatus::Ok
                                             : SysExStatus::BadSize;
            default: return SysExStatus::BadSize;
        }
        if(n != want_total)
            return SysExStatus::BadSize;
        SaveHeader h;
        std::memcpy(&h, b, sizeof(h));
        if(h.magic != kPatchMagic)
            return SysExStatus::BadEncoding;
        if(h.version != kPatchVersion)
            return SysExStatus::BadVersion;
        if(h.payload_size != static_cast<uint16_t>(want_payload))
            return SysExStatus::BadSize;
        return SysExStatus::Ok;
    }

    uint8_t  dev_        = 0;
    bool     bulk_active_ = false;
    SysExCmd bulk_cmd_   = SysExCmd::Ack;
    uint8_t  bulk_slot_  = 0;
    uint16_t bulk_next_  = 0;
    uint16_t bulk_total_ = 0;
    size_t   bulk_len_   = 0;
};

// --- building replies --------------------------------------------------------

inline size_t BuildAck(uint8_t dev, SysExCmd about, SysExStatus st,
                       uint8_t *o, size_t cap)
{
    if(cap < 7) return 0;
    o[0] = kSysExStart; o[1] = kSysExManufacturer; o[2] = dev;
    o[3] = static_cast<uint8_t>(SysExCmd::Ack);
    o[4] = static_cast<uint8_t>(about);
    o[5] = static_cast<uint8_t>(st);
    o[6] = kSysExEnd;
    return 7;
}

/// One chunk of a dump. `data` is the whole payload; this emits chunk `chunk`.
inline size_t BuildDumpChunk(uint8_t dev, SysExCmd cmd, uint8_t slot,
                             uint16_t chunk, const uint8_t *data, size_t n,
                             uint8_t *o, size_t cap)
{
    const uint16_t total = ChunkCount(n);
    if(chunk >= total) return 0;
    const size_t off = static_cast<size_t>(chunk) * kSysExChunkData;
    const size_t len = (n - off) < kSysExChunkData ? (n - off) : kSysExChunkData;
    if(cap < 10 + Packed7Size(len)) return 0;

    size_t w = 0;
    o[w++] = kSysExStart; o[w++] = kSysExManufacturer; o[w++] = dev;
    o[w++] = static_cast<uint8_t>(cmd);
    o[w++] = slot;
    o[w++] = static_cast<uint8_t>((chunk >> 7) & 0x7F);
    o[w++] = static_cast<uint8_t>(chunk & 0x7F);
    o[w++] = static_cast<uint8_t>((total >> 7) & 0x7F);
    o[w++] = static_cast<uint8_t>(total & 0x7F);
    w += Pack7(data + off, len, o + w, cap - w - 1);
    o[w++] = kSysExEnd;
    return w;
}

inline size_t BuildParamReply(uint8_t dev, uint8_t track, uint8_t param,
                              float value, uint8_t *o, size_t cap)
{
    if(cap < 9) return 0;
    const uint16_t v = NormToVal14(value);
    o[0] = kSysExStart; o[1] = kSysExManufacturer; o[2] = dev;
    o[3] = static_cast<uint8_t>(SysExCmd::QueryParamReply);
    o[4] = track; o[5] = param;
    o[6] = static_cast<uint8_t>((v >> 7) & 0x7F);
    o[7] = static_cast<uint8_t>(v & 0x7F);
    o[8] = kSysExEnd;
    return 9;
}

inline size_t BuildInquiryReply(uint8_t dev, uint8_t fw_major, uint8_t fw_minor,
                                const uint8_t hash[5], uint8_t *o, size_t cap)
{
    if(cap < 13) return 0;
    o[0] = kSysExStart; o[1] = kSysExManufacturer; o[2] = dev;
    o[3] = static_cast<uint8_t>(SysExCmd::InquiryReply);
    o[4] = fw_major & 0x7F;
    o[5] = fw_minor & 0x7F;
    o[6] = static_cast<uint8_t>(kPatchVersion & 0x7F);
    for(int i = 0; i < 5; ++i)
        o[7 + i] = hash[i] & 0x7F;
    o[12] = kSysExEnd;
    return 13;
}

} // namespace drom
