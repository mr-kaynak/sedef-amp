#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Builds Sedef-Amp-Windows-Setup.exe with Inno Setup 6 (Sedef Amp and Sedef MIDI). Runs on Windows
# in Git Bash or MSYS2.
#
#   build-installer.sh <artefacts-dir> <output-dir>
#
# <artefacts-dir> is the JUCE output folder, e.g. build/release/plugin/SedefAmp_artefacts/Release.
# Sedef MIDI's files come from its sibling folder (SedefMidi_artefacts/Release), or from
# $SEDEF_MIDI_ARTEFACTS.
# ISCC is looked up on PATH and in the default install folders, or taken from $ISCC. With
# SEDEF_INSTALL_INNO=1 (CI), a missing Inno Setup is installed with Chocolatey at a pinned version.

set -euo pipefail

INNO_VERSION=6.7.1

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <artefacts-dir> <output-dir>" >&2
    exit 2
fi
artefacts="$(cd "$1" && pwd)"
midi_artefacts="${SEDEF_MIDI_ARTEFACTS:-$(cd "$artefacts/../.." && pwd)/SedefMidi_artefacts/$(basename "$artefacts")}"
mkdir -p "$2"
outdir="$(cd "$2" && pwd)"

here="$(cd "$(dirname "$0")" && pwd)"
version="$("$here/../version.sh")"

for item in "VST3/Sedef Amp.vst3" "CLAP/Sedef Amp.clap" "Standalone/Sedef Amp.exe"; do
    if [[ ! -e "$artefacts/$item" ]]; then
        echo "error: missing $artefacts/$item (build SedefAmp_All and SedefAmp_CLAP first)" >&2
        exit 1
    fi
done
for item in "VST3/Sedef MIDI.vst3" "CLAP/Sedef MIDI.clap" "Standalone/Sedef MIDI.exe"; do
    if [[ ! -e "$midi_artefacts/$item" ]]; then
        echo "error: missing $midi_artefacts/$item (build SedefMidi_All and SedefMidi_CLAP first)" >&2
        exit 1
    fi
done

find_iscc() {
    local candidate
    for candidate in "${ISCC:-}" "$(command -v iscc 2>/dev/null || true)" \
        "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe" \
        "${LOCALAPPDATA:+$(cygpath -u "$LOCALAPPDATA")/Programs/Inno Setup 6/ISCC.exe}"; do
        if [[ -n "$candidate" && -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

if ! iscc="$(find_iscc)"; then
    if [[ "${SEDEF_INSTALL_INNO:-0}" == "1" ]]; then
        echo "Inno Setup not found; installing $INNO_VERSION with Chocolatey"
        choco install innosetup --version "$INNO_VERSION" --yes --no-progress
        iscc="$(find_iscc)" || { echo "error: ISCC still not found" >&2; exit 1; }
    else
        echo "error: Inno Setup 6 (ISCC.exe) not found; install it or set ISCC" >&2
        exit 1
    fi
fi
echo "Using $iscc"

# ISCC is a Windows program: hand it Windows paths and keep MSYS from rewriting the /D and /O
# switches as POSIX paths.
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$iscc" \
    "/DAppVersion=$version" \
    "/DArtefactsDir=$(cygpath -w "$artefacts")" \
    "/DMidiArtefactsDir=$(cygpath -w "$midi_artefacts")" \
    "/O$(cygpath -w "$outdir")" \
    "$(cygpath -w "$here/sedef-amp.iss")"

ls -l "$outdir/Sedef-Amp-Windows-Setup.exe"
