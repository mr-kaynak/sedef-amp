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
`https://github.com/mr-kaynak/sedef-amp/releases/latest/download/<asset>`. Renaming an asset
breaks those links, so change the README, [install.md](install.md) and
[.github/release-notes.md](../.github/release-notes.md) with it.

Some identifiers must stay fixed so upgrades and uninstalls find earlier installs: the Inno Setup
`AppId` in `tools/packaging/windows/sedef-amp.iss` and the macOS package IDs
`io.github.mr-kaynak.sedef-amp.{app,vst3,au,clap}`.

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
`tools/packaging/release-notes.sh`.

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
