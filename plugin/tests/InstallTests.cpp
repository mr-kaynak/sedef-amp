// SPDX-License-Identifier: AGPL-3.0-or-later
// The installer's logic (plugin/src/update/InstallLogic.h) and, where the OS allows it without a
// password, its real steps: the install scripts run as this user (macOS: with a stand-in for
// Apple's installer, which never runs here; Linux: with a stand-in install.sh), with a scripted
// stand-in for the prompt, and the processes, files and helpers around them.
#include "Installer.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <juce_cryptography/juce_cryptography.h>
#include <map>
#include <set>
#include <thread>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
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

juce::String sha256Of (const juce::String& text)
{
    return juce::SHA256 (text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString().toLowerCase();
}

/// Waits up to `ms` for `done`.
template <typename Done>
bool within (int ms, Done&& done)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (ms);
    while (!done() && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (20);
    return done();
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
    // Nothing installed: all four, and Sedef MIDI.
    CHECK (macChoicesToInstall (root, {}) ==
           juce::StringArray { "app", "vst3", "au", "clap", "midi" });

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

    // Sedef MIDI is kept when any of its bundles is installed, and left out when none is.
    for (const auto& midi : kMacMidiBundles)
    {
        INFO (midi.bundle);
        makeBundle (macBundlePath (midi, root), "0.6.0");
        CHECK (macChoicesToInstall (root, {}) == juce::StringArray { "vst3", "au", "midi" });
        REQUIRE (macBundlePath (midi, root).deleteRecursively());
        CHECK (macChoicesToInstall (root, {}) == juce::StringArray { "vst3", "au" });
    }

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
    REQUIRE (items->size() == 5); // every choice, so none keeps its start_selected default
    const char* ids[] = { "app", "vst3", "au", "clap", "midi" };
    const int selected[] = { 0, 1, 0, 1, 0 };
    for (int i = 0; i < 5; ++i)
    {
        const auto& item = (*items)[i];
        CHECK (item["choiceIdentifier"].toString() == ids[i]);
        CHECK (item["choiceAttribute"].toString() == "selected");
        CHECK (item["attributeSetting"].isInt64());
        CHECK (static_cast<int> (item["attributeSetting"]) == selected[i]);
    }
    const auto all = parsePlist (choiceChangesXml ({ "app", "vst3", "au", "clap", "midi" }));
    REQUIRE (all.getArray() != nullptr);
    for (const auto& item : *all.getArray())
        CHECK (static_cast<int> (item["attributeSetting"]) == 1);
}

TEST_CASE ("Property lists become vars", "[update][install][macos]")
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
			<key>dev-entry</key><string>/dev/disk5s1</string>
			<key>mount-point</key><string>/private/var/tmp/sedef-update.YUFE7x/mnt</string>
			<key>potentially-mountable</key><true/>
			<key>size</key><integer>48600000</integer>
		</dict>
	</array>
</dict>
</plist>)";
    const auto plist = parsePlist (attach);
    CHECK (plist["system-entities"][1]["mount-point"].toString() ==
           "/private/var/tmp/sedef-update.YUFE7x/mnt");
    CHECK_FALSE (static_cast<bool> (plist["system-entities"][0]["potentially-mountable"]));
    CHECK (static_cast<bool> (plist["system-entities"][1]["potentially-mountable"]));
    CHECK (static_cast<juce::int64> (plist["system-entities"][1]["size"]) == 48600000);
    CHECK (parsePlist ("hdiutil: attach failed - image not recognized").isVoid());
    CHECK (parsePlist ({}).isVoid());
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

    // The install script itself, through the same chain: it reaches /bin/sh whole, takes its
    // arguments, and stops at the copy's checksum (82), before anything is mounted or installed.
    Scratch s;
    const auto file = s.dir.getChildFile ("it's a \"dmg\".dmg");
    REQUIRE (file.replaceWithText ("not the verified file"));
    auto install = osascriptArgv (
        macInstallCommand (file, juce::String::repeatedString ("0", 64), choiceChangesXml ({})),
        "unused");
    for (auto& line : install)
        line = line.replace (" with administrator privileges", "");
    const auto stopped = runProcess (install, 20000);
    INFO (stopped.summary());
    CHECK (appleScriptErrorNumber (stopped.errors) == static_cast<int> (ScriptExit::Checksum));
#endif
}

TEST_CASE ("The install scripts are constant; everything else is an argument", "[update][install]")
{
    const auto dmg = juce::File::getCurrentWorkingDirectory().getChildFile ("a b/it's.dmg");
    const auto mac = macInstallCommand (dmg, "ABC", "<xml 'q' \"$(x)\">");
    REQUIRE (mac.size() == 7);
    CHECK (mac[0] == "/bin/sh");
    CHECK (mac[1] == "-c");
    CHECK (mac[2] == macInstallScript());
    CHECK (mac[3] == "sedef"); // $0
    CHECK (mac[4] == dmg.getFullPathName());
    CHECK (mac[5] == "abc");
    CHECK (mac[6] == "<xml 'q' \"$(x)\">");
    CHECK (macInstallCommand (dmg.getSiblingFile ("other.dmg"), "1", "2")[2] == mac[2]);

    const auto archive = juce::File::getCurrentWorkingDirectory().getChildFile ("a b/x.tar.gz");
    const auto system = linuxInstallCommand (archive, "DEF", LinuxScope::System);
    REQUIRE (system.size() == 7);
    CHECK (system[2] == linuxInstallScript());
    CHECK (system[4] == archive.getFullPathName());
    CHECK (system[5] == "def");
    CHECK (system[6] == "system");
    CHECK (linuxInstallCommand (archive, "DEF", LinuxScope::User)[6] == "user");

    // What each promises: a folder only its user opens, the copy's own checksum, cleanup on
    // every way out, no stop at a closed pipe, and three arguments.
    for (const auto& script : { macInstallScript(), linuxInstallScript() })
    {
        CHECK (script.startsWith ("set -u\numask 077\n"));
        CHECK (script.contains ("d=$(mktemp -d "));
        CHECK (script.contains ("trap finish EXIT"));
        CHECK (script.contains ("trap '' PIPE HUP"));
        CHECK (script.contains ("[ \"${actual%% *}\" = \"$sum\" ] || exit 82"));
        CHECK (script.contains ("rm -rf \"$d\""));
        CHECK_FALSE (script.contains ("$4"));
    }
    CHECK (macInstallScript().contains ("hdiutil attach -quiet -nobrowse -readonly -noautoopen "
                                        "-mountpoint \"$d/mnt\" \"$d/i.dmg\""));
    CHECK (
        macInstallScript().contains ("if [ ! -f \"$pkg\" ] || [ -L \"$pkg\" ]; then exit 84; fi"));
    CHECK (linuxInstallScript().contains ("tar --no-same-owner --no-same-permissions -xzf"));
}

// Windows -----------------------------------------------------------------------------------------

TEST_CASE ("Setup's switches for an update", "[update][install][windows]")
{
    CHECK (windowsSetupParameters (1234, 5678, { "vst3", "clap" }, false) ==
           "/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010 "
           "/COMPONENTS=vst3,clap /WAITPID=1234 /WAITPIDTIME=5678");
    // Without the creation time Setup could not tell the host from a later process: no switches,
    // and no Setup.
    CHECK (windowsSetupParameters (1234, 0, { "vst3", "clap" }, false).isEmpty());
    CHECK (windowsSetupParameters (4294967295u, 133790123456789012ull, { "standalone" }, true) ==
           "/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010 "
           "/COMPONENTS=standalone /WAITPID=4294967295 /WAITPIDTIME=133790123456789012 /RELAUNCH");
    CHECK_FALSE (windowsSetupParameters (1, 2, { "vst3", "clap", "standalone" }, true)
                     .containsAnyOf ("\"'&|<>^%"));
}

TEST_CASE ("Setup installs the formats there are, plus the running one",
           "[update][install][windows]")
{
    Scratch s;
    const auto common = s.dir.getChildFile ("Common Files");
    const auto app = s.dir.getChildFile ("Program Files/Sedef Amp");
    const auto vst3 = common.getChildFile ("VST3/Sedef Amp.vst3");
    const auto clap = common.getChildFile ("CLAP/Sedef Amp.clap");
    const auto exe = app.getChildFile ("Sedef Amp.exe");
    const juce::StringArray all { "vst3", "clap", "standalone" };

    // Nothing installed: all three, and Sedef MIDI.
    CHECK (windowsComponentsToInstall (common, app, {}) ==
           juce::StringArray { "vst3", "clap", "standalone", "midi" });
    REQUIRE (vst3.getChildFile ("Contents/x86_64-win").createDirectory());
    CHECK (windowsComponentsToInstall (common, app, {}) == juce::StringArray { "vst3" });
    // The CLAP this process runs from counts, installed or not.
    CHECK (windowsComponentsToInstall (common, app, clap) == juce::StringArray { "vst3", "clap" });
    REQUIRE (exe.getParentDirectory().createDirectory());
    REQUIRE (exe.replaceWithText ("exe"));
    CHECK (windowsComponentsToInstall (common, app, {}) ==
           juce::StringArray { "vst3", "standalone" });
    REQUIRE (clap.getParentDirectory().createDirectory());
    REQUIRE (clap.replaceWithText ("clap"));
    CHECK (windowsComponentsToInstall (common, app, {}) == all);
    // A folder named like the CLAP is not one.
    REQUIRE (clap.deleteFile());
    REQUIRE (clap.createDirectory());
    CHECK (windowsComponentsToInstall (common, app, {}) ==
           juce::StringArray { "vst3", "standalone" });

    // Sedef MIDI is kept when any of its files is installed, and left out when none is.
    const juce::File midiFiles[] = { common.getChildFile ("VST3/Sedef MIDI.vst3"),
                                     common.getChildFile ("CLAP/Sedef MIDI.clap"),
                                     app.getChildFile ("Sedef MIDI.exe") };
    for (const auto& f : midiFiles)
    {
        INFO (f.getFullPathName());
        if (f.hasFileExtension ("vst3"))
            REQUIRE (f.createDirectory());
        else
            REQUIRE (f.replaceWithText ("midi"));
        CHECK (windowsComponentsToInstall (common, app, {}) ==
               juce::StringArray { "vst3", "standalone", "midi" });
        REQUIRE (f.deleteRecursively());
        CHECK (windowsComponentsToInstall (common, app, {}) ==
               juce::StringArray { "vst3", "standalone" });
    }

    // Setup updates only what it put where it puts it.
    const auto module = vst3.getChildFile ("Contents/x86_64-win/Sedef Amp.vst3");
    CHECK (windowsSetupUpdates (module, common, app));
    CHECK (windowsSetupUpdates (common.getChildFile ("CLAP/Sedef Amp.clap"), common, app));
    CHECK (windowsSetupUpdates (exe, common, app));
    CHECK_FALSE (windowsSetupUpdates (s.dir.getChildFile ("Portable/Sedef Amp.exe"), common, app));
    CHECK_FALSE (windowsSetupUpdates (exe, common, {})); // no app folder known
    CHECK_FALSE (
        windowsSetupUpdates (s.dir.getChildFile ("build/VST3/Sedef Amp.vst3/x.vst3"), common, app));
    CHECK_FALSE (windowsSetupUpdates (common.getChildFile ("CLAP/Other.clap"), common, app));
    CHECK_FALSE (windowsSetupUpdates (module, {}, app));
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
    CHECK (listing.entries[7].sha256 == sha256Of ("clap"));
    CHECK (listing.entries[0].sha256.isEmpty()); // a folder
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

TEST_CASE ("Only files have data, and every header counts", "[update][install][linux]")
{
    // A folder or a link with data would have its bytes read and counted as nothing.
    const juce::MemoryBlock bytes (5000, true);
    TarBuilder folder;
    folder.header (juce::String (kTop) + "/d/", '5', 5000, 0755);
    folder.data (bytes.getData(), bytes.getSize());
    CHECK (list (folder).error.contains ("has data but is not a file"));
    TarBuilder link;
    link.header ("l", '2', 10, 0777, "target");
    link.data ("0123456789", 10);
    CHECK (list (link).error.contains ("has data but is not a file"));

    // Long-name headers are headers too.
    TarBuilder names;
    for (int i = 0; i <= kMaxTarEntries; ++i)
        names.longName ("n");
    CHECK (list (names).error.contains ("too many entries"));

    // A cancel stops the listing before the next header.
    TarBuilder t;
    linuxLayout (t);
    const auto archive = t.gzipped();
    juce::MemoryInputStream in (archive, false);
    CHECK (listTarGz (in, [] { return true; }).error == "cancelled");
}

#if !JUCE_WINDOWS
TEST_CASE ("What install.sh copies is checked against the archive", "[update][install][linux]")
{
    TarBuilder t;
    linuxLayout (t);
    t.file (juce::String (kTop) + "/VST3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so", "so");
    const auto listing = list (t);
    REQUIRE (listing.error.isEmpty());
    const juce::File home ("/home/u s");
    std::map<juce::String, juce::String> files;
    for (const auto& f : linuxInstalledFiles (listing.entries, kTop, LinuxScope::User, home))
        files[f.file.getFullPathName()] = f.sha256;
    CHECK (files == std::map<juce::String, juce::String> {
                        { "/home/u s/.vst3/Sedef Amp.vst3/Contents/Resources/moduleinfo.json",
                          sha256Of ("{\"Name\": \"Sedef Amp\", \"Version\": \"0.2.0\"}") },
                        { "/home/u s/.vst3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so",
                          sha256Of ("so") },
                        { "/home/u s/.clap/Sedef Amp.clap", sha256Of ("clap") },
                        { "/home/u s/.local/bin/sedef-amp", sha256Of ("app") } });
    std::set<juce::String> system;
    for (const auto& f : linuxInstalledFiles (listing.entries, kTop, LinuxScope::System, home))
        system.insert (f.file.getFullPathName());
    CHECK (system == std::set<juce::String> {
                         "/usr/lib/vst3/Sedef Amp.vst3/Contents/Resources/moduleinfo.json",
                         "/usr/lib/vst3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so",
                         "/usr/lib/clap/Sedef Amp.clap", "/usr/local/bin/sedef-amp" });
}
#endif

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

    // A cancel or a timeout kills it, and what it started, within moments; nothing is left.
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto cancelled =
        runProcess ({ "/bin/sh", "-c", "/bin/sleep 30 & /bin/sleep 30" }, 0, [&started] {
            return juce::Time::getMillisecondCounterHiRes() - started > 200;
        });
    CHECK (cancelled.killed);
    CHECK_FALSE (cancelled.abandoned);
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

TEST_CASE ("A program left to finish goes on, and nothing waits for it",
           "[update][install][process]")
{
    Scratch s;
    const auto marker = s.dir.getChildFile ("finished");
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto elapsed = [&started] {
        return juce::Time::getMillisecondCounterHiRes() - started;
    };
    const auto r = runProcess (
        { "/bin/sh", "-c", "/bin/sleep 1; echo done > \"$0\"", marker.getFullPathName() }, 0,
        [&elapsed] { return elapsed() > 200; }, OnCancel::LetFinish);
    CHECK (r.killed);
    CHECK (r.abandoned);
    CHECK (elapsed() < 900);
    CHECK_FALSE (marker.exists());
    CHECK (within (10000, [&marker] { return marker.existsAsFile(); }));
}

TEST_CASE ("No descriptor of this process reaches what it starts", "[update][install][process]")
{
    // One open without close-on-exec, as a host's audio device or lock file might be.
    const int fd = open ("/dev/null", O_RDONLY);
    REQUIRE (fd >= 0);
    REQUIRE (dup2 (fd, 77) == 77);
    close (fd);
    REQUIRE ((fcntl (77, F_GETFD) & FD_CLOEXEC) == 0);
    const juce::String probe = "if [ -e /dev/fd/77 ]; then echo open; else echo closed; fi";
    // A shell sees its own open descriptors this way.
    CHECK (runProcess ({ "/bin/sh", "-c", "exec 7</dev/null; " + probe.replace ("77", "7") }, 10000)
               .output == "open\n");
    CHECK (runProcess ({ "/bin/sh", "-c", probe }, 10000).output == "closed\n");
    Scratch s;
    const auto out = s.dir.getChildFile ("detached");
    REQUIRE (spawnDetached ({ "/bin/sh", "-c", probe + " > \"$0\"", out.getFullPathName() }));
    CHECK (within (10000, [&out] { return out.loadFileAsString() == "closed\n"; }));
    close (77);
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

/// Stands in for the OS's prompt: answers `status`; on Ok it runs the command as this user and
/// reports how it went, as the prompt reports a command it ran as root.
Elevate answering (Elevated::Status status, std::atomic<int>* calls = nullptr)
{
    return [status, calls] (const juce::StringArray& command, const juce::String&,
                            Cancellation& cancel) {
        if (calls != nullptr)
            ++*calls;
        Elevated e;
        e.status = status;
        e.process.started = true;
        if (status != Elevated::Status::Ok)
        {
            e.process.exitCode = 1;
            return e;
        }
        e.process = runProcess (command, 60000, [&cancel] { return cancel.isCancelled(); });
        if (!e.process.succeeded())
        {
            e.status = Elevated::Status::Failed;
            e.exitCode = e.process.exitCode;
        }
        return e;
    };
}

/// The install scripts' folders (mktemp -d /var/tmp/sedef-update.XXXXXX) that exist now.
std::set<juce::String> scriptFolders()
{
    std::set<juce::String> out;
    for (const auto& f : juce::File ("/var/tmp")
                             .findChildFiles (juce::File::findDirectories, false, "sedef-update.*"))
        out.insert (f.getFileName());
    return out;
}

/// Stands in for install.sh: copies the formats into $SEDEF_TEST_HOME as the real one copies them
/// into your home folder, and notes its arguments.
const juce::String kStandIn = R"(#!/bin/sh
h="$SEDEF_TEST_HOME"
here=$(dirname "$0")
mkdir -p "$h/.vst3" "$h/.clap" "$h/.local/bin"
rm -rf "$h/.vst3/Sedef Amp.vst3"
cp -R "$here/VST3/Sedef Amp.vst3" "$h/.vst3/"
cp "$here/CLAP/Sedef Amp.clap" "$h/.clap/"
cp "$here/Standalone/Sedef Amp" "$h/.local/bin/sedef-amp"
echo "$@" > "$h/args"
)";

juce::File linuxArchive (const juce::File& file, const juce::String& script = kStandIn)
{
    TarBuilder t;
    linuxLayout (t, script);
    t.file (juce::String (kTop) + "/VST3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so", "so");
    return t.save (file);
}
} // namespace

TEST_CASE ("The Linux route checks the archive, runs the script and verifies every file",
           "[update][install][linux]")
{
    Scratch s;
    const auto home = s.dir.getChildFile ("home");
    REQUIRE (home.createDirectory());
    const Env testHome ("SEDEF_TEST_HOME", home.getFullPathName());
    const auto archive = linuxArchive (s.dir.getChildFile ("Sedef-Amp-Linux-x86_64.tar.gz"));
    const auto before = scriptFolders();

    InstallerConfig config;
    config.home = home;
    config.running = home.getChildFile (".clap/Sedef Amp.clap");
    std::atomic<int> asked { 0 };
    config.elevate = answering (Elevated::Status::Ok, &asked);
    const InstallRequest request { archive, sha256Hex (archive), "0.2.0" };
    Cancellation cancel;
    juce::StringArray steps;
    std::atomic<bool> installing { false };
    const auto onStep = [&steps, &installing] (InstallStep step) {
        steps.add (toString (step));
        if (step == InstallStep::Installing)
            installing = true;
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
        CHECK (home.getChildFile (".vst3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so")
                   .loadFileAsString() == "so");
        // The archive's modes: the script's private umask is for its own folder only.
        struct stat info {};
        REQUIRE (stat (home.getChildFile (".clap/Sedef Amp.clap").getFullPathName().toRawUTF8(),
                       &info) == 0);
        CHECK ((info.st_mode & 0777) == 0755);
        CHECK (scriptFolders() == before); // the script's folder went with it
    }
    SECTION ("system-wide, through the prompt, with --system")
    {
        config.scope = LinuxScope::System;
        const auto r = makeInstaller (Platform::Linux, config)->install (request, onStep, cancel);
        CHECK (steps.contains ("permission"));
        CHECK (asked == 1);
        CHECK (home.getChildFile ("args").loadFileAsString().trim() == "--system");
        // It verifies /usr/lib/vst3 and the rest, where this stand-in wrote nothing.
        CHECK (r.error == Error::Unverified);
        CHECK (r.detail.contains ("/usr/lib/vst3/Sedef Amp.vst3"));
        CHECK (scriptFolders() == before);
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
    SECTION ("a file install.sh got wrong is found")
    {
        const auto wrong = linuxArchive (s.dir.getChildFile ("wrong.tar.gz"),
                                         kStandIn + "echo other > \"$h/.clap/Sedef Amp.clap\"\n");
        const auto r = makeInstaller (Platform::Linux, config)
                           ->install ({ wrong, sha256Hex (wrong), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::Unverified);
        CHECK (r.detail.contains (".clap/Sedef Amp.clap differs from the release"));
        CHECK_FALSE (r.detail.contains ("sedef-amp differs")); // the rest is right
    }
    SECTION ("a failing install.sh, a changed file and a bad archive")
    {
        const auto failing = linuxArchive (s.dir.getChildFile ("failing.tar.gz"),
                                           "#!/bin/sh\necho broken >&2\nexit 1\n");
        auto installer = makeInstaller (Platform::Linux, config);
        auto r = installer->install ({ failing, sha256Hex (failing), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::InstallFailed);
        CHECK (r.detail.contains ("broken"));

        r = installer->install ({ archive, sha256Hex (failing), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::ChecksumMismatch);

        TarBuilder linked;
        linuxLayout (linked, kStandIn);
        linked.link (juce::String (kTop) + "/x", '2', "/");
        const auto evil = linked.save (s.dir.getChildFile ("linked.tar.gz"));
        r = installer->install ({ evil, sha256Hex (evil), "0.2.0" }, onStep, cancel);
        CHECK (r.error == Error::BadPackage);
        CHECK_FALSE (home.getChildFile ("args").exists()); // nothing ran
        CHECK (scriptFolders() == before);
    }
    SECTION ("closing mid-install stops the wait, and install.sh finishes")
    {
        const auto slow = linuxArchive (s.dir.getChildFile ("slow.tar.gz"),
                                        "#!/bin/sh\n/bin/sleep 1\n" +
                                            kStandIn.fromFirstOccurrenceOf ("\n", false, false));
        auto installer = makeInstaller (Platform::Linux, config);
        InstallResult r;
        std::thread worker (
            [&] { r = installer->install ({ slow, sha256Hex (slow), "0.2.0" }, onStep, cancel); });
        const bool reached = within (10000, [&installing] { return installing.load(); });
        juce::Thread::sleep (200);
        const auto stopped = juce::Time::getMillisecondCounterHiRes();
        cancel.cancel();
        worker.join();
        CHECK (juce::Time::getMillisecondCounterHiRes() - stopped < 500);
        REQUIRE (reached);
        CHECK (r.outcome == InstallResult::Outcome::Cancelled);
        CHECK (within (10000, [&home] { return home.getChildFile ("args").existsAsFile(); }));
        CHECK (within (5000, [&before] { return scriptFolders() == before; }));
    }
    SECTION ("where it runs decides whether it can run at all")
    {
        config.running = s.dir.getChildFile ("build/Sedef Amp.clap");
        CHECK (makeInstaller (Platform::Linux, config)->manualReason() == ManualReason::Elsewhere);
    }
}

TEST_CASE ("The Linux install script stops at what is wrong and leaves nothing behind",
           "[update][install][linux]")
{
    Scratch s;
    const auto before = scriptFolders();
    const auto run = [] (const juce::File& file, const juce::String& sum) {
        return runProcess (linuxInstallCommand (file, sum, LinuxScope::User), 30000);
    };
    const auto text = s.dir.getChildFile ("not an archive.tar.gz");
    REQUIRE (text.replaceWithText ("plain text"));
    CHECK (run (text, juce::String::repeatedString ("0", 64)).exitCode ==
           static_cast<int> (ScriptExit::Checksum));
    CHECK (run (text, sha256Hex (text)).exitCode == static_cast<int> (ScriptExit::Unpack));
    TarBuilder flat;
    flat.folder ("top/");
    flat.file ("top/README", "no install.sh");
    const auto noScript = flat.save (s.dir.getChildFile ("flat.tar.gz"));
    CHECK (run (noScript, sha256Hex (noScript)).exitCode ==
           static_cast<int> (ScriptExit::NoInstaller));
    const auto failing = linuxArchive (s.dir.getChildFile ("failing.tar.gz"),
                                       "#!/bin/sh\necho broken >&2\nexit 3\n");
    const auto r = run (failing, sha256Hex (failing));
    CHECK (r.exitCode == static_cast<int> (ScriptExit::Failed));
    CHECK (r.errors.contains ("broken")); // the end of its log
    CHECK (scriptFolders() == before);
}
#endif

#if JUCE_MAC
namespace
{
/// A disk image like Sedef-Amp-macOS.dmg, made with hdiutil, holding a stand-in pkg.
juce::File makeImage (const juce::File& dir, bool pkgIsLink)
{
    const auto src = dir.getChildFile (pkgIsLink ? "linked-image" : "image");
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
    const auto dmg = dir.getChildFile (pkgIsLink ? "linked.dmg" : "Sedef-Amp-macOS.dmg");
    const auto r =
        runProcess ({ "/usr/bin/hdiutil", "create", "-quiet", "-srcfolder", src.getFullPathName(),
                      "-volname", "Sedef Amp 0.2.0", "-format", "UDZO", dmg.getFullPathName() },
                    120000);
    INFO (r.summary());
    REQUIRE (r.succeeded());
    return dmg;
}

/// Disk images attached in an install script's folder.
juce::StringArray scriptMounts()
{
    const auto info = runProcess ({ "/usr/bin/hdiutil", "info", "-plist" }, 20000);
    juce::StringArray points;
    const auto parsed = parsePlist (info.output);
    if (const auto* images = parsed["images"].getArray())
        for (const auto& image : *images)
            if (const auto* entities = image["system-entities"].getArray())
                for (const auto& e : *entities)
                    if (const auto point = e["mount-point"].toString();
                        point.contains ("/sedef-update."))
                        points.add (point);
    return points;
}

/// Apple's installer never runs here. It stands in for it: notes what it was handed in
/// $SEDEF_TEST_OUT and exits with $SEDEF_TEST_EXIT.
const juce::String kInstallerStandIn = R"(#!/bin/sh
out="$SEDEF_TEST_OUT"
printf '%s\n' "$@" > "$out/args"
cat "$2" > "$out/pkg"
cp "$4" "$out/choices.xml"
if [ -w "$2" ]; then echo yes > "$out/writable"; fi
if [ "${SEDEF_TEST_EXIT:-0}" != 0 ]; then echo "installer: the stand-in failed" >&2; fi
exit "${SEDEF_TEST_EXIT:-0}"
)";

/// The macOS install command, with the stand-in where the script runs /usr/sbin/installer.
juce::StringArray withStandIn (juce::StringArray command, const juce::File& standIn)
{
    const auto script = command[2];
    REQUIRE (script.indexOf ("/usr/sbin/installer") >= 0);
    REQUIRE (script.indexOf ("/usr/sbin/installer") == script.lastIndexOf ("/usr/sbin/installer"));
    REQUIRE_FALSE (standIn.getFullPathName().containsChar (' '));
    command.set (2, script.replace ("/usr/sbin/installer", standIn.getFullPathName()));
    return command;
}

struct MacFixture
{
    Scratch s;
    juce::File dmg = makeImage (s.dir, false);
    juce::File out = s.dir.getChildFile ("out");
    juce::File standIn = s.dir.getChildFile ("installer");
    juce::File root = s.dir.getChildFile ("root");
    std::set<juce::String> before = scriptFolders();

    MacFixture()
    {
        REQUIRE (out.createDirectory());
        REQUIRE (standIn.replaceWithText (kInstallerStandIn, false, false, "\n"));
        REQUIRE (standIn.setExecutePermission (true));
    }

    ProcessResult run (const juce::File& image, const juce::String& sum,
                       const juce::StringArray& choices = { "vst3" })
    {
        return runProcess (
            withStandIn (macInstallCommand (image, sum, choiceChangesXml (choices)), standIn),
            120000);
    }

    /// Nothing attached and no folder left by the script.
    bool clean() const { return scriptMounts().isEmpty() && scriptFolders() == before; }
};
} // namespace

TEST_CASE ("The macOS route installs the chosen formats through the script and verifies them",
           "[update][install][macos]")
{
    MacFixture f;
    const Env outEnv ("SEDEF_TEST_OUT", f.out.getFullPathName());
    makeBundle (macBundlePath (kMacComponents[1], f.root), "0.1.0"); // vst3
    makeBundle (macBundlePath (kMacComponents[2], f.root), "0.1.0"); // au

    InstallerConfig config;
    config.root = f.root;
    config.running = macBundlePath (kMacComponents[1], f.root);

    // Stands in for osascript: the script runs as this user, with the stand-in installer, then
    // the chosen bundles are "installed" into the fake root at the new version.
    juce::StringArray command;
    juce::String prompt;
    std::optional<Elevated::Status> answer;
    std::atomic<bool> block { false }, waiting { false };
    juce::String installAs = "0.2.0";
    config.elevate = [&] (const juce::StringArray& c, const juce::String& p, Cancellation& cancel) {
        command = c;
        prompt = p;
        waiting = true;
        while (block && !cancel.isCancelled())
            juce::Thread::sleep (2);
        Elevated e;
        e.process.started = true;
        if (cancel.isCancelled())
            return e;
        if (answer)
        {
            e.status = *answer;
            return e;
        }
        e.process = runProcess (withStandIn (c, f.standIn), 120000);
        if (!e.process.succeeded())
        {
            e.status = Elevated::Status::Failed;
            e.exitCode = e.process.exitCode;
            return e;
        }
        const auto choices = parsePlist (f.out.getChildFile ("choices.xml").loadFileAsString());
        if (const auto* items = choices.getArray())
            for (const auto& item : *items)
                if (static_cast<int> (item["attributeSetting"]) == 1)
                    for (const auto& comp : kMacComponents)
                        if (item["choiceIdentifier"].toString() == comp.choice)
                            makeBundle (macBundlePath (comp, f.root), installAs);
        e.status = Elevated::Status::Ok;
        return e;
    };
    InstallRequest request { f.dmg, sha256Hex (f.dmg), "0.2.0" };
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
        CHECK (prompt == "Sedef Amp wants to install version 0.2.0.");
        REQUIRE (command.size() == 7);
        CHECK (command[2] == macInstallScript());
        CHECK (command[4] == f.dmg.getFullPathName());
        CHECK (command[5] == request.expectedHash);

        // What Apple's installer would have been handed: the pkg in the script's own mount, in
        // its own folder, read-only, and the choices next to it.
        juce::StringArray args;
        args.addLines (f.out.getChildFile ("args").loadFileAsString());
        args.removeEmptyStrings();
        REQUIRE (args.size() == 6);
        CHECK (args[0] == "-pkg");
        CHECK (args[1].startsWith ("/private/var/tmp/sedef-update."));
        CHECK (args[1].endsWith ("/mnt/" + juce::String (kMacPackageName)));
        CHECK (args[2] == "-applyChoiceChangesXML");
        const auto folder = juce::File (args[1]).getParentDirectory().getParentDirectory();
        CHECK (args[3] == folder.getChildFile ("choices.xml").getFullPathName());
        CHECK (args[4] == "-target");
        CHECK (args[5] == "/");
        CHECK (f.out.getChildFile ("pkg").loadFileAsString() == "pkg");
        CHECK_FALSE (f.out.getChildFile ("writable").exists());
        CHECK (f.out.getChildFile ("choices.xml").loadFileAsString() ==
               choiceChangesXml ({ "vst3", "au" }));
        CHECK (bundleShortVersion (macBundlePath (kMacComponents[1], f.root)) == "0.2.0");
        CHECK (bundleShortVersion (macBundlePath (kMacComponents[2], f.root)) == "0.2.0");
        CHECK_FALSE (macBundlePath (kMacComponents[0], f.root).exists()); // not asked for
        CHECK_FALSE (folder.exists());
        CHECK (f.clean());
    }
    SECTION ("cancelling the password dialog is no error")
    {
        answer = Elevated::Status::Declined;
        const auto r = makeInstaller (Platform::MacOS, config)->install (request, onStep, cancel);
        CHECK (r.outcome == InstallResult::Outcome::Declined);
    }
    SECTION ("a failed install, a refused prompt and a wrong version are errors")
    {
        auto installer = makeInstaller (Platform::MacOS, config);
        {
            const Env fail ("SEDEF_TEST_EXIT", "1");
            const auto r = installer->install (request, onStep, cancel);
            CHECK (r.error == Error::InstallFailed);
        }
        answer = Elevated::Status::Unavailable;
        CHECK (installer->install (request, onStep, cancel).error == Error::ElevationFailed);
        answer.reset();
        installAs = "0.1.9";
        const auto r = installer->install (request, onStep, cancel);
        CHECK (r.error == Error::Unverified);
        CHECK (r.detail.contains ("0.1.9"));
        CHECK (f.clean());
    }
    SECTION ("closing mid-install stops the wait at once")
    {
        block = true;
        InstallResult r;
        auto installer = makeInstaller (Platform::MacOS, config);
        std::thread worker ([&] { r = installer->install (request, onStep, cancel); });
        const bool reached = within (60000, [&waiting] { return waiting.load(); });
        const auto started = juce::Time::getMillisecondCounterHiRes();
        cancel.cancel();
        worker.join();
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < 1000);
        REQUIRE (reached);
        CHECK (r.outcome == InstallResult::Outcome::Cancelled);
    }
    SECTION ("a file that changed since it was verified is not handed on")
    {
        request.expectedHash = juce::String::repeatedString ("0", 64);
        const auto r = makeInstaller (Platform::MacOS, config)->install (request, onStep, cancel);
        CHECK (r.error == Error::ChecksumMismatch);
        CHECK (command.isEmpty());
    }
    SECTION ("running from elsewhere, or sandboxed, means Install manually")
    {
        config.running = f.s.dir.getChildFile ("build/Sedef Amp.vst3");
        CHECK (makeInstaller (Platform::MacOS, config)->manualReason() == ManualReason::Elsewhere);
        config.running = macBundlePath (kMacComponents[1], f.root);
        const Env sandbox ("APP_SANDBOX_CONTAINER_ID", "test");
        CHECK (makeInstaller (Platform::MacOS, config)->manualReason() == ManualReason::Sandboxed);
    }
}

TEST_CASE ("The macOS install script checks its own copy and leaves nothing behind",
           "[update][install][macos]")
{
    MacFixture f;
    const Env outEnv ("SEDEF_TEST_OUT", f.out.getFullPathName());
    const auto exit = [] (const ProcessResult& r) {
        INFO (r.summary());
        return r.exitCode;
    };

    // The copy's checksum, before anything is mounted.
    CHECK (exit (f.run (f.dmg, juce::String::repeatedString ("0", 64))) ==
           static_cast<int> (ScriptExit::Checksum));
    // Not a disk image.
    const auto text = f.s.dir.getChildFile ("text.dmg");
    REQUIRE (text.replaceWithText ("not an image"));
    CHECK (exit (f.run (text, sha256Hex (text))) == static_cast<int> (ScriptExit::Unpack));
    // A pkg that is a symbolic link in the image.
    const auto linked = makeImage (f.s.dir, true);
    CHECK (exit (f.run (linked, sha256Hex (linked))) == static_cast<int> (ScriptExit::NoInstaller));
    CHECK_FALSE (f.out.getChildFile ("args").exists()); // the installer never ran
    // The installer failing: its exit, and the end of the log on stderr.
    {
        const Env fail ("SEDEF_TEST_EXIT", "1");
        const auto r = f.run (f.dmg, sha256Hex (f.dmg));
        CHECK (exit (r) == static_cast<int> (ScriptExit::Failed));
        CHECK (r.errors.contains ("installer: the stand-in failed"));
    }
    CHECK (f.clean());
}
#endif

#if JUCE_WINDOWS
TEST_CASE ("A held setup can't be changed, moved or deleted", "[update][install][windows]")
{
    Scratch s;
    const auto setup = s.dir.getChildFile ("Sedef-Amp-Windows-Setup.exe");
    REQUIRE (setup.replaceWithText ("the verified setup"));
    {
        HeldFile held (setup);
        REQUIRE (held.isOpen());
        CHECK (held.sha256() == sha256Hex (setup));
        CHECK (held.sha256() == sha256Hex (setup)); // from the start again
        {
            juce::FileOutputStream writer (setup);
            CHECK_FALSE (writer.openedOk());
        }
        CHECK_FALSE (setup.moveFileTo (s.dir.getChildFile ("moved.exe")));
        CHECK_FALSE (setup.deleteFile());
        CHECK (setup.loadFileAsString() == "the verified setup");
        // Readers are welcome: Windows' loader, and Setup holding its own file.
        HeldFile second (setup);
        CHECK (second.isOpen());
        juce::FileInputStream reader (setup);
        CHECK (reader.openedOk());
    }
    CHECK (setup.deleteFile()); // let go
    CHECK_FALSE (HeldFile (setup).isOpen());

    // A real setup is many buffers long: every byte counts, once.
    juce::MemoryBlock bytes (200 * 1024 + 123);
    juce::Random random (42);
    random.fillBitsRandomly (bytes.getData(), bytes.getSize());
    const auto big = s.dir.getChildFile ("big.exe");
    REQUIRE (big.replaceWithData (bytes.getData(), bytes.getSize()));
    HeldFile held (big);
    REQUIRE (held.isOpen());
    CHECK (held.sha256() == sha256Hex (big));
    CHECK (held.sha256() == juce::SHA256 (bytes).toHexString().toLowerCase());
    CHECK (held.sha256() != sha256Of ({})); // never the hash of nothing
}

TEST_CASE ("The setup stays held only while a Setup may run", "[update][install][windows]")
{
    Scratch s;
    const auto setup = s.dir.getChildFile ("Sedef-Amp-Windows-Setup.exe");
    REQUIRE (setup.replaceWithText ("the verified setup"));
    InstallerConfig config;
    config.commonFiles = s.dir.getChildFile ("Common Files");
    config.appFolder = s.dir.getChildFile ("Sedef Amp");
    // Stands in for ShellExecuteEx, on launchElevated's thread: notes the switches, answers.
    juce::String parameters;
    std::atomic<int> launched { 0 };
    std::atomic<bool> block { false };
    auto answer = Elevated::Status::Ok;
    config.launch = [&] (const juce::File&, const juce::String& p, void*) {
        parameters = p;
        ++launched;
        while (block)
            juce::Thread::sleep (5);
        LaunchResult r;
        r.status = answer;
        return r;
    };
    const InstallRequest request { setup, sha256Hex (setup), "0.5.0" };
    Cancellation cancel;
    const auto install = [&config, &request, &cancel] {
        return makeInstaller (Platform::Windows, config)
            ->install (request, [] (InstallStep) {}, cancel);
    };

    SECTION ("No at the prompt lets it go")
    {
        answer = Elevated::Status::Declined;
        CHECK (install().outcome == InstallResult::Outcome::Declined);
        CHECK (launched == 1);
        CHECK (setup.deleteFile());
    }
    SECTION ("a failed launch lets it go")
    {
        answer = Elevated::Status::Unavailable;
        CHECK (install().error == Error::ElevationFailed);
        CHECK (setup.deleteFile());
    }
    SECTION ("a cancel before the launch lets it go")
    {
        cancel.cancel();
        CHECK (install().outcome == InstallResult::Outcome::Cancelled);
        CHECK (launched == 0);
        CHECK (setup.deleteFile());
    }
    SECTION ("without the host's creation time, no Setup starts")
    {
        config.waitPid = 0xfffffff0u; // no such process
        const auto r = install();
        CHECK (r.error == Error::ElevationFailed);
        CHECK (r.detail.contains ("started"));
        CHECK (launched == 0);
        CHECK (setup.deleteFile());
    }
    SECTION ("a Setup that started keeps it held, and waits for this very process")
    {
        const auto self = static_cast<juce::uint32> (currentProcessId());
        CHECK (install().outcome == InstallResult::Outcome::Scheduled);
        CHECK (parameters.contains (" /WAITPID=" + juce::String (self) + " "));
        CHECK (parameters.contains (" /WAITPIDTIME=" + juce::String (processCreationTime (self))));
        // Nothing is installed in the scratch folders: every component goes in.
        CHECK (parameters.contains (" /COMPONENTS=vst3,clap,standalone,midi "));
        CHECK_FALSE (setup.deleteFile()); // until this process exits
    }
    SECTION ("a prompt still open when Sedef closes keeps it, until it ends in No")
    {
        block = true;
        answer = Elevated::Status::Declined;
        InstallResult r;
        std::thread worker ([&] { r = install(); });
        const bool asked = within (10000, [&launched] { return launched.load() == 1; });
        cancel.cancel();
        worker.join();
        REQUIRE (asked);
        CHECK (r.outcome == InstallResult::Outcome::Cancelled);
        CHECK_FALSE (setup.deleteFile()); // the prompt may still say Yes
        block = false;
        CHECK (within (10000, [&setup] { return setup.deleteFile(); }));
    }
}

TEST_CASE ("Creation times tell a process from a later one with its id",
           "[update][install][windows]")
{
    const auto self = static_cast<juce::uint32> (currentProcessId());
    CHECK (processCreationTime (self) != 0);
    CHECK (processCreationTime (self) == processCreationTime (self));
    CHECK (processCreationTime (0xfffffff0u) == 0);
}

TEST_CASE ("The app opens again once the process has exited", "[update][install][windows]")
{
    Scratch s;
    const auto done = s.dir.getChildFile ("done.txt");
    const auto app = s.dir.getChildFile ("app.cmd");
    REQUIRE (app.replaceWithText ("@echo opened> \"%~dp0done.txt\"\n")); // CRLF, as cmd likes
    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    PROCESS_INFORMATION dummy {};
    std::wstring line = L"cmd.exe /c ping -n 4 127.0.0.1 >nul";
    REQUIRE (CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &startup, &dummy));
    CloseHandle (dummy.hThread);
    REQUIRE (openAfterExit (dummy.dwProcessId, app));
    CHECK (within (60000, [&done] { return done.existsAsFile(); }));
    CHECK (WaitForSingleObject (dummy.hProcess, 0) == WAIT_OBJECT_0); // it had exited first
    CloseHandle (dummy.hProcess);
}
#endif
