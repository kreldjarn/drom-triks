#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// The seam between the storage module and the chip, mirroring IVoice and
// IChannel: an interface here, a libDaisy-backed implementation in
// src/platform/, and a RAM-backed fake for the host.
//
// That is what lets every byte of the layout, validation and slot arithmetic be
// tested natively — which matters more here than anywhere else in the codebase,
// because the failure mode is a bricked unit or a silently corrupted pattern.

namespace drom {

class IFlash
{
  public:
    virtual ~IFlash() = default;

    /// Erase the range [start, end] **inclusive of an end address, not a
    /// length** — `QSPIHandle::Erase(start_addr, end_addr)` is shaped that way
    /// and passing a size instead erases 4 kB and silently leaves the rest of
    /// the slot holding the previous save.
    ///
    /// The start is aligned **down** to a 4 kB sector by the hardware driver,
    /// so an unaligned start also erases the tail of the slot before it. Slot
    /// strides are sector-aligned in storage_layout.h to make that impossible.
    virtual bool Erase(uint32_t start, uint32_t end) = 0;

    virtual bool Write(uint32_t addr, uint32_t size, const uint8_t *src) = 0;

    /// Memory-mapped read pointer for `size` bytes at `addr`, or nullptr if the
    /// range is out of bounds. Only valid until the next Erase or Write: the
    /// driver leaves memory-mapped mode to do those.
    virtual const uint8_t *Read(uint32_t addr, uint32_t size) const = 0;

    /// Total addressable bytes, for bounds checks.
    virtual uint32_t size() const = 0;
};

/// A RAM-backed IFlash for the host tests.
///
/// Models the two behaviours that actually bite: erase sets bytes to 0xFF (NOR
/// flash erases to ones, not zeros — a struct read back from an un-erased slot
/// is all-0xFF, not all-zero), and a write can only clear bits, never set them.
/// Without the second rule a test would happily "overwrite" a slot that on real
/// hardware would come back as garbage.
template <uint32_t N>
class RamFlash : public IFlash
{
  public:
    RamFlash() { std::memset(mem_, 0xFF, N); }

    bool Erase(uint32_t start, uint32_t end) override
    {
        if(start > end || end >= N)
            return false;
        start &= ~(kSector - 1u); // the driver aligns down; model it
        std::memset(mem_ + start, 0xFF, end - start + 1);
        ++erases;
        return true;
    }

    bool Write(uint32_t addr, uint32_t size, const uint8_t *src) override
    {
        if(!src || addr + size > N)
            return false;
        for(uint32_t i = 0; i < size; ++i)
            mem_[addr + i] &= src[i]; // NOR: a write only clears bits
        bytes_written += size;
        return true;
    }

    const uint8_t *Read(uint32_t addr, uint32_t size) const override
    {
        return (addr + size <= N) ? mem_ + addr : nullptr;
    }

    uint32_t size() const override { return N; }

    /// Corrupt a byte, to prove the header rejects rather than reinterprets.
    void Poke(uint32_t addr, uint8_t v) { mem_[addr] = v; }

    int      erases        = 0;
    uint32_t bytes_written = 0;

  private:
    static constexpr uint32_t kSector = 4096;
    uint8_t                   mem_[N];
};

} // namespace drom
