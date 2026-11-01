#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Packages a Linux build into Sedef-Amp-Linux-x86_64.tar.gz: the VST3, CLAP and standalone
# builds plus install.sh / uninstall.sh, a menu entry and an icon.
#
#   package.sh <artefacts-dir> <output-dir>
#
# <artefacts-dir> is the JUCE output folder, e.g. build/release/plugin/SedefAmp_artefacts/Release.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <artefacts-dir> <output-dir>" >&2
    exit 2
fi
artefacts="$(cd "$1" && pwd)"
mkdir -p "$2"
outdir="$(cd "$2" && pwd)"

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
version="$("$here/../version.sh")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

top="sedef-amp-$version-linux-x86_64"
stage="$work/$top"
mkdir -p "$stage/VST3" "$stage/CLAP" "$stage/Standalone"

for item in "VST3/Sedef Amp.vst3" "CLAP/Sedef Amp.clap" "Standalone/Sedef Amp"; do
    if [[ ! -e "$artefacts/$item" ]]; then
        echo "error: missing $artefacts/$item (build SedefAmp_All and SedefAmp_CLAP first)" >&2
        exit 1
    fi
    cp -R "$artefacts/$item" "$stage/$item"
done

install -m 755 "$here/install.sh" "$here/uninstall.sh" "$stage/"
install -m 644 "$here/sedef-amp.desktop" "$stage/sedef-amp.desktop"
install -m 644 "$repo/tools/packaging/assets/icon.svg" "$stage/sedef-amp.svg"
install -m 644 "$repo/tools/packaging/assets/icon-256.png" "$stage/sedef-amp.png"
install -m 644 "$repo/LICENSE" "$stage/LICENSE"
install -m 644 "$repo/THIRD_PARTY_NOTICES.md" "$stage/THIRD_PARTY_NOTICES.md"
cat >"$stage/README.txt" <<EOF
Sedef Amp $version for Linux (x86_64)
Open-source guitar amp and effects suite.

INSTALL
  ./install.sh            for you only (~/.vst3, ~/.clap, ~/.local/bin, app menu entry)
  ./install.sh --system   for every user (/usr/lib/vst3, /usr/lib/clap, /usr/local), via sudo

Then rescan plugins in your DAW. The interface needs WebKitGTK 4.1
(Debian/Ubuntu: libwebkit2gtk-4.1-0, Fedora: webkit2gtk4.1); the standalone app uses ALSA or JACK.

UNINSTALL
  ./uninstall.sh  (or --system); a copy is installed in ~/.local/share/sedef-amp/
  Presets and settings in ~/.config/Sedef are kept.

MANUAL INSTALL
  Copy VST3/Sedef Amp.vst3 to ~/.vst3 and CLAP/Sedef Amp.clap to ~/.clap; the standalone
  app (Standalone/Sedef Amp) runs from anywhere.

More: https://github.com/mr-kaynak/sedef-amp/blob/main/docs/install.md
Sedef Amp is free software under the GNU Affero General Public License v3.0 or later.
EOF

out="$outdir/Sedef-Amp-Linux-x86_64.tar.gz"
export COPYFILE_DISABLE=1 # macOS tar: no AppleDouble ._ files
rm -f "$out"
# Owner root and no local user names in the archive.
if tar --version 2>/dev/null | grep -q 'GNU tar'; then
    tar -C "$work" --owner=0 --group=0 --numeric-owner -czf "$out" "$top"
else
    tar -C "$work" --uid 0 --gid 0 -czf "$out" "$top"
fi

tar -tzvf "$out"
ls -l "$out"
