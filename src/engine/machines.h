#pragma once
#include <cstddef>
#include <new>

#include "voice.h"
#include "voices/drums.h"
#include "voices/synth.h"

// Selectable machines, one per track.
//
// A track's sound is not fixed to its legend: any machine can go on any track,
// the way a Machinedrum works. The panel silkscreen is a default rather than a
// constraint — worth knowing before the legend is cut in metal
// (docs/01-hardware.md §5).
//
// Switching is a **placement-new into per-track storage**, never an allocation:
// the audio side owns voice state and CLAUDE.md forbids allocating there. The
// storage is sized for the largest machine and a static_assert holds that true
// as machines are added.

namespace drom {

/// Sized for the largest machine today (OpenHat, 608 B) with headroom. The
/// static_assert in Make<> is what stops a new machine silently overflowing it.
inline constexpr size_t kMachineBytes = 768;

/// One track's machine, constructed in place.
class MachineSlot
{
  public:
    void Init(float sample_rate, MachineId id)
    {
        sample_rate_ = sample_rate;
        id_          = MachineId::Count; // force Set to do the work
        Set(id);
    }

    /// Returns true if the machine actually changed.
    ///
    /// Destroys the old one first. Every machine is trivially destructible in
    /// practice, but IVoice has a virtual destructor and calling it is what
    /// makes this correct rather than merely working.
    bool Set(MachineId id)
    {
        if(id == id_ || static_cast<int>(id) >= static_cast<int>(MachineId::Count))
            return false;
        if(voice_)
            voice_->~IVoice();
        switch(id)
        {
            case MachineId::BdAnalog:  voice_ = Make<BassDrum>();       break;
            case MachineId::BdBoom:    voice_ = Make<BassDrumBoom>();   break;
            case MachineId::SdSynth:   voice_ = Make<SnareDrum>();      break;
            case MachineId::SdPunch:   voice_ = Make<SnareDrumPunch>(); break;
            case MachineId::HatClosed: voice_ = Make<ClosedHat>();      break;
            case MachineId::HatOpen:   voice_ = Make<OpenHat>();        break;
            case MachineId::Tom:       voice_ = Make<drom::Tom>();      break;
            case MachineId::Clap:      voice_ = Make<drom::Clap>();     break;
            case MachineId::RimShot:   voice_ = Make<drom::RimShot>();  break;
            case MachineId::FmPerc:    voice_ = Make<FmVoice>();        break;
            default:                   voice_ = Make<EmptySlot>();      break;
        }
        id_ = id;
        voice_->Init(sample_rate_);
        return true;
    }

    IVoice   *voice() { return voice_; }
    MachineId id() const { return id_; }

  private:
    template <class T>
    IVoice *Make()
    {
        static_assert(sizeof(T) <= kMachineBytes,
                      "machine does not fit the slot — raise kMachineBytes");
        static_assert(alignof(T) <= alignof(max_align_t),
                      "machine needs more alignment than the slot provides");
        return new(storage_) T();
    }

    alignas(max_align_t) unsigned char storage_[kMachineBytes];
    IVoice   *voice_       = nullptr;
    MachineId id_          = MachineId::Count;
    float     sample_rate_ = 48000.f;
};

} // namespace drom
