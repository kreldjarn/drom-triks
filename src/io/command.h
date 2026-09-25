#pragma once
#include <cstdint>

// The UI -> audio contract, as plain data.
//
// Lives in its own header rather than in machine.h because it is also the shape
// the SysEx control API speaks (docs/06-midi.md §8.2), and sysex.h must stay
// free of DaisySP so the protocol can be tested natively. Nothing here needs
// more than <cstdint>.

namespace drom {

/// Every edit the UI can make, as data.
///
/// Routing edits through a queue rather than letting the UI touch the pattern
/// is what lets the audio side run without a single lock: it is the only
/// writer of sequencer, voice and pattern state.
struct Command
{
    enum class Type : uint8_t
    {
        None = 0,
        SetKitParam,   ///< track, param, value
        SetMachine,    ///< track, param = MachineId
        SetFxParam,    ///< param = FxId, value — master delay/reverb/compressor
        Snapshot,      ///< copy the patch to snapshot_dst_ for the main loop
        LoadPatch,     ///< adopt the patch at load_src_, from the main loop
        ToggleStep,    ///< track, step
        SetStepLock,   ///< track, step, param, value
        SetStepField,  ///< track, step, param = StepField, value 0..1
        ClearStepLocks,///< track, step
        SetTrackMute,  ///< track, value != 0
        SetTempo,      ///< value = BPM
        Start,
        Stop,
        Continue,
    };

    Type    type  = Type::None;
    uint8_t track = 0;
    uint8_t step  = 0;
    uint8_t param = 0;
    float   value = 0.f;
};

} // namespace drom
