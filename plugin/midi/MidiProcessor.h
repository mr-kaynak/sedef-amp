// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "MidiPortRouter.h"
#include "sedef/midi/GuitarToMidi.h"
#include "sedef/midi/MidiParameters.h"

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <mutex>

namespace sedef::midiplugin
{

/// Sedef MIDI: a guitar's mono DI in, MIDI notes out (docs/design/guitar-to-midi.md).
///
/// One processor for every format. Built as the audio effect (AU effect, VST3, CLAP, standalone)
/// it takes the guitar on its main input; built with SEDEF_MIDI_INSTRUMENT (the "Sedef MIDI
/// (Instrument)" AU) it is an instrument whose only input is a side chain, which is how Logic
/// lets other tracks record an instrument's MIDI. The notes go to the host as the plug-in's MIDI
/// output, and, when the user picks one, to a MIDI port through MidiPortRouter (the standalone
/// app's only way out, and the plug-ins' way into apps that can't take a plug-in's MIDI).
class MidiProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int kBaseWidth = 760;
    static constexpr int kBaseHeight = 600;

    MidiProcessor();
    ~MidiProcessor() override;

    // AudioProcessor ------------------------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return isInstrument(); }
    /// The standalone app sends through its own port, never through JUCE's MIDI output setting
    /// (which would send from the audio thread).
    bool producesMidi() const override { return !isStandalone(); }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    // Sedef MIDI ----------------------------------------------------------------------------------
    static constexpr bool isInstrument() noexcept
    {
#if SEDEF_MIDI_INSTRUMENT
        return true;
#else
        return false;
#endif
    }
    bool isStandalone() const noexcept { return wrapperType == wrapperType_Standalone; }

    juce::AudioProcessorValueTreeState& state() noexcept { return state_; }
    float paramValue (midi::ParamId id) const noexcept
    {
        return raw_[midi::index (id)]->load (std::memory_order_relaxed);
    }
    void setParamValue (midi::ParamId id, float realValue);

    /// Where the notes go besides the host: MidiPortRouter's "off", "virtual" or "device:<id>".
    juce::String destination() const { return router_.destination(); }
    void setDestination (const juce::String& d);
    MidiPortRouter& router() noexcept { return router_; }
    /// The virtual port's name other apps see.
    juce::String virtualPortName() const;

    /// Ends every note on every channel, at the next block (and on the port).
    void panic() noexcept { panicRequested_.store (true, std::memory_order_relaxed); }

    /// What the UI shows, written by the audio thread after each block.
    struct Telemetry
    {
        float levelDb = -120.0f;
        float gateDb = -120.0f; ///< the gate in effect (automatic, or the Gate parameter)
        int note = -1;
        float pitch = 0.0f;
        int velocity = 0;
        std::uint32_t notesSent = 0, lateNotes = 0;
        bool sounding = false;
        int lowNote = 1;   ///< the Lowest Note range in effect: 1 E2, 2 B1, 3 F#1
        int blockSize = 0; ///< samples per block, as the last block came
    };
    Telemetry telemetry() const noexcept;
    double sampleRate() const noexcept { return sampleRate_.load (std::memory_order_relaxed); }
    /// The standalone app's chores on the message thread, twice a second: the small default
    /// audio buffer, once a device is open (see the definition). Nothing in a plug-in.
    static void standaloneHousekeeping();
    /// The automatic Lowest Note range starts again as a six-string's (at the next block).
    void forgetLowNote() noexcept { learnedRequest_.store (1, std::memory_order_relaxed); }

private:
    static BusesProperties makeBuses();
    void convert (const float* mono, int numFrames, juce::MidiBuffer& midiOut, int offset);
    void sendEvents (juce::MidiBuffer& midiOut, int offset);
    void readSettings() noexcept;
    void endEverything (juce::MidiBuffer& midiOut, bool everyChannel);

    juce::AudioProcessorValueTreeState state_;
    std::array<std::atomic<float>*, midi::kNumParams> raw_ {};
    midi::Values lastValues_ {};

    /// A value as the last restored state held it, and the value it gave the parameter. Saving
    /// writes a parameter still at that value back as it was loaded, so load-then-save
    /// reproduces a state byte for byte (APVTS would store denormalise (normalise (x)), which
    /// can differ from x in the last bit).
    struct LoadedValue
    {
        juce::var saved;
        float applied = 0.0f;
        bool valid = false;
    };
    std::mutex loadedLock_; ///< state calls come from any thread, never the audio thread
    std::array<LoadedValue, midi::kNumParams> loaded_ {};

    midi::GuitarToMidi converter_;
    midi::EventList events_, pendingOffs_;
    MidiPortRouter router_;
    std::vector<float> mono_;

    std::atomic<double> sampleRate_ { 48000.0 };
    std::atomic<bool> panicRequested_ { false }, endRequested_ { false };
    /// What the automatic range is to be told at the next block: 0 nothing, else a LowNote
    /// (forget: e2; a restored session: what it had learned).
    std::atomic<int> learnedRequest_ { 0 };
    std::atomic<int> tLearned_ { 1 }, tLowNote_ { 1 }, tBlockSize_ { 0 };
    int blockFrames_ = 0; ///< the block being processed
    bool wasBypassed_ = false, wasPlaying_ = false;
    float gain_ = 1.0f; ///< audio through (1) or muted (0), ramped

    std::atomic<float> tLevelDb_ { -120.0f }, tGateDb_ { -120.0f }, tPitch_ { 0.0f };
    std::atomic<int> tNote_ { -1 }, tVelocity_ { 0 }, tSounding_ { 0 };
    std::atomic<std::uint32_t> tNotesSent_ { 0 }, tLateNotes_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiProcessor)
};

} // namespace sedef::midiplugin
