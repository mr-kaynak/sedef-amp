// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "AnalysisWorker.h"
#include "IrLibrary.h"
#include "PresetManager.h"
#include "sedef/Engine.h"

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>

namespace sedef::plugin
{

class SedefProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    static constexpr int kBaseWidth = 1200;
    static constexpr int kBaseHeight = 760;

    SedefProcessor();
    ~SedefProcessor() override;

    // AudioProcessor ------------------------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Sedef Amp"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    // Sedef ---------------------------------------------------------------------------------------
    juce::AudioProcessorValueTreeState& state() noexcept { return state_; }
    Engine& engine() noexcept { return engine_; }
    PresetManager& presets() noexcept { return presets_; }
    IrLibrary& irs() noexcept { return irs_; }
    AnalysisWorker& analysis() noexcept { return analysis_; }

    float paramValue (ParamId id) const noexcept
    {
        return raw_[index (id)]->load (std::memory_order_relaxed);
    }
    void setParamValue (ParamId id, float realValue);

    // UI state, scale and the metronome pattern are saved by getStateInformation(), which hosts
    // may call from any thread, so they live behind extraLock_ (or are atomic).
    juce::var uiState() const;
    void setUiState (const juce::var& s);
    double uiScale() const noexcept { return uiScale_.load (std::memory_order_relaxed); }
    void setUiScale (double s) noexcept { uiScale_.store (s, std::memory_order_relaxed); }

    void setMetronomePattern (const std::array<std::uint8_t, 13>& steps);
    std::array<std::uint8_t, 13> metronomePattern() const;
    /// Registers a tap and returns the new tempo (BPM).
    float tapTempo();

    float cpuLoad() const noexcept
    {
        return static_cast<float> (loadMeasurer_.getLoadAsProportion());
    }
    juce::String layoutDescription() const;
    bool isStandalone() const noexcept { return wrapperType == wrapperType_Standalone; }

    /// Bumped whenever preset or IR state changes so the editor can push updates.
    std::atomic<int> presetRevision { 0 };
    std::atomic<int> irRevision { 0 };

private:
    void timerCallback() override;

    juce::AudioProcessorValueTreeState state_;
    std::array<std::atomic<float>*, kNumParams> raw_ {};

    Engine engine_;
    std::mutex engineLock_; // serialises prepare() against IR installation
    PresetManager presets_;
    IrLibrary irs_;
    AnalysisWorker analysis_;

    juce::AudioProcessLoadMeasurer loadMeasurer_;

    mutable std::mutex extraLock_;
    juce::String uiStateJson_;                     // guarded by extraLock_
    juce::String savedPresetId_, savedPresetName_; // guarded; mirrored from presets_ by the timer
    std::array<std::uint8_t, 13> metronomePattern_ { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    std::atomic<double> uiScale_ { 1.0 };
    std::array<double, 4> taps_ {};
    int numTaps_ = 0;

    std::atomic<bool> lastPrev_ { false }, lastNext_ { false }; // also seeded by state restore
    bool prepared_ = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SedefProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SedefProcessor)
};

} // namespace sedef::plugin
