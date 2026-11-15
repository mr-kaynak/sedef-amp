// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/Engine.h"
#include "sedef/record/ReampPlayer.h"
#include "sedef/record/TakeRecorder.h"

namespace sedef::record
{

/// The engine with the recorder and re-amp around it, in the order docs/recorder.md (section 2)
/// gives: re-amp replaces or mixes into the input, the DI is taken, the engine runs, the output
/// is taken. The standalone app's audio callback and the tests both go through here.
class Deck
{
public:
    /// Non-real-time, while the audio callback does not run (after Engine::prepare()).
    void prepare (double sampleRate, int numInputs, int maxBlock)
    {
        reamp_.prepare (sampleRate, numInputs, maxBlock);
    }

    TakeRecorder& recorder() noexcept { return recorder_; }
    ReampPlayer& reamp() noexcept { return reamp_; }

    /// Audio thread. `inputs` are changed in place by re-amp; they may be the same buffers as
    /// `outputs` (a host's in-place buffer).
    void process (Engine& engine, float* const* inputs, float* const* outputs, int numInputs,
                  int numOutputs, int numFrames, const ParamSnapshot& params,
                  const Transport& transport) noexcept
    {
        reamp_.process (inputs, numInputs, numFrames);
        recorder_.captureInput (inputs, numInputs, numFrames);
        engine.process (inputs, outputs, numFrames, params, transport);
        recorder_.captureOutput (outputs, numOutputs, numFrames, engine.latencySamples());
    }

private:
    TakeRecorder recorder_;
    ReampPlayer reamp_;
};

} // namespace sedef::record
