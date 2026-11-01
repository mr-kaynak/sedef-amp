// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PresetManager.h"

#include "ParameterLayout.h"
#include "SedefPresetData.h"

#include <cmath>

namespace sedef::plugin
{
namespace
{
juce::File sedefFolder()
{
#if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/Sedef");
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Sedef");
#endif
}

bool sameValue (float a, float b, const ParamSpec& spec)
{
    return std::abs (a - b) <= 1.0e-4f * (spec.max - spec.min);
}

/// The normalised value that leaves `param` holding exactly `target`. The parameter stores
/// convertFrom0to1 (n) and the APVTS raw value is convertFrom0to1 (getValue()), a round trip that
/// can move a skewed value by an ULP; a neighbour of the normalised value it had lands exactly.
float normalisedFor (const juce::RangedAudioParameter& param, float target, float hint)
{
    const auto lands = [&param, target] (float n) {
        return juce::exactlyEqual (
            param.convertFrom0to1 (param.convertTo0to1 (param.convertFrom0to1 (n))), target);
    };
    float down = hint, up = hint;
    for (int step = 0; step <= 8; ++step)
    {
        if (lands (down))
            return down;
        if (lands (up))
            return up;
        down = std::nextafter (down, 0.0f);
        up = std::nextafter (up, 1.0f);
    }
    return param.convertTo0to1 (target);
}

juce::Array<juce::var> toVarArray (const juce::StringArray& strings)
{
    juce::Array<juce::var> list;
    for (const auto& s : strings)
        list.add (s);
    return list;
}
} // namespace

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state) : state_ (state)
{
    loadSettings();
    scan();
}

std::optional<PresetManager::Slot> PresetManager::slotFromString (const juce::String& name)
{
    if (name == "browse")
        return Slot::browse;
    if (name == "undo")
        return Slot::undo;
    return std::nullopt;
}

bool PresetManager::isGlobal (ParamId id) noexcept
{
    static constexpr ParamId kGlobal[] = {
        ParamId::inputGain,       ParamId::outputGain,
        ParamId::transpose,       ParamId::stereoMode,
        ParamId::tempo,           ParamId::tunerReference,
        ParamId::tunerMute,       ParamId::metronomeEnabled,
        ParamId::metronomeVolume, ParamId::metronomeSignature,
        ParamId::metronomeSound,  ParamId::presetPrevious,
        ParamId::presetNext,
    };
    return std::find (std::begin (kGlobal), std::end (kGlobal), id) != std::end (kGlobal);
}

juce::File PresetManager::userFolder()
{
    return sedefFolder().getChildFile ("Presets");
}

juce::File PresetManager::settingsFile()
{
    return sedefFolder().getChildFile ("settings.json");
}

juce::String PresetManager::userId (const juce::File& file)
{
    return "user:" + file.getRelativePathFrom (userFolder()).replaceCharacter ('\\', '/');
}

juce::String PresetManager::slugify (const juce::String& s)
{
    return s.toLowerCase()
        .retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 -")
        .trim()
        .replaceCharacter (' ', '-');
}

void PresetManager::scan()
{
    entries_.clear();

    for (int i = 0; i < SedefPresetData::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data =
            SedefPresetData::getNamedResource (SedefPresetData::namedResourceList[i], size);
        const auto json = juce::String::fromUTF8 (data, size);
        const auto parsed = juce::JSON::parse (json);
        if (!parsed.isObject())
            continue;
        Entry e;
        e.name = parsed["name"].toString();
        e.id = "factory:" + slugify (e.name);
        e.category = "Factory";
        e.folder = parsed["folder"].toString();
        e.author = parsed["author"].toString();
        e.description = parsed["description"].toString();
        if (auto* tags = parsed["tags"].getArray())
            for (const auto& t : *tags)
                e.tags.add (t.toString());
        describe (e, parsed["parameters"]);
        e.factoryJson = json;
        entries_.push_back (std::move (e));
    }

    const auto root = userFolder();
    for (const auto& f :
         root.findChildFiles (juce::File::findFiles, true, juce::String ("*") + kExtension))
    {
        const auto parsed = juce::JSON::parse (f);
        if (!parsed.isObject())
            continue;
        Entry e;
        e.file = f;
        e.name = parsed["name"].toString().isNotEmpty() ? parsed["name"].toString()
                                                        : f.getFileNameWithoutExtension();
        e.id = userId (f);
        e.category = "User";
        e.folder =
            f.getParentDirectory() == root ? juce::String() : f.getParentDirectory().getFileName();
        e.author = parsed["author"].toString();
        e.description = parsed["description"].toString();
        if (auto* tags = parsed["tags"].getArray())
            for (const auto& t : *tags)
                e.tags.add (t.toString());
        describe (e, parsed["parameters"]);
        entries_.push_back (std::move (e));
    }

    std::stable_sort (entries_.begin(), entries_.end(), [] (const Entry& a, const Entry& b) {
        if (a.category != b.category)
            return a.category == "Factory";
        if (a.folder != b.folder)
            return a.folder.compareNatural (b.folder) < 0;
        return a.name.compareNatural (b.name) < 0;
    });
}

const PresetManager::Entry* PresetManager::find (const juce::String& id) const
{
    for (const auto& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

void PresetManager::describe (Entry& e, const juce::var& parameters)
{
    const auto value = [&parameters] (ParamId id) {
        const auto& spec = sedef::spec (id);
        const juce::Identifier key (toJuce (spec.id));
        return spec.sanitise (parameters.hasProperty (key) ? static_cast<float> (parameters[key])
                                                           : spec.defaultValue);
    };
    const auto on = [&value] (ParamId id) {
        return value (id) >= 0.5f;
    };

    e.amp = juce::roundToInt (value (ParamId::ampModel));
    e.cab = on (ParamId::ampCabLink) ? e.amp : juce::roundToInt (value (ParamId::cabModel));

    // The amp's EQ bank in ParameterList.inc order: switch, nine bands, low cut, high cut. It
    // counts only when it shapes the sound.
    static_assert (index (ParamId::eqLagoonHighCut) == index (ParamId::eqLagoonEnabled) + 11);
    static_assert (index (ParamId::eqReefHighCut) == index (ParamId::eqReefEnabled) + 11);
    static_assert (index (ParamId::eqAbyssHighCut) == index (ParamId::eqAbyssEnabled) + 11);
    static constexpr ParamId kEqBanks[] = { ParamId::eqLagoonEnabled, ParamId::eqReefEnabled,
                                            ParamId::eqAbyssEnabled };
    const auto bank = index (kEqBanks[juce::jlimit (0, 2, e.amp)]);
    const auto eq = [bank] (std::size_t offset) {
        return static_cast<ParamId> (bank + offset);
    };
    bool eqShapes = value (eq (10)) > 20.5f || value (eq (11)) < 19990.0f;
    for (std::size_t band = 1; band <= 9; ++band)
        eqShapes = eqShapes || std::abs (value (eq (band))) > 0.05f;

    e.blocks.clear();
    const auto add = [&] (ParamId id, const char* name) {
        if (on (id))
            e.blocks.add (name);
    };
    add (ParamId::gateEnabled, "gate");
    add (ParamId::tideEnabled, "tide");
    add (ParamId::pearlEnabled, "pearl");
    add (ParamId::coralEnabled, "coral");
    add (ParamId::rippleEnabled, "ripple");
    if (eqShapes)
        add (eq (0), "eq");
    add (ParamId::doublerEnabled, "doubler");
    add (ParamId::driftEnabled, "drift");
    add (ParamId::fathomEnabled, "fathom");
}

std::optional<juce::var> PresetManager::read (const Entry& e) const
{
    const auto parsed =
        e.file.existsAsFile() ? juce::JSON::parse (e.file) : juce::JSON::parse (e.factoryJson);
    if (!parsed.isObject() || !parsed["parameters"].isObject())
        return std::nullopt;
    return parsed;
}

void PresetManager::apply (const juce::var& parameters)
{
    loadedValues_.clear();
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        const auto id = static_cast<ParamId> (i);
        if (isGlobal (id))
            continue;
        const auto& spec = kParamSpecs[i];
        const auto key = toJuce (spec.id);
        const float value = parameters.hasProperty (key)
                                ? static_cast<float> (parameters[juce::Identifier (key)])
                                : spec.defaultValue;
        const float clean = spec.sanitise (value);
        if (auto* param = state_.getParameter (key))
            param->setValueNotifyingHost (spec.toNormalised (clean));
        loadedValues_[id] = clean;
    }
}

bool PresetManager::load (const juce::String& id, bool audition)
{
    const auto* e = find (id);
    if (e == nullptr)
        return false;
    const auto parsed = read (*e);
    if (!parsed)
        return false;
    apply ((*parsed)["parameters"]);
    currentId_ = e->id;
    currentName_ = e->name;
    if (!audition)
        markRecent (currentId_);
    return true;
}

void PresetManager::markRecent (const juce::String& id)
{
    if (find (id) == nullptr)
        return;
    loadSettings(); // another instance may have picked presets since
    recent_.removeString (id);
    recent_.insert (0, id);
    recent_.removeRange (kMaxRecent, recent_.size());
    storeSettings();
}

void PresetManager::loadInit()
{
    apply (juce::var (new juce::DynamicObject()));
    currentId_ = {};
    currentName_ = "Init";
}

juce::var PresetManager::captureParameters() const
{
    auto* params = new juce::DynamicObject();
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        if (isGlobal (static_cast<ParamId> (i)))
            continue;
        const auto key = toJuce (kParamSpecs[i].id);
        if (const auto* raw = state_.getRawParameterValue (key))
            params->setProperty (key, static_cast<double> (raw->load()));
    }
    return juce::var (params);
}

juce::String PresetManager::save (const juce::String& name, const juce::String& folder,
                                  const juce::var& details)
{
    const auto cleanName = name.trim().isEmpty() ? juce::String ("Untitled") : name.trim();
    auto dir = userFolder();
    if (folder.trim().isNotEmpty())
        dir = dir.getChildFile (juce::File::createLegalFileName (folder.trim()));
    dir.createDirectory();
    const auto file = dir.getChildFile (juce::File::createLegalFileName (cleanName) + kExtension);

    // Overwriting keeps the author, and without new details what the preset said about itself.
    const auto existing = file.existsAsFile() ? juce::JSON::parse (file) : juce::var();
    auto author = existing["author"].toString();
    if (author.isEmpty())
        author = juce::SystemStats::getFullUserName();
    juce::Array<juce::var> tags;
    juce::String description;
    if (details.isObject())
    {
        juce::StringArray clean;
        if (const auto* list = details["tags"].getArray())
            for (const auto& t : *list)
            {
                const auto tag = t.toString().trim().toLowerCase().substring (0, 24).trim();
                if (tag.isNotEmpty() && clean.size() < 8)
                    clean.addIfNotAlreadyThere (tag);
            }
        tags = toVarArray (clean);
        description = details["description"]
                          .toString()
                          .replaceCharacters ("\r\n\t", "   ")
                          .trim()
                          .substring (0, 140)
                          .trim();
    }
    else
    {
        if (const auto* list = existing["tags"].getArray())
            tags = *list;
        description = existing["description"].toString();
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "sedef-preset");
    root->setProperty ("version", 1);
    root->setProperty ("name", cleanName);
    root->setProperty ("author", author);
    root->setProperty ("tags", tags);
    if (description.isNotEmpty())
        root->setProperty ("description", description);
    root->setProperty ("parameters", captureParameters());
    file.replaceWithText (juce::JSON::toString (juce::var (root)));

    scan();
    currentId_ = userId (file);
    currentName_ = cleanName;
    // The saved state is the new reference for "dirty".
    if (const auto* e = find (currentId_))
        if (const auto parsed = read (*e))
            for (std::size_t i = 0; i < kNumParams; ++i)
                if (!isGlobal (static_cast<ParamId> (i)))
                    loadedValues_[static_cast<ParamId> (i)] = static_cast<float> (
                        (*parsed)["parameters"][juce::Identifier (toJuce (kParamSpecs[i].id))]);
    markRecent (currentId_);
    return currentId_;
}

bool PresetManager::remove (const juce::String& id)
{
    const auto* e = find (id);
    if (e == nullptr || e->category != "User")
        return false;
    // Kept so the editor can offer Undo: the same file comes back with the same id.
    Removed removed { e->file, e->file.loadFileAsString(), e->id, e->name, currentId_ == id };
    const bool ok = e->file.moveToTrash() || e->file.deleteFile();
    if (ok)
    {
        removed_ = std::move (removed);
        if (currentId_ == id)
            currentId_ = {};
    }
    scan();
    return ok;
}

bool PresetManager::undoRemove()
{
    if (!removed_ || removed_->file.exists())
        return false;
    const auto r = *removed_;
    r.file.getParentDirectory().createDirectory();
    if (!r.file.replaceWithText (r.text))
        return false;
    removed_.reset();
    scan();
    // It becomes current again only if nothing else was loaded since it went.
    if (r.wasCurrent && currentId_.isEmpty() && currentName_ == r.name)
        currentId_ = r.id;
    return true;
}

bool PresetManager::rename (const juce::String& id, const juce::String& newName)
{
    const auto* e = find (id);
    if (e == nullptr || e->category != "User" || newName.trim().isEmpty())
        return false;
    auto parsed = read (*e);
    if (!parsed)
        return false;
    if (auto* obj = parsed->getDynamicObject())
        obj->setProperty ("name", newName.trim());
    const auto target =
        e->file.getSiblingFile (juce::File::createLegalFileName (newName.trim()) + kExtension);
    if (target.exists() && target != e->file)
        return false;
    target.replaceWithText (juce::JSON::toString (*parsed));
    if (target != e->file)
        e->file.deleteFile();
    const bool wasCurrent = currentId_ == id;
    scan();
    const auto newId = userId (target);
    if (wasCurrent)
    {
        currentId_ = newId;
        currentName_ = newName.trim();
    }

    // Favourites and Recent follow the preset to its new id.
    loadSettings();
    bool moved = false;
    for (auto* list : { &favorites_, &recent_ })
        if (const int i = list->indexOf (id); i >= 0 && newId != id)
        {
            list->set (i, newId);
            moved = true;
        }
    if (moved)
        storeSettings();
    return true;
}

void PresetManager::setFavorite (const juce::String& id, bool favorite)
{
    loadSettings(); // another instance may have changed them since
    if (favorite)
        favorites_.addIfNotAlreadyThere (id);
    else
        favorites_.removeString (id);
    storeSettings();
}

void PresetManager::step (int delta)
{
    if (entries_.empty())
        return;
    int index = -1;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].id == currentId_)
            index = static_cast<int> (i);
    const int count = static_cast<int> (entries_.size());
    const int next =
        index < 0 ? (delta > 0 ? 0 : count - 1) : ((index + delta) % count + count) % count;
    load (entries_[static_cast<size_t> (next)].id, true); // stepping through is not a pick
}

// Snapshots and A/B -------------------------------------------------------------------------------

void PresetManager::syncGeneration()
{
    const int generation = generation_.load();
    if (generation == seenGeneration_)
        return;
    seenGeneration_ = generation;
    for (auto& s : slots_)
        s.reset();
    compareOther_.reset();
    compareIsB_ = false;
}

PresetManager::Snapshot PresetManager::capture() const
{
    Snapshot s;
    // Read first: a host restore bumps the generation after it has replaced the values, so a
    // snapshot that overlaps one is dropped instead of being restored half old, half new.
    s.generation = generation_.load();
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        if (isGlobal (static_cast<ParamId> (i)))
            continue;
        const auto key = toJuce (kParamSpecs[i].id);
        if (const auto* param = state_.getParameter (key))
            s.normalised[i] = param->getValue();
        if (const auto* raw = state_.getRawParameterValue (key))
            s.raw[i] = raw->load();
    }
    s.loadedValues = loadedValues_;
    s.id = currentId_;
    s.name = currentName_;
    s.dirty = isDirty();
    return s;
}

bool PresetManager::restore (const Snapshot& s)
{
    if (s.generation != generation_.load())
        return false;
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        if (isGlobal (static_cast<ParamId> (i)))
            continue;
        const auto key = toJuce (kParamSpecs[i].id);
        auto* param = state_.getParameter (key);
        const auto* raw = state_.getRawParameterValue (key);
        // Untouched parameters are left alone, so they keep the exact value they were loaded
        // with (the processor's state then saves the loaded text again, byte for byte).
        if (param == nullptr || raw == nullptr || juce::exactlyEqual (raw->load(), s.raw[i]))
            continue;
        param->setValueNotifyingHost (normalisedFor (*param, s.raw[i], s.normalised[i]));
    }
    currentId_ = s.id;
    currentName_ = s.name;
    loadedValues_ = s.loadedValues;
    return true;
}

void PresetManager::snapshot (Slot slot)
{
    syncGeneration();
    slots_[static_cast<std::size_t> (slot)] = capture();
}

bool PresetManager::restore (Slot slot)
{
    syncGeneration();
    const auto& s = slots_[static_cast<std::size_t> (slot)];
    return s.has_value() && restore (*s);
}

void PresetManager::compareSwitch()
{
    syncGeneration();
    auto live = capture();
    if (!compareOther_)
        compareOther_ = live;
    if (!restore (*compareOther_))
        return;
    compareOther_ = std::move (live);
    compareIsB_ = !compareIsB_;
}

void PresetManager::compareCopy()
{
    syncGeneration();
    compareOther_ = capture();
}

// -------------------------------------------------------------------------------------------------

bool PresetManager::isDirty() const
{
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        const auto id = static_cast<ParamId> (i);
        if (isGlobal (id))
            continue;
        const auto& spec = kParamSpecs[i];
        const auto* raw = state_.getRawParameterValue (toJuce (spec.id));
        if (raw == nullptr)
            continue;
        const auto it = loadedValues_.find (id);
        const float reference = it != loadedValues_.end() ? it->second : spec.defaultValue;
        if (!sameValue (raw->load(), reference, spec))
            return true;
    }
    return false;
}

void PresetManager::restoreCurrent (const juce::String& id, const juce::String& name)
{
    currentId_ = id;
    currentName_ = name.isNotEmpty() ? name : juce::String ("Init");
    loadedValues_.clear();
    if (const auto* e = find (id))
        if (const auto parsed = read (*e))
            for (std::size_t i = 0; i < kNumParams; ++i)
            {
                const auto key = juce::Identifier (toJuce (kParamSpecs[i].id));
                const auto& params = (*parsed)["parameters"];
                loadedValues_[static_cast<ParamId> (i)] = params.hasProperty (key)
                                                              ? static_cast<float> (params[key])
                                                              : kParamSpecs[i].defaultValue;
            }
}

juce::var PresetManager::stateAsVar()
{
    syncGeneration();
    juce::Array<juce::var> items;
    for (const auto& e : entries_)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", e.id);
        o->setProperty ("name", e.name);
        o->setProperty ("category", e.category);
        o->setProperty ("folder", e.folder);
        o->setProperty ("author", e.author);
        o->setProperty ("tags", toVarArray (e.tags));
        o->setProperty ("favorite", favorites_.contains (e.id));
        o->setProperty ("description", e.description);
        o->setProperty ("amp", e.amp);
        o->setProperty ("cab", e.cab);
        o->setProperty ("blocks", toVarArray (e.blocks));
        items.add (juce::var (o));
    }

    juce::var other;
    if (compareOther_)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", compareOther_->name);
        o->setProperty ("dirty", compareOther_->dirty);
        other = juce::var (o);
    }
    auto* compare = new juce::DynamicObject();
    compare->setProperty ("active", compareIsB_ ? "B" : "A");
    compare->setProperty ("other", other);

    auto* root = new juce::DynamicObject();
    root->setProperty ("items", items);
    root->setProperty ("currentId", currentId_.isEmpty() ? juce::var() : juce::var (currentId_));
    root->setProperty ("currentName", currentName_);
    root->setProperty ("dirty", isDirty());
    root->setProperty ("recent", toVarArray (recent_));
    root->setProperty ("compare", juce::var (compare));
    root->setProperty ("revision", ++revision_);
    return juce::var (root);
}

void PresetManager::loadSettings()
{
    const auto parsed = juce::JSON::parse (settingsFile());
    const auto list = [&parsed] (const char* key) {
        juce::StringArray strings;
        if (const auto* a = parsed[key].getArray())
            for (const auto& v : *a)
                strings.add (v.toString());
        return strings;
    };
    favorites_ = list ("favorites");
    recent_ = list ("recent");
    recent_.removeRange (kMaxRecent, recent_.size());
}

void PresetManager::storeSettings() const
{
    auto parsed = juce::JSON::parse (settingsFile());
    if (!parsed.isObject())
        parsed = juce::var (new juce::DynamicObject());
    parsed.getDynamicObject()->setProperty ("favorites", toVarArray (favorites_));
    parsed.getDynamicObject()->setProperty ("recent", toVarArray (recent_));
    settingsFile().getParentDirectory().createDirectory();
    settingsFile().replaceWithText (juce::JSON::toString (parsed));
}

} // namespace sedef::plugin
