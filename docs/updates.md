# Updates

Sedef Amp can tell you when a new version is out and install it for you. You click **Update
now** and answer your OS's own password or permission prompt; Sedef downloads the release,
verifies it and hands it to the OS's installer, which replaces the formats you have. Sedef never
writes into plug-in folders itself: a plug-in's binaries are loaded, and on Windows locked, by your
DAW, and only the OS's installer (Apple's `installer`, the Inno Setup program, `install.sh`) writes
there.

![The Updates panel](images/updates.png)

This works from the first release after 0.4.0 on. Copies of 0.2.0, 0.3.0 and 0.4.0 still download
the installer into Downloads and open it, as described under [Install manually](#install-manually),
for their next update; once a newer release is installed, every later update installs this way.

## What happens

1. **Check.** When you open a Sedef window, and at most once a day, Sedef asks GitHub for the
   project's latest release. Sedef doesn't check while your DAW scans or loads the plug-in, and the
   check never runs on the audio thread or holds up the window. However many instances your session
   holds, one check runs, and a small file (`update.json`, below) keeps other processes and later
   sessions from repeating it. After a failed check Sedef waits an hour before trying again.
2. **Notice.** When the latest stable release is newer than the one you run, a small "Update
   available" pill appears at the right end of the section tabs, under the header's tools. Drafts
   and pre-releases are never offered. The pill opens the Updates panel (also in Settings →
   Updates) with the release notes and three choices: **Update now**, **Skip this version** (no
   pill for that version again) and **Later** (no pill until you next open Sedef).
3. **Download.** Sedef downloads `SHA256SUMS.txt` and this OS's installer from the same release
   into a private folder of its own (below), checks the size the release lists and the installer's
   SHA-256 against that list. A file that doesn't match is deleted. **Cancel** stops the download.
   You can close the panel meanwhile; the pill reads "Updating…" until it's done.
4. **Install.** Sedef checks the file's SHA-256 once more and installs it through the OS's own
   installer:

   | OS | File | What Sedef does | What you see |
   | --- | --- | --- | --- |
   | macOS | `Sedef-Amp-macOS.dmg` | runs one fixed install script as root, which copies the disk image into a folder of its own, checks the copy's SHA-256, mounts it read-only and hidden, and runs Apple's `/usr/sbin/installer` on `Install Sedef Amp.pkg` for the formats you have installed (plus the one Sedef runs as), deselecting the others; then it detaches the image and deletes its folder | macOS asks for an administrator's password ("Sedef Amp wants to install version 0.5.0."). Then Sedef checks that every format has the new version |
   | Windows | `Sedef-Amp-Windows-Setup.exe` | starts the setup program elevated and silent with the formats installed now (plus the one Sedef runs as), told to wait until your DAW (this process) has exited, because Windows keeps a loaded plug-in locked | Windows asks for permission (UAC). The panel says *It installs when you close your DAW.*; Setup installs once you do |
   | Linux (x86-64) | `Sedef-Amp-Linux-x86_64.tar.gz` | checks what the archive holds (no links, devices or paths outside it), then runs one fixed install script, which copies the archive into a folder of its own, checks the copy's SHA-256, unpacks it and runs its `install.sh`: as you for a copy in `~/.vst3`, `~/.clap` and `~/.local/bin`, or as root through `pkexec` with `--system` for one in `/usr/lib` | nothing for your own copy; your desktop's password prompt (polkit) for a system-wide one. Then Sedef checks every file it installed |

   **Sedef MIDI** comes in the same package and is updated with Sedef Amp:

   | OS | When Sedef MIDI is installed | When it isn't (left out, or removed) | Updating from 0.5.0 or earlier |
   | --- | --- | --- | --- |
   | macOS | the pkg's `midi` choice is selected: all of it is updated | the `midi` choice is deselected: nothing is added | that updater doesn't know the choice, so the pkg's default applies: Sedef MIDI is installed |
   | Windows | the `midi` component is passed to Setup: all of it is updated | not passed: nothing is added | that updater names only the formats it knows: Sedef MIDI is not installed (run the setup once to add it) |
   | Linux | `install.sh` updates it | `install.sh --no-midi` left a mark that keeps it out; a copy removed by hand comes back | the new `install.sh` runs: Sedef MIDI is installed |

   Sedef MIDI has no updater of its own: it is updated when Sedef Amp is.

5. **Restart.** The panel ends on *Sedef Amp 0.5.0 is installed. Restart \<your DAW\> to use it.*
   (Sedef names the host when it knows it.) The pill now reads "Restart to update", in this window
   and in every other process that still runs the old version: another DAW, a new window. The first
   process that runs the new version clears that. In the standalone app, **Restart Sedef** quits it
   and opens the new version; on Windows Setup reopens it once it has installed.

Cancelling the password or permission prompt is not an error: the panel is ready to install again,
and the verified download is reused. Closing the last Sedef window, or unloading the plug-in,
while Sedef installs stops Sedef waiting, at once: a password dialog still open goes away, and an
install already under way finishes by itself (on Windows, a permission prompt stays up, and Setup
runs if you allow it). Keep the window open until the panel says it is installed, so Sedef can
check the result.

### Install manually

When the automatic route fails, or can't run here, the panel offers **Install manually**, which is
how 0.2.0 to 0.4.0 update: Sedef copies the verified installer into your Downloads folder with
the "downloaded from the internet" mark a browser gives it, and opens it (macOS: the disk image,
Windows: the setup program) or, on Linux, shows the archive in your file manager. You run the
installer yourself after quitting your DAW. The releases page is always one click away too.

The automatic route can't run, and the panel says why, when:

- this copy runs from a folder the installer doesn't update: a build folder, a copy you put in
  `~/Library/Audio/Plug-Ins` yourself (0.1.0 shipped that way), a VST3 or CLAP outside Windows'
  `Common Files`, a Linux copy outside the folders `install.sh` uses;
- the host's sandbox keeps a plug-in from asking for a password (macOS);
- a system-wide Linux install has no `pkexec`, or no polkit agent runs.

The installer copy carries the OS's downloaded-file mark, so the OS checks it before it runs, as
with any download. Until releases are signed ([signing status](releasing.md#signing-status)),
macOS says it can't open the installer: go to **System Settings → Privacy & Security** and click
**Open Anyway**. On Windows, SmartScreen may say *Windows protected your PC*: click **More info**,
then **Run anyway**. On Windows the interactive setup lists a DAW that still holds the plug-in and
offers to close it, or finishes the update when you restart Windows.

If a release has no installer for your system (for example on ARM Linux, or v0.1.0, which shipped
zips), the panel offers the releases page instead.

## What is sent

One HTTPS request per check, and nothing else:

```
GET https://api.github.com/repos/mr-kaynak/sedef-amp/releases/latest
Accept: application/vnd.github+json
X-GitHub-Api-Version: 2022-11-28
User-Agent: SedefAmp/0.7.0 (macOS)
```

The User-Agent carries the Sedef version and the OS family, nothing more: no identifier, no OS
version, no host or DAW name, no settings. Sedef adds no cookies. On Windows and Linux the
system's HTTP stack keeps none either; on macOS requests go through the system's URL loading,
which shares the host application's cookie store, so a cookie GitHub set for these addresses
would be sent back on later requests (GitHub sets none for them today). Like any web request
the check reaches GitHub from your IP address; [GitHub's privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement)
covers what GitHub does with it. Sedef keeps no record of you anywhere.

Only when you click **Update now** or **Install manually**, Sedef fetches `SHA256SUMS.txt` and the
installer from `https://github.com/mr-kaynak/sedef-amp/releases/download/<tag>/`, with the same
User-Agent. GitHub redirects these to its download servers. Sedef follows a redirect only over
HTTPS to `github.com`, `objects.githubusercontent.com` or `release-assets.githubusercontent.com`,
or within the server it asked, at most five times. Installing sends nothing.

## Turning it off

- **In Sedef:** Settings → Updates, switch off **Check for updates automatically**. **Check now**
  still works when you ask for it.
- **Per machine or in CI:** set the environment variable `SEDEF_DISABLE_UPDATE_CHECK=1`. Sedef
  then never checks on its own. The editor smoke test (`SEDEF_UI_SMOKE_TEST`) has the same effect.
- **At build time:** configure with `-DSEDEF_UPDATE_CHECK=OFF`. The checker is not compiled in
  at all, and the UI shows nothing about updates. Use this for distribution packages that update
  through the package manager.

On Linux the check uses the system's libcurl (`libcurl.so.4`: `libcurl4` on Debian and Ubuntu,
`libcurl` on Fedora), loaded only when a check runs. Without it Sedef works as usual and the
Updates panel says it couldn't reach GitHub.

## Where things are kept

`update.json` holds the two settings (automatic checks, skipped version), the time of the last
check, the release it found and, between an install and the first process that runs it, the
installed version (`installedVersion`). It is global for your user account and never part of a
DAW project: the plug-in state your DAW saves does not change.

| OS | `update.json` | Downloads for Update now |
| --- | --- | --- |
| macOS | `~/Library/Application Support/Sedef/` | `~/Library/Caches/Sedef/Updates/` |
| Windows | `%APPDATA%\Sedef\` | `%LOCALAPPDATA%\Sedef\Updates\` |
| Linux | `~/.config/Sedef/` | `${XDG_CACHE_HOME:-~/.cache}/sedef/updates/` |

`update.json` is written to a temporary file and renamed into place, under a lock shared between
processes. The window never waits for that lock: if another process holds it when you change a
setting, the setting applies at once and Sedef's update thread writes it. Deleting the file resets
the settings and makes the next window open check again.

The download folder is created with mode 0700 on macOS and Linux (Windows' per-user folders are
private already) and holds the current installer only. An installed update's file is deleted; a
download for a version you then don't install is deleted once a check finds nothing newer. On
Windows the setup stays there until it has run, and while it is held (below) a newer download takes
the next free name. The install script's own folder (`/private/var/tmp/sedef-update.*` on macOS,
`/var/tmp/sedef-update.*` on Linux) exists only while it runs.

**Install manually** saves to `~/Downloads` (on Linux the `XDG_DOWNLOAD_DIR` folder), or to your
home folder when there is none. A verified copy already there is reused; a different file with the
same name is left alone and the copy is saved as `<name> (2)`.

## Threat model

**What protects the download.** The request and the downloads use HTTPS with certificate checks by
the system (NSURLSession on macOS, WinINet on Windows, libcurl on Linux). Sedef only downloads from
this repository's release download URLs for the tag it found, and only links to this repository's
releases page, and follows redirects only to GitHub's download hosts (above). The installer must
match the SHA-256 that `SHA256SUMS.txt` of the same release lists, and the size the release lists,
or it is deleted before it is ever used. A release whose installer lists no size or more than
1 GiB is not downloaded, and a checksum list that names a file twice with different hashes counts
as no checksum. Response sizes are capped (2 MB for the API, 1 MB for the checksum list), the
check and the checksum list time out after 10 s, and a download fails after 60 s without data.

That catches corruption, truncated downloads, a cache or proxy serving the wrong bytes, and files
from a different release.

**What runs with elevated rights, and when.**

- Nothing installs without your click on **Update now** followed by the OS's own password or
  permission prompt. Sedef never stores, sees or caches a password.
- What runs elevated is fixed: on macOS and Linux one constant install script (in
  `plugin/src/update/InstallLogic.cpp`), run through `/usr/bin/osascript`'s "with administrator
  privileges" or `/usr/bin/pkexec`; on Windows the verified setup program, through
  `ShellExecuteEx` with `runas`. An install for your own folders on Linux runs the same script as
  you.
- **Nothing your own programs can write is used once you have given the password.** Your other
  programs run as you, so the private download folder doesn't stop them. So, as root, the script
  makes a folder only root can open (`mktemp -d`, `umask 077`), copies the file into it, checks
  the copy's SHA-256 against the one Sedef verified, and mounts (macOS) or unpacks (Linux) that
  copy and installs from it; it writes the choice changes there too. Swapping the downloaded file,
  a mount or an unpacked file while the password dialog is open changes nothing: the copy fails its
  check, or the swapped files are never read. The script detaches the image and deletes its folder
  on every way out. On Windows, Sedef opens the setup with no write and no delete sharing, checks
  its SHA-256 through that handle and keeps it open until Sedef's process exits; with `/WAITPID`,
  Setup opens its own file the same way before it waits, while Sedef still holds it, and keeps it
  until it has finished. Nobody can change, replace or rename the setup between the check and the
  end of the install.
- No string from the network, the release or the file system is ever spliced into shell,
  AppleScript or PowerShell text. Every command is an argv. The scripts are constants; the file,
  its SHA-256 and the choice changes reach them as arguments (on macOS through `on run argv` and
  `quoted form of`). Every program runs through `posix_spawn` with no descriptor of the host's
  beyond stdin, stdout and stderr. The Windows switches are names and numbers Sedef writes itself.
- A Linux archive is read before anything runs, from the very bytes whose SHA-256 was checked:
  links, devices, absolute paths, `..`, colons and backslashes, duplicates, data in anything but a
  file, more than 20000 headers and more than 1 GiB unpacked are refused, and the layout must be
  what `install.sh` expects. The script unpacks only a copy with that SHA-256, so it unpacks what
  was checked, with `tar --no-same-owner --no-same-permissions`.
- After an install Sedef checks the result: `CFBundleShortVersionString` of every macOS bundle it
  installed; on Linux, every file of the VST3, CLAP and standalone app against the archive's, by
  SHA-256, and the VST3's `moduleinfo.json`. A mismatch is reported.
- On Windows, `/WAITPIDTIME` carries the creation time of the process to wait for: if its id has
  been reused by another process by the time Setup starts, Setup doesn't wait for that one. Sedef
  never starts Setup without it: if it can't read when the host started, it installs nothing and
  offers Install manually. The setup file stays held only once a Setup has started (or while the
  permission prompt is still open); after No or a failure it is let go. The
  wait has no limit, as the panel says (*It installs when you close your DAW.*).

**What it does not protect against.**

- Someone who controls the GitHub account, the repository or a release workflow token can publish
  a malicious installer together with a matching `SHA256SUMS.txt`. The checksum comes from the
  same place as the file, so it proves the file is the one published, not who published it. With
  Update now, such an installer would run with administrator rights after your password, as it
  would if you ran it yourself.
- Until releases are signed, the automatic route skips the Gatekeeper and SmartScreen prompts that
  an unsigned download gets, which users were told to click through anyway: neither prompt checks
  a publisher for an unsigned file.
- A program that already runs as you can still change your DAW, your plug-in folders in your home
  folder (Linux), or what you type into the password dialog's owner. The install keeps it from
  turning your password into root for its own code, not from everything a program running as you
  can do.

**The next layers.**

- **Code signing and notarization** (Developer ID and notarization on macOS, Authenticode on
  Windows) make the OS check the publisher. The release workflow turns them on once signing
  secrets exist; until then the installers are unsigned. The current state is in
  [docs/releasing.md](releasing.md#signing-status). Once they exist, Update now should also require
  the pkg's Developer ID Installer signature (`pkgutil --check-signature`) or the setup's
  Authenticode signature from the expected team before it installs; not implemented yet, since
  there is no signature to check.
- **A signed checksum list**: a signature over `SHA256SUMS.txt`, checked against a public key
  built into Sedef, would make a compromised GitHub account insufficient on its own. Not
  implemented yet.

**Other properties.**

- Sedef never writes into plug-in folders itself.
- Closing the last Sedef window or unloading the plug-in never hangs: the update thread is
  cancelled, and every wait ends at once. A password dialog goes away with the program that shows
  it; an install that runs as root by then can't be stopped by Sedef and finishes by itself, owned
  by the system from then on; an install in your own folders is left to finish rather than stopped
  halfway. On Windows the permission prompt waits on a thread of its own that keeps the plug-in's
  code loaded until the prompt is answered.
- The copy that **Install manually** saves gets the OS's downloaded-file mark, as a browser
  download would: the quarantine attribute on macOS (Gatekeeper checks it before it opens), the
  `Zone.Identifier` stream with the Internet zone on Windows (SmartScreen and the attachment
  manager check it). A file that already has a mark keeps it. File systems without extended
  attributes or alternate streams (FAT32, some network drives) cannot carry the mark; the SHA-256
  check still applies. The private copy for Update now carries no mark: Sedef verified it and
  hands it to the installer itself.
- Release notes are untrusted text. The panel renders a small Markdown subset (headings, lists,
  bold, italics, code, links) as plain elements, never as HTML. Only `https` links become links,
  and they open in your browser through the plug-in, never inside its window. The parser does
  linear work per line, and shows lines over 1000 characters as plain text, so hostile notes
  cannot stall the panel.

## For developers

| Where | What |
| --- | --- |
| `plugin/src/update/UpdateLogic.*` | versions, the release model, installer names, checksum lists, the throttle, `update.json`: no network, no threads |
| `plugin/src/update/UpdateClient.*` | the GET, the verified download (blocking, cancellable, redirects checked by hand) and the verified copy for Install manually |
| `plugin/src/update/InstallLogic.*` | the cache, the macOS formats and choice changes, the install scripts and their exit codes, the `osascript` argv, Setup's switches and components, the tar reader and its checks: no processes |
| `plugin/src/update/InstallProcess.*` | argv-only child processes (`posix_spawn`, no inherited descriptors) under a shell that always dies at a kill, so a cancel never waits |
| `plugin/src/update/Installer.*` | the install per OS, how it asks for permission (`osascript`, `pkexec`, `ShellExecuteEx` on its own thread), the held setup file and the restart after exit on Windows |
| `plugin/src/update/DownloadMark.*` | the quarantine attribute (macOS) and `Zone.Identifier` stream (Windows) on Install manually's copy |
| `plugin/src/update/UpdateService.*` | the per-process service: worker thread, state, `update.json` locking |
| `plugin/src/update/UpdateBridge.*` | the editor's `sedef_update_*` functions and `sedef:update` events, the host's name |
| `tools/packaging/windows/sedef-amp.iss` | `/WAITPID`, `/WAITPIDTIME`, the held setup file and `/RELAUNCH` |
| `plugin/tests/` | `sedef_update_tests`: the logic, the client against a local HTTP server, the service with a scripted installer, and on macOS and Linux the real routes with a stand-in for the prompt |
| `plugin/tools/sedef_update_check.cpp` | `sedef-update-check`: the same code against the real API, and `--install` |
| `tools/ci/install-test.sh` | the release workflow's install test (CI machines only) |
| `ui/src/features/updates/` | the header pill, the Updates panel, the Markdown renderer |
| `ui/src/bridge/mock/updateSim.ts` | the mock backend's updates (`?update=` in the browser) |

```sh
cmake --build --preset release --target sedef_update_tests sedef-update-check
ctest --test-dir build/release -R update
build/release/plugin/sedef-update-check_artefacts/Release/sedef-update-check --current 0.0.1
build/release/plugin/sedef-update-check_artefacts/Release/sedef-update-check --download /tmp/sedef-dl
```

`--download <folder>` fetches and verifies this OS's installer the way the plug-in does;
`--asset <name>` picks another file that `SHA256SUMS.txt` lists, and `--platform`/`--arm` pretend
to be another system. Each run makes one API request; GitHub allows 60 an hour without a token.

`--install <file>` runs the automatic install on a package you built, without the API: it asks
for root with `sudo -n` where the plug-in shows the OS's prompt (that difference is compiled into
this tool only, never into the plug-in, and no environment variable switches it), so it installs
into your system's plug-in folders. Leave it to CI: `release.yml` runs it on every OS through
`tools/ci/install-test.sh` ([docs/releasing.md](releasing.md#install-test)). `--expect <version>`
is the version to check for, `--system` installs Linux system-wide, and on Windows
`--waitpid <pid>` is the process Setup waits for (`--waitpid-time <FILETIME>` overrides its
creation time) and `--log <file>` keeps Setup's log.

The unit tests never ask for a password or install anything: they run the real install scripts as
you, the macOS one with a stand-in where it runs `/usr/sbin/installer` (it copies, checks, mounts
and detaches real disk images made with `hdiutil create`) and a stand-in for `osascript` that
installs into a fake root; the Linux one with a stand-in `install.sh`.

In the browser preview, `?update=available` shows a pending update (also `current`, `skipped`,
`error`, `noinstaller`, `mismatch` and `unsupported`), and the install's states: `declined` (the
password dialog is cancelled), `installfail`, `elsewhere`, `installed` (by another window),
`standalone`, `windows`, `scheduled` and `linux`.

The installer names above are a contract with every installed copy of Sedef: renaming an asset in
`.github/workflows/release.yml` breaks one-click updates for everyone who has not updated yet
(they fall back to the releases page). So are the pkg's choice ids (`app`, `vst3`, `au`, `clap`,
`midi`), the Inno Setup `AppId`, its component names (`vst3`, `clap`, `standalone`, `midi`), the
Linux `install.sh` options and its `no-midi` mark, and the `/WAITPID`,
`/WAITPIDTIME` and `/RELAUNCH` switches, which the installed copies pass to the next release's
installer.
