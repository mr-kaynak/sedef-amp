<!--
  Template for the GitHub release notes; tools/packaging/release-notes.sh fills it in.
  @TAG@ is the release tag, @CHANGELOG@ (alone on a line) the CHANGELOG section for the version
  and @MACOS_FIRST_LAUNCH@ the macOS first-launch advice, which depends on notarization.
  The asset names are stable: the README links to releases/latest/download/<name>.
-->
## Download

| Your computer | Download | Then |
|---|---|---|
| **macOS** 11 or later, Apple silicon and Intel | [**Sedef-Amp-macOS.dmg**](https://github.com/mr-kaynak/sedef-amp/releases/download/@TAG@/Sedef-Amp-macOS.dmg) | Open it and double-click **Install Sedef Amp** |
| **Windows** 10 or 11, x64 | [**Sedef-Amp-Windows-Setup.exe**](https://github.com/mr-kaynak/sedef-amp/releases/download/@TAG@/Sedef-Amp-Windows-Setup.exe) | Run it and follow the steps |
| **Linux** x86_64 | [**Sedef-Amp-Linux-x86_64.tar.gz**](https://github.com/mr-kaynak/sedef-amp/releases/download/@TAG@/Sedef-Amp-Linux-x86_64.tar.gz) | Unpack it and run `./install.sh` |

Every installer lets you pick the formats: VST3, CLAP and the standalone app, plus AU on macOS.
Afterwards, rescan plugins in your DAW and look for **Sedef Amp** by **Sedef**.

## What's new

@CHANGELOG@

## First launch

**macOS:** @MACOS_FIRST_LAUNCH@

**Windows:** the installer isn't code-signed yet, so SmartScreen may show *Windows protected your
PC*. Click **More info**, then **Run anyway**.

**Linux:** the interface needs WebKitGTK 4.1 (`libwebkit2gtk-4.1-0` on Debian and Ubuntu,
`webkit2gtk4.1` on Fedora); `install.sh` tells you if it's missing. `./install.sh --system`
installs for every user.

**Copied Sedef Amp into your plugin folders by hand before (0.1.0)?** On macOS, run
**Uninstall Sedef Amp** from the disk image first so your DAW doesn't list two copies. The
Windows and Linux installers replace the old copy. Presets are kept.

Manual install, uninstalling and building from source:
[docs/install.md](https://github.com/mr-kaynak/sedef-amp/blob/@TAG@/docs/install.md).
`SHA256SUMS.txt` has a checksum for every download.
