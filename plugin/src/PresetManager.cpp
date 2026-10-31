// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PresetManager.h"

#include "ParameterLayout.h"
#include "SedefPresetData.h"

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
} // namespace

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state) : state_ (state)
{
    loadFavorites();
    scan();
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
        if (auto* tags = parsed["tags"].getArray())
            for (const auto& t : *tags)
                e.tags.add (t.toString());
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
        e.id = "user:" + f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
        e.category = "User";
        e.folder =
            f.getParentDirectory() == root ? juce::String() : f.getParentDirectory().getFileName();
        e.author = parsed["author"].toString();
        if (auto* tags = parsed["tags"].getArray())
            for (const auto& t : *tags)
                e.tags.add (t.toString());
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

bool PresetManager::load (const juce::String& id)
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
    return true;
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

juce::String PresetManager::save (const juce::String& name, const juce::String& folder)
{
    const auto cleanName = name.trim().isEmpty() ? juce::String ("Untitled") : name.trim();
    auto dir = userFolder();
    if (folder.trim().isNotEmpty())
        dir = dir.getChildFile (juce::File::createLegalFileName (folder.trim()));
    dir.createDirectory();

    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "sedef-preset");
    root->setProperty ("version", 1);
    root->setProperty ("name", cleanName);
    root->setProperty ("author", juce::SystemStats::getFullUserName());
    root->setProperty ("tags", juce::Array<juce::var>());
    root->setProperty ("parameters", captureParameters());

    const auto file = dir.getChildFile (juce::File::createLegalFileName (cleanName) + kExtension);
    file.replaceWithText (juce::JSON::toString (juce::var (root)));

    scan();
    currentId_ = "user:" + file.getRelativePathFrom (userFolder()).replaceCharacter ('\\', '/');
    currentName_ = cleanName;
    // The saved state is the new reference for "dirty".
    if (const auto* e = find (currentId_))
        if (const auto parsed = read (*e))
            for (std::size_t i = 0; i < kNumParams; ++i)
                if (!isGlobal (static_cast<ParamId> (i)))
                    loadedValues_[static_cast<ParamId> (i)] = static_cast<float> (
                        (*parsed)["parameters"][juce::Identifier (toJuce (kParamSpecs[i].id))]);
    return currentId_;
}

bool PresetManager::remove (const juce::String& id)
{
    const auto* e = find (id);
    if (e == nullptr || e->category != "User")
        return false;
    const bool ok = e->file.moveToTrash() || e->file.deleteFile();
    if (ok && currentId_ == id)
        currentId_ = {};
    scan();
    return ok;
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
    if (wasCurrent)
    {
        currentId_ =
            "user:" + target.getRelativePathFrom (userFolder()).replaceCharacter ('\\', '/');
        currentName_ = newName.trim();
    }
    return true;
}

void PresetManager::setFavorite (const juce::String& id, bool favorite)
{
    if (favorite)
        favorites_.addIfNotAlreadyThere (id);
    else
        favorites_.removeString (id);
    storeFavorites();
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
    load (entries_[static_cast<size_t> (next)].id);
}

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

juce::var PresetManager::stateAsVar() const
{
    juce::Array<juce::var> items;
    for (const auto& e : entries_)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", e.id);
        o->setProperty ("name", e.name);
        o->setProperty ("category", e.category);
        o->setProperty ("folder", e.folder);
        o->setProperty ("author", e.author);
        juce::Array<juce::var> tags;
        for (const auto& t : e.tags)
            tags.add (t);
        o->setProperty ("tags", tags);
        o->setProperty ("favorite", favorites_.contains (e.id));
        items.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("items", items);
    root->setProperty ("currentId", currentId_.isEmpty() ? juce::var() : juce::var (currentId_));
    root->setProperty ("currentName", currentName_);
    root->setProperty ("dirty", isDirty());
    return juce::var (root);
}

void PresetManager::loadFavorites()
{
    const auto parsed = juce::JSON::parse (settingsFile());
    favorites_.clear();
    if (auto* list = parsed["favorites"].getArray())
        for (const auto& f : *list)
            favorites_.add (f.toString());
}

void PresetManager::storeFavorites() const
{
    auto parsed = juce::JSON::parse (settingsFile());
    if (!parsed.isObject())
        parsed = juce::var (new juce::DynamicObject());
    juce::Array<juce::var> list;
    for (const auto& f : favorites_)
        list.add (f);
    parsed.getDynamicObject()->setProperty ("favorites", list);
    settingsFile().getParentDirectory().createDirectory();
    settingsFile().replaceWithText (juce::JSON::toString (parsed));
}

} // namespace sedef::plugin
