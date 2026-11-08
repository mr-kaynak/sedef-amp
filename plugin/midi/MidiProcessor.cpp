// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MidiProcessor.h"

#include "../src/ParameterLayout.h"
#include "MidiEditor.h"
#include "MidiProduct.h"

#if JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_audio_utils/juce_audio_utils.h>
#endif

#include <cmath>

namespace sedef::midiplugin
{
namespace
{
constexpr int kChunk = 512; // analysis runs in chunks of this many samples at most

juce::String virtualNameFor (juce::AudioProcessor::WrapperType type)
{
    return type == juce::AudioProcessor::wrapperType_Standalone ? product::kVirtualPort
                                                                : product::kPluginVirtualPort;
}
} // namespace

juce::AudioProcessor::BusesProperties MidiProcessor::makeBuses()
{
    if (isInstrument())
        // Logic routes a track to an instrument's side chain into its first input.
        return BusesProperties()
            .withInput ("Side Chain", juce::AudioChannelSet::mono(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    return BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::mono(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

MidiProcessor::MidiProcessor()
    : AudioProcessor (makeBuses()),
      state_ (*this, nullptr, "SedefParameters",
              plugin::createParameterLayout (std::span<const ParamSpec> (midi::kParamSpecs))),
      router_ (virtualNameFor (wrapperType))
{
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
    {
        raw_[i] = state_.getRawParameterValue (plugin::toJuce (midi::kParamSpecs[i].id));
        jassert (raw_[i] != nullptr);
    }
    lastValues_.fill (std::nanf (""));

    // The standalone app and the instrument play the notes, not the guitar: their audio starts
    // muted. The standalone app opens its virtual port, and hears its input (JUCE mutes a
    // standalone app's input by default, against feedback; with the audio muted there is none).
    if (isStandalone() || isInstrument())
        setParamValue (midi::ParamId::audio, 1.0f);
    if (isStandalone())
    {
        setDestination (MidiPortRouter::virtualSupported() ? MidiPortRouter::kVirtual
                                                           : MidiPortRouter::kOff);
#if JucePlugin_Build_Standalone
        juce::MessageManager::callAsync ([] {
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
                holder->getMuteInputValue().setValue (false);
        });
#endif
    }
}

MidiProcessor::~MidiProcessor() = default;

const juce::String MidiProcessor::getName() const
{
    return isInstrument() ? product::kInstrumentName : product::kName;
}

juce::String MidiProcessor::virtualPortName() const
{
    return virtualNameFor (wrapperType);
}

bool MidiProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s) {
        return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo();
    };
    // The instrument's side chain may be switched off (no guitar yet): it then plays nothing.
    return ok (out) && (ok (in) || (isInstrument() && in.isDisabled()));
}

void MidiProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Preparing forgets the notes; the host may still hold some from before (a reset, a new
    // sample rate): their note-offs go out with the first block.
    pendingOffs_.clear();
    if (converter_.soundingCount() > 0)
        converter_.allNotesOff (pendingOffs_, 0);
    converter_.prepare (sampleRate);
    mono_.assign (static_cast<std::size_t> (kChunk), 0.0f);
    sampleRate_.store (sampleRate, std::memory_order_relaxed);
    lastValues_.fill (std::nanf (""));
    readSettings();
    gain_ = midi::audioThrough (lastValues_) ? 1.0f : 0.0f;
    wasBypassed_ = false;
    wasPlaying_ = false;
    endRequested_.store (false, std::memory_order_relaxed);
    router_.endAllSoon(); // whatever the port still holds from before
    juce::ignoreUnused (samplesPerBlock);
}

void MidiProcessor::releaseResources()
{
    // No block will come to end the notes: the port ends its own now, and the next block ends
    // what the host still holds.
    router_.endAllSoon();
    endRequested_.store (true, std::memory_order_relaxed);
}

void MidiProcessor::reset()
{
    endRequested_.store (true, std::memory_order_relaxed);
}

void MidiProcessor::readSettings() noexcept
{
    midi::Values v {};
    bool changed = false;
    for (std::size_t i = 0; i < midi::kNumParams; ++i)
    {
        v[i] = raw_[i]->load (std::memory_order_relaxed);
        changed = changed || !juce::exactlyEqual (v[i], lastValues_[i]);
    }
    if (!changed)
        return;
    lastValues_ = v;
    converter_.setSettings (midi::settingsFrom (v));
}

void MidiProcessor::sendEvents (juce::MidiBuffer& midiOut, int offset)
{
    if (events_.empty())
        return;
    if (producesMidi())
        for (const auto& e : events_)
            midiOut.addEvent (e.bytes, e.size, offset + e.offset);
    router_.push (events_, offset, sampleRate_.load (std::memory_order_relaxed));
    events_.clear();
}

void MidiProcessor::endEverything (juce::MidiBuffer& midiOut, bool everyChannel)
{
    events_.clear();
    if (everyChannel)
        converter_.panic (events_, 0);
    else
        converter_.allNotesOff (events_, 0);
    sendEvents (midiOut, 0);
}

void MidiProcessor::convert (const float* mono, int numFrames, juce::MidiBuffer& midiOut,
                             int offset)
{
    events_.clear();
    converter_.process (mono, numFrames, events_);
    sendEvents (midiOut, offset);
}

void MidiProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear(); // nothing passes through
    const int n = buffer.getNumSamples();
    const int ins = std::min (getTotalNumInputChannels(), buffer.getNumChannels());
    const int outs = std::min (getTotalNumOutputChannels(), buffer.getNumChannels());
    wasBypassed_ = false;

    // A stopped transport ends the sounding note (the next pick starts a new one).
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            const bool playing = position->getIsPlaying();
            if (wasPlaying_ && !playing)
                endRequested_.store (true, std::memory_order_relaxed);
            wasPlaying_ = playing;
        }
    if (!pendingOffs_.empty())
    {
        events_.clear();
        for (const auto& e : pendingOffs_)
            events_.push (e);
        pendingOffs_.clear();
        sendEvents (midi, 0);
    }
    if (panicRequested_.exchange (false, std::memory_order_relaxed))
    {
        endRequested_.store (false, std::memory_order_relaxed);
        endEverything (midi, true);
    }
    if (endRequested_.exchange (false, std::memory_order_relaxed))
        endEverything (midi, false);
    readSettings();

    for (int start = 0; start < n; start += kChunk)
    {
        const int len = std::min (kChunk, n - start);
        auto* m = mono_.data();
        if (ins == 0)
            std::fill (m, m + len, 0.0f);
        else
        {
            std::copy_n (buffer.getReadPointer (0, start), len, m);
            for (int ch = 1; ch < ins; ++ch)
                juce::FloatVectorOperations::add (m, buffer.getReadPointer (ch, start), len);
            if (ins > 1)
                juce::FloatVectorOperations::multiply (m, 1.0f / static_cast<float> (ins), len);
        }
        convert (m, len, midi, start);
    }

    // The audio: the guitar through, or nothing. The instrument's "through" is its side chain.
    const float target = midi::audioThrough (lastValues_) ? 1.0f : 0.0f;
    if (ins == 0)
        buffer.clear();
    else
        for (int ch = ins; ch < outs; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, n);
    for (int ch = 0; ch < outs; ++ch)
        buffer.applyGainRamp (ch, 0, n, gain_, target);
    for (int ch = outs; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);
    gain_ = target;

    tLevelDb_.store (converter_.levelDb(), std::memory_order_relaxed);
    tPitch_.store (converter_.pitch(), std::memory_order_relaxed);
    tNote_.store (converter_.note(), std::memory_order_relaxed);
    tVelocity_.store (converter_.lastVelocity(), std::memory_order_relaxed);
    tSounding_.store (converter_.soundingCount(), std::memory_order_relaxed);
    tNotesSent_.store (converter_.notesSent(), std::memory_order_relaxed);
    tLateNotes_.store (converter_.lateNotes(), std::memory_order_relaxed);
}

void MidiProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Bypass ends every note and forgets the signal; the effect passes its input, the
    // instrument is silent.
    midi.clear();
    if (!wasBypassed_)
    {
        wasBypassed_ = true;
        endEverything (midi, false);
        converter_.reset();
    }
    const int n = buffer.getNumSamples();
    const int ins = std::min (getTotalNumInputChannels(), buffer.getNumChannels());
    if (isInstrument() || ins == 0)
        buffer.clear();
    else
        for (int ch = ins; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, n);
    tNote_.store (-1, std::memory_order_relaxed);
    tSounding_.store (0, std::memory_order_relaxed);
}

MidiProcessor::Telemetry MidiProcessor::telemetry() const noexcept
{
    Telemetry t;
    t.levelDb = tLevelDb_.load (std::memory_order_relaxed);
    t.pitch = tPitch_.load (std::memory_order_relaxed);
    t.note = tNote_.load (std::memory_order_relaxed);
    t.velocity = tVelocity_.load (std::memory_order_relaxed);
    t.sounding = tSounding_.load (std::memory_order_relaxed) > 0;
    t.notesSent = tNotesSent_.load (std::memory_order_relaxed);
    t.lateNotes = tLateNotes_.load (std::memory_order_relaxed);
    return t;
}

void MidiProcessor::setParamValue (midi::ParamId id, float realValue)
{
    const auto& spec = midi::spec (id);
    if (auto* p = state_.getParameter (plugin::toJuce (spec.id)))
        p->setValueNotifyingHost (spec.toNormalised (spec.sanitise (realValue)));
}

void MidiProcessor::setDestination (const juce::String& d)
{
    router_.setDestination (d);
}

juce::AudioProcessorEditor* MidiProcessor::createEditor()
{
    return new MidiEditor (*this);
}

void MidiProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("SedefMidiState");
    root.setProperty ("version", 1, nullptr);
    root.addChild (state_.copyState(), -1, nullptr);
    juce::ValueTree extra ("Extra");
    extra.setProperty ("destination", destination(), nullptr);
    root.addChild (extra, -1, nullptr);
    if (const auto xml = root.createXml())
        copyXmlToBinary (*xml, dest);
}

void MidiProcessor::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || !xml->hasTagName ("SedefMidiState"))
        return;
    const auto root = juce::ValueTree::fromXml (*xml);
    const auto params = root.getChildWithName (state_.state.getType());
    if (params.isValid())
        state_.replaceState (params);
    const auto extra = root.getChildWithName ("Extra");
    if (extra.hasProperty ("destination"))
        setDestination (extra.getProperty ("destination").toString());
}

} // namespace sedef::midiplugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sedef::midiplugin::MidiProcessor();
}
