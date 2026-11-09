#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Installs Sedef Amp and Sedef MIDI from this folder.
#
#   ./install.sh            for you only: ~/.vst3, ~/.clap, ~/.local/bin, ~/.local/share
#   ./install.sh --system   for every user (asks for sudo): /usr/lib/vst3, /usr/lib/clap,
#                           /usr/local/bin, /usr/local/share
#   --no-midi               leave Sedef MIDI out (and remove it if it is installed)
#   --midi                  install Sedef MIDI after an earlier --no-midi
#
# Running it again updates an existing installation and keeps your choice about Sedef MIDI. To
# remove it, run uninstall.sh with the same option (a copy is installed in <share>/sedef-amp/).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
system=0
midi=keep
for arg in "$@"; do
    case "$arg" in
        --system) system=1 ;;
        --midi) midi=1 ;;
        --no-midi) midi=0 ;;
        -h | --help)
            sed -n '4,13p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

if [[ $system == 1 ]]; then
    if [[ $(id -u) -ne 0 ]]; then
        echo "Installing for every user needs root; asking sudo."
        exec sudo -- "$0" "$@"
    fi
    vst3_dir=/usr/lib/vst3
    clap_dir=/usr/lib/clap
    bin_dir=/usr/local/bin
    share_dir=/usr/local/share
else
    if [[ $(id -u) -eq 0 ]]; then
        echo "note: installing for root only; use --system to install for every user."
    fi
    vst3_dir="$HOME/.vst3"
    clap_dir="$HOME/.clap"
    bin_dir="$HOME/.local/bin"
    share_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
fi

for item in "VST3/Sedef Amp.vst3" "CLAP/Sedef Amp.clap" "Standalone/Sedef Amp" sedef-amp.svg \
    sedef-amp.png sedef-amp.desktop uninstall.sh "VST3/Sedef MIDI.vst3" "CLAP/Sedef MIDI.clap" \
    "Standalone/Sedef MIDI" sedef-midi.desktop; do
    if [[ ! -e "$here/$item" ]]; then
        echo "error: $here/$item is missing; unpack the whole archive first." >&2
        exit 1
    fi
done

mkdir -p "$vst3_dir" "$clap_dir" "$bin_dir" "$share_dir/applications" \
    "$share_dir/icons/hicolor/scalable/apps" "$share_dir/icons/hicolor/256x256/apps" \
    "$share_dir/sedef-amp"

rm -rf "${vst3_dir:?}/Sedef Amp.vst3"
cp -R "$here/VST3/Sedef Amp.vst3" "$vst3_dir/"
install -m 755 "$here/CLAP/Sedef Amp.clap" "$clap_dir/Sedef Amp.clap"
install -m 755 "$here/Standalone/Sedef Amp" "$bin_dir/sedef-amp"
install -m 644 "$here/sedef-amp.svg" "$share_dir/icons/hicolor/scalable/apps/sedef-amp.svg"
install -m 644 "$here/sedef-amp.png" "$share_dir/icons/hicolor/256x256/apps/sedef-amp.png"
install -m 755 "$here/uninstall.sh" "$share_dir/sedef-amp/uninstall.sh"
# The menu entry runs the binary by its full path, so it works even if bin_dir isn't on PATH.
sed "s|@EXEC@|$bin_dir/sedef-amp|" "$here/sedef-amp.desktop" >"$share_dir/applications/sedef-amp.desktop"
chmod 644 "$share_dir/applications/sedef-amp.desktop"

# Sedef MIDI, unless it was left out: --no-midi leaves a mark that later runs (and Sedef's
# updates, which run this script) keep, until --midi.
no_midi_mark="$share_dir/sedef-amp/no-midi"
if [[ $midi == keep ]]; then
    if [[ -e "$no_midi_mark" ]]; then midi=0; else midi=1; fi
fi
midi_items=(
    "$vst3_dir/Sedef MIDI.vst3"
    "$clap_dir/Sedef MIDI.clap"
    "$bin_dir/sedef-midi"
    "$share_dir/applications/sedef-midi.desktop"
)
if [[ $midi == 1 ]]; then
    rm -rf "${vst3_dir:?}/Sedef MIDI.vst3"
    cp -R "$here/VST3/Sedef MIDI.vst3" "$vst3_dir/"
    install -m 755 "$here/CLAP/Sedef MIDI.clap" "$clap_dir/Sedef MIDI.clap"
    install -m 755 "$here/Standalone/Sedef MIDI" "$bin_dir/sedef-midi"
    sed "s|@EXEC@|$bin_dir/sedef-midi|" "$here/sedef-midi.desktop" \
        >"$share_dir/applications/sedef-midi.desktop"
    chmod 644 "$share_dir/applications/sedef-midi.desktop"
    rm -f "$no_midi_mark"
else
    rm -rf "${midi_items[@]}"
    : >"$no_midi_mark"
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "$share_dir/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$share_dir/icons/hicolor" 2>/dev/null || true
fi

echo "Installed Sedef Amp:"
echo "  VST3        $vst3_dir/Sedef Amp.vst3"
echo "  CLAP        $clap_dir/Sedef Amp.clap"
echo "  Standalone  $bin_dir/sedef-amp (and Sedef Amp in your app menu)"
if [[ $midi == 1 ]]; then
    echo "Installed Sedef MIDI:"
    echo "  VST3        $vst3_dir/Sedef MIDI.vst3"
    echo "  CLAP        $clap_dir/Sedef MIDI.clap"
    echo "  Standalone  $bin_dir/sedef-midi (and Sedef MIDI in your app menu)"
else
    echo "Sedef MIDI is left out (./install.sh --midi adds it)."
fi
echo "Rescan plugins in your DAW. To uninstall: $share_dir/sedef-amp/uninstall.sh$([[ $system == 1 ]] && echo ' --system')"

# The UI needs WebKitGTK 4.1; say so now rather than show an empty window later.
if command -v ldconfig >/dev/null 2>&1 && ! ldconfig -p 2>/dev/null | grep -q 'libwebkit2gtk-4\.1\.so'; then
    echo
    echo "warning: WebKitGTK 4.1 was not found; Sedef Amp's interface needs it. Install it with:"
    echo "  Debian/Ubuntu: sudo apt install libwebkit2gtk-4.1-0"
    echo "  Fedora:        sudo dnf install webkit2gtk4.1"
    echo "  Arch:          sudo pacman -S webkit2gtk-4.1"
fi
case ":$PATH:" in
    *":$bin_dir:"*) ;;
    *) echo "note: $bin_dir is not on your PATH; start Sedef Amp from the app menu or by its full path." ;;
esac
