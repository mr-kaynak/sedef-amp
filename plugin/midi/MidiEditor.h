// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "MidiProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace sedef::midiplugin
{

/// Sedef MIDI's editor: the web UI (ui/midi.html, built to ui/dist-midi) in a WebView, over a
/// small bridge of native functions and events (ui/src/midi/bridge).
class MidiEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kProtocolVersion = 1;

    explicit MidiEditor (MidiProcessor&);
    ~MidiEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Browser;

    juce::WebBrowserComponent::Options makeOptions();
    juce::var hello();
    juce::var schema() const;
    juce::var allValues() const;
    juce::var ports() const;
    void timerCallback() override;
    void emitParams();
    void emitTelemetry();
    void smokeTestTick();
    void finishSmokeTest (bool ok, const juce::var& page, const juce::String& error = {});

    MidiProcessor& processor_;
    std::unique_ptr<Browser> browser_;
    std::array<float, midi::kNumParams> lastSent_ {};
    juce::var lastPorts_;
    bool pageReady_ = false;
    int tick_ = 0;

    juce::File smokeResult_;
    int smokeTicks_ = 0, smokeReadyAt_ = -1;
    bool smokeDone_ = false, smokeEvaluating_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiEditor)
};

} // namespace sedef::midiplugin
