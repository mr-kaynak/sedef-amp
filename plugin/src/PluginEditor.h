// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "PluginProcessor.h"
#include "update/UpdateBridge.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace sedef::plugin
{

/// Hosts the web UI and implements the bridge protocol (ui/src/bridge/protocol.ts, version 2).
class SedefEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kProtocolVersion = 2;

    explicit SedefEditor (SedefProcessor& processor);
    ~SedefEditor() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    class Browser;

    juce::WebBrowserComponent::Options makeOptions();
    void timerCallback() override;

    // Bridge --------------------------------------------------------------------------------------
    juce::var hello();
    juce::var schema() const;
    juce::var allValues() const;
    void emitParams();
    void emitTelemetry();
    void emitPitch();
    void emitPresets();
    void emitIrs();
    void loadIr (int mic, int cab, juce::WebBrowserComponent::NativeFunctionCompletion completion);
    void emitCapture();
    juce::var boardState();
    void emitBoard();
    void loadCapture (juce::WebBrowserComponent::NativeFunctionCompletion completion);

    // UI smoke test (SEDEF_UI_SMOKE_TEST=<result file>): see smokeTestTick().
    void smokeTestTick();
    void finishSmokeTest (bool ok, const juce::var& page, const juce::var& presets = {});
    juce::File smokeResult_;
    int smokeTicks_ = 0;
    int smokeReadyAt_ = -1;
    bool smokeEvaluating_ = false;
    bool smokeDone_ = false;
    juce::var smokePage_; ///< the page check's report, once it ran
    bool smokePageOk_ = false;
    bool smokePresetsStarted_ = false;

    SedefProcessor& processor_;
    std::unique_ptr<update::Bridge> updates_; // outlives browser_, whose functions call it
    std::unique_ptr<Browser> browser_;
    std::unique_ptr<juce::FileChooser> chooser_;

    std::array<float, kNumParams> lastSent_ {};
    int lastPresetRevision_ = -1;
    int lastIrRevision_ = -1;
    int lastCaptureRevision_ = -1;
    int lastBoardRevision_ = -1;
    juce::int64 boardStateRevision_ = 0; ///< BoardState.revision: counts every board state sent
    bool lastDirty_ = false;
    int tick_ = 0;
    bool pageReady_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SedefEditor)
};

} // namespace sedef::plugin
