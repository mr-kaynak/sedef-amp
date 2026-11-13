// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LiveState.h"

#include "ParameterLayout.h"
#include "PluginProcessor.h"

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone && !SEDEF_PLUGIN_TESTS
// The standalone holder needs juce_audio_utils first; the comment keeps include sorting off.
#include <juce_audio_utils/juce_audio_utils.h>
//
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#define SEDEF_LIVE_STANDALONE 1
#else
#define SEDEF_LIVE_STANDALONE 0
#endif

namespace sedef::plugin
{
namespace
{
enum class Form
{
    file,    ///< only what differs from the scene in use; that scene {}
    full,    ///< every value of every scene, the one in use read live
    session, ///< every value of the other scenes; the one in use {}
};

bool isNumber (const juce::var& v)
{
    return v.isInt() || v.isInt64() || v.isDouble() || v.isBool();
}

juce::var scenesVar (const live::SceneBank& bank, const std::function<float (ParamId)>& live,
                     Form form)
{
    juce::Array<juce::var> picked;
    for (int i = static_cast<int> (live::kSceneSwitches.size()); i < bank.numParams; ++i)
        picked.add (toJuce (kParamSpecs[bank.params[static_cast<std::size_t> (i)]].id));
    juce::Array<juce::var> list;
    for (int s = 0; s < live::kNumScenes; ++s)
    {
        if (!bank.isMade (s))
        {
            list.add (juce::var()); // not made yet: null
            continue;
        }
        auto* o = new juce::DynamicObject();
        const bool active = s == bank.active;
        if (!active || form == Form::full)
            for (int i = 0; i < bank.numParams; ++i)
            {
                const auto id = static_cast<ParamId> (bank.params[static_cast<std::size_t> (i)]);
                const float v =
                    active
                        ? live (id)
                        : bank.values[static_cast<std::size_t> (s)][static_cast<std::size_t> (i)];
                if (form != Form::file || !juce::exactlyEqual (v, live (id)))
                    o->setProperty (toJuce (spec (id).id), static_cast<double> (v));
            }
        list.add (juce::var (o));
    }
    while (!list.isEmpty() && list.getLast().isVoid())
        list.removeLast();
    auto* root = new juce::DynamicObject();
    root->setProperty ("active", bank.active);
    root->setProperty ("values", picked);
    root->setProperty ("list", list);
    return juce::var (root);
}

bool sameValue (float a, float b, ParamId id)
{
    const auto& s = spec (id);
    return std::abs (a - b) <= 1.0e-4f * (s.max - s.min);
}

std::function<float (ParamId)> parametersOf (const juce::var& parameters)
{
    return [parameters] (ParamId id) {
        const auto& s = spec (id);
        const juce::Identifier key (toJuce (s.id));
        return parameters.hasProperty (key) && isNumber (parameters[key])
                   ? s.sanitise (static_cast<float> (static_cast<double> (parameters[key])))
                   : s.defaultValue;
    };
}
} // namespace

juce::var scenesToVar (const live::SceneBank& bank, const std::function<float (ParamId)>& live,
                       bool full)
{
    return scenesVar (bank, live, full ? Form::full : Form::file);
}

live::SceneBank scenesFromVar (const juce::var& scenes, const std::function<float (ParamId)>& base)
{
    ParamSnapshot values;
    for (std::size_t i = 0; i < kNumParams; ++i)
        values.values[i] = base (static_cast<ParamId> (i));
    auto bank = live::SceneBank::withSwitches (values);
    if (!scenes.isObject())
        return bank;
    if (const auto* picked = scenes["values"].getArray())
        for (const auto& v : *picked)
            if (const auto id = findParam (v.toString().toStdString());
                id && !PresetManager::isGlobal (*id))
                bank.addValue (*id, values[*id]);
    bank.made = 0;
    if (const auto* list = scenes["list"].getArray())
        for (int s = 0; s < std::min (list->size(), live::kNumScenes); ++s)
            if (const auto* o = (*list)[s].getDynamicObject())
                for (bank.made = static_cast<std::uint8_t> (bank.made | 1u << s);
                     const auto& [key, value] : o->getProperties())
                    if (const auto id = findParam (key.toString().toStdString());
                        id && isNumber (value))
                        if (const int at = bank.find (*id); at >= 0)
                            bank.values[static_cast<std::size_t> (s)]
                                       [static_cast<std::size_t> (at)] = spec (*id).sanitise (
                                static_cast<float> (static_cast<double> (value)));
    const auto& active = scenes["active"];
    bank.active = static_cast<std::uint8_t> (
        isNumber (active) ? juce::jlimit (0, live::kNumScenes - 1, static_cast<int> (active)) : 0);
    bank.made = static_cast<std::uint8_t> (bank.made | 1u << bank.active);
    return bank;
}

bool scenesAreTrivial (const live::SceneBank& bank)
{
    return bank.numValues() == 0 && bank.made == (1u << bank.active);
}

// -------------------------------------------------------------------------------------------------

LiveState::LiveState (SedefProcessor& processor) : processor_ (processor)
{
    settings_.load();
    settingsText_ = LiveSettings::file().loadFileAsString();
    control_.setMap (settings_.toMap());
    {
        const std::scoped_lock lock (bankLock_);
        ParamSnapshot values;
        for (std::size_t i = 0; i < kNumParams; ++i)
            values.values[i] = liveValue (static_cast<ParamId> (i));
        bank_ = live::SceneBank::withSwitches (values);
        publish (false);
    }
    startTimerHz (kTickHz);
}

LiveState::~LiveState()
{
    stopTimer();
}

float LiveState::liveValue (ParamId id) const
{
    return processor_.paramValue (id);
}

void LiveState::write (ParamId id, float value)
{
    if (!juce::exactlyEqual (processor_.paramValue (id), value))
        processor_.changeParamValue (id, value);
}

void LiveState::publish (bool applySwitch)
{
    bank_.applySwitch = applySwitch;
    control_.setScenes (bank_);
}

void LiveState::tick()
{
    ++ticks_;
    if (const auto t = control_.audioTime(); t != lastAudioTime_)
    {
        lastAudioTime_ = t;
        ticksSinceAudio_ = 0;
    }
    else
        ++ticksSinceAudio_;

    control_.syncParameters ([this] (ParamId id, float v) { write (id, v); });
    processEvents();
    processLearn();

    // An on-screen switch the audio thread hasn't taken in 250 ms (it stopped): the parameters
    // were written already, so it must not apply stale values when it starts again (§7.4).
    if (pendingSwitch_ != 0)
    {
        if (control_.consumedSwitchCount() == pendingSwitch_)
            pendingSwitch_ = 0;
        else if (ticks_ - pendingSince_ >= kTickHz / 4)
        {
            const std::scoped_lock lock (bankLock_);
            if (bank_.switchCount == pendingSwitch_)
                publish (false);
            pendingSwitch_ = 0;
        }
    }

    // Another instance (or the standalone app) changed the MIDI setup. The text is compared, not
    // the modification time: two saves within the file system's time resolution (a millisecond
    // in JUCE, a second or two on some volumes) would look like none.
    if (ticks_ % kTickHz == 0)
        if (auto text = LiveSettings::file().loadFileAsString(); text != settingsText_)
        {
            settingsText_ = std::move (text);
            LiveSettings fresh;
            if (fresh.load() && !(fresh == settings_) &&
                !(learn_ && learn_->phase != Learn::Phase::done))
            {
                settings_ = fresh;
                control_.setMap (settings_.toMap());
                ++revision;
            }
        }
}

void LiveState::processEvents()
{
    // A program change loads a preset, whose scenes come back through select(): never nest.
    if (processingEvents_)
        return;
    const juce::ScopedValueSetter<bool> busy (processingEvents_, true);
    live::LiveEvent e;
    bool tookReports = false, presetsMoved = false;
    while (control_.popEvent (e))
    {
        switch (e.type)
        {
        case live::LiveEvent::Type::sceneSwitched:
        {
            const std::scoped_lock lock (bankLock_);
            const auto& r = e.scene;
            // A report made on a bank that was replaced or switched on screen since is stale:
            // that change already decided where the scenes are (§7.2).
            if (r.basedOn == bank_.switchCount)
            {
                for (int i = 0; i < r.numParams; ++i)
                    if (const int at = bank_.find (
                            static_cast<ParamId> (r.params[static_cast<std::size_t> (i)]));
                        at >= 0)
                        bank_.values[r.from][static_cast<std::size_t> (at)] =
                            r.values[static_cast<std::size_t> (i)];
                if (!bank_.isMade (r.to))
                    bank_.values[r.to] = bank_.values[r.from]; // made as a copy (§6.3)
                bank_.made = static_cast<std::uint8_t> (bank_.made | 1u << r.from | 1u << r.to);
                bank_.active = r.to;
            }
            ++bank_.reportsSeen;
            tookReports = true;
            break;
        }
        case live::LiveEvent::Type::program:
        {
            const auto assigned = settings_.programs.find (e.program);
            const auto id = assigned != settings_.programs.end()
                                ? assigned->second.presetId
                                : processor_.presets().idAt (e.program);
            if (id.isNotEmpty() && processor_.presets().load (id, true))
                presetsMoved = true;
            break;
        }
        case live::LiveEvent::Type::presetStep:
            processor_.presets().step (e.step);
            presetsMoved = true;
            break;
        }
    }
    if (tookReports)
    {
        {
            const std::scoped_lock lock (bankLock_);
            publish (false);
        }
        ++revision;
    }
    if (presetsMoved)
        processor_.presetsChanged();
}

void LiveState::processLearn()
{
    live::LearnMessage m;
    while (control_.popLearn (m))
    {
        if (!learn_ || learn_->phase == Learn::Phase::done)
            continue;
        auto& l = *learn_;
        const auto& msg = m.message;
        const int value = msg.data2;
        if (l.phase == Learn::Phase::waiting)
        {
            if (!msg.isControlChange() || !live::isLearnable (msg.data1))
                continue;
            l.binding =
                settings_.bind (l.target, msg.channel(), msg.data1, live::SwitchMode::toggle);
            if (l.binding < 0)
            {
                finishLearn (std::nullopt); // the map is full
                continue;
            }
            if (!l.target.isSwitch())
            {
                finishLearn (std::nullopt);
                continue;
            }
            const auto window = static_cast<std::int64_t> (live::kLearnWindowSeconds *
                                                           processor_.engine().sampleRate());
            l.classifier.emplace (window);
            l.phase = Learn::Phase::listening;
            if (const auto mode = l.classifier->first (value, m.time))
                finishLearn (mode);
            else
                settingsChanged();
            continue;
        }
        const auto& b = settings_.bindings[static_cast<std::size_t> (l.binding)];
        if (msg.isControlChange() && msg.channel() == b.channel && msg.data1 == b.controller)
            if (const auto mode = l.classifier->next (value, m.time))
                finishLearn (mode);
    }
    if (learn_ && learn_->phase == Learn::Phase::listening)
        if (const auto mode = learn_->classifier->elapsed (control_.audioTime()))
            finishLearn (mode);
}

void LiveState::finishLearn (std::optional<live::SwitchMode> mode)
{
    if (!learn_)
        return;
    if (mode && learn_->binding >= 0 &&
        learn_->binding < static_cast<int> (settings_.bindings.size()))
        settings_.bindings[static_cast<std::size_t> (learn_->binding)].mode = *mode;
    learn_->phase = Learn::Phase::done;
    control_.setLearning (false);
    settingsChanged();
}

void LiveState::settingsChanged()
{
    settings_.save();
    settingsText_ = LiveSettings::file().loadFileAsString();
    control_.setMap (settings_.toMap());
    ++revision;
}

// Scenes ------------------------------------------------------------------------------------------

int LiveState::activeScene() const
{
    const std::scoped_lock lock (bankLock_);
    return bank_.active;
}

void LiveState::selectScene (int scene)
{
    if (scene < 0 || scene >= live::kNumScenes)
        return;
    // What the controllers did so far belongs to the scene being left.
    control_.syncParameters ([this] (ParamId id, float v) { write (id, v); });
    processEvents();

    std::vector<std::pair<ParamId, float>> values;
    {
        const std::scoped_lock lock (bankLock_);
        if (scene == bank_.active)
            return;
        auto& left = bank_.values[bank_.active];
        for (std::size_t i = 0; i < bank_.numParams; ++i)
            left[i] = liveValue (static_cast<ParamId> (bank_.params[i]));
        if (!bank_.isMade (scene))
            bank_.values[static_cast<std::size_t> (scene)] = left; // made as a copy (§6.3)
        bank_.made = static_cast<std::uint8_t> (bank_.made | 1u << scene);
        bank_.active = static_cast<std::uint8_t> (scene);
        ++bank_.switchCount;
        // While audio runs, the audio thread applies the whole scene in one block; the
        // parameters written below then only catch up with it.
        const bool running = ticksSinceAudio_ < kTickHz / 4;
        publish (running);
        pendingSwitch_ = running ? bank_.switchCount : 0;
        pendingSince_ = ticks_;
        for (std::size_t i = 0; i < bank_.numParams; ++i)
            values.emplace_back (static_cast<ParamId> (bank_.params[i]),
                                 bank_.values[static_cast<std::size_t> (scene)][i]);
    }
    for (const auto& [id, v] : values)
        write (id, v);
    write (ParamId::scene, static_cast<float> (scene));
    ++revision;
}

void LiveState::copyScene (int scene)
{
    if (scene < 0 || scene >= live::kNumScenes)
        return;
    control_.syncParameters ([this] (ParamId id, float v) { write (id, v); });
    processEvents();
    {
        const std::scoped_lock lock (bankLock_);
        if (scene == bank_.active)
            return;
        for (std::size_t i = 0; i < bank_.numParams; ++i)
            bank_.values[static_cast<std::size_t> (scene)][i] =
                liveValue (static_cast<ParamId> (bank_.params[i]));
        bank_.made = static_cast<std::uint8_t> (bank_.made | 1u << scene);
        publish (false);
    }
    ++revision;
}

bool LiveState::rememberInScenes (ParamId id, bool remember)
{
    if (PresetManager::isGlobal (id))
        return false;
    bool changed = false;
    {
        const std::scoped_lock lock (bankLock_);
        changed = remember ? bank_.addValue (id, liveValue (id)) : bank_.removeValue (id);
        if (changed)
            publish (false);
    }
    if (changed)
        ++revision;
    return changed;
}

// Presets -----------------------------------------------------------------------------------------

juce::var LiveState::reference (bool forPreset)
{
    const auto live = [this] (ParamId id) {
        return liveValue (id);
    };
    const std::scoped_lock lock (bankLock_);
    if (forPreset)
        return scenesAreTrivial (bank_) ? juce::var() : scenesVar (bank_, live, Form::file);
    return scenesVar (bank_, live, Form::full);
}

void LiveState::select (const juce::var& scenes, bool)
{
    // Reports of switches made on the bank being replaced arrive with the old switch count and
    // are passed over (processEvents).
    auto bank = scenesFromVar (scenes, [this] (ParamId id) { return liveValue (id); });
    int active = 0;
    {
        const std::scoped_lock lock (bankLock_);
        bank.switchCount = bank_.switchCount + 1; // the audio thread takes it as it is
        bank.reportsSeen = bank_.reportsSeen;
        bank_ = bank;
        publish (false);
        active = bank_.active;
        pendingSwitch_ = 0;
    }
    write (ParamId::scene, static_cast<float> (active));
    ++revision;
}

bool LiveState::differs (const juce::var& loaded)
{
    const auto live = [this] (ParamId id) {
        return liveValue (id);
    };
    const auto reference = scenesFromVar (loaded, live);
    live::SceneBank current;
    {
        const std::scoped_lock lock (bankLock_);
        current = bank_;
    }
    if (current.numParams != reference.numParams)
        return true;
    for (int s = 0; s < live::kNumScenes; ++s)
        if (reference.isMade (s) && !current.isMade (s))
            return true;
    for (int i = 0; i < current.numParams; ++i)
    {
        const auto id = static_cast<ParamId> (current.params[static_cast<std::size_t> (i)]);
        const int at = reference.find (id);
        if (at < 0)
            return true;
        const auto col = static_cast<std::size_t> (at);
        for (int s = 0; s < live::kNumScenes; ++s)
        {
            if (!current.isMade (s))
                continue;
            const float now =
                s == current.active
                    ? live (id)
                    : current.values[static_cast<std::size_t> (s)][static_cast<std::size_t> (i)];
            // A scene the file doesn't have, made as an untouched copy of the file's own sound,
            // is not a change (§6.4).
            const auto row = static_cast<std::size_t> (reference.isMade (s) ? s : reference.active);
            if (!sameValue (now, reference.values[row][col], id))
                return true;
        }
    }
    return false;
}

juce::var LiveState::resolve (const juce::var& stored, const juce::var& parameters) const
{
    // "parameters" is the sound of the active scene, whatever the file's scene says (§6.4).
    const auto base =
        parametersOf (parameters.isObject() ? parameters : juce::var (new juce::DynamicObject()));
    auto bank = scenesFromVar (stored, base);
    return scenesVar (bank, base, Form::full);
}

bool LiveState::owns (ParamId id) const
{
    const std::scoped_lock lock (bankLock_);
    return bank_.find (id) >= 0;
}

// Sessions ----------------------------------------------------------------------------------------

juce::String LiveState::sessionText()
{
    const auto live = [this] (ParamId id) {
        return liveValue (id);
    };
    const std::scoped_lock lock (bankLock_);
    // Only the scene in use made and nothing picked: the Scene parameter says all there is.
    if (scenesAreTrivial (bank_))
        return {};
    return juce::JSON::toString (scenesVar (bank_, live, Form::session), true);
}

int LiveState::restoreSession (const juce::String& text,
                               const std::function<float (ParamId)>& saved)
{
    const auto parsed = text.isEmpty() ? juce::var() : juce::JSON::parse (text);
    auto bank = scenesFromVar (parsed, saved);
    // The Scene parameter is the selection (§6.6). A host can set it and save before the message
    // thread hears of the switch (a validator does, with no event loop): the session then says one
    // scene and the parameter another. The parameter wins, and its scene holds the session's
    // sound, as the scene in use always does (§6.4), so the state round-trips as saved.
    if (const auto selected = static_cast<int> (std::lround (saved (ParamId::scene)));
        selected != bank.active && selected >= 0 && selected < live::kNumScenes)
    {
        // A session without scenes (or with only the scene in use) keeps having only one.
        const auto others = scenesAreTrivial (bank) ? 0u : static_cast<unsigned> (bank.made);
        const auto row = static_cast<std::size_t> (selected);
        for (std::size_t i = 0; i < bank.numParams; ++i)
            bank.values[row][i] = saved (static_cast<ParamId> (bank.params[i]));
        bank.made = static_cast<std::uint8_t> (others | 1u << selected);
        bank.active = static_cast<std::uint8_t> (selected);
    }
    int active = 0;
    {
        const std::scoped_lock lock (bankLock_);
        bank.switchCount = bank_.switchCount + 1;
        bank.reportsSeen = bank_.reportsSeen;
        bank_ = bank;
        publish (false);
        active = bank_.active;
    }
    ++revision;
    return active;
}

// Learn and the MIDI setup ------------------------------------------------------------------------

bool LiveState::learn (const juce::String& target)
{
    const auto t = LiveTarget::parse (target);
    if (!t)
        return false;
    live::LearnMessage stale;
    while (control_.popLearn (stale))
        ;
    learn_.emplace();
    learn_->target = *t;
    control_.setLearning (true);
    ++revision;
    return true;
}

void LiveState::cancelLearn()
{
    if (!learn_)
        return;
    if (learn_->phase == Learn::Phase::listening)
    {
        // Ended before the window: nothing else came, so a latching switch (§4.4).
        finishLearn (learn_->classifier->mode().value_or (live::SwitchMode::latch));
    }
    learn_.reset();
    control_.setLearning (false);
    ++revision;
}

void LiveState::forget (const juce::String& target)
{
    if (const auto t = LiveTarget::parse (target))
        if (const int at = settings_.find (*t); at >= 0)
        {
            settings_.bindings.erase (settings_.bindings.begin() + at);
            settingsChanged();
        }
}

void LiveState::updateBinding (int index, const juce::var& changes)
{
    if (index < 0 || index >= static_cast<int> (settings_.bindings.size()) || !changes.isObject())
        return;
    auto& b = settings_.bindings[static_cast<std::size_t> (index)];
    if (changes.hasProperty ("mode"))
        b.mode = changes["mode"].toString() == "latch" ? live::SwitchMode::latch
                                                       : live::SwitchMode::toggle;
    const auto unit = [] (const juce::var& v, float fallback) {
        return isNumber (v)
                   ? juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (v)))
                   : fallback;
    };
    if (changes.hasProperty ("low"))
        b.low = unit (changes["low"], b.low);
    if (changes.hasProperty ("high"))
        b.high = unit (changes["high"], b.high);
    settingsChanged();
}

void LiveState::removeBinding (int index)
{
    if (index < 0 || index >= static_cast<int> (settings_.bindings.size()))
        return;
    settings_.bindings.erase (settings_.bindings.begin() + index);
    learn_.reset(); // a learn result pointing at it would be wrong now
    control_.setLearning (false);
    settingsChanged();
}

void LiveState::clearBindings()
{
    settings_.bindings.clear();
    learn_.reset();
    control_.setLearning (false);
    settingsChanged();
}

void LiveState::setProgram (int program, const juce::var& target)
{
    if (program < 0 || program > 127)
        return;
    if (target.isObject() && target.hasProperty ("scene"))
    {
        const int scene = isNumber (target["scene"]) ? static_cast<int> (target["scene"]) : -1;
        if (scene < 0 || scene >= live::kNumScenes)
            return;
        settings_.programs[program] = ProgramTarget { {}, scene };
    }
    else if (target.isObject() && target["preset"].toString().isNotEmpty())
        settings_.programs[program] = ProgramTarget { target["preset"].toString(), -1 };
    else
        settings_.programs.erase (program);
    settingsChanged();
}

void LiveState::setProgramChannel (int channel)
{
    settings_.programChannel = juce::jlimit (-1, 15, channel);
    settingsChanged();
}

void LiveState::setInput (const juce::String& identifier, bool enabled)
{
#if SEDEF_LIVE_STANDALONE
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        holder->deviceManager.setMidiInputDeviceEnabled (identifier, enabled);
#else
    juce::ignoreUnused (identifier, enabled);
#endif
    ++revision;
}

// The bridge's view -------------------------------------------------------------------------------

juce::var LiveState::stateAsVar()
{
    const auto obj = [] {
        return new juce::DynamicObject();
    };

    auto* scenes = obj();
    {
        const std::scoped_lock lock (bankLock_);
        scenes->setProperty ("active", bank_.active);
        scenes->setProperty ("count", live::kNumScenes);
        scenes->setProperty ("maxValues", live::kMaxSceneValues);
        juce::Array<juce::var> picked, switches;
        for (int i = 0; i < bank_.numParams; ++i)
        {
            const auto id = toJuce (kParamSpecs[bank_.params[static_cast<std::size_t> (i)]].id);
            (i < static_cast<int> (live::kSceneSwitches.size()) ? switches : picked).add (id);
        }
        scenes->setProperty ("values", picked);
        scenes->setProperty ("switches", switches);
        juce::Array<juce::var> made;
        for (int sc = 0; sc < live::kNumScenes; ++sc)
            made.add (bank_.isMade (sc));
        scenes->setProperty ("made", made);
    }

    juce::Array<juce::var> bindings;
    for (std::size_t i = 0; i < settings_.bindings.size(); ++i)
    {
        const auto& b = settings_.bindings[i];
        auto* o = obj();
        o->setProperty ("index", static_cast<int> (i));
        o->setProperty ("target", b.target.toString());
        o->setProperty ("channel", b.channel + 1);
        o->setProperty ("controller", b.controller);
        o->setProperty ("switch", b.target.isSwitch());
        o->setProperty ("mode", b.mode == live::SwitchMode::latch ? "latch" : "toggle");
        o->setProperty ("low", static_cast<double> (b.low));
        o->setProperty ("high", static_cast<double> (b.high));
        bindings.add (juce::var (o));
    }

    juce::Array<juce::var> assigned;
    for (const auto& [program, target] : settings_.programs)
    {
        auto* o = obj();
        o->setProperty ("program", program + 1);
        if (target.scene >= 0)
            o->setProperty ("scene", target.scene);
        else
            o->setProperty ("preset", target.presetId);
        assigned.add (juce::var (o));
    }
    auto* programs = obj();
    programs->setProperty ("channel", settings_.programChannel + 1);
    programs->setProperty ("assigned", assigned);

    juce::var learn;
    if (learn_)
    {
        auto* o = obj();
        o->setProperty ("target", learn_->target.toString());
        static constexpr const char* kPhases[] = { "waiting", "listening", "done" };
        o->setProperty ("phase", kPhases[static_cast<int> (learn_->phase)]);
        o->setProperty ("binding", learn_->binding);
        learn = juce::var (o);
    }

    const auto wrapper = processor_.wrapperType;
    const char* midiIn = wrapper == juce::AudioProcessor::wrapperType_VST3        ? "controllers"
                         : wrapper == juce::AudioProcessor::wrapperType_AudioUnit ? "host"
                                                                                  : "full";

    juce::var inputs;
#if SEDEF_LIVE_STANDALONE
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        juce::Array<juce::var> list;
        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            auto* o = obj();
            o->setProperty ("id", device.identifier);
            o->setProperty ("name", device.name);
            o->setProperty ("enabled",
                            holder->deviceManager.isMidiInputDeviceEnabled (device.identifier));
            list.add (juce::var (o));
        }
        inputs = list;
    }
#endif

    auto* root = obj();
    root->setProperty ("scenes", juce::var (scenes));
    root->setProperty ("bindings", bindings);
    root->setProperty ("maxBindings", live::kMaxBindings);
    root->setProperty ("programs", juce::var (programs));
    root->setProperty ("learn", learn);
    root->setProperty ("midiIn", midiIn);
    root->setProperty ("inputs", inputs);
    root->setProperty ("revision", revision.load());
    return juce::var (root);
}

} // namespace sedef::plugin
