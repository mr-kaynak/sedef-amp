# Installing Sedef Amp

Each release has one zip per OS. Inside are the plugin formats and the standalone app; copy the
ones you use.

| OS | VST3 | AU | CLAP | Standalone |
|---|---|---|---|---|
| macOS | `~/Library/Audio/Plug-Ins/VST3` | `~/Library/Audio/Plug-Ins/Components` | `~/Library/Audio/Plug-Ins/CLAP` | `/Applications` |
| Windows | `C:\Program Files\Common Files\VST3` | — | `C:\Program Files\Common Files\CLAP` | anywhere |
| Linux | `~/.vst3` | — | `~/.clap` | anywhere |

Rescan plugins in your DAW afterwards. Sedef shows up as **Sedef Amp** by **Sedef**.

## macOS: unsigned builds

Release builds are ad-hoc signed but not notarized, so macOS quarantines them after download.
Clear the flag once after copying:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Sedef Amp.vst3"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Sedef Amp.component"
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/CLAP/"Sedef Amp.clap"
xattr -dr com.apple.quarantine /Applications/"Sedef Amp.app"
```

If Logic or GarageBand still doesn't list the AU, run `killall -9 AudioComponentRegistrar` and
reopen the host.

## Windows

The UI runs in Microsoft Edge WebView2, which ships with Windows 10 and 11. On older systems,
install the [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).

## Linux

The UI needs WebKitGTK 4.1 (`libwebkit2gtk-4.1-0` on Debian and Ubuntu, `webkit2gtk4.1` on
Fedora). The standalone app uses ALSA or JACK.

## Checking the download

`SHA256SUMS.txt` lists a checksum for every zip:

```sh
sha256sum -c SHA256SUMS.txt --ignore-missing    # Linux
shasum -a 256 -c SHA256SUMS.txt --ignore-missing  # macOS
```
