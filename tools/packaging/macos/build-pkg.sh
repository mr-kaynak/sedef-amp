#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Builds "Install Sedef Amp.pkg": one component package per format and a product archive with a
# welcome page, the licence and a list of formats to choose from.
#
#   build-pkg.sh <bundles-dir> <version> <output.pkg>
#
# <bundles-dir> uses the JUCE artefacts layout (Standalone/Sedef Amp.app, VST3/Sedef Amp.vst3,
# AU/Sedef Amp.component, CLAP/Sedef Amp.clap) and its bundles must already be signed.
# When MACOS_SIGN_INSTALLER_IDENTITY names a "Developer ID Installer" identity, the product
# archive is signed with it (MACOS_KEYCHAIN optionally names the keychain to search).

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <bundles-dir> <version> <output.pkg>" >&2
    exit 2
fi
bundles="$(cd "$1" && pwd)"
version="$2"
out="$3"

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# key | folder in <bundles-dir> | bundle | install location
components=(
    "app|Standalone|Sedef Amp.app|/Applications"
    "vst3|VST3|Sedef Amp.vst3|/Library/Audio/Plug-Ins/VST3"
    "au|AU|Sedef Amp.component|/Library/Audio/Plug-Ins/Components"
    "clap|CLAP|Sedef Amp.clap|/Library/Audio/Plug-Ins/CLAP"
)

mkdir -p "$work/packages"
for entry in "${components[@]}"; do
    IFS='|' read -r key folder bundle location <<<"$entry"
    src="$bundles/$folder/$bundle"
    if [[ ! -d "$src" ]]; then
        echo "error: missing $src" >&2
        exit 1
    fi

    root="$work/root-$key"
    mkdir -p "$root"
    ditto --noextattr --noqtn "$src" "$root/$bundle"

    scripts="$work/scripts-$key"
    mkdir -p "$scripts"
    install -m 755 "$here/scripts/preinstall" "$scripts/preinstall"
    if [[ "$key" == "au" ]]; then
        install -m 755 "$here/scripts/postinstall-au" "$scripts/postinstall"
    fi
    # Extended attributes would travel in the payload as ._ AppleDouble entries.
    xattr -cr "$root" "$scripts" 2>/dev/null || true

    # Bundles are relocatable by default: Installer would then "upgrade" any copy with the same
    # bundle ID it finds elsewhere on the disk (a build folder, a copy in Downloads) instead of
    # installing where we say.
    plist="$work/$key.plist"
    pkgbuild --analyze --root "$root" "$plist" >/dev/null
    i=0
    while plutil -extract "$i" xml1 -o /dev/null "$plist" 2>/dev/null; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
        i=$((i + 1))
    done

    pkgbuild --quiet \
        --root "$root" \
        --component-plist "$plist" \
        --identifier "io.github.mr-kaynak.sedef-amp.$key" \
        --version "$version" \
        --install-location "$location" \
        --scripts "$scripts" \
        "$work/packages/sedef-amp-$key.pkg"
done

resources="$work/resources"
mkdir -p "$resources"
cp "$here/resources/welcome.html" "$here/resources/conclusion.html" "$resources/"
cp "$repo/LICENSE" "$resources/LICENSE.txt"
cp "$repo/tools/packaging/assets/installer-background.png" "$resources/background.png"
sed "s/@VERSION@/$version/g" "$here/distribution.xml.in" >"$work/distribution.xml"

sign_args=()
if [[ -n "${MACOS_SIGN_INSTALLER_IDENTITY:-}" ]]; then
    sign_args=(--sign "$MACOS_SIGN_INSTALLER_IDENTITY" --timestamp)
    if [[ -n "${MACOS_KEYCHAIN:-}" ]]; then
        sign_args+=(--keychain "$MACOS_KEYCHAIN")
    fi
fi

mkdir -p "$(dirname "$out")"
rm -f "$out"
productbuild --quiet \
    --distribution "$work/distribution.xml" \
    --package-path "$work/packages" \
    --resources "$resources" \
    ${sign_args[@]+"${sign_args[@]}"} \
    "$out"

if pkgutil --payload-files "$out" | grep -q '/\._'; then
    # Usually com.apple.provenance, which a sandboxed shell adds to every file it writes and
    # can't remove. Installer turns these entries back into attributes, so the result works.
    echo "warning: the payload carries extended attributes (._ entries)" >&2
fi
if [[ -n "${MACOS_SIGN_INSTALLER_IDENTITY:-}" ]]; then
    pkgutil --check-signature "$out"
else
    echo "note: $out is not signed (MACOS_SIGN_INSTALLER_IDENTITY is not set)"
fi
echo "Built $out"
