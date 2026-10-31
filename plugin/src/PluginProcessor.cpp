// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginProcessor.h"

#include "ParameterLayout.h"
#include "PluginEditor.h"

namespace sedef::plugin
{

SedefProcessor::SedefProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state_ (*this, nullptr, "SedefParameters", createParameterLayout()),
      presets_ (state_),
      irs_ (engine_, engineLock_),
      analysis_ (engine_)
{
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        raw_[i] = state_.getRawParameterValue (toJuce (kParamSpecs[i].id));
        jassert (raw_[i] != nullptr);
    }
    engine_.setMetronomePattern (metronomePattern_);
    startTimerHz (20);
}

SedefProcessor::~SedefProcessor()
{
    stopTimer();
    analysis_.setActive (false);
}

bool SedefProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s) {
        return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo();
    };
    return ok (in) && ok (out);
}

juce::String SedefProcessor::layoutDescription() const
{
    const int ins = getTotalNumInputChannels();
    const int outs = getTotalNumOutputChannels();
    if (ins == 1)
        return outs == 1 ? "Mono" : "Mono → Stereo";
    return outs == 1 ? "Stereo → Mono" : "Stereo";
}

void SedefProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int ins = juce::jlimit (1, 2, getTotalNumInputChannels());
    const int outs = juce::jlimit (1, 2, getTotalNumOutputChannels());
    // The tracker is rate-specific: stop feeding it until it is re-prepared below.
    engine_.setAnalysisEnabled (false);
    {
        const std::scoped_lock lock (engineLock_);
        engine_.prepare (sampleRate, samplesPerBlock, ins, outs);
        engine_.setMetronomeAllowed (isStandalone());
        engine_.setMetronomePattern (metronomePattern_);
    }
    setLatencySamples (engine_.latencySamples());
    loadMeasurer_.reset (sampleRate, samplesPerBlock);
    prepared_ = true;

    // Kernels and the pitch tracker depend on the sample rate.
    juce::MessageManager::callAsync ([safe = juce::WeakReference<SedefProcessor> (this)] {
        if (auto* p = safe.get())
        {
            p->irs_.rebuildAll();
            p->analysis_.engineWasPrepared();
        }
    });
}

void SedefProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int numSamples = buffer.getNumSamples();
    const juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer_, numSamples);

    if (!prepared_)
    {
        buffer.clear();
        return;
    }

    ParamSnapshot snapshot;
    for (std::size_t i = 0; i < kNumParams; ++i)
        snapshot.values[i] = raw_[i]->load (std::memory_order_relaxed);

    Transport transport;
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                transport.hasHostTempo = true;
                transport.bpm = *bpm;
            }
            transport.isPlaying = position->getIsPlaying();
            if (const auto ppq = position->getPpqPosition())
                transport.ppqPosition = *ppq;
        }

    const int ins = engine_.numInputs();
    const int outs = engine_.numOutputs();
    if (buffer.getNumChannels() < std::max (ins, outs))
    {
        buffer.clear();
        return;
    }

    std::array<const float*, 2> inputs { buffer.getReadPointer (0), ins > 1 ? buffer.getReadPointer (1) : nullptr };
    std::array<float*, 2> outputs { buffer.getWritePointer (0), outs > 1 ? buffer.getWritePointer (1) : nullptr };
    engine_.process (inputs.data(), outputs.data(), numSamples, snapshot, transport);

    for (int ch = outs; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);
}

double SedefProcessor::getTailLengthSeconds() const
{
    // Hosts stop processing after the tail; report the longest ring-out the rig can produce.
    double tail = 0.0;
    if (paramValue (ParamId::fathomEnabled) >= 0.5f)
        tail = std::max (tail, static_cast<double> (paramValue (ParamId::fathomDecay)) * 1.2);
    if (paramValue (ParamId::driftEnabled) >= 0.5f)
        tail = std::max (tail, 10.0);
    return tail;
}

void SedefProcessor::setParamValue (ParamId id, float realValue)
{
    const auto& spec = sedef::spec (id);
    if (auto* p = state_.getParameter (toJuce (spec.id)))
        p->setValueNotifyingHost (spec.toNormalised (realValue));
}

void SedefProcessor::setMetronomePattern (const std::array<std::uint8_t, 13>& steps)
{
    metronomePattern_ = steps;
    engine_.setMetronomePattern (steps);
}

float SedefProcessor::tapTempo()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (numTaps_ > 0 && now - taps_[static_cast<size_t> ((numTaps_ - 1) % 4)] > 2.0)
        numTaps_ = 0; // a pause restarts the measurement
    taps_[static_cast<size_t> (numTaps_ % 4)] = now;
    ++numTaps_;

    if (numTaps_ >= 2)
    {
        const int count = std::min (numTaps_, 4);
        const double first = taps_[static_cast<size_t> ((numTaps_ - count) % 4)];
        const double interval = (now - first) / (count - 1);
        const auto bpm = static_cast<float> (juce::jlimit (40.0, 240.0, 60.0 / interval));
        setParamValue (ParamId::tempo, bpm);
        return bpm;
    }
    return paramValue (ParamId::tempo);
}

void SedefProcessor::timerCallback()
{
    // Preset navigation through automatable switches (MIDI footswitches via the host).
    const bool prev = paramValue (ParamId::presetPrevious) >= 0.5f;
    const bool next = paramValue (ParamId::presetNext) >= 0.5f;
    if (prev && !lastPrev_)
    {
        presets_.step (-1);
        ++presetRevision;
    }
    if (next && !lastNext_)
    {
        presets_.step (1);
        ++presetRevision;
    }
    lastPrev_ = prev;
    lastNext_ = next;

    analysis_.setReferenceHz (paramValue (ParamId::tunerReference));
    irs_.collectGarbage();
}

juce::AudioProcessorEditor* SedefProcessor::createEditor()
{
    return new SedefEditor (*this);
}

// State -------------------------------------------------------------------------------------------

void SedefProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("SedefState");
    root.setProperty ("version", 1, nullptr);
    root.appendChild (state_.copyState(), nullptr);

    juce::ValueTree extra ("Extra");
    extra.setProperty ("uiState", juce::JSON::toString (uiState_, true), nullptr);
    extra.setProperty ("uiScale", uiScale_, nullptr);
    extra.setProperty ("presetId", presets_.currentId(), nullptr);
    extra.setProperty ("presetName", presets_.currentName(), nullptr);
    juce::StringArray steps;
    for (auto s : metronomePattern_)
        steps.add (juce::String (static_cast<int> (s)));
    extra.setProperty ("metronomePattern", steps.joinIntoString (","), nullptr);
    root.appendChild (extra, nullptr);

    juce::ValueTree irs ("IRs");
    for (int m = 0; m < IrLibrary::kMics; ++m)
        for (int c = 0; c < IrLibrary::kCabs; ++c)
            if (const auto f = irs_.fileFor (m, c); f != juce::File())
            {
                juce::ValueTree slot ("Slot");
                slot.setProperty ("mic", m, nullptr);
                slot.setProperty ("cab", c, nullptr);
                slot.setProperty ("path", f.getFullPathName(), nullptr);
                irs.appendChild (slot, nullptr);
            }
    root.appendChild (irs, nullptr);

    if (const auto xml = root.createXml())
        copyXmlToBinary (*xml, dest);
}

void SedefProcessor::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || !xml->hasTagName ("SedefState"))
        return;
    const auto root = juce::ValueTree::fromXml (*xml);

    if (const auto params = root.getChildWithName (state_.state.getType()); params.isValid())
        state_.replaceState (params);

    const auto extra = root.getChildWithName ("Extra");
    uiState_ = juce::JSON::parse (extra.getProperty ("uiState").toString());
    uiScale_ = juce::jlimit (0.5, 2.5, static_cast<double> (extra.getProperty ("uiScale", 1.0)));
    presets_.restoreCurrent (extra.getProperty ("presetId").toString(), extra.getProperty ("presetName").toString());

    const auto steps = juce::StringArray::fromTokens (extra.getProperty ("metronomePattern").toString(), ",", "");
    if (steps.size() == 13)
    {
        std::array<std::uint8_t, 13> pattern {};
        for (int i = 0; i < 13; ++i)
            pattern[static_cast<size_t> (i)] = static_cast<std::uint8_t> (juce::jlimit (0, 3, steps[i].getIntValue()));
        setMetronomePattern (pattern);
    }

    const auto irs = root.getChildWithName ("IRs");
    juce::MessageManager::callAsync ([safe = juce::WeakReference<SedefProcessor> (this), irs] {
        if (auto* p = safe.get())
        {
            for (const auto& slot : irs)
            {
                const juce::File f (slot.getProperty ("path").toString());
                if (f.existsAsFile())
                    p->irs_.loadAsync (slot.getProperty ("mic"), slot.getProperty ("cab"), f,
                                       [safeAgain = juce::WeakReference<SedefProcessor> (p)] (const juce::String&) {
                                           if (auto* q = safeAgain.get())
                                               ++q->irRevision;
                                       });
            }
            ++p->irRevision;
            ++p->presetRevision;
        }
    });
}

} // namespace sedef::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sedef::plugin::SedefProcessor();
}
