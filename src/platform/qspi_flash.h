#pragma once
#include "daisy_seed.h"

#include "../io/flash.h"
#include "../io/storage_layout.h"

// The only libDaisy-dependent file outside main.cpp, and the reason src/io/
// stays host-compilable: IFlash is the seam, this is the chip behind it.
//
// Verified against lib/libDaisy/src/per/qspi.cpp rather than assumed, because
// three details here would each corrupt flash silently:
//
//  * **Addresses are chip offsets.** Write() and Erase() both mask with
//    0x0FFFFFFF and GetData() adds 0x90000000, so an offset and an absolute
//    address behave identically. The layout in storage_layout.h uses offsets.
//  * **Erase() takes an inclusive end address, not a length**, and aligns the
//    start *down* to a 4 kB sector.
//  * **Erase() will not overrun.** It only uses a 64 kB block erase when the
//    address is 64 kB-aligned *and* at least 64 kB remains before end_addr;
//    otherwise it steps in 4 kB sectors. So [addr, addr+stride) is exactly
//    what gets cleared, and a neighbouring slot is never caught in it.

namespace drom {

class QspiFlash : public IFlash
{
  public:
    void Init(daisy::QSPIHandle *qspi) { qspi_ = qspi; }

    bool Erase(uint32_t start, uint32_t end) override
    {
        if(!qspi_ || start > end || end >= kQspiBytes)
            return false;
        return qspi_->Erase(start, end) == daisy::QSPIHandle::Result::OK;
    }

    bool Write(uint32_t addr, uint32_t size, const uint8_t *src) override
    {
        if(!qspi_ || !src || addr + size > kQspiBytes)
            return false;
        // libDaisy's signature is non-const; it does not modify the buffer.
        return qspi_->Write(addr, size, const_cast<uint8_t *>(src))
               == daisy::QSPIHandle::Result::OK;
    }

    const uint8_t *Read(uint32_t addr, uint32_t size) const override
    {
        if(!qspi_ || addr + size > kQspiBytes)
            return nullptr;
        // Memory-mapped, so a read is a pointer dereference — but only while
        // the peripheral is in memory-mapped mode. Erase and Write leave it,
        // so never hold this pointer across one.
        return static_cast<const uint8_t *>(qspi_->GetData(addr));
    }

    uint32_t size() const override { return kQspiBytes; }

  private:
    daisy::QSPIHandle *qspi_ = nullptr;
};

} // namespace drom
