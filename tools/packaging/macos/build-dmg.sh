#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Builds the disk image around the installer: "Install Sedef Amp.pkg", "Read Me.txt" and
# "Uninstall Sedef Amp.command" in a Finder window with the Sedef background.
#
#   build-dmg.sh <Install Sedef Amp.pkg> <version> <output.dmg>
#
# Uses dmgbuild (https://github.com/dmgbuild/dmgbuild). If it isn't on PATH (or in $DMGBUILD),
# a pinned copy is installed into a virtual environment under $TMPDIR.
# When MACOS_SIGN_APP_IDENTITY names a "Developer ID Application" identity, the image is signed.

set -euo pipefail
# Bash 5.2 turns & in the replacement of ${var//pattern/replacement} into the match; the
# substituted texts contain "Privacy & Security". Older versions have no such option.
shopt -u patsub_replacement 2>/dev/null || true

DMGBUILD_VERSION=1.6.7
DS_STORE_VERSION=1.3.3
MAC_ALIAS_VERSION=2.2.3

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <pkg> <version> <output.dmg>" >&2
    exit 2
fi
pkg="$1"
version="$2"
out="$3"

here="$(cd "$(dirname "$0")" && pwd)"
assets="$(cd "$here/../assets" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- dmgbuild ------------------------------------------------------------------------------------
dmgbuild="${DMGBUILD:-$(command -v dmgbuild || true)}"
if [[ -z "$dmgbuild" ]]; then
    venv="${TMPDIR:-/tmp}"
    venv="${venv%/}/sedef-dmgbuild-$DMGBUILD_VERSION"
    if [[ ! -x "$venv/bin/dmgbuild" ]]; then
        echo "Installing dmgbuild $DMGBUILD_VERSION into $venv"
        python3 -m venv "$venv"
        "$venv/bin/python" -m pip install --quiet --disable-pip-version-check \
            "dmgbuild==$DMGBUILD_VERSION" "ds_store==$DS_STORE_VERSION" "mac_alias==$MAC_ALIAS_VERSION"
    fi
    dmgbuild="$venv/bin/dmgbuild"
fi

# --- Contents ------------------------------------------------------------------------------------
# The read-me's first-launch advice depends on whether Apple has notarized the installer.
if xcrun stapler validate -q "$pkg" >/dev/null 2>&1; then
    first_launch='The installer is signed and notarized by Apple, so it opens directly. If macOS
still refuses it, the download may be incomplete: download the disk image again.'
else
    first_launch='The installer is not notarized by Apple yet, so macOS stops it the first time:
  1. Click Done.
  2. Open System Settings > Privacy & Security.
  3. Under Security, next to "Install Sedef Amp.pkg" was blocked, click Open Anyway
     and enter your password.
You only do this once; the app and plugins it installs open without further prompts.
On macOS 14 and earlier you can also Control-click the installer and choose Open.'
fi
readme_text="$(<"$here/read-me.txt.in")"
readme_text="${readme_text//@VERSION@/$version}"
readme_text="${readme_text//@MACOS_FIRST_LAUNCH@/$first_launch}"
printf '%s\n' "$readme_text" >"$work/Read Me.txt"

install -m 755 "$here/uninstall.command" "$work/Uninstall Sedef Amp.command"
ditto --noextattr --noqtn "$pkg" "$work/Install Sedef Amp.pkg"

# The volume icon: the app icon as an .icns.
iconset="$work/Sedef Amp.iconset"
mkdir -p "$iconset"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$assets/icon-1024.png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$assets/icon-1024.png" \
        --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$work/volume.icns"

# --- Image ---------------------------------------------------------------------------------------
mkdir -p "$(dirname "$out")"
rm -f "$out"
"$dmgbuild" -s "$here/dmg-settings.py" \
    -D pkg="$work/Install Sedef Amp.pkg" \
    -D readme="$work/Read Me.txt" \
    -D uninstall="$work/Uninstall Sedef Amp.command" \
    -D background="$assets/dmg-background.png" \
    -D icon="$work/volume.icns" \
    "Sedef Amp $version" "$out"

if [[ -n "${MACOS_SIGN_APP_IDENTITY:-}" ]]; then
    keychain_args=()
    if [[ -n "${MACOS_KEYCHAIN:-}" ]]; then
        keychain_args=(--keychain "$MACOS_KEYCHAIN")
    fi
    codesign --force --timestamp --sign "$MACOS_SIGN_APP_IDENTITY" \
        ${keychain_args[@]+"${keychain_args[@]}"} "$out"
    codesign --verify --verbose=2 "$out"
fi

hdiutil verify -quiet "$out"
echo "Built $out"
