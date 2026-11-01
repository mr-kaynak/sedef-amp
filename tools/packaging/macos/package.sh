#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Packages a macOS build into Sedef-Amp-macOS.dmg: signs the bundles, builds
# "Install Sedef Amp.pkg", wraps it in the disk image and, with Apple credentials, notarizes and
# staples both.
#
#   package.sh <artefacts-dir> <output-dir>
#
# <artefacts-dir> is the JUCE output folder, e.g. build/release/plugin/SedefAmp_artefacts/Release.
#
# Signing comes from the environment (signing-keychain.sh sets these in CI):
#   MACOS_SIGN_APP_IDENTITY        "Developer ID Application: ..."; empty = ad hoc signatures
#   MACOS_SIGN_INSTALLER_IDENTITY  "Developer ID Installer: ...";   empty = unsigned installer
#   MACOS_KEYCHAIN                 keychain holding both (optional)
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD   notarization; needs both identities
# SEDEF_REQUIRE_UNIVERSAL=1 fails the build unless every binary has arm64 and x86_64 slices.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <artefacts-dir> <output-dir>" >&2
    exit 2
fi
artefacts="$(cd "$1" && pwd)"
mkdir -p "$2"
outdir="$(cd "$2" && pwd)"

here="$(cd "$(dirname "$0")" && pwd)"
version="$("$here/../version.sh")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

app_identity="${MACOS_SIGN_APP_IDENTITY:-}"
installer_identity="${MACOS_SIGN_INSTALLER_IDENTITY:-}"
notarize=0
if [[ -n "$app_identity" && -n "$installer_identity" && -n "${APPLE_ID:-}" &&
    -n "${APPLE_TEAM_ID:-}" && -n "${APPLE_APP_PASSWORD:-}" ]]; then
    notarize=1
elif [[ -n "$app_identity" || -n "$installer_identity" ]]; then
    echo "note: signing without notarization (needs both identities and APPLE_ID," \
        "APPLE_TEAM_ID, APPLE_APP_PASSWORD)"
fi
keychain_args=()
if [[ -n "${MACOS_KEYCHAIN:-}" ]]; then
    keychain_args=(--keychain "$MACOS_KEYCHAIN")
fi

echo "Packaging Sedef Amp $version for macOS"

# --- Stage and sign the bundles ------------------------------------------------------------------
stage="$work/bundles"
for item in "Standalone/Sedef Amp.app" "VST3/Sedef Amp.vst3" "AU/Sedef Amp.component" \
    "CLAP/Sedef Amp.clap"; do
    if [[ ! -d "$artefacts/$item" ]]; then
        echo "error: missing $artefacts/$item (build SedefAmp_All and SedefAmp_CLAP first)" >&2
        exit 1
    fi
    mkdir -p "$stage/$(dirname "$item")"
    ditto --noextattr --noqtn "$artefacts/$item" "$stage/$item"

    binary="$stage/$item/Contents/MacOS/Sedef Amp"
    archs="$(lipo -archs "$binary")"
    echo "  $item: $archs"
    if [[ "${SEDEF_REQUIRE_UNIVERSAL:-0}" == "1" && ! ("$archs" == *arm64* && "$archs" == *x86_64*) ]]; then
        echo "error: $item is not universal (arm64 and x86_64)" >&2
        exit 1
    fi

    if [[ -n "$app_identity" ]]; then
        entitlements=()
        if [[ "$item" == Standalone/* ]]; then
            entitlements=(--entitlements "$here/entitlements.plist")
        fi
        codesign --force --timestamp --options runtime --sign "$app_identity" \
            ${keychain_args[@]+"${keychain_args[@]}"} \
            ${entitlements[@]+"${entitlements[@]}"} "$stage/$item"
    else
        codesign --force --sign - "$stage/$item"
    fi
    codesign --verify --deep --strict "$stage/$item"
done

# --- Installer and disk image --------------------------------------------------------------------
pkg="$work/Install Sedef Amp.pkg"
"$here/build-pkg.sh" "$stage" "$version" "$pkg"
if [[ $notarize == 1 ]]; then
    "$here/notarize.sh" "$pkg"
fi

dmg="$outdir/Sedef-Amp-macOS.dmg"
"$here/build-dmg.sh" "$pkg" "$version" "$dmg"
if [[ $notarize == 1 ]]; then
    "$here/notarize.sh" "$dmg"
fi

if [[ $notarize == 1 ]]; then
    echo "Signed and notarized."
else
    echo "Not notarized: ad hoc signed bundles and an unsigned installer."
fi
ls -l "$dmg"
