// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CaptureLibrary.h"

#include "sedef/dsp/capture/CaptureStage.h"

#include <cmath>
#include <juce_cryptography/juce_cryptography.h>

namespace sedef::plugin
{
namespace
{
constexpr auto kMaxBytes = dsp::CaptureFile::kMaxBytes;

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        o->setProperty (k, v);
    return juce::var (o);
}

juce::var orVoid (const std::string& s)
{
    return s.empty() ? juce::var() : juce::var (juce::String::fromUTF8 (s.c_str()));
}

juce::var orVoid (const std::optional<double>& v)
{
    return v ? juce::var (*v) : juce::var();
}

/// A dropped file's name made safe to keep: the last path component, no characters file systems
/// refuse, no leading dots, a .nam extension.
juce::String safeFileName (const juce::String& name)
{
    auto stem =
        name.fromLastOccurrenceOf ("/", false, false)
            .fromLastOccurrenceOf ("\\", false, false)
            .upToLastOccurrenceOf (".nam", false, true)
            .removeCharacters ("<>:\"/\\|?*")
            .retainCharacters (juce::String ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                             "0123456789 ()[]_-+.,'&#@!"))
            .trim();
    while (stem.startsWithChar ('.'))
        stem = stem.substring (1);
    stem = stem.trimCharactersAtEnd (". ").substring (0, 80).trim();
    return (stem.isEmpty() ? juce::String ("Capture") : stem) + ".nam";
}

std::optional<std::string> readFile (const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "Couldn't find " + file.getFileName() + ".";
        return std::nullopt;
    }
    if (file.getSize() > static_cast<juce::int64> (kMaxBytes))
    {
        error = file.getFileName() + " is larger than 24 MB, so it isn't a capture Sedef can play.";
        return std::nullopt;
    }
    juce::MemoryBlock data;
    if (!file.loadFileAsData (data))
    {
        error = "Couldn't read " + file.getFileName() + ".";
        return std::nullopt;
    }
    return std::string (static_cast<const char*> (data.getData()), data.getSize());
}

juce::String encode (const std::string& bytes)
{
    juce::MemoryOutputStream compressed;
    {
        juce::GZIPCompressorOutputStream gzip (compressed, 9);
        gzip.write (bytes.data(), bytes.size());
    }
    return juce::Base64::toBase64 (compressed.getData(), compressed.getDataSize());
}

std::optional<std::string> decode (const juce::String& base64)
{
    juce::MemoryOutputStream compressed;
    if (base64.isEmpty() || !juce::Base64::convertFromBase64 (compressed, base64))
        return std::nullopt;
    juce::MemoryInputStream in (compressed.getData(), compressed.getDataSize(), false);
    juce::GZIPDecompressorInputStream gzip (in);
    std::string bytes;
    char buffer[16384];
    for (;;)
    {
        const int n = gzip.read (buffer, sizeof (buffer));
        if (n <= 0)
            break;
        if (bytes.size() + static_cast<std::size_t> (n) > kMaxBytes)
            return std::nullopt;
        bytes.append (buffer, static_cast<std::size_t> (n));
    }
    if (bytes.empty())
        return std::nullopt;
    return bytes;
}

juce::String kindName (dsp::CaptureKind k)
{
    switch (k)
    {
    case dsp::CaptureKind::AmpOnly:
        return "amp";
    case dsp::CaptureKind::FullRig:
        return "fullRig";
    case dsp::CaptureKind::Unknown:
        break;
    }
    return "unknown";
}
} // namespace

CaptureLibrary::CaptureLibrary (Engine& engine, std::mutex& engineLock, juce::File folder)
    : engine_ (engine),
      engineLock_ (engineLock),
      folder_ (std::move (folder))
{
}

CaptureLibrary::~CaptureLibrary()
{
    worker_.removeAllJobs (true, 10000);
}

juce::String CaptureLibrary::hashOf (const std::string& bytes)
{
    return juce::SHA256 (bytes.data(), bytes.size()).toHexString();
}

juce::File CaptureLibrary::droppedFolder() const
{
    return folder_.getChildFile ("Captures");
}

juce::File CaptureLibrary::recentFile() const
{
    return folder_.getChildFile ("captures.json");
}

juce::String CaptureLibrary::displayName (const dsp::CaptureInfo& info,
                                          const juce::String& fileName)
{
    if (!info.name.empty())
        return juce::String::fromUTF8 (info.name.c_str());
    const auto stem = fileName.upToLastOccurrenceOf (".", false, false);
    return stem.isNotEmpty() ? stem : fileName;
}

// Requests
// -----------------------------------------------------------------------------------------

std::uint64_t CaptureLibrary::request (const juce::var& pendingRef)
{
    const std::scoped_lock lock (lock_);
    pending_ = pendingRef;
    return ++generation_;
}

bool CaptureLibrary::isNewest (std::uint64_t generation) const
{
    const std::scoped_lock lock (lock_);
    return generation == generation_;
}

void CaptureLibrary::finish (std::uint64_t generation, const juce::String& error)
{
    const std::scoped_lock lock (lock_);
    if (generation != generation_)
        return; // a newer request took over; it reports for itself
    error_ = error;
    pending_ = juce::var();
    changed();
}

void CaptureLibrary::changed()
{
    ++revision;
}

bool CaptureLibrary::isLoading() const
{
    const std::scoped_lock lock (lock_);
    return inFlight_ > 0 && !pending_.isVoid();
}

int CaptureLibrary::jobsInFlight() const
{
    const std::scoped_lock lock (lock_);
    return inFlight_;
}

void CaptureLibrary::loadAsync (std::function<juce::String()> job,
                                std::function<void (const juce::String&)> done)
{
    {
        const std::scoped_lock lock (lock_);
        ++inFlight_;
    }
    changed();
    worker_.addJob ([this, work = std::move (job), onDone = std::move (done)] {
        const auto error = work();
        {
            const std::scoped_lock lock (lock_);
            --inFlight_;
        }
        changed();
        if (onDone)
            juce::MessageManager::callAsync ([onDone, error] { onDone (error); });
    });
}

// Loading
// ------------------------------------------------------------------------------------------

juce::String CaptureLibrary::loadFile (const juce::File& file)
{
    return loadPath (file, true, request (object ({ { "path", file.getFullPathName() } })));
}

void CaptureLibrary::loadFileAsync (const juce::File& file,
                                    std::function<void (const juce::String&)> done)
{
    const auto generation = request (object ({ { "path", file.getFullPathName() } }));
    loadAsync ([this, file, generation] { return loadPath (file, true, generation); },
               std::move (done));
}

juce::String CaptureLibrary::loadPath (const juce::File& file, bool rememberIt,
                                       std::uint64_t generation)
{
    juce::String error;
    auto bytes = readFile (file, error);
    if (!bytes)
    {
        finish (generation, error);
        return error;
    }
    return loadBytes (file.getFileName(), std::make_shared<const std::string> (std::move (*bytes)),
                      file, std::nullopt, rememberIt, generation);
}

juce::String CaptureLibrary::loadDropped (const juce::String& fileName, const std::string& bytes)
{
    return loadDropped (fileName, bytes, request (object ({ { "fileName", fileName } })));
}

juce::String CaptureLibrary::loadDropped (const juce::String& fileName, const std::string& bytes,
                                          std::uint64_t generation)
{
    if (!isNewest (generation))
        return {};
    if (bytes.size() > kMaxBytes)
    {
        const juce::String error =
            "This file is larger than 24 MB, so it isn't a capture Sedef can play.";
        finish (generation, error);
        return error;
    }
    // Validate before keeping anything: a file that isn't a capture is not copied.
    auto shared = std::make_shared<const std::string> (bytes);
    auto parsed = dsp::CaptureFile::parse (*shared);
    if (parsed.file == nullptr)
    {
        const auto error = juce::String::fromUTF8 (parsed.error.c_str());
        finish (generation, error);
        return error;
    }

    // Kept under Captures/: the same file again reuses its copy, a different one with the same
    // name gets a number.
    const auto folder = droppedFolder();
    folder.createDirectory();
    const auto name = safeFileName (fileName);
    const auto sha = hashOf (bytes);
    auto target = folder.getChildFile (name);
    for (int n = 2; target.existsAsFile(); ++n)
    {
        juce::String ignored;
        if (const auto existing = readFile (target, ignored); existing && hashOf (*existing) == sha)
            break;
        target = folder.getChildFile (name.upToLastOccurrenceOf (".nam", false, true) + " (" +
                                      juce::String (n) + ").nam");
    }
    if (!target.existsAsFile() && !target.replaceWithData (bytes.data(), bytes.size()))
        target = juce::File(); // still plays, just not by path
    return loadBytes (target != juce::File() ? target.getFileName() : safeFileName (fileName),
                      std::move (shared), target, parsed.file->normalisationDb(), true, generation);
}

void CaptureLibrary::loadDroppedAsync (const juce::String& fileName, std::string bytes,
                                       std::function<void (const juce::String&)> done)
{
    const auto generation = request (object ({ { "fileName", fileName } }));
    loadAsync ([this, name = fileName, data = std::move (bytes),
                generation] { return loadDropped (name, data, generation); },
               std::move (done));
}

juce::String CaptureLibrary::loadBytes (const juce::String& fileName,
                                        std::shared_ptr<const std::string> bytes,
                                        const juce::File& path,
                                        std::optional<float> knownNormalisationDb, bool rememberIt,
                                        std::uint64_t generation,
                                        const juce::ValueTree& restoredState)
{
    if (!isNewest (generation))
        return {}; // superseded before it started
    const auto sha = hashOf (*bytes);
    Entry entry;
    if (auto hit = cached (sha))
        entry = std::move (*hit);
    else
    {
        auto parsed = dsp::CaptureFile::parse (*bytes, knownNormalisationDb);
        if (parsed.file == nullptr)
        {
            const auto error = juce::String::fromUTF8 (parsed.error.c_str());
            finish (generation, error);
            return error;
        }
        entry.file = std::move (parsed.file);
        entry.bytes = std::move (bytes);
        entry.sha256 = sha;
    }
    entry.fileName = fileName;
    entry.path = path;

    const std::scoped_lock lock (lock_);
    if (generation != generation_)
        return {}; // a newer load, clear or restore came in while this one was building
    if (const auto error = install (entry.file); error.isNotEmpty())
    {
        finish (generation, error);
        return error;
    }
    makeCurrent (entry, restoredState);
    remember (entry);
    finish (generation, {});
    if (rememberIt && path != juce::File())
    {
        const Recent r { displayName (entry.file->info(), fileName), fileName,
                         path.getFullPathName(), sha };
        writeRecent ([&r] (std::vector<Recent>& list) {
            std::erase_if (
                list, [&r] (const Recent& x) { return x.path == r.path || x.sha256 == r.sha256; });
            list.insert (list.begin(), r);
            if (list.size() > static_cast<std::size_t> (kMaxRecent))
                list.resize (static_cast<std::size_t> (kMaxRecent));
        });
    }
    return {};
}

juce::String CaptureLibrary::install (const std::shared_ptr<const dsp::CaptureFile>& file)
{
    // Under the engine lock, so prepare() can't change the spec between building and handing
    // over. Every chain gets its own instance (each keeps its own history).
    const std::scoped_lock lock (engineLock_);
    std::vector<std::unique_ptr<dsp::CaptureModel>> models;
    for (int c = 0; c < engine_.numCabs(); ++c)
    {
        if (file == nullptr)
        {
            models.push_back (nullptr);
            continue;
        }
        std::string error;
        auto model = file->instantiate (engine_.capture (c).modelSpec(), &error);
        if (model == nullptr)
            return juce::String::fromUTF8 (error.c_str());
        models.push_back (std::move (model));
    }
    for (int c = 0; c < engine_.numCabs(); ++c)
        engine_.capture (c).setModel (std::move (models[static_cast<std::size_t> (c)]));
    return {};
}

juce::ValueTree CaptureLibrary::makeState (const Entry& e)
{
    juce::ValueTree t ("Capture");
    t.setProperty ("name", displayName (e.file->info(), e.fileName), nullptr);
    t.setProperty ("fileName", e.fileName, nullptr);
    t.setProperty ("path", e.path != juce::File() ? e.path.getFullPathName() : juce::String(),
                   nullptr);
    t.setProperty ("sha256", e.sha256, nullptr);
    // Enough digits to read back the same float, so a restored session plays bit for bit.
    t.setProperty ("normalisationDb",
                   juce::String (static_cast<double> (e.file->normalisationDb()), 9), nullptr);
    t.setProperty ("data", encode (*e.bytes), nullptr);
    return t;
}

void CaptureLibrary::makeCurrent (Entry entry, const juce::ValueTree& restoredState)
{
    const std::scoped_lock lock (lock_);
    currentState_ = restoredState.isValid() ? restoredState.createCopy() : makeState (entry);
    current_ = std::move (entry);
    missing_ = juce::var();
    missingState_ = {};
    changed();
}

void CaptureLibrary::clear()
{
    const std::scoped_lock lock (lock_);
    request();
    install (nullptr);
    current_.reset();
    currentState_ = {};
    missing_ = juce::var();
    missingState_ = {};
    error_ = juce::String();
    changed();
}

void CaptureLibrary::rebuild()
{
    const std::scoped_lock lock (lock_);
    if (current_)
        if (const auto error = install (current_->file); error.isNotEmpty())
        {
            // Only below 16 kHz: the stage plays the built-in amp until the rate changes again.
            error_ = error;
            changed();
        }
}

void CaptureLibrary::collectGarbage()
{
    const std::scoped_lock lock (engineLock_);
    for (int c = 0; c < engine_.numCabs(); ++c)
        engine_.capture (c).collectGarbage();
}

// Cache
// --------------------------------------------------------------------------------------------

std::optional<CaptureLibrary::Entry> CaptureLibrary::cached (const juce::String& sha256) const
{
    const std::scoped_lock lock (lock_);
    for (const auto& e : cache_)
        if (e.sha256 == sha256)
            return e;
    return std::nullopt;
}

void CaptureLibrary::remember (const Entry& e)
{
    const std::scoped_lock lock (lock_);
    std::erase_if (cache_, [&e] (const Entry& x) { return x.sha256 == e.sha256; });
    cache_.insert (cache_.begin(), e);
    if (cache_.size() > static_cast<std::size_t> (kCacheSize))
        cache_.resize (static_cast<std::size_t> (kCacheSize));
}

// Session state
// ------------------------------------------------------------------------------------

juce::ValueTree CaptureLibrary::toState() const
{
    const std::scoped_lock lock (lock_);
    if (current_)
        return currentState_.createCopy();
    return missingState_.isValid() ? missingState_.createCopy() : juce::ValueTree();
}

void CaptureLibrary::restoreState (const juce::ValueTree& tree)
{
    if (!tree.isValid() || !tree.hasType ("Capture"))
    {
        clear();
        return;
    }
    const auto sha = tree.getProperty ("sha256").toString();
    const auto ref = object ({ { "name", tree.getProperty ("name") },
                               { "fileName", tree.getProperty ("fileName") },
                               { "path", tree.getProperty ("path") },
                               { "sha256", sha } });
    {
        const std::scoped_lock lock (lock_);
        if (current_ && current_->sha256 == sha)
        {
            request(); // whatever was loading belongs to the state being replaced
            currentState_ = tree.createCopy();
            changed();
            return;
        }
    }
    const auto generation = request (ref);
    const auto fileName = tree.getProperty ("fileName").toString();
    const juce::File path (tree.getProperty ("path").toString());
    juce::String ignored;

    // The file inside the session, if it is intact: then the level measured when it was first
    // loaded is trusted and the restore skips measuring again.
    if (auto bytes = decode (tree.getProperty ("data").toString()); bytes && hashOf (*bytes) == sha)
    {
        std::optional<float> known;
        const auto level = tree.getProperty ("normalisationDb").toString();
        if (level.containsOnly ("-+.0123456789") && level.isNotEmpty())
            known = level.getFloatValue();
        const auto error =
            loadBytes (fileName, std::make_shared<const std::string> (std::move (*bytes)), path,
                       known, false, generation, tree);
        if (error.isEmpty())
            return;
    }
    else if (const auto found =
                 path.existsAsFile() && hashOf (readFile (path, ignored).value_or ("")) == sha
                     ? path
                     : findByHash (sha, fileName);
             found != juce::File())
    {
        juce::String error;
        if (auto fromDisk = readFile (found, error))
            if (loadBytes (found.getFileName(),
                           std::make_shared<const std::string> (std::move (*fromDisk)), found,
                           std::nullopt, false, generation)
                    .isEmpty())
                return;
    }

    const std::scoped_lock lock (lock_);
    if (generation != generation_)
        return;
    install (nullptr);
    current_.reset();
    currentState_ = {};
    markMissing (ref, tree);
    pending_ = juce::var();
}

void CaptureLibrary::markMissing (const juce::var& ref, const juce::ValueTree& state)
{
    const std::scoped_lock lock (lock_);
    missing_ = ref;
    missingState_ = state.isValid() ? state.createCopy() : juce::ValueTree();
    if (!missingState_.isValid() && ref.isObject())
    {
        // A preset's reference: kept in the session so it can still be found later.
        missingState_ = juce::ValueTree ("Capture");
        for (const auto* key : { "name", "fileName", "path", "sha256" })
            missingState_.setProperty (key, ref[key].toString(), nullptr);
    }
    changed();
}

juce::File CaptureLibrary::findByHash (const juce::String& sha256,
                                       const juce::String& fileName) const
{
    // The recent list first, then a dropped copy kept under Captures/ with the same name.
    if (sha256.isEmpty())
        return {};
    for (const auto& r : readRecent())
        if (r.sha256 == sha256)
            if (const juce::File f (r.path); f.existsAsFile())
                return f;
    if (fileName.isNotEmpty())
        if (const auto f = droppedFolder().getChildFile (safeFileName (fileName)); f.existsAsFile())
        {
            juce::String ignored;
            if (const auto bytes = readFile (f, ignored); bytes && hashOf (*bytes) == sha256)
                return f;
        }
    return {};
}

// Presets
// ------------------------------------------------------------------------------------------

juce::var CaptureLibrary::reference() const
{
    const std::scoped_lock lock (lock_);
    if (!current_)
        return missing_.isObject() ? missing_.clone() : juce::var(); // still wanted: keep asking
    return object ({ { "name", displayName (current_->file->info(), current_->fileName) },
                     { "fileName", current_->fileName },
                     { "path", current_->path != juce::File() ? current_->path.getFullPathName()
                                                              : juce::String() },
                     { "sha256", current_->sha256 } });
}

void CaptureLibrary::select (const juce::var& ref, bool exact)
{
    if (!ref.isObject())
    {
        if (exact)
            clear();
        return;
    }
    const auto sha = ref["sha256"].toString();
    const auto fileName = ref["fileName"].toString();
    const juce::File path (ref["path"].toString());
    {
        const std::scoped_lock lock (lock_);
        if (current_ && current_->sha256 == sha)
        {
            request(); // drops a load in flight for something else
            pending_ = juce::var();
            changed();
            return;
        }
    }

    // Built earlier this session: instantly, as A/B and undo need.
    if (auto hit = cached (sha))
    {
        const auto generation = request (ref);
        const std::scoped_lock lock (lock_);
        if (const auto error = install (hit->file); error.isEmpty())
        {
            makeCurrent (*hit, {});
            remember (*hit);
            finish (generation, {});
            return;
        }
    }

    // From disk on the worker: the referenced path, else a recent or kept copy with the hash.
    const auto generation = request (ref);
    loadAsync (
        [this, ref, sha, fileName, path, generation] {
            juce::File file = path.existsAsFile() ? path : findByHash (sha, fileName);
            if (file == juce::File())
            {
                const std::scoped_lock lock (lock_);
                if (generation == generation_)
                {
                    install (nullptr);
                    current_.reset();
                    currentState_ = {};
                    markMissing (ref, {});
                    pending_ = juce::var();
                }
                return juce::String();
            }
            return loadPath (file, false, generation);
        },
        {});
}

bool CaptureLibrary::differs (const juce::var& ref) const
{
    if (!ref.isObject())
        return false;
    const auto sha = ref["sha256"].toString();
    const std::scoped_lock lock (lock_);
    if (current_)
        return current_->sha256 != sha &&
               !(pending_.isObject() && pending_["sha256"].toString() == sha);
    if (missing_.isObject() && missing_["sha256"].toString() == sha)
        return false;
    return !(pending_.isObject() && pending_["sha256"].toString() == sha);
}

std::optional<bool> CaptureLibrary::suggestedCab() const
{
    const std::scoped_lock lock (lock_);
    if (!current_)
        return std::nullopt;
    switch (current_->file->info().kind)
    {
    case dsp::CaptureKind::FullRig:
        return false;
    case dsp::CaptureKind::AmpOnly:
        return true;
    case dsp::CaptureKind::Unknown:
        break;
    }
    return std::nullopt;
}

juce::String CaptureLibrary::currentHash() const
{
    const std::scoped_lock lock (lock_);
    return current_ ? current_->sha256 : juce::String();
}

// Bridge
// -------------------------------------------------------------------------------------------

juce::var CaptureLibrary::stateAsVar() const
{
    const std::scoped_lock lock (lock_);
    juce::var loaded;
    if (current_)
    {
        const auto& info = current_->file->info();
        juce::StringArray gear;
        if (!info.gearMake.empty())
            gear.add (juce::String::fromUTF8 (info.gearMake.c_str()));
        if (!info.gearModel.empty())
            gear.add (juce::String::fromUTF8 (info.gearModel.c_str()));
        loaded = object (
            { { "name", displayName (info, current_->fileName) },
              { "fileName", current_->fileName },
              { "gear", gear.isEmpty() ? juce::var() : juce::var (gear.joinIntoString (" ")) },
              { "gearType", orVoid (info.gearType) },
              { "toneType", orVoid (info.toneType) },
              { "author", orVoid (info.modeledBy) },
              { "architecture", juce::String (info.architecture) },
              { "sampleRate", info.sampleRateKnown ? juce::var (info.sampleRate) : juce::var() },
              { "kind", kindName (info.kind) },
              { "inputLevelDbu", orVoid (info.inputLevelDbu) },
              { "normalisationDb", current_->file->normalisationDb() },
              { "path", current_->path != juce::File()
                            ? juce::var (current_->path.getFullPathName())
                            : juce::var() } });
    }
    juce::var missing;
    if (missing_.isObject())
        missing = object ({ { "name", missing_["name"] },
                            { "fileName", missing_["fileName"] },
                            { "path", missing_["path"] } });

    juce::Array<juce::var> recent;
    for (const auto& r : readRecent())
        if (juce::File (r.path).existsAsFile())
            recent.add (object ({ { "name", r.name },
                                  { "fileName", r.fileName },
                                  { "path", r.path },
                                  { "current", current_ && current_->sha256 == r.sha256 } }));

    return object ({ { "loaded", loaded },
                     { "loading", inFlight_ > 0 && !pending_.isVoid() },
                     { "error", error_.isEmpty() ? juce::var() : juce::var (error_) },
                     { "missing", missing },
                     { "recent", recent } });
}

// Recent list
// --------------------------------------------------------------------------------------

std::vector<CaptureLibrary::Recent> CaptureLibrary::readRecent() const
{
    std::vector<Recent> list;
    const auto parsed = juce::JSON::parse (recentFile());
    if (const auto* items = parsed["recent"].getArray())
        for (const auto& item : *items)
        {
            Recent r { item["name"].toString().substring (0, 120),
                       item["fileName"].toString().substring (0, 120), item["path"].toString(),
                       item["sha256"].toString() };
            if (r.path.isNotEmpty() && juce::File::isAbsolutePath (r.path) &&
                list.size() < kMaxRecent)
                list.push_back (std::move (r));
        }
    return list;
}

void CaptureLibrary::writeRecent (const std::function<void (std::vector<Recent>&)>& change)
{
    // Other instances (another host, the standalone app) share the file.
    const juce::InterProcessLock::ScopedLockType lock (recentLock_);
    if (!lock.isLocked())
        return;
    auto list = readRecent();
    change (list);
    juce::Array<juce::var> items;
    for (const auto& r : list)
        items.add (object ({ { "name", r.name },
                             { "fileName", r.fileName },
                             { "path", r.path },
                             { "sha256", r.sha256 } }));
    folder_.createDirectory();
    recentFile().replaceWithText (juce::JSON::toString (object ({ { "recent", items } })));
    changed();
}

} // namespace sedef::plugin
