#pragma once
#include <cstdint>
#include <cstring>
#include "flash.h"
#include "patch.h"
#include "settings.h"
#include "storage_layout.h"

// Patterns and kits to QSPI, through a static staging buffer.
//
// Deliberately NOT libDaisy's PersistentStorage: that class declares its save
// struct as a **stack local** and keeps two more copies of T as members, which
// at T = Patch is ~30 kB of stack and ~60 kB of .bss out of a 128 kB DTCM that
// also holds .data, the heap and everything else — and the linker script
// defines no _Min_Stack_Size, so nothing would fail at link time. It would
// simply scribble over .bss at runtime. See docs/02-firmware.md §8.

namespace drom {

/// A saved payload's size has to fit SaveHeader::payload_size. If Step ever
/// grows past this, the field truncates and a mismatched save would validate
/// as correct — defeating the one mechanism that exists to catch exactly that.
static_assert(sizeof(Pattern) + sizeof(Kit) <= 0xFFFF,
              "payload_size is 16 bits; a larger Patch needs a wider field");

/// Owns one Patch-sized staging buffer, so **never declare one as a local** —
/// same rule as Machine, and for the same reason.
class Storage
{
  public:
    enum class Result : uint8_t
    {
        Ok = 0,
        NoFlash,     ///< Init was never called
        BadSlot,     ///< slot index out of range
        OutOfBounds, ///< the slot would run past the end of the chip
        WriteFailed,
        Empty,       ///< never written: the slot is erased (all 0xFF)
        BadMagic,
        BadVersion,
        BadSize,
    };

    void Init(IFlash *flash) { flash_ = flash; }

    // ---- patterns ----------------------------------------------------------

    Result SavePatch(uint32_t slot, const Patch &p)
    {
        if(slot >= kPatternSlots)
            return Result::BadSlot;
        // Copy through staging rather than writing the caller's object: the
        // header has to be stamped with the *current* sizes, and the audio side
        // may still own the original.
        staging_ = p;
        StampHeader(staging_.header,
                    static_cast<uint16_t>(sizeof(Pattern) + sizeof(Kit)));
        return WriteSlot(PatternAddr(slot), kPatternStride,
                         reinterpret_cast<const uint8_t *>(&staging_),
                         sizeof(Patch));
    }

    Result LoadPatch(uint32_t slot, Patch &out) const
    {
        if(slot >= kPatternSlots)
            return Result::BadSlot;
        return ReadSlot(PatternAddr(slot), sizeof(Patch),
                        static_cast<uint16_t>(sizeof(Pattern) + sizeof(Kit)),
                        reinterpret_cast<uint8_t *>(&out));
    }

    // ---- kits --------------------------------------------------------------
    //
    // A Kit has no embedded header (a Patch does), so a kit slot is a header
    // followed by the payload. This is the case the staging buffer exists for:
    // the two have to reach the chip as one contiguous write.

    Result SaveKit(uint32_t slot, const Kit &k)
    {
        if(slot >= kKitSlots)
            return Result::BadSlot;
        auto *buf = reinterpret_cast<uint8_t *>(&staging_);
        SaveHeader h;
        StampHeader(h, static_cast<uint16_t>(sizeof(Kit)));
        std::memcpy(buf, &h, sizeof(h));
        std::memcpy(buf + sizeof(h), &k, sizeof(Kit));
        return WriteSlot(KitAddr(slot), kKitStride, buf, sizeof(h) + sizeof(Kit));
    }

    Result LoadKit(uint32_t slot, Kit &out) const
    {
        if(slot >= kKitSlots)
            return Result::BadSlot;
        if(!flash_)
            return Result::NoFlash;
        const uint8_t *src = flash_->Read(KitAddr(slot), sizeof(SaveHeader) + sizeof(Kit));
        if(!src)
            return Result::OutOfBounds;
        SaveHeader h;
        std::memcpy(&h, src, sizeof(h));
        const Result v = Validate(h, static_cast<uint16_t>(sizeof(Kit)));
        if(v != Result::Ok)
            return v;
        std::memcpy(&out, src + sizeof(h), sizeof(Kit));
        return Result::Ok;
    }

    /// The buffer the audio side snapshots into before a save, so the main loop
    /// never memcpys a Patch the audio side is concurrently writing. A torn
    /// save passes every header check — the magic, version and size are all
    /// still right — and shows up later as one wrong step.
    Patch &staging() { return staging_; }

    Result SaveStaged(uint32_t slot)
    {
        if(slot >= kPatternSlots)
            return Result::BadSlot;
        StampHeader(staging_.header,
                    static_cast<uint16_t>(sizeof(Pattern) + sizeof(Kit)));
        return WriteSlot(PatternAddr(slot), kPatternStride,
                         reinterpret_cast<const uint8_t *>(&staging_),
                         sizeof(Patch));
    }

    static const char *Describe(Result r)
    {
        switch(r)
        {
            case Result::Ok:          return "ok";
            case Result::NoFlash:     return "no flash";
            case Result::BadSlot:     return "bad slot";
            case Result::OutOfBounds: return "out of bounds";
            case Result::WriteFailed: return "write failed";
            case Result::Empty:       return "empty";
            case Result::BadMagic:    return "bad magic";
            case Result::BadVersion:  return "version mismatch";
            case Result::BadSize:     return "size mismatch";
        }
        return "?";
    }

  private:
    static void StampHeader(SaveHeader &h, uint16_t payload)
    {
        h.magic        = kPatchMagic;
        h.version      = kPatchVersion;
        h.payload_size = payload;
    }

    static Result Validate(const SaveHeader &h, uint16_t payload)
    {
        // An erased slot reads back as all ones, not zeros. Distinguishing
        // "never written" from "corrupt" is worth a separate result: one is
        // normal on a fresh unit, the other means something went wrong.
        if(h.magic == 0xFFFFFFFFu && h.version == 0xFFFFu)
            return Result::Empty;
        if(h.magic != kPatchMagic)
            return Result::BadMagic;
        if(h.version != kPatchVersion)
            return Result::BadVersion;
        if(h.payload_size != payload)
            return Result::BadSize;
        return Result::Ok;
    }

    Result WriteSlot(uint32_t addr, uint32_t stride, const uint8_t *src, uint32_t size)
    {
        if(!flash_)
            return Result::NoFlash;
        if(addr + stride > flash_->size())
            return Result::OutOfBounds;
        // Erase takes an inclusive END address, not a length. Passing `stride`
        // here would erase one sector and leave the rest of the slot holding
        // the previous save, which reads back as a valid header followed by
        // stale data.
        if(!flash_->Erase(addr, addr + stride - 1))
            return Result::WriteFailed;
        if(!flash_->Write(addr, size, src))
            return Result::WriteFailed;
        return Result::Ok;
    }

    Result ReadSlot(uint32_t addr, uint32_t size, uint16_t payload, uint8_t *dst) const
    {
        if(!flash_)
            return Result::NoFlash;
        const uint8_t *src = flash_->Read(addr, size);
        if(!src)
            return Result::OutOfBounds;
        SaveHeader h;
        std::memcpy(&h, src, sizeof(h));
        const Result v = Validate(h, payload);
        if(v != Result::Ok)
            return v;
        std::memcpy(dst, src, size);
        return Result::Ok;
    }

    IFlash *flash_ = nullptr;
    Patch   staging_{};
};

} // namespace drom
