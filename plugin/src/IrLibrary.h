// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "sedef/Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <mutex>

namespace sedef::plugin
{

/// User impulse responses for the cab mics: one slot per (mic, cab). Keeps the decoded samples so
/// kernels can be rebuilt when the sample rate changes, and remembers file paths for the state.
class IrLibrary
{
public:
    static constexpr int kMics = 2;
    static constexpr int kCabs = 3;

    /// `engineLock` serialises IR installation against Engine::prepare (both off the audio thread).
    IrLibrary (Engine& engine, std::mutex& engineLock);
    ~IrLibrary();

    /// Message thread. Returns an error message, or an empty string on success.
    juce::String load (int mic, int cab, const juce::File& file);
    void clear (int mic, int cab);

    /// Rebuilds every loaded kernel for the engine's current sample rate (after prepare).
    void rebuildAll();
    /// Frees kernels retired by the audio thread. Message thread, periodically.
    void collectGarbage();

    juce::var stateAsVar() const;
    juce::File fileFor (int mic, int cab) const;

private:
    struct Slot
    {
        juce::File file;
        std::vector<float> samples;
        double sampleRate = 0.0;
    };

    void install (int mic, int cab);

    Engine& engine_;
    std::mutex& engineLock_;
    juce::AudioFormatManager formats_;
    std::array<std::array<Slot, kCabs>, kMics> slots_;
};

} // namespace sedef::plugin
