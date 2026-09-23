#pragma once
#include <cstdint>
#include "patch.h"

// QSPI layout: addresses, slot strides, and the invariants that keep them
// honest. Deliberately free of libDaisy so the host build compiles it and the
// static_asserts below fire there too.
//
// This file exists because the layout used to be a hand-computed table in
// docs/02-firmware.md §8, derived when a track count of 8 made sizeof(Patch)
// about 11.5 kB. Tracks went to 12, Patch went to 17.3 kB, and 128 slots
// silently overran the region by 112 kB into the songs area — with nothing
// anywhere to notice. Every number here is derived from sizeof, and the
// assertions are what a future struct change will hit instead of flash.

namespace drom {

/// Slot granularity, and not a stylistic choice: `QSPIHandle::Erase` aligns its
/// *start* address DOWN to 4 kB (lib/libDaisy/src/per/qspi.cpp). Erasing a slot
/// that does not start on a sector boundary therefore also erases the tail of
/// the slot before it.
inline constexpr uint32_t kSectorBytes = 4096;

inline constexpr uint32_t kQspiBytes = 8u * 1024u * 1024u;

/// Below this lives the Daisy boot layout and the staged app image. Nothing
/// user-facing may start lower — and `PersistentStorage::Init()` defaults its
/// offset to 0, so it must always be given one explicitly.
inline constexpr uint32_t kUserBase = 0x100000;

/// Round a payload up to whole sectors.
constexpr uint32_t SlotStride(uint32_t payload)
{
    return ((payload + kSectorBytes - 1) / kSectorBytes) * kSectorBytes;
}

inline constexpr uint32_t kSettingsSlots = 1;
inline constexpr uint32_t kKitSlots      = 32;
inline constexpr uint32_t kPatternSlots  = 128;
inline constexpr uint32_t kSongSlots     = 16;

inline constexpr uint32_t kSettingsStride = kSectorBytes;
inline constexpr uint32_t kKitStride      = SlotStride(sizeof(SaveHeader) + sizeof(Kit));
inline constexpr uint32_t kPatternStride  = SlotStride(sizeof(Patch));
inline constexpr uint32_t kSongStride     = kSectorBytes;

inline constexpr uint32_t kSettingsBase = kUserBase;
inline constexpr uint32_t kKitBase      = kSettingsBase + kSettingsSlots * kSettingsStride;
inline constexpr uint32_t kPatternBase  = kKitBase + kKitSlots * kKitStride;
inline constexpr uint32_t kSongBase     = kPatternBase + kPatternSlots * kPatternStride;
inline constexpr uint32_t kFreeBase     = kSongBase + kSongSlots * kSongStride;

constexpr uint32_t PatternAddr(uint32_t slot) { return kPatternBase + slot * kPatternStride; }
constexpr uint32_t KitAddr(uint32_t slot) { return kKitBase + slot * kKitStride; }
constexpr uint32_t SongAddr(uint32_t slot) { return kSongBase + slot * kSongStride; }

// --- invariants -------------------------------------------------------------

static_assert(kPatternStride >= sizeof(Patch),
              "a pattern slot must hold a whole Patch");
static_assert(kKitStride >= sizeof(SaveHeader) + sizeof(Kit),
              "a kit slot must hold a whole Kit plus its header");

static_assert(kUserBase % kSectorBytes == 0, "user data must start on a sector");
static_assert(kSettingsStride % kSectorBytes == 0, "settings slots must be sector-aligned");
static_assert(kKitStride % kSectorBytes == 0, "kit slots must be sector-aligned");
static_assert(kPatternStride % kSectorBytes == 0, "pattern slots must be sector-aligned");
static_assert(kSongStride % kSectorBytes == 0, "song slots must be sector-aligned");

static_assert(kFreeBase <= kQspiBytes, "the layout overflows the chip");

} // namespace drom
