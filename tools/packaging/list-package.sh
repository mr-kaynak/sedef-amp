#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Lists what a release package holds and checks that every bundle of Sedef Amp and Sedef MIDI is
# in it: the macOS installer's component packages and their payloads, or the Linux archive.
# (The Windows setup is checked by tools/ci/install-test.sh, which lists what it installed.)
#
#   list-package.sh <dist dir>

set -euo pipefail

dist="$(cd "${1:?usage: list-package.sh <dist dir>}" && pwd)"
work="$(mktemp -d)"
missing=0
trap 'if [[ -d "$work/image" ]]; then hdiutil detach "$work/image" -force >/dev/null 2>&1 || true; fi; rm -rf "$work"' EXIT

expect() { # <listing file> <path that must appear>
    if grep -qF -- "$2" "$1"; then
        echo "ok: $2"
    else
        echo "MISSING: $2" >&2
        missing=$((missing + 1))
    fi
}

case "$(uname -s)" in
Darwin)
    hdiutil attach -nobrowse -readonly -mountpoint "$work/image" "$dist/Sedef-Amp-macOS.dmg" >/dev/null
    echo "== The disk image"
    ls -l "$work/image"
    pkg="$work/image/Install Sedef Amp.pkg"
    pkgutil --expand-full "$pkg" "$work/expanded"
    echo "== Choices (distribution.xml)"
    grep -E '<choice |<pkg-ref id=[^>]*/>' "$work/expanded/Distribution"
    echo "== Component packages and their bundles"
    : >"$work/listing"
    for component in "$work/expanded"/*.pkg; do
        location="$(sed -n 's/.*install-location="\([^"]*\)".*/\1/p' "$component/PackageInfo" | head -n 1)"
        echo "-- $(basename "$component") -> $location"
        (cd "$component/Payload" && find . -maxdepth 4 -path '*/Contents/MacOS/*' -type f) |
            sed "s#^\./#$location/#" | tee -a "$work/listing"
    done
    for path in "/Applications/Sedef Amp.app" "/Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3" \
        "/Library/Audio/Plug-Ins/Components/Sedef Amp.component" \
        "/Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap" "/Applications/Sedef MIDI.app" \
        "/Library/Audio/Plug-Ins/VST3/Sedef MIDI.vst3" \
        "/Library/Audio/Plug-Ins/Components/Sedef MIDI.component" \
        "/Library/Audio/Plug-Ins/Components/Sedef MIDI Instrument.component" \
        "/Library/Audio/Plug-Ins/CLAP/Sedef MIDI.clap"; do
        expect "$work/listing" "$path/Contents/MacOS/"
    done
    ;;
*)
    archive="$dist/Sedef-Amp-Linux-x86_64.tar.gz"
    echo "== $(basename "$archive")"
    tar -tzvf "$archive" | grep -vE '/Contents/Resources/' | tee "$work/listing"
    for path in "VST3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so" "CLAP/Sedef Amp.clap" \
        "Standalone/Sedef Amp" "sedef-amp.desktop" "VST3/Sedef MIDI.vst3/Contents/x86_64-linux/Sedef MIDI.so" \
        "CLAP/Sedef MIDI.clap" "Standalone/Sedef MIDI" "sedef-midi.desktop" "install.sh" "uninstall.sh"; do
        expect "$work/listing" "/$path"
    done
    ;;
esac

if ((missing > 0)); then
    echo "$missing item(s) missing from the package" >&2
    exit 1
fi
echo "Every bundle of Sedef Amp and Sedef MIDI is in the package."
