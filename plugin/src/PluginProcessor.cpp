// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginProcessor.h"

#include "ParameterLayout.h"
#if !SEDEF_PLUGIN_TESTS
#include "PluginEditor.h"
#endif

namespace sedef::plugin
{

SedefProcessor::SedefProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state_ (*this, nullptr, "SedefParameters", createParameterLayout()),
      presets_ (state_),
      irs_ (engine_, engineLock_),
      analysis_ (engine_),
      captures_ (engine_, engineLock_, PresetManager::userFolder().getParentDirectory())
{
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        raw_[i] = state_.getRawParameterValue (toJuce (kParamSpecs[i].id));
        jassert (raw_[i] != nullptr);
    }
    engine_.setMetronomePattern (metronomePattern_);
    // The saved-state snapshot must match the preset manager from the start, or the first save
    // and a save after the timer has run would differ (clap-validator checks state byte for byte).
    savedPresetId_ = presets_.currentId();
    savedPresetName_ = presets_.currentName();
    // User presets keep a reference to their capture (docs/dsp/nam.md), only when it plays.
    presets_.addAttachment (
        { "capture",
          [this] (bool forPreset) {
              return forPreset && paramValue (ParamId::ampSource) < 0.5f ? juce::var()
                                                                         : captures_.reference();
          },
          [this] (const juce::var& ref, bool exact) { captures_.select (ref, exact); },
          [this] (const juce::var& ref) { return captures_.differs (ref); },
          {},
          false });
    // Presets carry the pedalboard. One saved before it existed loads the board that matches its
    // parameters (Board::legacyFor); so does Init.
    presets_.addAttachment ({ "board", [this] (bool) { return boardToVar (board()); },
                              [this] (const juce::var& ref, bool) {
                                  if (const auto b = boardFromVar (ref))
                                      setBoard (*b);
                              },
                              [this] (const juce::var& ref) {
                                  const auto b = boardFromVar (ref);
                                  return !b || *b != board();
                              },
                              [] (const juce::var& stored, const juce::var& parameters) {
                                  if (const auto b = boardFromVar (stored))
                                      return boardToVar (*b);
                                  return boardToVar (legacyBoardFor (parameters));
                              },
                              true });
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
        engine_.setMetronomePattern (metronomePattern());
    }
    captures_.rebuild(); // now, so the capture plays from the first block at the new rate
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

    std::array<const float*, 2> inputs { buffer.getReadPointer (0),
                                         ins > 1 ? buffer.getReadPointer (1) : nullptr };
    std::array<float*, 2> outputs { buffer.getWritePointer (0),
                                    outs > 1 ? buffer.getWritePointer (1) : nullptr };
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

void SedefProcessor::changeParamValue (ParamId id, float realValue)
{
    const auto& spec = sedef::spec (id);
    if (auto* p = state_.getParameter (toJuce (spec.id)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (spec.toNormalised (realValue));
        p->endChangeGesture();
    }
}

void SedefProcessor::presetsChanged()
{
    mirrorPreset();
    ++presetRevision;
}

void SedefProcessor::mirrorPreset()
{
    presets_.sync();
    const auto id = presets_.currentId();
    const auto name = presets_.currentName();
    const std::scoped_lock lock (extraLock_);
    // A host restore that came in after sync() has already put its own preset here.
    if (!presets_.hostRestorePending())
    {
        savedPresetId_ = id;
        savedPresetName_ = name;
    }
}

Board SedefProcessor::board() const
{
    const std::scoped_lock lock (extraLock_);
    return board_;
}

void SedefProcessor::setBoard (const Board& board)
{
    {
        const std::scoped_lock lock (extraLock_);
        if (board == board_)
            return;
        board_ = board;
        engine_.setBoard (board);
    }
    ++boardRevision;
}

void SedefProcessor::setMetronomePattern (const std::array<std::uint8_t, 13>& steps)
{
    {
        const std::scoped_lock lock (extraLock_);
        metronomePattern_ = steps;
    }
    engine_.setMetronomePattern (steps);
}

std::array<std::uint8_t, 13> SedefProcessor::metronomePattern() const
{
    const std::scoped_lock lock (extraLock_);
    return metronomePattern_;
}

juce::var SedefProcessor::uiState() const
{
    juce::String json;
    {
        const std::scoped_lock lock (extraLock_);
        json = uiStateJson_;
    }
    return json.isEmpty() ? juce::var() : juce::JSON::parse (json);
}

void SedefProcessor::setUiState (const juce::var& s)
{
    const auto json = juce::JSON::toString (s, true);
    const std::scoped_lock lock (extraLock_);
    uiStateJson_ = json;
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
        changeParamValue (ParamId::tempo, bpm);
        return bpm;
    }
    return paramValue (ParamId::tempo);
}

void SedefProcessor::timerCallback()
{
    // Preset navigation through automatable switches (MIDI footswitches via the host). Each switch
    // acts on its rising edge and then springs back to off, so it can fire again.
    const bool prev = paramValue (ParamId::presetPrevious) >= 0.5f;
    const bool next = paramValue (ParamId::presetNext) >= 0.5f;
    if (prev && !lastPrev_)
    {
        presets_.step (-1);
        presetsChanged();
        changeParamValue (ParamId::presetPrevious, 0.0f);
    }
    if (next && !lastNext_)
    {
        presets_.step (1);
        presetsChanged();
        changeParamValue (ParamId::presetNext, 0.0f);
    }
    lastPrev_ = paramValue (ParamId::presetPrevious) >= 0.5f;
    lastNext_ = paramValue (ParamId::presetNext) >= 0.5f;

    // Every preset call mirrors at once; this catches a host restore whose message-thread call
    // hasn't run yet.
    if (presets_.hostRestorePending())
        presetsChanged();

    analysis_.setReferenceHz (paramValue (ParamId::tunerReference));
    irs_.collectGarbage();
    captures_.collectGarbage();
}

void SedefProcessor::captureLoaded()
{
    changeParamValue (ParamId::ampSource, 1.0f);
    if (const auto cab = captures_.suggestedCab())
        changeParamValue (ParamId::captureCab, *cab ? 1.0f : 0.0f);
}

juce::AudioProcessorEditor* SedefProcessor::createEditor()
{
#if SEDEF_PLUGIN_TESTS
    return nullptr; // plugin/tests: the processor without its web UI
#else
    return new SedefEditor (*this);
#endif
}

// State -------------------------------------------------------------------------------------------

void SedefProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("SedefState");
    root.setProperty ("version", 1, nullptr);
    auto params = state_.copyState();
    {
        // A parameter untouched since the last load is written back exactly as it was loaded.
        // JUCE stores denormalise (normalise (x)), which can differ from x in the last bit for
        // skewed ranges, and load-then-save must reproduce a state byte for byte.
        const std::scoped_lock lock (extraLock_);
        for (auto child : params)
            if (const auto id = findParam (child.getProperty ("id").toString().toStdString()))
            {
                const auto i = index (*id);
                if (loaded_[i].valid && juce::exactlyEqual (paramValue (*id), loaded_[i].applied))
                    child.setProperty ("value", loaded_[i].saved, nullptr);
            }
    }
    root.appendChild (params, nullptr);

    // Hosts call this from any thread: everything below comes from atomics or extraLock_.
    juce::ValueTree extra ("Extra");
    {
        const std::scoped_lock lock (extraLock_);
        extra.setProperty ("uiState", uiStateJson_, nullptr);
        extra.setProperty ("presetId", savedPresetId_, nullptr);
        extra.setProperty ("presetName", savedPresetName_, nullptr);
        juce::StringArray steps;
        for (auto s : metronomePattern_)
            steps.add (juce::String (static_cast<int> (s)));
        extra.setProperty ("metronomePattern", steps.joinIntoString (","), nullptr);
        extra.setProperty ("board", boardToText (board_), nullptr);
    }
    extra.setProperty ("uiScale", uiScale(), nullptr);
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

    // The capture's path and the file itself (CaptureLibrary::toState).
    if (auto capture = captures_.toState(); capture.isValid())
        root.appendChild (capture, nullptr);

    if (const auto xml = root.createXml())
        copyXmlToBinary (*xml, dest);
}

void SedefProcessor::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || !xml->hasTagName ("SedefState"))
        return;
    const auto root = juce::ValueTree::fromXml (*xml);

    const auto params = root.getChildWithName (state_.state.getType());
    // This can run on any thread (Logic and AUHost restore AU state off the message thread), so
    // only lock-guarded and atomic members are written here; the preset manager takes the preset
    // over on the message thread (sync()).
    const auto extra = root.getChildWithName ("Extra");
    const auto presetId = extra.getProperty ("presetId").toString();
    // Same fallback as PresetManager::applyCurrent, so the saved state matches what it will hold.
    const auto savedName = extra.getProperty ("presetName").toString();
    const auto presetName = savedName.isNotEmpty() ? savedName : juce::String ("Init");
    {
        // The values, what they were loaded from and the preset they belong to change as one:
        // the editor's preset loads and snapshot restores wait on this lock, so they can't mix
        // their values into the restored ones.
        const auto parametersLock = presets_.lockParameters();
        std::array<LoadedValue, kNumParams> loaded {};
        if (params.isValid())
        {
            // Read the saved values first: replaceState() adopts this tree, and APVTS then writes
            // the values it actually applied back into it.
            for (const auto& child : params)
                if (const auto id = findParam (child.getProperty ("id").toString().toStdString()))
                    loaded[index (*id)] = { child.getProperty ("value"), 0.0f, true };

            // A session saved with a preset switch held on restores it on (hosts and validators
            // expect state to round-trip exactly), but that must not count as a press. Hold the
            // edge detector "on" while the values change, so the timer can't see a rising edge
            // mid-restore, then seed it with the restored values.
            lastPrev_ = true;
            lastNext_ = true;
            state_.replaceState (params);
            lastPrev_ = paramValue (ParamId::presetPrevious) >= 0.5f;
            lastNext_ = paramValue (ParamId::presetNext) >= 0.5f;
            for (std::size_t i = 0; i < kNumParams; ++i)
                loaded[i].applied = paramValue (static_cast<ParamId> (i));
        }
        // The board: a session from before it had none, and gets the one its parameters played.
        const auto restoredBoard =
            extra.hasProperty ("board")
                ? boardFromText (extra.getProperty ("board").toString())
                : Board::legacyFor ([this] (ParamId id) { return paramValue (id) >= 0.5f; });
        const std::scoped_lock lock (extraLock_);
        if (params.isValid())
            loaded_ = loaded;
        if (restoredBoard != board_)
        {
            board_ = restoredBoard;
            engine_.setBoard (restoredBoard);
            ++boardRevision;
        }
        uiStateJson_ = extra.getProperty ("uiState").toString();
        savedPresetId_ = presetId;
        savedPresetName_ = presetName;
        // Snapshots the editor took (browser, undo, A/B) belong to the state just replaced.
        // Under extraLock_ with the lines above, so mirrorPreset() never writes the preset from
        // before this restore over them.
        presets_.hostRestored (presetId, presetName);
    }
    // The CLAP wrapper only tells the host to re-read parameter values after a "parameter info
    // changed" notification (VST3 and AU hosts pick restored values up by themselves).
    if (params.isValid() && wrapperType == wrapperType_Undefined)
        updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    if (juce::MessageManager::existsAndIsCurrentThread())
        presetsChanged();

    setUiScale (juce::jlimit (0.5, 2.5, static_cast<double> (extra.getProperty ("uiScale", 1.0))));

    const auto steps =
        juce::StringArray::fromTokens (extra.getProperty ("metronomePattern").toString(), ",", "");
    if (steps.size() == 13)
    {
        std::array<std::uint8_t, 13> pattern {};
        for (int i = 0; i < 13; ++i)
            pattern[static_cast<size_t> (i)] =
                static_cast<std::uint8_t> (juce::jlimit (0, 3, steps[i].getIntValue()));
        setMetronomePattern (pattern);
    }

    // A capture embedded in the state (every saved session has it) is synchronous, so it plays
    // from the first block; one known only by its path loads on the worker. None clears it.
    captures_.restoreState (root.getChildWithName ("Capture"));

    // The restored state is the whole truth: slots it doesn't mention are empty. The wanted files
    // are recorded now, so a save before the loads finish still reports them.
    std::array<std::array<juce::File, IrLibrary::kCabs>, IrLibrary::kMics> wanted;
    for (const auto& slot : root.getChildWithName ("IRs"))
    {
        const int m = slot.getProperty ("mic");
        const int c = slot.getProperty ("cab");
        // The path comes from a session someone else may have written: only an absolute path to
        // a regular file is used (IrLibrary::checkFile), never a FIFO, device or relative path.
        const auto path = slot.getProperty ("path").toString();
        if (m >= 0 && m < IrLibrary::kMics && c >= 0 && c < IrLibrary::kCabs &&
            IrLibrary::checkFile (path).isEmpty())
            wanted[static_cast<size_t> (m)][static_cast<size_t> (c)] = juce::File (path);
    }
    for (int m = 0; m < IrLibrary::kMics; ++m)
        for (int c = 0; c < IrLibrary::kCabs; ++c)
            irs_.request (m, c, wanted[static_cast<size_t> (m)][static_cast<size_t> (c)]);

    juce::MessageManager::callAsync ([safe = juce::WeakReference<SedefProcessor> (this), wanted] {
        if (auto* p = safe.get())
        {
            p->presetsChanged(); // the preset, if no preset call has taken it over yet

            for (int m = 0; m < IrLibrary::kMics; ++m)
                for (int c = 0; c < IrLibrary::kCabs; ++c)
                {
                    const auto& f = wanted[static_cast<size_t> (m)][static_cast<size_t> (c)];
                    if (f == juce::File())
                        p->irs_.clear (m, c);
                    else
                        p->irs_.loadAsync (m, c, f,
                                           [safeAgain = juce::WeakReference<SedefProcessor> (p)] (
                                               const juce::String&) {
                                               if (auto* q = safeAgain.get())
                                                   ++q->irRevision;
                                           });
                }
            ++p->irRevision;
        }
    });
}

} // namespace sedef::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sedef::plugin::SedefProcessor();
}
