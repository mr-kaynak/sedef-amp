# Installing Sedef Amp

Each release has one download per OS. The links below always point to the latest release; older
versions are on the [releases page](https://github.com/mr-kaynak/sedef-amp/releases).

| OS | Download | What it is |
|---|---|---|
| macOS 11 or later, Apple silicon and Intel | [Sedef-Amp-macOS.dmg](https://github.com/mr-kaynak/sedef-amp/releases/latest/download/Sedef-Amp-macOS.dmg) | Disk image with the installer for VST3, AU, CLAP and the standalone app |
| Windows 10 or 11, x64 | [Sedef-Amp-Windows-Setup.exe](https://github.com/mr-kaynak/sedef-amp/releases/latest/download/Sedef-Amp-Windows-Setup.exe) | Installer for VST3, CLAP and the standalone app |
| Linux x86_64 | [Sedef-Amp-Linux-x86_64.tar.gz](https://github.com/mr-kaynak/sedef-amp/releases/latest/download/Sedef-Amp-Linux-x86_64.tar.gz) | VST3, CLAP and the standalone app with an install script |

After installing, rescan plugins in your DAW. Sedef shows up as **Sedef Amp** by **Sedef**.
Presets and settings live outside the install folders and survive updates and uninstalls:
`~/Library/Application Support/Sedef` on macOS, `%APPDATA%\Sedef` on Windows and `~/.config/Sedef`
on Linux.

## macOS

Open the disk image and double-click **Install Sedef Amp**. It lists the formats, all selected,
and asks for an administrator password because the plugins go into the folders shared by every
user:

| Format | Installed to |
|---|---|
| Standalone app | `/Applications/Sedef Amp.app` |
| VST3 | `/Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3` |
| AU | `/Library/Audio/Plug-Ins/Components/Sedef Amp.component` |
| CLAP | `/Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap` |

### First launch

The release notes say whether a release is notarized by Apple. Until releases are, macOS stops
the installer the first time with *"Install Sedef Amp.pkg" Not Opened*:

1. Click **Done**.
2. Open **System Settings → Privacy & Security**.
3. Under **Security**, click **Open Anyway** next to the message about the installer, then enter
   your password.

On macOS 14 and earlier you can instead Control-click the installer and choose **Open**. You only
do this once: files put in place by an installer aren't quarantined, so DAWs load the plugins and
the app opens without further prompts.

If Logic or GarageBand doesn't list the AU, run `killall -9 AudioComponentRegistrar` and reopen
the host (the installer already does this once).

### Uninstall

Double-click **Uninstall Sedef Amp** in the disk image. It opens in Terminal, lists what it will
remove and asks before doing so: the app and plugins in the system folders, and any copies in
`~/Library/Audio/Plug-Ins` or `~/Applications` (where 0.1.0 was copied by hand). If macOS blocks
the script, run it from Terminal:

```sh
bash "/Volumes/Sedef Amp <version>/Uninstall Sedef Amp.command"
```

### Manual install (no administrator password)

The installer's contents can be unpacked without running it:

```sh
pkgutil --expand-full "/Volumes/Sedef Amp <version>/Install Sedef Amp.pkg" ~/Desktop/sedef-amp
```

Each format is then in `~/Desktop/sedef-amp/sedef-amp-<format>.pkg/Payload/`. Copy the plugins to
`~/Library/Audio/Plug-Ins/VST3`, `~/Library/Audio/Plug-Ins/Components` and
`~/Library/Audio/Plug-Ins/CLAP`, and the app to `~/Applications`. If macOS then reports a plugin
as damaged, clear the download flag once, e.g.
`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Sedef Amp.vst3"`.

## Windows

Run **Sedef-Amp-Windows-Setup.exe** and pick the formats on *Select Components* (all are selected):

| Format | Installed to |
|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\Sedef Amp.vst3` |
| CLAP | `C:\Program Files\Common Files\CLAP\Sedef Amp.clap` |
| Standalone app | `C:\Program Files\Sedef Amp\Sedef Amp.exe`, with a Start menu shortcut |

The installer isn't code-signed yet, so SmartScreen may say *Windows protected your PC*: click
**More info**, then **Run anyway**.

The interface runs in Microsoft Edge WebView2, which ships with Windows 10 and 11. On older
systems, install the [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).

If a DAW has the plugin loaded while you update, Setup lists it and offers to close it; save your
project first. If you keep it open, the new files are put in place when you restart Windows.

Uninstall from **Settings → Apps → Installed apps → Sedef Amp**. For unattended installs, the
installer takes the usual Inno Setup switches, e.g.
`Sedef-Amp-Windows-Setup.exe /VERYSILENT /NORESTART /COMPONENTS="vst3,clap,standalone"`. Close
DAWs first: a silent install closes programs that hold the plugin, and without `/NORESTART` it
restarts Windows on its own if a file was still in use.

## Linux

```sh
tar xzf Sedef-Amp-Linux-x86_64.tar.gz
cd sedef-amp-*-linux-x86_64
./install.sh             # for you only
./install.sh --system    # for every user; asks for sudo
```

| | `./install.sh` | `./install.sh --system` |
|---|---|---|
| VST3 | `~/.vst3/Sedef Amp.vst3` | `/usr/lib/vst3/Sedef Amp.vst3` |
| CLAP | `~/.clap/Sedef Amp.clap` | `/usr/lib/clap/Sedef Amp.clap` |
| Standalone app | `~/.local/bin/sedef-amp` | `/usr/local/bin/sedef-amp` |
| Menu entry and icon | `~/.local/share` | `/usr/local/share` |

The interface needs WebKitGTK 4.1 (`libwebkit2gtk-4.1-0` on Debian and Ubuntu, `webkit2gtk4.1`
on Fedora); `install.sh` warns if it's missing. The standalone app uses ALSA or JACK. The update
check loads `libcurl.so.4` when it runs (`libcurl4` on Debian and Ubuntu, `libcurl` on Fedora);
without it Sedef works but can't check for updates.

To uninstall, run `~/.local/share/sedef-amp/uninstall.sh`, or
`/usr/local/share/sedef-amp/uninstall.sh --system` for a system-wide install.

For a manual install, copy `VST3/Sedef Amp.vst3` to `~/.vst3` and `CLAP/Sedef Amp.clap` to
`~/.clap`; `Standalone/Sedef Amp` runs from anywhere.

## Building from source

See [Build from source](../README.md#build-from-source) in the README. Configuring with
`-DSEDEF_COPY_PLUGIN=ON` copies the plugins into your user plugin folders after each build. How
the release packages are made is in [releasing.md](releasing.md).

## Checking the download

`SHA256SUMS.txt` in each release lists a checksum for every download:

```sh
shasum -a 256 -c SHA256SUMS.txt --ignore-missing  # macOS
sha256sum -c SHA256SUMS.txt --ignore-missing      # Linux
```

On Windows, compare the output of `certutil -hashfile Sedef-Amp-Windows-Setup.exe SHA256` with the
line in `SHA256SUMS.txt`.
