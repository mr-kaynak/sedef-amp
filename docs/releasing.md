# Releasing Sedef Amp

Pushing a `v*` tag runs [.github/workflows/release.yml](../.github/workflows/release.yml), which
builds every OS, packages it with the scripts in [tools/packaging](../tools/packaging) and
publishes a GitHub release. The same scripts run locally.

## What a release contains

| Asset | Contents |
|---|---|
| `Sedef-Amp-macOS.dmg` | `Install Sedef Amp.pkg` (standalone app, VST3, AU and CLAP, universal), `Read Me.txt` and `Uninstall Sedef Amp.command` |
| `Sedef-Amp-Windows-Setup.exe` | Inno Setup installer for VST3, CLAP and the standalone app (x64), with an uninstaller |
| `Sedef-Amp-Linux-x86_64.tar.gz` | VST3, CLAP and the standalone app with `install.sh` / `uninstall.sh`, a menu entry and an icon |
| `SHA256SUMS.txt` | `sha256sum` output for the three packages |

The names carry no version on purpose: the README and docs link to
`https://github.com/mr-kaynak/sedef-amp/releases/latest/download/<asset>`, and the in-app updater
downloads these exact names, refuses a file without a matching line in that release's
`SHA256SUMS.txt` and installs it ([updates.md](updates.md)). Keep the names and the checksum file in every release; renaming an asset means
changing the README, [install.md](install.md), [.github/release-notes.md](../.github/release-notes.md)
and the update check with it. The update check also skips tags with a pre-release suffix, so a
regular release tag is `vX.Y.Z`.

Some identifiers must stay fixed so upgrades and uninstalls find earlier installs: the Inno Setup
`AppId` in `tools/packaging/windows/sedef-amp.iss` and the macOS package IDs
`io.github.mr-kaynak.sedef-amp.{app,vst3,au,clap}`. Installed copies of Sedef also pass the next
release's installer the pkg's choice ids (`app`, `vst3`, `au`, `clap` in
`tools/packaging/macos/distribution.xml.in`), Setup's component names (`vst3`, `clap`,
`standalone`) and its `/WAITPID`, `/WAITPIDTIME` and `/RELAUNCH` switches: keep them too.

## Signing status

| Package | Today | Once set up |
|---|---|---|
| macOS | Bundles ad hoc signed; installer and disk image unsigned and not notarized | Developer ID signatures with hardened runtime, notarized and stapled ([below](#macos-signing-and-notarization)); switches on when the secrets exist |
| Windows | Installer and binaries unsigned; SmartScreen warns | Code signing, not planned in detail yet ([below](#windows-code-signing)) |
| Linux | Unsigned archive | No signing planned |

Until then, the only integrity check on a download is `SHA256SUMS.txt`, which comes from the same
GitHub release as the files. It catches a broken or swapped download, but not a compromised
release or repository. The release notes say which state each release is in.

## Cutting a release

1. Set the version in the top-level `CMakeLists.txt` (`project(SedefAmp VERSION x.y.z ...)`).
2. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [x.y.z] - YYYY-MM-DD`, add a new empty
   `## [Unreleased]` above it and update the compare links at the bottom.
3. Commit to `main` and wait for CI.
4. Optional but cheap: a dry run (below) to check the packages and the release notes.
5. Tag and push:

   ```sh
   git tag -a vx.y.z -m "Sedef Amp x.y.z"
   git push origin vx.y.z
   ```

The workflow fails early if the tag doesn't match the CMake version. A tag with a suffix, such as
`v0.3.0-rc1`, becomes a pre-release; its notes take the CHANGELOG section named after the full
version (`## [0.3.0-rc1]`) or fall back to a link to the CHANGELOG.

The release notes are [.github/release-notes.md](../.github/release-notes.md) with the download
table, the CHANGELOG section and the first-launch advice filled in by
`tools/packaging/release-notes.sh`. The CHANGELOG's relative links (`docs/dsp/nam.md`) would
resolve against the release page, so the script points them at the files as of the tag.

## Dry run

Run the workflow by hand to build and upload every package as a workflow artifact without
creating a tag or a release:

```sh
gh workflow run release.yml --ref main      # or any branch
gh run watch                                # pick the run
gh run download <run-id> --dir release-dry-run
```

The run's artifacts are `package-macOS`, `package-Windows`, `package-Linux` and
`release-preview` (the rendered `notes.md` and `SHA256SUMS.txt`). Its release notes use the
version from `CMakeLists.txt`.

## Install test

After packaging, each build job installs the package it just built through the in-app updater's
own code (`sedef-update-check --install`, see [updates.md](updates.md#for-developers)), driven by
[tools/ci/install-test.sh](../tools/ci/install-test.sh). It runs on the dry run and on every tag;
a failure stops the release before anything is published.

| OS | What it checks |
|---|---|
| macOS | Installs the previous release (`OLD_TAG`: the latest one published, which users update from) with `sudo installer`, then the new disk image the way **Update now** does: the formats installed now are chosen, and the install script runs as root with `sudo -n` in place of the password prompt (copy, checksum, mount, Apple's installer, detach). Every bundle must have the new version and the new pkg's binary, and no disk image or script folder may stay behind. Then it removes the AU, puts the previous release's VST3 back and installs again: the VST3 must be replaced and the AU must stay removed. |
| Windows | Installs the previous release with VST3 and CLAP only, passing the update's switches, which that setup doesn't know and must ignore. Then the new setup starts elevated with the update's switches and a dummy process as `/WAITPID`: while it waits, nothing may write or move the setup file; it must wait for that process, install this build's VST3 and CLAP binaries and leave the standalone app out (`/COMPONENTS` names the formats found). Then the setup runs alone: it must hold its own file while it waits, and must not wait for a process whose creation time differs from `/WAITPIDTIME` (a reused id). |
| Linux | Installs the previous release for the runner's user and system-wide, then the new archive the way Update now does, in the home folder and then system-wide through the root script (with `sudo -n` in place of `pkexec`). The VST3's version and every binary must be the new build's, the system copy root's, and no script folder may stay behind. |

The script installs into the system's plug-in folders with root rights, which CI machines grant
without a password. Don't run it on your own computer; `sedef_update_tests` covers the same code
without installing anything.

## Building the packages locally

Build the plugin first, then run the packaging script for your OS. Each script writes to the
output folder you give it.

macOS (needs the Xcode command line tools and `python3`; `dmgbuild` is installed into a virtual
environment under `$TMPDIR` on first use):

```sh
export CPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake --preset release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build --preset release --target SedefAmp_All SedefAmp_CLAP
tools/packaging/macos/package.sh build/release/plugin/SedefAmp_artefacts/Release dist
```

Windows, in Git Bash with [Inno Setup 6](https://jrsoftware.org/isinfo.php) installed:

```sh
tools/packaging/windows/build-installer.sh build/release/plugin/SedefAmp_artefacts/Release dist
```

Linux:

```sh
tools/packaging/linux/package.sh build/release/plugin/SedefAmp_artefacts/Release dist
```

Preview the release notes with `tools/packaging/release-notes.sh v<version>`. The images (app
icon, DMG background, installer background) are rendered from their sources in
`tools/packaging/assets` by `render-assets.sh`; commit the PNGs after changing a source.

In CI, the Windows runner image ships Inno Setup; if it ever doesn't, `build-installer.sh`
installs the version pinned in the script with Chocolatey.

## macOS signing and notarization

Without signing secrets (the state today), the bundles get ad hoc signatures and the installer is
unsigned. Installed files aren't quarantined, so this works, but macOS stops the installer once
and the release notes explain **Open Anyway** in Privacy & Security.

With the six secrets below, the macOS build instead:

- signs the app and plugins with the Developer ID Application certificate, hardened runtime and
  a secure timestamp (the app gets the microphone entitlement from
  `tools/packaging/macos/entitlements.plist`);
- signs the installer with the Developer ID Installer certificate;
- notarizes and staples the installer, then signs, notarizes and staples the disk image;
- switches the release notes and the DMG's read-me to the notarized wording.

If only some of the secrets exist, the build fails instead of quietly shipping unsigned.

### Adding the secrets

You need a paid Apple Developer Program membership; only its Account Holder can create Developer
ID certificates.

1. Create a **Developer ID Application** and a **Developer ID Installer** certificate: in Xcode,
   *Settings → Accounts → Manage Certificates → +*, or on developer.apple.com under
   *Certificates, Identifiers & Profiles*.
2. In Keychain Access, under *My Certificates*, export each certificate with its private key as a
   `.p12` file. Use the same password for both.
3. Create an app-specific password for your Apple ID at account.apple.com (*Sign-In and
   Security → App-Specific Passwords*). Notarization uses it instead of your real password.
4. Find your Team ID on developer.apple.com under *Membership details*.
5. Add the repository secrets (`gh secret set NAME` without `--body` prompts for the value):

   ```sh
   gh secret set MACOS_DEVELOPER_ID_APP_P12 < <(base64 -i DeveloperIDApplication.p12)
   gh secret set MACOS_DEVELOPER_ID_INSTALLER_P12 < <(base64 -i DeveloperIDInstaller.p12)
   gh secret set MACOS_P12_PASSWORD     # the .p12 export password
   gh secret set APPLE_ID               # the Apple ID's email address
   gh secret set APPLE_TEAM_ID          # e.g. A1B2C3D4E5
   gh secret set APPLE_APP_PASSWORD     # the app-specific password
   ```

6. Start a dry run and check the downloaded DMG:

   ```sh
   xcrun stapler validate Sedef-Amp-macOS.dmg
   hdiutil attach -nobrowse Sedef-Amp-macOS.dmg
   pkgutil --check-signature "/Volumes/Sedef Amp <version>/Install Sedef Amp.pkg"
   spctl --assess --type install -vv "/Volumes/Sedef Amp <version>/Install Sedef Amp.pkg"
   ```

To sign a local build, put both certificates in your login keychain and export
`MACOS_SIGN_APP_IDENTITY` and `MACOS_SIGN_INSTALLER_IDENTITY` (the full names shown by
`security find-identity -v`) before running `package.sh`; add `APPLE_ID`, `APPLE_TEAM_ID` and
`APPLE_APP_PASSWORD` to notarize as well.

## Windows code signing

Not set up: the installer is unsigned and SmartScreen warns until a download builds reputation.
Signing would go into `build-installer.sh` (Inno Setup's `SignTool` setting signs the installer
and its uninstaller) with a code-signing certificate or a cloud signing service.
