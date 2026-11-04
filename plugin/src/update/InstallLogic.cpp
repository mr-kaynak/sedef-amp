// SPDX-License-Identifier: AGPL-3.0-or-later
#include "InstallLogic.h"

#include <algorithm>
#include <cstring>
#include <set>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif
#if !JUCE_WINDOWS
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sedef::update
{
namespace
{
juce::File home()
{
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
}

/// A path relative to `root`: "Library/Audio" under "/" is "/Library/Audio".
juce::File under (const juce::File& root, const char* relative)
{
    return root.getChildFile (relative);
}

juce::var plistValue (const juce::XmlElement& e)
{
    const auto tag = e.getTagName();
    if (tag == "dict")
    {
        auto* o = new juce::DynamicObject();
        const juce::var result (o);
        for (const auto* key = e.getFirstChildElement(); key != nullptr;)
        {
            const auto* value = key->getNextElement();
            if (key->getTagName() != "key" || value == nullptr)
                break;
            o->setProperty (key->getAllSubText(), plistValue (*value));
            key = value->getNextElement();
        }
        return result;
    }
    if (tag == "array")
    {
        juce::Array<juce::var> items;
        for (const auto* child : e.getChildIterator())
            items.add (plistValue (*child));
        return items;
    }
    if (tag == "string" || tag == "date" || tag == "data")
        return e.getAllSubText();
    if (tag == "integer")
        return e.getAllSubText().trim().getLargeIntValue();
    if (tag == "real")
        return e.getAllSubText().trim().getDoubleValue();
    if (tag == "true" || tag == "false")
        return tag == "true";
    return {};
}

#if JUCE_MAC
/// A binary property list's CFBundleShortVersionString (Info.plist files may be binary).
juce::String binaryPlistString (const juce::MemoryBlock& data, const char* key)
{
    const auto bytes = CFDataCreate (nullptr, static_cast<const UInt8*> (data.getData()),
                                     static_cast<CFIndex> (data.getSize()));
    if (bytes == nullptr)
        return {};
    juce::String out;
    if (const auto list = CFPropertyListCreateWithData (nullptr, bytes, kCFPropertyListImmutable,
                                                        nullptr, nullptr))
    {
        const auto name = CFStringCreateWithCString (nullptr, key, kCFStringEncodingUTF8);
        if (CFGetTypeID (list) == CFDictionaryGetTypeID() && name != nullptr)
            if (const auto value = CFDictionaryGetValue (static_cast<CFDictionaryRef> (list), name);
                value != nullptr && CFGetTypeID (value) == CFStringGetTypeID())
            {
                char buffer[256] = {};
                if (CFStringGetCString (static_cast<CFStringRef> (value), buffer, sizeof (buffer),
                                        kCFStringEncodingUTF8))
                    out = juce::String::fromUTF8 (buffer);
            }
        if (name != nullptr)
            CFRelease (name);
        CFRelease (list);
    }
    CFRelease (bytes);
    return out;
}
#endif

// Tar ---------------------------------------------------------------------------------------------

constexpr int kBlock = 512;
constexpr juce::int64 kMaxMetaBytes = 64 * 1024; // a long name or pax header

int readFully (juce::InputStream& in, void* buffer, int bytes)
{
    int done = 0;
    while (done < bytes)
    {
        const int n = in.read (static_cast<char*> (buffer) + done, bytes - done);
        if (n <= 0)
            break;
        done += n;
    }
    return done;
}

bool skip (juce::InputStream& in, juce::int64 bytes)
{
    char scratch[8192];
    while (bytes > 0)
    {
        const int chunk = static_cast<int> (juce::jmin<juce::int64> (bytes, sizeof (scratch)));
        if (readFully (in, scratch, chunk) != chunk)
            return false;
        bytes -= chunk;
    }
    return true;
}

juce::int64 padding (juce::int64 size)
{
    return (kBlock - size % kBlock) % kBlock;
}

/// An octal header field (space or NUL terminated), or base-256 when the top bit is set. -1 when
/// malformed or negative.
juce::int64 numberField (const unsigned char* field, int length)
{
    if ((field[0] & 0x80) != 0)
    {
        if ((field[0] & 0x40) != 0)
            return -1; // negative
        juce::int64 value = field[0] & 0x3f;
        for (int i = 1; i < length; ++i)
        {
            if (value > (juce::int64 { 1 } << 54))
                return -1;
            value = (value << 8) | field[i];
        }
        return value;
    }
    int i = 0;
    while (i < length && (field[i] == ' ' || field[i] == 0))
        ++i;
    juce::int64 value = 0;
    for (; i < length && field[i] != ' ' && field[i] != 0; ++i)
    {
        if (field[i] < '0' || field[i] > '7' || value > (juce::int64 { 1 } << 58))
            return -1;
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

juce::String textField (const unsigned char* field, int length)
{
    int n = 0;
    while (n < length && field[n] != 0)
        ++n;
    return juce::String::fromUTF8 (reinterpret_cast<const char*> (field), n);
}

bool checksumMatches (const unsigned char* header)
{
    const auto stored = numberField (header + 148, 8);
    juce::int64 unsignedSum = 0, signedSum = 0;
    for (int i = 0; i < kBlock; ++i)
    {
        const bool inField = i >= 148 && i < 156;
        unsignedSum += inField ? ' ' : header[i];
        signedSum += inField ? ' ' : static_cast<signed char> (header[i]);
    }
    return stored >= 0 && (stored == unsignedSum || stored == signedSum);
}

TarEntry::Type typeOf (char flag)
{
    switch (flag)
    {
    case '0':
    case '\0':
    case '7': // contiguous file: a regular file to everyone but one old system
        return TarEntry::Type::File;
    case '5':
        return TarEntry::Type::Directory;
    case '1':
        return TarEntry::Type::Hardlink;
    case '2':
        return TarEntry::Type::Symlink;
    case '3':
    case '4':
        return TarEntry::Type::Device;
    case '6':
        return TarEntry::Type::Fifo;
    default:
        return TarEntry::Type::Other;
    }
}

/// Reads exactly `length` bytes of an entry, no further; the rest is skipped by the caller.
class EntryStream final : public juce::InputStream
{
public:
    EntryStream (juce::InputStream& source, juce::int64 length) : source_ (source), left_ (length)
    {
        total_ = length;
    }
    juce::int64 getTotalLength() override { return total_; }
    bool isExhausted() override { return left_ <= 0; }
    juce::int64 getPosition() override { return total_ - left_; }
    bool setPosition (juce::int64) override { return false; }
    int read (void* buffer, int bytes) override
    {
        const int n = static_cast<int> (juce::jmin<juce::int64> (bytes, left_));
        const int got = n > 0 ? readFully (source_, buffer, n) : 0;
        left_ -= got;
        return got;
    }
    juce::int64 remaining() const noexcept { return left_; }

private:
    juce::InputStream& source_;
    juce::int64 total_ = 0, left_ = 0;
};

/// Calls `visit` for every entry with a stream over its data, and returns an error, or empty when
/// the archive was read to its end. Long names and pax paths apply to the entry that follows them.
template <typename Visit>
juce::String forEachEntry (juce::InputStream& in, Visit&& visit)
{
    juce::String longName, paxPath;
    juce::int64 unpacked = 0;
    int count = 0;
    for (bool first = true;; first = false)
    {
        unsigned char header[kBlock];
        const int got = readFully (in, header, kBlock);
        if (got == 0 && first)
            return "the file is not a gzip-compressed tar archive";
        if (got == 0)
            return {}; // no end blocks: tolerated
        if (got != kBlock)
            return "the archive ends inside a header";
        if (std::all_of (header, header + kBlock, [] (unsigned char c) { return c == 0; }))
            return {};
        if (!checksumMatches (header))
            return "a header's checksum does not match";

        const auto size = numberField (header + 124, 12);
        if (size < 0)
            return "a header has no valid size";
        const char flag = static_cast<char> (header[156]);
        const bool posix = std::memcmp (header + 257, "ustar\0", 6) == 0;

        if (flag == 'L' || flag == 'K' || flag == 'x' || flag == 'g')
        {
            if (size > kMaxMetaBytes)
                return "a long name or extended header is too large";
            juce::MemoryBlock data (static_cast<size_t> (size));
            if (readFully (in, data.getData(), static_cast<int> (size)) != size ||
                !skip (in, padding (size)))
                return "the archive ends inside an extended header";
            if (flag == 'L')
                longName = textField (static_cast<const unsigned char*> (data.getData()),
                                      static_cast<int> (size));
            if (flag == 'x')
            {
                // Records are "<length> <key>=<value>\n", the length counting the whole record.
                const auto* p = static_cast<const char*> (data.getData());
                juce::int64 at = 0;
                while (at < size)
                {
                    juce::int64 length = 0, i = at;
                    while (i < size && p[i] >= '0' && p[i] <= '9' && length < size)
                        length = length * 10 + (p[i++] - '0');
                    if (i >= size || p[i] != ' ' || length <= i - at || at + length > size ||
                        p[at + length - 1] != '\n')
                        return "an extended header is malformed";
                    const auto record = juce::String::fromUTF8 (
                        p + i + 1, static_cast<int> (at + length - 1 - (i + 1)));
                    const auto key = record.upToFirstOccurrenceOf ("=", false, false);
                    if (key == "path")
                        paxPath = record.fromFirstOccurrenceOf ("=", false, false);
                    else if (key == "size")
                        return "an extended header overrides a size";
                    at += length;
                }
            }
            continue;
        }

        if (++count > kMaxTarEntries)
            return "the archive has too many entries";
        TarEntry entry;
        entry.type = typeOf (flag);
        entry.size = entry.type == TarEntry::Type::File ? size : 0;
        entry.mode = static_cast<int> (juce::jmax<juce::int64> (0, numberField (header + 100, 8)));
        auto name = textField (header, 100);
        if (posix && header[345] != 0)
            name = textField (header + 345, 155) + "/" + name;
        entry.path = paxPath.isNotEmpty() ? paxPath : (longName.isNotEmpty() ? longName : name);
        paxPath.clear();
        longName.clear();
        unpacked += entry.size;
        if (unpacked > kMaxUnpackedBytes)
            return "the archive unpacks to more than 1 GiB";

        EntryStream data (in, size);
        if (const auto error = visit (entry, data); error.isNotEmpty())
            return error;
        if (!skip (in, data.remaining() + padding (size)))
            return "the archive ends inside " + entry.path;
    }
}

/// "./a/b/" -> "a/b"; empty for the archive's own "./".
juce::String normalisedPath (juce::String path)
{
    while (path.startsWith ("./"))
        path = path.substring (2);
    while (path.endsWithChar ('/'))
        path = path.dropLastCharacters (1);
    return path;
}

/// Why `path` (normalised) may not be written, or empty when it may.
juce::String pathProblem (const juce::String& path)
{
    if (path.startsWithChar ('/'))
        return path + " is an absolute path";
    for (auto p = path.getCharPointer(); !p.isEmpty(); ++p)
        if (*p < ' ' || *p == 0x7f || *p == '\\' || *p == ':')
            return "a path holds a control character, a colon or a backslash";
    juce::StringArray parts;
    parts.addTokens (path, "/", {});
    for (const auto& part : parts)
        if (part.isEmpty() || part == "." || part == "..")
            return path + " leaves its folder or has an empty part";
    return {};
}

juce::String typeProblem (const TarEntry& e)
{
    switch (e.type)
    {
    case TarEntry::Type::File:
    case TarEntry::Type::Directory:
        return {};
    case TarEntry::Type::Symlink:
        return e.path + " is a symbolic link";
    case TarEntry::Type::Hardlink:
        return e.path + " is a hard link";
    case TarEntry::Type::Device:
        return e.path + " is a device";
    case TarEntry::Type::Fifo:
        return e.path + " is a pipe";
    case TarEntry::Type::Other:
        break;
    }
    return e.path + " has a type Sedef does not unpack";
}

bool isPlainName (const juce::String& name)
{
    return name.isNotEmpty() && name.length() <= 128 && !name.startsWithChar ('.') &&
           name.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-");
}

// Files install.sh needs (tools/packaging/linux/install.sh), and the one Sedef verifies with.
constexpr const char* kLinuxLayout[] = {
    "install.sh",          "uninstall.sh",
    "CLAP/Sedef Amp.clap", "Standalone/Sedef Amp",
    "sedef-amp.svg",       "sedef-amp.png",
    "sedef-amp.desktop",   "VST3/Sedef Amp.vst3/Contents/Resources/moduleinfo.json",
};
} // namespace

// The private cache -------------------------------------------------------------------------------

juce::File updateCacheFolder()
{
#if JUCE_MAC
    return home().getChildFile ("Library/Caches/Sedef/Updates");
#elif JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
        .getChildFile ("Sedef")
        .getChildFile ("Updates");
#else
    auto cache = juce::SystemStats::getEnvironmentVariable ("XDG_CACHE_HOME", {});
    if (!juce::File::isAbsolutePath (cache))
        cache = home().getChildFile (".cache").getFullPathName();
    return juce::File (cache).getChildFile ("sedef").getChildFile ("updates");
#endif
}

bool makePrivateFolder (const juce::File& folder)
{
    if (!folder.createDirectory())
        return false;
#if JUCE_WINDOWS
    return folder.isDirectory();
#else
    struct stat info {};
    const auto path = folder.getFullPathName().toRawUTF8();
    if (lstat (path, &info) != 0 || !S_ISDIR (info.st_mode) || info.st_uid != getuid())
        return false;
    return (info.st_mode & 0777) == 0700 || chmod (path, 0700) == 0;
#endif
}

juce::File realPath (const juce::File& file)
{
#if JUCE_WINDOWS
    return file;
#else
    char buffer[PATH_MAX] = {};
    return realpath (file.getFullPathName().toRawUTF8(), buffer) != nullptr
               ? juce::File (juce::String::fromUTF8 (buffer))
               : file;
#endif
}

// macOS -------------------------------------------------------------------------------------------

juce::File macBundlePath (const MacComponent& component, const juce::File& root)
{
    return under (root, component.location).getChildFile (component.bundle);
}

juce::String macChoiceFor (const juce::File& runningBundle, const juce::File& root)
{
    if (runningBundle == juce::File())
        return {};
    for (const auto& c : kMacComponents)
        if (runningBundle == macBundlePath (c, root))
            return c.choice;
    return {};
}

juce::StringArray macChoicesToInstall (const juce::File& root, const juce::File& runningBundle)
{
    const auto running = macChoiceFor (runningBundle, root);
    juce::StringArray out;
    for (const auto& c : kMacComponents)
        if (macBundlePath (c, root).isDirectory() || running == c.choice)
            out.add (c.choice);
    if (out.isEmpty())
        for (const auto& c : kMacComponents)
            out.add (c.choice);
    return out;
}

juce::String choiceChangesXml (const juce::StringArray& selected)
{
    juce::String xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<array>\n";
    for (const auto& c : kMacComponents)
        xml << "\t<dict>\n"
               "\t\t<key>attributeSetting</key>\n"
               "\t\t<integer>"
            << (selected.contains (c.choice) ? 1 : 0)
            << "</integer>\n"
               "\t\t<key>choiceAttribute</key>\n"
               "\t\t<string>selected</string>\n"
               "\t\t<key>choiceIdentifier</key>\n"
               "\t\t<string>"
            << c.choice << "</string>\n\t</dict>\n";
    xml << "</array>\n</plist>\n";
    return xml;
}

juce::var parsePlist (const juce::String& xml)
{
    const auto doc = juce::XmlDocument::parse (xml);
    if (doc == nullptr || !doc->hasTagName ("plist"))
        return {};
    const auto* first = doc->getFirstChildElement();
    return first != nullptr ? plistValue (*first) : juce::var();
}

juce::String parseMountPoint (const juce::String& plist)
{
    juce::StringArray points;
    const auto parsed = parsePlist (plist); // keeps the arrays below alive
    if (const auto* entities = parsed["system-entities"].getArray())
        for (const auto& e : *entities)
            if (const auto point = e["mount-point"].toString(); point.isNotEmpty())
                points.add (point);
    return points.size() == 1 ? points[0] : juce::String();
}

juce::StringArray mountPointsUnder (const juce::String& infoPlist, const juce::File& folder)
{
    juce::StringArray points;
    const auto parsed = parsePlist (infoPlist); // keeps the arrays below alive
    if (const auto* images = parsed["images"].getArray())
        for (const auto& image : *images)
            if (const auto* entities = image["system-entities"].getArray())
                for (const auto& e : *entities)
                    if (const auto point = e["mount-point"].toString();
                        juce::File::isAbsolutePath (point) &&
                        juce::File (point).isAChildOf (folder))
                        points.addIfNotAlreadyThere (point);
    return points;
}

juce::String bundleShortVersion (const juce::File& bundle)
{
    juce::MemoryBlock data;
    if (!bundle.getChildFile ("Contents/Info.plist").loadFileAsData (data) || data.getSize() == 0)
        return {};
    if (data.getSize() >= 6 && std::memcmp (data.getData(), "bplist", 6) == 0)
    {
#if JUCE_MAC
        return binaryPlistString (data, "CFBundleShortVersionString");
#else
        return {};
#endif
    }
    return parsePlist (data.toString())["CFBundleShortVersionString"].toString().trim();
}

juce::StringArray osascriptArgv (const juce::StringArray& command, const juce::String& prompt)
{
    // Item 1 is the prompt, the rest is the command. `quoted form of` single-quotes each item
    // for /bin/sh (a ' inside becomes '\''), so no argument can end its quotes.
    juce::StringArray argv { "/usr/bin/osascript",
                             "-e",
                             "on run argv",
                             "-e",
                             "set command to \"\"",
                             "-e",
                             "repeat with i from 2 to count of argv",
                             "-e",
                             "set command to command & \" \" & quoted form of (item i of argv)",
                             "-e",
                             "end repeat",
                             "-e",
                             "do shell script command with prompt (item 1 of argv) with "
                             "administrator privileges",
                             "-e",
                             "end run",
                             "--",
                             prompt };
    argv.addArray (command);
    return argv;
}

std::optional<int> appleScriptErrorNumber (const juce::String& errorOutput)
{
    const auto text = errorOutput.trim();
    if (!text.endsWithChar (')'))
        return std::nullopt;
    const auto number = text.fromLastOccurrenceOf ("(", false, false).dropLastCharacters (1);
    const auto digits = number.startsWithChar ('-') ? number.substring (1) : number;
    if (digits.isEmpty() || digits.length() > 9 || !digits.containsOnly ("0123456789"))
        return std::nullopt;
    return number.getIntValue();
}

juce::StringArray installerArgv (const juce::File& pkg, const juce::File& choices)
{
    return { "/usr/sbin/installer",
             "-pkg",
             pkg.getFullPathName(),
             "-applyChoiceChangesXML",
             choices.getFullPathName(),
             "-target",
             "/" };
}

juce::String installPrompt (const juce::String& version)
{
    return "Sedef Amp wants to install version " + version + ".";
}

// Windows -----------------------------------------------------------------------------------------

juce::String windowsSetupParameters (juce::uint32 waitPid, bool relaunch)
{
    juce::String p (
        "/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010");
    p << " /WAITPID=" << juce::String (waitPid);
    if (relaunch)
        p << " /RELAUNCH";
    return p;
}

bool windowsSetupUpdates (const juce::File& runningModule, const juce::File& commonFiles)
{
    if (runningModule.getFileName().equalsIgnoreCase ("Sedef Amp.exe"))
        return true; // the standalone app: Setup reuses the folder it was installed to
    if (commonFiles == juce::File())
        return false;
    return runningModule.isAChildOf (
               commonFiles.getChildFile ("VST3").getChildFile ("Sedef Amp.vst3")) ||
           runningModule == commonFiles.getChildFile ("CLAP").getChildFile ("Sedef Amp.clap");
}

// Linux -------------------------------------------------------------------------------------------

TarListing listTarGz (juce::InputStream& gzipped)
{
    TarListing listing;
    juce::GZIPDecompressorInputStream in (&gzipped, false,
                                          juce::GZIPDecompressorInputStream::gzipFormat);
    listing.error = forEachEntry (in, [&listing] (const TarEntry& e, juce::InputStream&) {
        listing.entries.push_back (e);
        return juce::String();
    });
    return listing;
}

TarListing listTarGz (const juce::File& archive)
{
    juce::FileInputStream file (archive);
    if (!file.openedOk())
        return { "cannot read " + archive.getFileName(), {} };
    juce::BufferedInputStream buffered (file, 1 << 16);
    return listTarGz (buffered);
}

juce::String checkLinuxArchive (const std::vector<TarEntry>& entries, juce::String& error)
{
    const auto fail = [&error] (juce::String why) {
        error = std::move (why);
        return juce::String();
    };
    juce::String top;
    std::set<juce::String> files, folders;
    juce::int64 total = 0;
    for (const auto& e : entries)
    {
        if (const auto problem = typeProblem (e); problem.isNotEmpty())
            return fail (problem);
        const auto path = normalisedPath (e.path);
        if (path.isEmpty())
        {
            if (e.type == TarEntry::Type::Directory)
                continue; // "./"
            return fail ("an entry has no name");
        }
        if (const auto problem = pathProblem (path); problem.isNotEmpty())
            return fail (problem);
        const auto first = path.upToFirstOccurrenceOf ("/", false, false);
        if (top.isEmpty())
            top = first;
        else if (first != top)
            return fail ("the archive has more than one top folder");
        if (path == top && e.type != TarEntry::Type::Directory)
            return fail (top + " is not a folder");
        auto& set = e.type == TarEntry::Type::Directory ? folders : files;
        if (files.count (path) > 0 || folders.count (path) > 0)
            return fail (path + " is listed twice");
        set.insert (path);
        total += e.size;
    }
    if (top.isEmpty())
        return fail ("the archive is empty");
    if (!isPlainName (top))
        return fail ("the top folder's name is unexpected");
    if (total > kMaxUnpackedBytes)
        return fail ("the archive unpacks to more than 1 GiB");
    for (const auto* item : kLinuxLayout)
        if (files.count (top + "/" + item) == 0)
            return fail (juce::String ("the archive has no ") + item);
    error.clear();
    return top;
}

bool extractTarGz (const juce::File& archive, const juce::File& destination, juce::String& error,
                   juce::String* top)
{
    const auto listing = listTarGz (archive);
    if (listing.error.isNotEmpty())
    {
        error = listing.error;
        return false;
    }
    const auto folder = checkLinuxArchive (listing.entries, error);
    if (folder.isEmpty())
        return false;
    if (top != nullptr)
        *top = folder;

    juce::FileInputStream file (archive);
    if (!file.openedOk())
    {
        error = "cannot read " + archive.getFileName();
        return false;
    }
    juce::BufferedInputStream buffered (file, 1 << 16);
    juce::GZIPDecompressorInputStream in (&buffered, false,
                                          juce::GZIPDecompressorInputStream::gzipFormat);
    size_t index = 0;
    error = forEachEntry (in, [&] (const TarEntry& e, juce::InputStream& data) -> juce::String {
        // The same entries as the listing that was checked, or nothing is written.
        if (index >= listing.entries.size() || listing.entries[index].path != e.path ||
            listing.entries[index].type != e.type || listing.entries[index].size != e.size)
            return "the archive changed while it was unpacked";
        ++index;
        const auto path = normalisedPath (e.path);
        if (path.isEmpty())
            return {};
        if (const auto problem = typeProblem (e) + pathProblem (path); problem.isNotEmpty())
            return problem;
        const auto target = destination.getChildFile (path);
        if (!target.isAChildOf (destination))
            return path + " leaves its folder";
        if (e.type == TarEntry::Type::Directory)
            return target.createDirectory() ? juce::String() : "cannot create " + path;
        if (target.exists() || !target.getParentDirectory().createDirectory())
            return "cannot create " + path;
        {
            juce::FileOutputStream out (target);
            if (!out.openedOk() || out.writeFromInputStream (data, -1) != e.size)
                return "cannot write " + path;
            out.flush();
            if (out.getStatus().failed())
                return "cannot write " + path;
        }
#if !JUCE_WINDOWS
        // Windows files have no execute bit (JUCE reports failure there), and the archive is
        // only installed on Linux; elsewhere it is unpacked for the tests.
        if ((e.mode & 0111) != 0 && !target.setExecutePermission (true))
            return "cannot make " + path + " executable";
#endif
        return {};
    });
    if (error.isEmpty() && index != listing.entries.size())
        error = "the archive changed while it was unpacked";
    return error.isEmpty();
}

LinuxScope linuxScopeOf (const juce::File& running, const juce::File& homeFolder)
{
    const auto in = [&running] (const juce::File& vst3, const juce::File& clap,
                                const juce::File& bin) {
        return running.isAChildOf (vst3.getChildFile ("Sedef Amp.vst3")) ||
               running == clap.getChildFile ("Sedef Amp.clap") ||
               running == bin.getChildFile ("sedef-amp");
    };
    if (homeFolder != juce::File() &&
        in (homeFolder.getChildFile (".vst3"), homeFolder.getChildFile (".clap"),
            homeFolder.getChildFile (".local/bin")))
        return LinuxScope::User;
    if (in (juce::File ("/usr/lib/vst3"), juce::File ("/usr/lib/clap"),
            juce::File ("/usr/local/bin")))
        return LinuxScope::System;
    return LinuxScope::Elsewhere;
}

juce::File linuxVst3Bundle (LinuxScope scope, const juce::File& homeFolder)
{
    return (scope == LinuxScope::System ? juce::File ("/usr/lib/vst3")
                                        : homeFolder.getChildFile (".vst3"))
        .getChildFile ("Sedef Amp.vst3");
}

juce::File linuxStandalone (LinuxScope scope, const juce::File& homeFolder)
{
    return (scope == LinuxScope::System ? juce::File ("/usr/local/bin")
                                        : homeFolder.getChildFile (".local/bin"))
        .getChildFile ("sedef-amp");
}

juce::String moduleInfoVersion (const juce::String& json)
{
    if (const auto parsed = juce::JSON::parse (json); parsed.getDynamicObject() != nullptr)
        return parsed["Version"].toString().trim();
    // moduleinfo.json may be JSON5 (trailing commas, comments): find the key by hand.
    const int key = json.indexOf ("\"Version\"");
    if (key < 0)
        return {};
    const auto rest = json.substring (key + 9).trimStart();
    if (!rest.startsWithChar (':'))
        return {};
    const auto value = rest.substring (1).trimStart();
    if (!value.startsWithChar ('"'))
        return {};
    return value.substring (1).upToFirstOccurrenceOf ("\"", false, false).trim();
}

juce::String vst3BundleVersion (const juce::File& bundle)
{
    const auto file = bundle.getChildFile ("Contents/Resources/moduleinfo.json");
    return file.getSize() > 0 && file.getSize() < (1 << 20)
               ? moduleInfoVersion (file.loadFileAsString())
               : juce::String();
}

// Restarting the standalone app -------------------------------------------------------------------

juce::StringArray waitThenRunArgv (int pid, const juce::StringArray& command)
{
    juce::StringArray argv { "/bin/sh", "-c",
                             "n=0; while kill -0 \"$1\" 2>/dev/null && [ \"$n\" -lt 600 ]; do "
                             "sleep 0.1; n=$((n+1)); done; shift; exec \"$@\"",
                             "sh", juce::String (pid) };
    argv.addArray (command);
    return argv;
}

} // namespace sedef::update
