// SPDX-License-Identifier: AGPL-3.0-or-later
// The installer's logic (plugin/src/update/InstallLogic.h) and, where the OS allows it without a
// password, its real steps: mounting and detaching a disk image with hdiutil (macOS), unpacking an
// archive and running its install.sh (macOS and Linux), with a scripted stand-in for the prompt.
#include "Installer.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if !JUCE_WINDOWS
#include <unistd.h>
#endif

using namespace sedef::update;

namespace
{
juce::File scratch()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("sedef-install-tests-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory());
    return dir;
}

struct Scratch
{
    juce::File dir = scratch();
    ~Scratch() { dir.deleteRecursively(); }
};

/// Writes Contents/Info.plist with `version`, as a JUCE bundle has it.
void makeBundle (const juce::File& bundle, const juce::String& version)
{
    REQUIRE (bundle.getChildFile ("Contents").createDirectory());
    REQUIRE (bundle.getChildFile ("Contents/Info.plist")
                 .replaceWithText ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                   "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                   "<plist version=\"1.0\"><dict>\n"
                                   "<key>CFBundleName</key><string>Sedef Amp</string>\n"
                                   "<key>CFBundleShortVersionString</key><string>" +
                                   version + "</string>\n</dict></plist>\n"));
}

bool isExecutable (const juce::File& file)
{
#if JUCE_WINDOWS
    juce::ignoreUnused (file);
    return true;
#else
    return access (file.getFullPathName().toRawUTF8(), X_OK) == 0;
#endif
}

/// Builds a tar archive by hand, header by header, so tests can write what tar(1) never would.
class TarBuilder
{
public:
    enum Magic
    {
        Posix,
        Gnu
    };

    void file (const juce::String& name, const juce::String& content, int mode = 0644)
    {
        header (name, '0', static_cast<juce::int64> (content.getNumBytesAsUTF8()), mode);
        data (content.toRawUTF8(), content.getNumBytesAsUTF8());
    }
    void folder (const juce::String& name) { header (name, '5', 0, 0755); }
    void link (const juce::String& name, char type, const juce::String& target)
    {
        header (name, type, 0, 0777, target);
    }
    void special (const juce::String& name, char type) { header (name, type, 0, 0644); }
    void longName (const juce::String& name)
    {
        const auto bytes = name.getNumBytesAsUTF8() + 1; // with its NUL, as GNU tar writes it
        header ("././@LongLink", 'L', static_cast<juce::int64> (bytes), 0644, {}, Gnu);
        data (name.toRawUTF8(), bytes);
    }
    void paxPath (const juce::String& path)
    {
        // "<length> path=<path>\n", the length counting its own digits.
        const auto body = " path=" + path + "\n";
        const int bodyBytes = static_cast<int> (body.getNumBytesAsUTF8());
        int total = bodyBytes + 1;
        while (juce::String (total).length() + bodyBytes != total)
            total = juce::String (total).length() + bodyBytes;
        const auto record = juce::String (total) + body;
        header ("PaxHeader", 'x', static_cast<juce::int64> (record.getNumBytesAsUTF8()), 0644);
        data (record.toRawUTF8(), record.getNumBytesAsUTF8());
    }
    void header (const juce::String& name, char type, juce::int64 size, int mode,
                 const juce::String& linkName = {}, Magic magic = Posix,
                 const juce::String& prefix = {}, bool corrupt = false)
    {
        char h[512] = {};
        std::memcpy (h, name.toRawUTF8(), juce::jmin<size_t> (100, name.getNumBytesAsUTF8()));
        std::snprintf (h + 100, 8, "%07o", static_cast<unsigned> (mode));
        std::snprintf (h + 108, 8, "%07o", 0u);
        std::snprintf (h + 116, 8, "%07o", 0u);
        std::snprintf (h + 124, 12, "%011llo", static_cast<unsigned long long> (size));
        std::snprintf (h + 136, 12, "%011o", 0u);
        std::memset (h + 148, ' ', 8);
        h[156] = type;
        std::memcpy (h + 157, linkName.toRawUTF8(),
                     juce::jmin<size_t> (100, linkName.getNumBytesAsUTF8()));
        std::memcpy (h + 257,
                     magic == Posix ? "ustar\0"
                                      "00"
                                    : "ustar  \0",
                     8);
        std::memcpy (h + 345, prefix.toRawUTF8(),
                     juce::jmin<size_t> (155, prefix.getNumBytesAsUTF8()));
        unsigned sum = 0;
        for (const char c : h)
            sum += static_cast<unsigned char> (c);
        std::snprintf (h + 148, 8, "%06o", sum + (corrupt ? 1u : 0u));
        h[155] = ' ';
        out_.write (h, sizeof (h));
    }
    void data (const void* bytes, size_t size)
    {
        out_.write (bytes, size);
        const char zeros[512] = {};
        out_.write (zeros, (512 - size % 512) % 512);
    }
    juce::MemoryBlock gzipped (bool finish = true) const
    {
        juce::MemoryBlock tar (out_.getData(), out_.getDataSize());
        if (finish)
            tar.append (juce::MemoryBlock (1024, true).getData(), 1024);
        juce::MemoryOutputStream result;
        {
            juce::GZIPCompressorOutputStream gz (result, 6,
                                                 juce::GZIPCompressorOutputStream::windowBitsGZIP);
            gz.write (tar.getData(), tar.getSize());
        }
        return result.getMemoryBlock();
    }
    juce::File save (const juce::File& file) const
    {
        const auto bytes = gzipped();
        REQUIRE (file.replaceWithData (bytes.getData(), bytes.getSize()));
        return file;
    }

private:
    juce::MemoryOutputStream out_;
};

constexpr const char* kTop = "sedef-amp-0.2.0-linux-x86_64";

/// The layout tools/packaging/linux/package.sh writes, with `script` as install.sh.
void linuxLayout (TarBuilder& t, const juce::String& script = "#!/bin/sh\nexit 0\n")
{
    const juce::String top (kTop);
    t.folder (top + "/");
    t.folder (top + "/VST3/");
    t.folder (top + "/VST3/Sedef Amp.vst3/");
    t.folder (top + "/VST3/Sedef Amp.vst3/Contents/");
    t.folder (top + "/VST3/Sedef Amp.vst3/Contents/Resources/");
    t.file (top + "/VST3/Sedef Amp.vst3/Contents/Resources/moduleinfo.json",
            "{\"Name\": \"Sedef Amp\", \"Version\": \"0.2.0\"}");
    t.folder (top + "/CLAP/");
    t.file (top + "/CLAP/Sedef Amp.clap", "clap", 0755);
    t.folder (top + "/Standalone/");
    t.file (top + "/Standalone/Sedef Amp", "app", 0755);
    t.file (top + "/install.sh", script, 0755);
    t.file (top + "/uninstall.sh", "#!/bin/sh\n", 0755);
    t.file (top + "/sedef-amp.svg", "<svg/>");
    t.file (top + "/sedef-amp.png", "png");
    t.file (top + "/sedef-amp.desktop", "[Desktop Entry]\n");
}

TarListing list (const TarBuilder& t, bool finish = true)
{
    const auto bytes = t.gzipped (finish);
    juce::MemoryInputStream in (bytes, false);
    return listTarGz (in);
}

juce::String check (const TarBuilder& t)
{
    const auto listing = list (t);
    REQUIRE (listing.error.isEmpty());
    juce::String error;
    checkLinuxArchive (listing.entries, error);
    return error;
}

/// Strings no argument handling may change: quotes, shell syntax, backslashes, non-ASCII.
const juce::StringArray kNasty {
    "/tmp/a folder/with spaces",
    "it's \"quoted\"",
    "$(touch PWNED) `touch PWNED2`",
    "back\\slash \\n",
    juce::String::fromUTF8 ("\xc4\x9f\xc3\xbc\xc5\x9f\xc4\xb0 \xe6\x97\xa5\xe6\x9c\xac"),
    "semi; colon && rm -rf PWNED3 | cat",
    "-pkg",
};
} // namespace

// The cache ---------------------------------------------------------------------------------------

TEST_CASE ("The update cache is private and per user", "[update][install]")
{
    const auto cache = updateCacheFolder();
#if JUCE_MAC
    CHECK (cache.getFullPathName().endsWith ("Library/Caches/Sedef/Updates"));
#elif JUCE_WINDOWS
    CHECK (cache.getFullPathName().endsWithIgnoreCase ("\\Sedef\\Updates"));
#else
    CHECK (cache.getFullPathName().endsWith ("sedef/updates"));
#endif
    Scratch s;
    const auto folder = s.dir.getChildFile ("a/b/Updates");
    REQUIRE (makePrivateFolder (folder));
    CHECK (folder.isDirectory());
#if !JUCE_WINDOWS
    const auto stat = runProcess ({ "/bin/ls", "-ld", folder.getFullPathName() }, 5000);
    CHECK (stat.output.startsWith ("drwx------"));
    // A symbolic link where the folder should be is refused.
    const auto link = s.dir.getChildFile ("link");
    REQUIRE (s.dir.getChildFile ("elsewhere").createDirectory());
    REQUIRE (s.dir.getChildFile ("elsewhere").createSymbolicLink (link, true));
    CHECK_FALSE (makePrivateFolder (link));
#endif
}

// macOS: formats, choices, hdiutil, the prompt ----------------------------------------------------

TEST_CASE ("Formats to install are the ones installed now, plus the running one",
           "[update][install][macos]")
{
    Scratch s;
    const auto root = s.dir.getChildFile ("root");
    REQUIRE (root.createDirectory());
    // Nothing installed: all four.
    CHECK (macChoicesToInstall (root, {}) == juce::StringArray { "app", "vst3", "au", "clap" });

    makeBundle (macBundlePath (kMacComponents[1], root), "0.1.0"); // vst3
    makeBundle (macBundlePath (kMacComponents[2], root), "0.1.0"); // au
    CHECK (macChoicesToInstall (root, {}) == juce::StringArray { "vst3", "au" });

    // The CLAP this process runs from counts too.
    const auto clap = macBundlePath (kMacComponents[3], root);
    CHECK (macChoiceFor (clap, root) == "clap");
    CHECK (macChoicesToInstall (root, clap) == juce::StringArray { "vst3", "au", "clap" });

    // A copy somewhere the pkg does not write is not one of its formats.
    const auto build = s.dir.getChildFile ("build/VST3/Sedef Amp.vst3");
    CHECK (macChoiceFor (build, root).isEmpty());
    CHECK (macChoiceFor (root.getChildFile ("Library/Audio/Plug-Ins/VST3/Other.vst3"), root)
               .isEmpty());
    CHECK (macChoiceFor ({}, root).isEmpty());
    CHECK (macChoicesToInstall (root, build) == juce::StringArray { "vst3", "au" });

    // A file named like a bundle is no bundle.
    REQUIRE (macBundlePath (kMacComponents[0], root).getParentDirectory().createDirectory());
    REQUIRE (macBundlePath (kMacComponents[0], root).replaceWithText ("x"));
    CHECK (macChoicesToInstall (root, {}) == juce::StringArray { "vst3", "au" });

#if !JUCE_WINDOWS
    CHECK (macBundlePath (kMacComponents[0], juce::File ("/")).getFullPathName() ==
           "/Applications/Sedef Amp.app");
    CHECK (macBundlePath (kMacComponents[2], juce::File ("/")).getFullPathName() ==
           "/Library/Audio/Plug-Ins/Components/Sedef Amp.component");
#endif
}

TEST_CASE ("The choice changes select exactly the formats to install", "[update][install][macos]")
{
    const auto xml = choiceChangesXml ({ "vst3", "clap" });
    CHECK (xml.startsWith ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    CHECK (xml.contains ("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""));
    const auto parsed = parsePlist (xml);
    const auto* items = parsed.getArray();
    REQUIRE (items != nullptr);
    REQUIRE (items->size() == 4); // every choice, so none keeps its start_selected default
    const char* ids[] = { "app", "vst3", "au", "clap" };
    const int selected[] = { 0, 1, 0, 1 };
    for (int i = 0; i < 4; ++i)
    {
        const auto& item = (*items)[i];
        CHECK (item["choiceIdentifier"].toString() == ids[i]);
        CHECK (item["choiceAttribute"].toString() == "selected");
        CHECK (item["attributeSetting"].isInt64());
        CHECK (static_cast<int> (item["attributeSetting"]) == selected[i]);
    }
    const auto all = parsePlist (choiceChangesXml ({ "app", "vst3", "au", "clap" }));
    REQUIRE (all.getArray() != nullptr);
    for (const auto& item : *all.getArray())
        CHECK (static_cast<int> (item["attributeSetting"]) == 1);
}

TEST_CASE ("The mount point comes from hdiutil's plist", "[update][install][macos]")
{
    // What `hdiutil attach -plist` printed for an APFS image (trimmed).
    const juce::String attach = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>system-entities</key>
	<array>
		<dict>
			<key>content-hint</key><string>GUID_partition_scheme</string>
			<key>dev-entry</key><string>/dev/disk4</string>
			<key>potentially-mountable</key><false/>
		</dict>
		<dict>
			<key>content-hint</key><string>41504653-0000-11AA-AA11-00306543ECAC</string>
			<key>dev-entry</key><string>/dev/disk5s1</string>
			<key>mount-point</key><string>/Users/a b/Library/Caches/Sedef/Updates/mnt/dmg.YUFE7x</string>
			<key>potentially-mountable</key><true/>
			<key>volume-kind</key><string>apfs</string>
		</dict>
	</array>
</dict>
</plist>)";
    CHECK (parseMountPoint (attach) == "/Users/a b/Library/Caches/Sedef/Updates/mnt/dmg.YUFE7x");

    const auto plist = parsePlist (attach);
    CHECK_FALSE (static_cast<bool> (plist["system-entities"][0]["potentially-mountable"]));
    CHECK (static_cast<bool> (plist["system-entities"][1]["potentially-mountable"]));

    const auto two = attach.replace ("<key>dev-entry</key><string>/dev/disk4</string>",
                                     "<key>mount-point</key><string>/Volumes/Other</string>");
    CHECK (parseMountPoint (two).isEmpty()); // ours has one volume
    CHECK (parseMountPoint (attach.replace ("mount-point", "nothing")).isEmpty());
    CHECK (parseMountPoint ("hdiutil: attach failed - image not recognized").isEmpty());
    CHECK (parseMountPoint ({}).isEmpty());

#if !JUCE_WINDOWS
    // `hdiutil info -plist`: images an earlier run left under the cache.
    const juce::String info = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>images</key><array>
<dict><key>system-entities</key><array>
  <dict><key>mount-point</key><string>/c/Updates/mnt/dmg.1</string></dict></array></dict>
<dict><key>system-entities</key><array>
  <dict><key>mount-point</key><string>/Volumes/Someone else</string></dict></array></dict>
</array></dict></plist>)";
    CHECK (mountPointsUnder (info, juce::File ("/c/Updates/mnt")) ==
           juce::StringArray { "/c/Updates/mnt/dmg.1" });
#endif
}

TEST_CASE ("Bundle versions come from Info.plist", "[update][install][macos]")
{
    Scratch s;
    const auto bundle = s.dir.getChildFile ("Sedef Amp.vst3");
    CHECK (bundleShortVersion (bundle).isEmpty());
    makeBundle (bundle, "0.3.1");
    CHECK (bundleShortVersion (bundle) == "0.3.1");
}

TEST_CASE ("The password prompt gets every argument as an argument", "[update][install][macos]")
{
    juce::StringArray command { "/usr/sbin/installer" };
    command.addArray (kNasty);
    const auto argv = osascriptArgv (command, installPrompt ("0.3.1"));
    REQUIRE (argv.size() > 3);
    CHECK (argv[0] == "/usr/bin/osascript");
    const int dashes = argv.indexOf ("--");
    REQUIRE (dashes > 0);
    CHECK (argv[dashes + 1] == "Sedef Amp wants to install version 0.3.1.");
    for (int i = 0; i < command.size(); ++i)
        CHECK (argv[dashes + 2 + i] == command[i]); // verbatim, never edited
    CHECK (argv.size() == dashes + 2 + command.size());
    // The script is the same whatever the arguments: none of them reaches it.
    const auto script = [] (const juce::StringArray& a) {
        juce::StringArray lines;
        for (int i = 1; i < a.indexOf ("--"); ++i)
            lines.add (a[i]);
        return lines.joinIntoString ("\n");
    };
    CHECK (script (argv) == script (osascriptArgv ({ "/bin/true" }, "x")));
    CHECK (script (argv).contains ("quoted form of (item i of argv)"));
    CHECK (script (argv).contains ("with administrator privileges"));

    CHECK (
        installerArgv (juce::File::getCurrentWorkingDirectory().getChildFile ("a b/x.pkg"),
                       juce::File::getCurrentWorkingDirectory().getChildFile ("c d/choices.xml")) ==
        juce::StringArray {
            "/usr/sbin/installer", "-pkg",
            juce::File::getCurrentWorkingDirectory().getChildFile ("a b/x.pkg").getFullPathName(),
            "-applyChoiceChangesXML",
            juce::File::getCurrentWorkingDirectory()
                .getChildFile ("c d/choices.xml")
                .getFullPathName(),
            "-target", "/" });

    CHECK (appleScriptErrorNumber ("0:285: execution error: User canceled. (-128)") == -128);
    CHECK (appleScriptErrorNumber ("execution error: installer: Error - failed (1)\n") == 1);
    CHECK_FALSE (appleScriptErrorNumber ("").has_value());
    CHECK_FALSE (appleScriptErrorNumber ("(abc)").has_value());
    CHECK_FALSE (appleScriptErrorNumber ("no number").has_value());

#if JUCE_MAC
    // The real chain, minus the privileges: AppleScript's quoting into /bin/sh hands printf each
    // argument unchanged, and nothing in them runs.
    juce::StringArray print { "/usr/bin/printf", "[%s]\\n" };
    print.addArray (kNasty);
    auto plain = osascriptArgv (print, "unused");
    for (auto& line : plain)
        line = line.replace (" with administrator privileges", "");
    const auto r = runProcess (plain, 20000);
    INFO (r.summary());
    REQUIRE (r.succeeded());
    juce::String expected;
    for (const auto& a : kNasty)
        expected << "[" << a << "]\n";
    // do shell script hands back its output with CR line endings.
    CHECK (r.output.replace ("\r", "\n").trimEnd() == expected.trimEnd());
    CHECK_FALSE (juce::File::getCurrentWorkingDirectory().getChildFile ("PWNED").exists());
    CHECK_FALSE (juce::File::getCurrentWorkingDirectory().getChildFile ("PWNED2").exists());
#endif
}

// Windows -----------------------------------------------------------------------------------------

TEST_CASE ("Setup's switches for an update", "[update][install][windows]")
{
    CHECK (windowsSetupParameters (1234, false) ==
           "/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010 "
           "/WAITPID=1234");
    CHECK (windowsSetupParameters (4294967295u, true) ==
           "/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010 "
           "/WAITPID=4294967295 /RELAUNCH");
    CHECK_FALSE (windowsSetupParameters (1, true).containsAnyOf ("\"'&|<>^%"));

    Scratch s;
    const auto common = s.dir.getChildFile ("Common Files");
    CHECK (windowsSetupUpdates (
        common.getChildFile ("VST3/Sedef Amp.vst3/Contents/x86_64-win/Sedef Amp.vst3"), common));
    CHECK (windowsSetupUpdates (common.getChildFile ("CLAP/Sedef Amp.clap"), common));
    CHECK (windowsSetupUpdates (s.dir.getChildFile ("Wherever/Sedef Amp.exe"), common));
    CHECK_FALSE (
        windowsSetupUpdates (s.dir.getChildFile ("build/VST3/Sedef Amp.vst3/x.vst3"), common));
    CHECK_FALSE (windowsSetupUpdates (common.getChildFile ("CLAP/Other.clap"), common));
}

// Linux: the archive ------------------------------------------------------------------------------

TEST_CASE ("A release archive lists and passes the checks", "[update][install][linux]")
{
    TarBuilder t;
    linuxLayout (t);
    const auto listing = list (t);
    REQUIRE (listing.error.isEmpty());
    REQUIRE (listing.entries.size() == 15);
    CHECK (listing.entries[0].type == TarEntry::Type::Directory);
    CHECK (listing.entries[7].path == juce::String (kTop) + "/CLAP/Sedef Amp.clap");
    CHECK (listing.entries[7].mode == 0755);
    CHECK (listing.entries[7].size == 4);
    juce::String error;
    CHECK (checkLinuxArchive (listing.entries, error) == kTop);
    CHECK (error.isEmpty());
}

TEST_CASE ("Long names and pax paths are read like tar reads them", "[update][install][linux]")
{
    const auto deep = juce::String (kTop) + "/" + juce::String::repeatedString ("deep/", 30) + "f";
    const auto pax = juce::String (kTop) + "/pax name " + juce::String::fromUTF8 ("\xc3\xbc");
    TarBuilder t;
    t.longName (deep);
    t.file ("truncated-name", "x");
    t.paxPath (pax);
    t.file ("other", "y");
    t.header ("name", '0', 0, 0644, {}, TarBuilder::Posix, "prefix/dir");
    const auto listing = list (t);
    REQUIRE (listing.error.isEmpty());
    REQUIRE (listing.entries.size() == 3);
    CHECK (listing.entries[0].path == deep);
    CHECK (listing.entries[1].path == pax);
    CHECK (listing.entries[2].path == "prefix/dir/name");
}

TEST_CASE ("Archives with links, devices or paths out of their folder are refused",
           "[update][install][linux]")
{
    const juce::String top (kTop);
    const auto with = [&top] (auto&& add) {
        TarBuilder t;
        linuxLayout (t);
        add (t, top);
        return check (t);
    };
    using T = TarBuilder;
    using S = const juce::String&;
    CHECK (with ([] (T& t, S p) {
               t.link (p + "/l", '2', "/etc/passwd");
           }).contains ("symbolic link"));
    CHECK (with ([] (T& t, S p) {
               t.link (p + "/h", '1', p + "/install.sh");
           }).contains ("hard link"));
    CHECK (with ([] (T& t, S p) { t.special (p + "/dev", '3'); }).contains ("device"));
    CHECK (with ([] (T& t, S p) { t.special (p + "/blk", '4'); }).contains ("device"));
    CHECK (with ([] (T& t, S p) { t.special (p + "/fifo", '6'); }).contains ("pipe"));
    CHECK (with ([] (T& t, S p) { t.special (p + "/odd", 'S'); }).contains ("type"));
    CHECK (with ([] (T& t, S) { t.file ("/etc/cron.d/x", "x"); }).contains ("absolute"));
    CHECK (with ([] (T& t, S p) { t.file (p + "/../../x", "x"); }).contains ("leaves"));
    CHECK (with ([] (T& t, S p) { t.file (p + "/a/./b", "x"); }).contains ("leaves"));
    CHECK (with ([] (T& t, S p) { t.file (p + "/a\\b", "x"); }).contains ("backslash"));
    CHECK (with ([] (T& t, S p) { t.file (p + "/C:x", "x"); }).contains ("colon"));
    CHECK (with ([] (T& t, S) { t.file ("other-top/x", "x"); }).contains ("more than one top"));
    CHECK (with ([] (T& t, S p) { t.file (p + "/install.sh", "again"); }).contains ("twice"));
    CHECK (with ([] (T& t, S p) {
               t.paxPath ("../escape");
               t.file (p + "/ok", "x");
           }).contains ("leaves"));

    // The layout install.sh needs.
    TarBuilder missing;
    missing.folder (top + "/");
    missing.file (top + "/install.sh", "#!/bin/sh\n", 0755);
    CHECK (check (missing).contains ("has no"));
    TarBuilder flat;
    flat.file ("install.sh", "x");
    CHECK (check (flat).contains ("not a folder"));
    const TarBuilder empty;
    CHECK (check (empty).contains ("empty"));
}

TEST_CASE ("Broken archives stop the listing", "[update][install][linux]")
{
    TarBuilder bad;
    bad.header ("x", '0', 0, 0644, {}, TarBuilder::Posix, {}, true);
    CHECK (list (bad).error.contains ("checksum"));

    TarBuilder cut;
    cut.header ("x", '0', 5000, 0644); // promises 5000 bytes, has none
    CHECK (list (cut, false).error.contains ("ends inside"));

    TarBuilder pax;
    pax.header ("PaxHeader", 'x', 11, 0644);
    pax.data ("11 size=99\n", 11);
    pax.file ("x", "x");
    CHECK (list (pax).error.contains ("size"));

    juce::MemoryInputStream notGzip ("plain text, not an archive", 26, false);
    CHECK (listTarGz (notGzip).error.isNotEmpty());
}

TEST_CASE ("A checked archive unpacks, and a bad one writes nothing", "[update][install][linux]")
{
    Scratch s;
    TarBuilder good;
    linuxLayout (good, "#!/bin/sh\necho installed\n");
    const auto archive = good.save (s.dir.getChildFile ("good.tar.gz"));
    const auto out = s.dir.getChildFile ("out");
    REQUIRE (out.createDirectory());
    juce::String error, top;
    const bool extracted = extractTarGz (archive, out, error, &top);
    CAPTURE (error);
    REQUIRE (extracted);
    CHECK (top == kTop);
    const auto script = out.getChildFile (kTop).getChildFile ("install.sh");
    CHECK (script.loadFileAsString() == "#!/bin/sh\necho installed\n");
    CHECK (vst3BundleVersion (out.getChildFile (kTop).getChildFile ("VST3/Sedef Amp.vst3")) ==
           "0.2.0");
#if !JUCE_WINDOWS
    CHECK (isExecutable (script));
    CHECK_FALSE (isExecutable (out.getChildFile (kTop).getChildFile ("sedef-amp.svg")));
#endif

    TarBuilder bad;
    linuxLayout (bad);
    bad.link (juce::String (kTop) + "/evil", '2', "/etc");
    const auto badArchive = bad.save (s.dir.getChildFile ("bad.tar.gz"));
    const auto out2 = s.dir.getChildFile ("out2");
    REQUIRE (out2.createDirectory());
    CHECK_FALSE (extractTarGz (badArchive, out2, error));
    CHECK (error.contains ("symbolic link"));
    CHECK (out2.findChildFiles (juce::File::findFilesAndDirectories, true).isEmpty());
}

#if !JUCE_WINDOWS
TEST_CASE ("Where this process runs decides the Linux route", "[update][install][linux]")
{
    const juce::File home ("/home/u s");
    CHECK (
        linuxScopeOf (home.getChildFile (".vst3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so"),
                      home) == LinuxScope::User);
    CHECK (linuxScopeOf (home.getChildFile (".clap/Sedef Amp.clap"), home) == LinuxScope::User);
    CHECK (linuxScopeOf (home.getChildFile (".local/bin/sedef-amp"), home) == LinuxScope::User);
    CHECK (linuxScopeOf (
               juce::File ("/usr/lib/vst3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so"),
               home) == LinuxScope::System);
    CHECK (linuxScopeOf (juce::File ("/usr/lib/clap/Sedef Amp.clap"), home) == LinuxScope::System);
    CHECK (linuxScopeOf (juce::File ("/usr/local/bin/sedef-amp"), home) == LinuxScope::System);
    CHECK (linuxScopeOf (juce::File ("/opt/build/Sedef Amp.so"), home) == LinuxScope::Elsewhere);
    CHECK (linuxScopeOf (home.getChildFile ("src/build/Sedef Amp.clap"), home) ==
           LinuxScope::Elsewhere);
    CHECK (linuxVst3Bundle (LinuxScope::System, home).getFullPathName() ==
           "/usr/lib/vst3/Sedef Amp.vst3");
    CHECK (linuxStandalone (LinuxScope::User, home).getFullPathName() ==
           "/home/u s/.local/bin/sedef-amp");
}
#endif

TEST_CASE ("VST3 versions come from moduleinfo.json", "[update][install][linux]")
{
    CHECK (moduleInfoVersion (R"({"Name": "Sedef Amp", "Version": "0.3.1", "Factory Info": {}})") ==
           "0.3.1");
    CHECK (moduleInfoVersion (
               "{\n  // JSON5\n  \"Name\": \"Sedef Amp\",\n  \"Version\" : \"0.3.1\",\n}") ==
           "0.3.1");
    CHECK (moduleInfoVersion (R"({"Name": "Sedef Amp"})").isEmpty());
    CHECK (moduleInfoVersion ("garbage").isEmpty());
}

// Processes ---------------------------------------------------------------------------------------

#if !JUCE_WINDOWS
TEST_CASE ("Commands run by argv and are killed when cancelled", "[update][install][process]")
{
    const auto r = runProcess ({ "/bin/sh", "-c", "echo out; echo err >&2; exit 3" }, 10000);
    CHECK (r.started);
    CHECK (r.exitCode == 3);
    CHECK_FALSE (r.succeeded());
    CHECK (r.output == "out\n");
    CHECK (r.errors == "err\n");

    CHECK_FALSE (runProcess ({ "sh", "-c", "true" }, 1000).started); // absolute paths only
    CHECK_FALSE (runProcess ({ "/no/such/program" }, 1000).succeeded());

    // A cancel or a timeout kills it, and what it started, within moments.
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto cancelled =
        runProcess ({ "/bin/sh", "-c", "/bin/sleep 30 & /bin/sleep 30" }, 0, [&started] {
            return juce::Time::getMillisecondCounterHiRes() - started > 200;
        });
    CHECK (cancelled.killed);
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 5000);
    const auto late = runProcess ({ "/bin/sleep", "30" }, 200);
    CHECK (late.killed);

    // The restart helper waits for a process that is gone, then runs its command.
    const auto helper = waitThenRunArgv (999999, { "/usr/bin/printf", "%s", "it's $(x)" });
    CHECK (helper[0] == "/bin/sh");
    CHECK (helper[4] == "999999");
    const auto ran = runProcess (helper, 10000);
    CHECK (ran.succeeded());
    CHECK (ran.output == "it's $(x)");
}

namespace
{
/// Sets an environment variable for one test.
struct Env
{
    Env (const char* n, const juce::String& v) : name (n) { setenv (n, v.toRawUTF8(), 1); }
    ~Env() { unsetenv (name); }
    Env (const Env&) = delete;
    Env& operator= (const Env&) = delete;
    const char* name;
};

/// Stands in for the OS's prompt: answers `status`; on Ok, runs the command as this user.
Elevate answering (Elevated::Status status, std::atomic<int>* calls = nullptr)
{
    return [status, calls] (const juce::StringArray& command, const juce::String&, Cancellation&) {
        if (calls != nullptr)
            ++*calls;
        Elevated e;
        e.status = status;
        e.process.started = true;
        e.process.exitCode = status == Elevated::Status::Ok ? 0 : 1;
        if (status == Elevated::Status::Ok && command.size() > 0 &&
            command[0].endsWith ("install.sh"))
            e.process = runProcess (command, 30000);
        return e;
    };
}
} // namespace

TEST_CASE ("The Linux route unpacks, runs install.sh and checks the version",
           "[update][install][linux]")
{
    Scratch s;
    const auto home = s.dir.getChildFile ("home");
    const Env testHome ("SEDEF_TEST_HOME", home.getFullPathName());
    // Stands in for install.sh: puts the VST3's moduleinfo.json where the real one would, and
    // notes its arguments.
    const juce::String script = "#!/bin/sh\n"
                                "d=\"$SEDEF_TEST_HOME/.vst3/Sedef Amp.vst3/Contents/Resources\"\n"
                                "mkdir -p \"$d\" && cp \"$(dirname \"$0\")/VST3/Sedef Amp.vst3/"
                                "Contents/Resources/moduleinfo.json\" \"$d/\"\n"
                                "echo \"$@\" > \"$SEDEF_TEST_HOME/args\"\n";
    TarBuilder t;
    linuxLayout (t, script);
    const auto archive = t.save (s.dir.getChildFile ("Sedef-Amp-Linux-x86_64.tar.gz"));

    InstallerConfig config;
    config.cache = s.dir.getChildFile ("cache");
    REQUIRE (makePrivateFolder (config.cache));
    config.home = home;
    config.running = home.getChildFile (".clap/Sedef Amp.clap");
    std::atomic<int> asked { 0 };
    config.elevate = answering (Elevated::Status::Ok, &asked);
    const InstallRequest request { archive, sha256Hex (archive), "0.2.0" };
    Cancellation cancel;
    juce::StringArray steps;
    const auto onStep = [&steps] (InstallStep step) {
        steps.add (toString (step));
    };

    SECTION ("in your home folder, without a password")
    {
        auto installer = makeInstaller (Platform::Linux, config);
        CHECK (installer->manualReason() == ManualReason::None);
        const auto r = installer->install (request, onStep, cancel);
        INFO (r.detail);
        REQUIRE (r.outcome == InstallResult::Outcome::Installed);
        CHECK (steps == juce::StringArray { "preparing", "installing", "finishing" });
        CHECK (asked == 0);
        CHECK (home.getChildFile ("args").loadFileAsString().trim().isEmpty());
        CHECK (vst3BundleVersion (linuxVst3Bundle (LinuxScope::User, home)) == "0.2.0");
        CHECK_FALSE (config.cache.getChildFile ("unpacked").exists()); // cleaned up
    }
    SECTION ("system-wide, through the prompt, with --system")
    {
        config.scope = LinuxScope::System;
        auto installer = makeInstaller (Platform::Linux, config);
        const auto r = installer->install (request, onStep, cancel);
        CHECK (steps.contains ("permission"));
        CHECK (asked == 1);
        CHECK (home.getChildFile ("args").loadFileAsString().trim() == "--system");
        // It verifies /usr/lib/vst3, where this stand-in wrote nothing.
        CHECK (r.outcome == InstallResult::Outcome::Failed);
        CHECK (r.error == Error::Unverified);
    }
    SECTION ("a declined prompt is not an error, an unavailable one is")
    {
        config.scope = LinuxScope::System;
        config.elevate = answering (Elevated::Status::Declined);
        CHECK (makeInstaller (Platform::Linux, config)->install (request, onStep, cancel).outcome ==
               InstallResult::Outcome::Declined);
        config.elevate = answering (Elevated::Status::Unavailable);
        const auto r = makeInstaller (Platform::Linux, config)->install (request, onStep, cancel);
        CHECK (r.error == Error::ElevationFailed);
    }
    SECTION ("a failing install.sh, a changed file and a bad archive")
    {
        TarBuilder failing;
        linuxLayout (failing, "#!/bin/sh\necho broken >&2\nexit 1\n");
        const auto bad = failing.save (s.dir.getChildFile ("failing.tar.gz"));
        auto installer = makeInstaller (Platform::Linux, config);
        auto r = installer->install ({ bad, sha256Hex (bad), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::InstallFailed);
        CHECK (r.detail.contains ("broken"));

        r = installer->install ({ archive, sha256Hex (bad), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::ChecksumMismatch);

        TarBuilder linked;
        linuxLayout (linked);
        linked.link (juce::String (kTop) + "/x", '2', "/");
        const auto evil = linked.save (s.dir.getChildFile ("linked.tar.gz"));
        r = installer->install ({ evil, sha256Hex (evil), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::BadPackage);
        CHECK_FALSE (home.getChildFile ("args").exists()); // nothing ran
    }
    SECTION ("where it runs decides whether it can run at all")
    {
        config.running = s.dir.getChildFile ("build/Sedef Amp.clap");
        CHECK (makeInstaller (Platform::Linux, config)->manualReason() == ManualReason::Elsewhere);
    }
}
#endif

#if JUCE_MAC
namespace
{
/// A disk image like Sedef-Amp-macOS.dmg, made with hdiutil, holding a stand-in pkg.
juce::File makeImage (const juce::File& dir, bool pkgIsLink)
{
    const auto src = dir.getChildFile ("image");
    REQUIRE (src.createDirectory());
    const auto pkg = src.getChildFile (kMacPackageName);
    if (pkgIsLink)
    {
        REQUIRE (dir.getChildFile ("real.pkg").replaceWithText ("pkg"));
        REQUIRE (dir.getChildFile ("real.pkg").createSymbolicLink (pkg, true));
    }
    else
    {
        REQUIRE (pkg.replaceWithText ("pkg"));
    }
    const auto dmg = dir.getChildFile ("Sedef-Amp-macOS.dmg");
    const auto r =
        runProcess ({ "/usr/bin/hdiutil", "create", "-quiet", "-srcfolder", src.getFullPathName(),
                      "-volname", "Sedef Amp 0.2.0", "-format", "UDZO", dmg.getFullPathName() },
                    120000);
    INFO (r.summary());
    REQUIRE (r.succeeded());
    return dmg;
}

/// Disk images attached under `folder`.
juce::StringArray attachedUnder (const juce::File& folder)
{
    const auto info = runProcess ({ "/usr/bin/hdiutil", "info", "-plist" }, 20000);
    return mountPointsUnder (info.output, realPath (folder));
}

/// What the stand-in for osascript + installer was handed (read once the install returned).
struct Asked
{
    juce::StringArray command, chosen;
    juce::String prompt, pkgText;
    bool pkgInImage = false, pkgWritable = true;
    std::atomic<bool> waiting { false };
};
} // namespace

TEST_CASE ("The macOS route mounts, installs the chosen formats, detaches and verifies",
           "[update][install][macos]")
{
    Scratch s;
    const auto dmg = makeImage (s.dir, false);
    const auto root = s.dir.getChildFile ("root");
    makeBundle (macBundlePath (kMacComponents[1], root), "0.1.0"); // vst3
    makeBundle (macBundlePath (kMacComponents[2], root), "0.1.0"); // au

    InstallerConfig config;
    config.cache = s.dir.getChildFile ("cache");
    REQUIRE (makePrivateFolder (config.cache));
    const auto mounts = config.cache.getChildFile ("mnt");
    config.root = root;
    config.running = macBundlePath (kMacComponents[1], root);

    // Stands in for osascript + installer: notes what it was handed, then "installs" the chosen
    // bundles at the new version into the fake root.
    Asked asked;
    std::atomic<bool> block { false };
    std::optional<Elevated::Status> answer;
    juce::String installAs = "0.2.0";
    config.elevate = [&] (const juce::StringArray& command, const juce::String& prompt,
                          Cancellation& c) {
        asked.command = command;
        asked.prompt = prompt;
        Elevated e;
        e.process.started = true;
        if (command.size() == 7)
        {
            const juce::File pkg (command[2]);
            asked.pkgInImage =
                pkg.isAChildOf (realPath (mounts)) && pkg.getFileName() == kMacPackageName;
            asked.pkgText = pkg.loadFileAsString();
            asked.pkgWritable = pkg.hasWriteAccess();
            const auto choices = parsePlist (juce::File (command[4]).loadFileAsString());
            if (const auto* items = choices.getArray())
                for (const auto& item : *items)
                    if (static_cast<int> (item["attributeSetting"]) == 1)
                        asked.chosen.add (item["choiceIdentifier"].toString());
        }
        asked.waiting = true;
        while (block && !c.isCancelled())
            juce::Thread::sleep (2);
        if (answer)
        {
            e.status = *answer;
            return e;
        }
        for (const auto& comp : kMacComponents)
            if (asked.chosen.contains (comp.choice))
                makeBundle (macBundlePath (comp, root), installAs);
        e.status = Elevated::Status::Ok;
        return e;
    };
    InstallRequest request { dmg, sha256Hex (dmg), "0.2.0" };
    Cancellation cancel;
    juce::StringArray steps;
    const auto onStep = [&steps] (InstallStep step) {
        steps.add (toString (step));
    };

    SECTION ("installs over the formats that are there")
    {
        auto installer = makeInstaller (Platform::MacOS, config);
        CHECK (installer->manualReason() == ManualReason::None);
        const auto r = installer->install (request, onStep, cancel);
        INFO (r.detail);
        REQUIRE (r.outcome == InstallResult::Outcome::Installed);
        CHECK (steps == juce::StringArray { "preparing", "permission", "finishing" });
        CHECK (asked.prompt == "Sedef Amp wants to install version 0.2.0.");
        REQUIRE (asked.command.size() == 7);
        CHECK (asked.command[0] == "/usr/sbin/installer");
        CHECK (asked.command[6] == "/");
        CHECK (asked.pkgInImage);
        CHECK (asked.pkgText == "pkg");
        CHECK_FALSE (asked.pkgWritable); // a read-only image
        CHECK (asked.chosen == juce::StringArray { "vst3", "au" });
        CHECK (bundleShortVersion (macBundlePath (kMacComponents[1], root)) == "0.2.0");
        CHECK (bundleShortVersion (macBundlePath (kMacComponents[2], root)) == "0.2.0");
        CHECK_FALSE (macBundlePath (kMacComponents[0], root).exists()); // not asked for
        CHECK (attachedUnder (mounts).isEmpty());
        CHECK_FALSE (config.cache.getChildFile ("choices.xml").exists());
    }
    SECTION ("cancelling the password dialog detaches and is no error")
    {
        answer = Elevated::Status::Declined;
        const auto r = makeInstaller (Platform::MacOS, config)->install (request, onStep, cancel);
        CHECK (r.outcome == InstallResult::Outcome::Declined);
        CHECK (attachedUnder (mounts).isEmpty());
    }
    SECTION ("a failed install, a refused prompt and a wrong version are errors")
    {
        auto installer = makeInstaller (Platform::MacOS, config);
        answer = Elevated::Status::Failed;
        CHECK (installer->install (request, onStep, cancel).error == Error::InstallFailed);
        answer = Elevated::Status::Unavailable;
        CHECK (installer->install (request, onStep, cancel).error == Error::ElevationFailed);
        answer.reset();
        installAs = "0.1.9";
        const auto r = installer->install (request, onStep, cancel);
        CHECK (r.error == Error::Unverified);
        CHECK (r.detail.contains ("0.1.9"));
        CHECK (attachedUnder (mounts).isEmpty());
    }
    SECTION ("closing mid-install kills the wait and detaches")
    {
        block = true;
        InstallResult r;
        auto installer = makeInstaller (Platform::MacOS, config);
        std::thread worker ([&] { r = installer->install (request, onStep, cancel); });
        const auto end = juce::Time::getMillisecondCounter() + 60000;
        while (!asked.waiting && juce::Time::getMillisecondCounter() < end)
            juce::Thread::sleep (5);
        const bool waited = asked.waiting;
        const auto mountedThen = attachedUnder (mounts).size();
        const auto started = juce::Time::getMillisecondCounterHiRes();
        cancel.cancel();
        worker.join();
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 6000);
        REQUIRE (waited);
        CHECK (mountedThen == 1);
        CHECK (r.outcome == InstallResult::Outcome::Cancelled);
        CHECK (attachedUnder (mounts).isEmpty());
    }
    SECTION ("a file that changed since it was verified is not mounted")
    {
        request.expectedHash = juce::String::repeatedString ("0", 64);
        const auto r = makeInstaller (Platform::MacOS, config)->install (request, onStep, cancel);
        CHECK (r.error == Error::ChecksumMismatch);
        CHECK (asked.command.isEmpty());
        CHECK (attachedUnder (mounts).isEmpty());
    }
    SECTION ("running from elsewhere, or sandboxed, means Install manually")
    {
        config.running = s.dir.getChildFile ("build/Sedef Amp.vst3");
        CHECK (makeInstaller (Platform::MacOS, config)->manualReason() == ManualReason::Elsewhere);
        config.running = macBundlePath (kMacComponents[1], root);
        const Env sandbox ("APP_SANDBOX_CONTAINER_ID", "test");
        CHECK (makeInstaller (Platform::MacOS, config)->manualReason() == ManualReason::Sandboxed);
    }
}

TEST_CASE ("A pkg that is a symbolic link in the image is refused", "[update][install][macos]")
{
    Scratch s;
    const auto dmg = makeImage (s.dir, true);
    InstallerConfig config;
    config.cache = s.dir.getChildFile ("cache");
    REQUIRE (makePrivateFolder (config.cache));
    config.root = s.dir.getChildFile ("root");
    std::atomic<int> asked { 0 };
    config.elevate = answering (Elevated::Status::Ok, &asked);
    Cancellation cancel;
    const auto r = makeInstaller (Platform::MacOS, config)
                       ->install ({ dmg, sha256Hex (dmg), "0.2.0" }, [] (InstallStep) {}, cancel);
    CHECK (r.error == Error::BadPackage);
    CHECK (asked == 0);
    CHECK (attachedUnder (config.cache.getChildFile ("mnt")).isEmpty());
}

TEST_CASE ("Images an earlier process left attached are detached", "[update][install][macos]")
{
    Scratch s;
    const auto dmg = makeImage (s.dir, false);
    InstallerConfig config;
    config.cache = s.dir.getChildFile ("cache");
    const auto mounts = config.cache.getChildFile ("mnt");
    REQUIRE (makePrivateFolder (mounts));
    REQUIRE (runProcess ({ "/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen",
                           "-mountrandom", mounts.getFullPathName(), dmg.getFullPathName() },
                         60000)
                 .succeeded());
    REQUIRE (attachedUnder (mounts).size() == 1);
    makeInstaller (Platform::MacOS, config)->cleanUp();
    CHECK (attachedUnder (mounts).isEmpty());
}
#endif
