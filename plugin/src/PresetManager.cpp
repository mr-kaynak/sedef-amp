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
        ParamId::presetNext,      ParamId::captureInterfaceLevel,
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

juce::String PresetManager::safeFileName (const juce::String& name)
{
    return juce::File::createLegalFileName (name.trim())
        .trimCharactersAtStart (". ")
        .trimCharactersAtEnd (". ");
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
    sync();
    loadSettings(); // favourites and Recent another instance changed show up too
    entries_.clear();

    for (int i = 0; i < SedefPresetData::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data =
            SedefPresetData::getNamedResource (SedefPresetData::namedResourceList[i], size);
        const auto json = juce::String::fromUTF8 (data, size);
        const auto parsed = juce::JSON::parse (json);
        if (!parsed.isObject() || !parsed["parameters"].isObject())
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
        if (!parsed.isObject() || !parsed["parameters"].isObject())
            continue; // not a preset load() could apply
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
    add (ParamId::trenchEnabled, "trench");
    add (ParamId::tideEnabled, "tide");
    add (ParamId::urchinEnabled, "urchin");
    add (ParamId::pearlEnabled, "pearl");
    add (ParamId::kelpEnabled, "kelp");
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

float PresetManager::storedValue (const juce::var& parameters, std::size_t i)
{
    const auto& spec = kParamSpecs[i];
    const juce::Identifier key (toJuce (spec.id));
    return spec.sanitise (parameters.hasProperty (key) ? static_cast<float> (parameters[key])
                                                       : spec.defaultValue);
}

void PresetManager::setWithGesture (juce::RangedAudioParameter& param, float normalised)
{
    // Each changed parameter is its own gesture, as if it had been turned: hosts writing
    // automation (touch, latch, write) record a preset change as steps on exactly the parameters
    // it moved, and VST3 hosts get their edits inside begin/end as the spec asks. One gesture
    // around the whole load would open every parameter's lane at once.
    param.beginChangeGesture();
    param.setValueNotifyingHost (normalised);
    param.endChangeGesture();
}

void PresetManager::apply (const juce::var& parameters)
{
    const auto lock = lockParameters();
    loadedValues_.clear();
    for (std::size_t i = 0; i < kNumParams; ++i)
    {
        const auto id = static_cast<ParamId> (i);
        if (isGlobal (id))
            continue;
        const float clean = storedValue (parameters, i);
        const float normalised = kParamSpecs[i].toNormalised (clean);
        // Only what changes: an unchanged parameter would still reach the host as an edit.
        if (auto* param = state_.getParameter (toJuce (kParamSpecs[i].id));
            param != nullptr && !juce::exactlyEqual (param->getValue(), normalised))
            setWithGesture (*param, normalised);
        loadedValues_[id] = clean;
    }
}

bool PresetManager::load (const juce::String& id, bool audition)
{
    sync();
    const auto* e = find (id);
    if (e == nullptr)
        return false;
    const auto parsed = read (*e);
    if (!parsed)
        return false;
    apply ((*parsed)["parameters"]);
    currentId_ = e->id;
    currentName_ = e->name;
    loadedAttachment_ =
        attachment_.key.isNotEmpty() ? (*parsed)[attachment_.key.toRawUTF8()] : juce::var();
    if (attachment_.select)
        attachment_.select (loadedAttachment_, false);
    if (!audition)
        markRecent (currentId_);
    return true;
}

void PresetManager::markRecent (const juce::String& id)
{
    if (find (id) == nullptr)
        return;
    updateSettings ([this, &id] {
        recent_.removeString (id);
        recent_.insert (0, id);
        recent_.removeRange (kMaxRecent, recent_.size());
    });
}

void PresetManager::loadInit()
{
    sync();
    apply (juce::var (new juce::DynamicObject()));
    currentId_ = {};
    currentName_ = "Init";
    loadedAttachment_ = juce::var();
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
    sync();
    const auto cleanName = name.trim().isEmpty() ? juce::String ("Untitled") : name.trim();
    auto dir = userFolder();
    // One level below Presets/ at most: a folder name can't climb out of it ("..") or hide.
    if (const auto folderName = safeFileName (folder); folderName.isNotEmpty())
        dir = dir.getChildFile (folderName);
    dir.createDirectory();
    auto stem = safeFileName (cleanName);
    if (stem.isEmpty())
        stem = "Untitled";
    const auto file = dir.getChildFile (stem + kExtension);

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
    const auto attached = attachment_.reference ? attachment_.reference (true) : juce::var();
    if (!attached.isVoid())
        root->setProperty (attachment_.key, attached);
    if (!file.replaceWithText (juce::JSON::toString (juce::var (root))))
        return {};
    loadedAttachment_ = attached;

    scan();
    currentId_ = userId (file);
    currentName_ = cleanName;
    // The saved state is the new reference for "dirty".
    if (const auto* e = find (currentId_))
        if (const auto parsed = read (*e))
            for (std::size_t i = 0; i < kNumParams; ++i)
                if (!isGlobal (static_cast<ParamId> (i)))
                    loadedValues_[static_cast<ParamId> (i)] =
                        storedValue ((*parsed)["parameters"], i);
    markRecent (currentId_);
    return currentId_;
}

bool PresetManager::remove (const juce::String& id)
{
    sync();
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
    sync();
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
    sync();
    const auto* e = find (id);
    const auto name = newName.trim();
    if (e == nullptr || e->category != "User" || name.isEmpty())
        return false;
    auto parsed = read (*e);
    if (!parsed)
        return false;
    if (auto* obj = parsed->getDynamicObject())
        obj->setProperty ("name", name);
    const auto text = juce::JSON::toString (*parsed);
    const auto source = e->file;
    auto stem = safeFileName (name);
    if (stem.isEmpty())
        stem = "Untitled";
    const auto target = source.getSiblingFile (stem + kExtension);

    // juce::File compares names the way the platform's file system usually does: without case on
    // macOS and Windows. The same file under another spelling is a case-only rename.
    if (target == source)
    {
        if (!source.replaceWithText (text))
            return false;
        // Writing in place keeps the old spelling on disk; a move changes only the case.
        if (target.getFullPathName() != source.getFullPathName() && !source.moveFileTo (target))
            return false;
    }
    else
    {
        if (target.exists())
            return false; // never over another preset's file
        if (!target.replaceWithText (text))
            return false;
        if (!source.deleteFile())
        {
            target.deleteFile(); // one copy, under the old name, rather than two
            return false;
        }
    }

    scan();
    // The file now carries the new name's spelling, so this is the id scan() just found.
    const auto newId = userId (target);
    if (currentId_ == id)
    {
        currentId_ = newId;
        currentName_ = name;
    }
    // Snapshots and the other A/B slot follow the preset to its new name.
    for (auto& slot : slots_)
        if (slot && slot->id == id)
        {
            slot->id = newId;
            slot->name = name;
        }
    if (compareOther_ && compareOther_->id == id)
    {
        compareOther_->id = newId;
        compareOther_->name = name;
    }

    // Favourites and Recent follow it too, without listing it twice.
    if (newId != id)
        updateSettings ([this, &id, &newId] {
            for (auto* list : { &favorites_, &recent_ })
            {
                if (const int i = list->indexOf (id); i >= 0)
                    list->set (i, newId);
                list->removeDuplicates (false);
            }
        });
    return true;
}

void PresetManager::setFavorite (const juce::String& id, bool favorite)
{
    updateSettings ([this, &id, favorite] {
        if (favorite)
            favorites_.addIfNotAlreadyThere (id);
        else
            favorites_.removeString (id);
    });
}

void PresetManager::step (int delta)
{
    sync();
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

void PresetManager::hostRestored (const juce::String& id, const juce::String& name)
{
    {
        const std::scoped_lock lock (hostLock_);
        hostRestore_ = HostRestore { id, name };
    }
    generation_.fetch_add (1);
}

bool PresetManager::hostRestorePending() const
{
    const std::scoped_lock lock (hostLock_);
    return hostRestore_.has_value();
}

void PresetManager::sync()
{
    std::optional<HostRestore> pending;
    {
        const std::scoped_lock lock (hostLock_);
        pending.swap (hostRestore_);
    }
    const int generation = generation_.load();
    if (generation == seenGeneration_ && !pending)
        return;
    seenGeneration_ = generation;
    // Taken before the restore (or before its bookkeeping arrived, so naming the old preset).
    for (auto& s : slots_)
        s.reset();
    compareOther_.reset();
    compareIsB_ = false;
    if (pending)
        applyCurrent (pending->id, pending->name);
}

PresetManager::Snapshot PresetManager::capture() const
{
    Snapshot s;
    // A host restore replaces the values and bumps the generation under this lock, so the values
    // and the generation read here belong together.
    const std::scoped_lock lock (parametersLock_);
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
    s.attachment = attachment_.reference ? attachment_.reference (false) : juce::var();
    s.loadedAttachment = loadedAttachment_;
    return s;
}

bool PresetManager::restore (const Snapshot& s)
{
    // Under the lock a host restore can't land halfway through, so checking the generation once
    // here covers the whole loop.
    const auto lock = lockParameters();
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
        setWithGesture (*param, normalisedFor (*param, s.raw[i], s.normalised[i]));
    }
    // A preset deleted since leaves its sound behind, no longer a preset.
    currentId_ = find (s.id) != nullptr ? s.id : juce::String();
    currentName_ = s.name;
    loadedValues_ = s.loadedValues;
    loadedAttachment_ = s.loadedAttachment;
    if (attachment_.select)
        attachment_.select (s.attachment, true);
    return true;
}

void PresetManager::snapshot (Slot slot)
{
    sync();
    slots_[static_cast<std::size_t> (slot)] = capture();
}

bool PresetManager::restore (Slot slot)
{
    sync();
    const auto& s = slots_[static_cast<std::size_t> (slot)];
    return s.has_value() && restore (*s);
}

void PresetManager::compareSwitch()
{
    sync();
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
    sync();
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
    return attachment_.differs && attachment_.differs (loadedAttachment_);
}

void PresetManager::applyCurrent (const juce::String& id, const juce::String& name)
{
    currentId_ = id;
    currentName_ = name.isNotEmpty() ? name : juce::String ("Init");
    // "Dirty" compares against the preset as a load would apply it; Init (or a preset this
    // machine doesn't have) against the defaults.
    loadedValues_.clear();
    loadedAttachment_ = juce::var();
    if (const auto* e = find (id))
        if (const auto parsed = read (*e))
        {
            for (std::size_t i = 0; i < kNumParams; ++i)
                if (!isGlobal (static_cast<ParamId> (i)))
                    loadedValues_[static_cast<ParamId> (i)] =
                        storedValue ((*parsed)["parameters"], i);
            if (attachment_.key.isNotEmpty())
                loadedAttachment_ = (*parsed)[attachment_.key.toRawUTF8()];
        }
}

juce::var PresetManager::stateAsVar()
{
    sync();
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

bool PresetManager::loadSettings()
{
    const auto file = settingsFile();
    if (!file.existsAsFile())
    {
        favorites_.clear(); // nothing stored yet (or the file was removed to start over)
        recent_.clear();
        return true;
    }
    // A file that is there but doesn't parse must not read as "no favourites": writing that
    // back would wipe them.
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    if (!parsed.isObject())
        return false;
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
    return true;
}

void PresetManager::updateSettings (const std::function<void()>& change)
{
    // Other processes hold it for a read and a write at most, so a short wait is plenty; the
    // message thread must not hang on a stuck one.
    const bool locked = settingsLock_.enter (500);
    const bool read = locked && loadSettings();
    change();
    if (read)
        storeSettings();
    if (locked)
        settingsLock_.exit();
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
