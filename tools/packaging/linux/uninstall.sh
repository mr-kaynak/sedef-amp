#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Removes Sedef Amp installed by install.sh.
#
#   ./uninstall.sh            remove the per-user installation
#   ./uninstall.sh --system   remove the installation for every user (asks for sudo)
#
# Presets and settings in ~/.config/Sedef are kept.

set -euo pipefail

system=0
for arg in "$@"; do
    case "$arg" in
        --system) system=1 ;;
        -h | --help)
            sed -n '4,9p' "$0" | sed 's/^# \{0,1\}//'
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
        echo "Removing the installation for every user needs root; asking sudo."
        exec sudo -- "$0" "$@"
    fi
    vst3_dir=/usr/lib/vst3
    clap_dir=/usr/lib/clap
    bin_dir=/usr/local/bin
    share_dir=/usr/local/share
else
    vst3_dir="$HOME/.vst3"
    clap_dir="$HOME/.clap"
    bin_dir="$HOME/.local/bin"
    share_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
fi

items=(
    "$vst3_dir/Sedef Amp.vst3"
    "$clap_dir/Sedef Amp.clap"
    "$bin_dir/sedef-amp"
    "$share_dir/applications/sedef-amp.desktop"
    "$share_dir/icons/hicolor/scalable/apps/sedef-amp.svg"
    "$share_dir/icons/hicolor/256x256/apps/sedef-amp.png"
    "$share_dir/sedef-amp"
)
removed=0
for item in "${items[@]}"; do
    if [[ -e "$item" ]]; then
        rm -rf "$item"
        echo "  removed $item"
        removed=1
    fi
done

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q "$share_dir/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t "$share_dir/icons/hicolor" 2>/dev/null || true
fi

if [[ $removed == 1 ]]; then
    echo "Sedef Amp is uninstalled. Presets and settings in ~/.config/Sedef are kept."
else
    echo "Sedef Amp was not installed$([[ $system == 1 ]] || echo ' for you') here. Nothing to do."
fi
