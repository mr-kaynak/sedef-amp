#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Uninstall Sedef Amp: removes the apps and plugins put in place by "Install Sedef Amp.pkg"
# (Sedef Amp and Sedef MIDI), and copies installed by hand in your home folder (the 0.1.0
# instructions). Your presets and settings in ~/Library/Application Support/Sedef are kept.
#
# Double-click it in Finder, or run it in Terminal: bash "Uninstall Sedef Amp.command"
# With --midi it removes Sedef MIDI only: bash "Uninstall Sedef Amp.command" --midi

set -u

midi_only=0
case "${1:-}" in
    --midi) midi_only=1 ;;
    "") ;;
    *)
        echo "usage: bash \"Uninstall Sedef Amp.command\" [--midi]" >&2
        exit 2
        ;;
esac

amp_bundles=(
    "Applications/Sedef Amp.app"
    "Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3"
    "Library/Audio/Plug-Ins/Components/Sedef Amp.component"
    "Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap"
)
midi_bundles=(
    "Applications/Sedef MIDI.app"
    "Library/Audio/Plug-Ins/VST3/Sedef MIDI.vst3"
    "Library/Audio/Plug-Ins/Components/Sedef MIDI.component"
    "Library/Audio/Plug-Ins/Components/Sedef MIDI Instrument.component"
    "Library/Audio/Plug-Ins/CLAP/Sedef MIDI.clap"
)
amp_receipts=(
    io.github.mr-kaynak.sedef-amp.app
    io.github.mr-kaynak.sedef-amp.vst3
    io.github.mr-kaynak.sedef-amp.au
    io.github.mr-kaynak.sedef-amp.clap
)
midi_receipts=(
    io.github.mr-kaynak.sedef-amp.midi-app
    io.github.mr-kaynak.sedef-amp.midi-vst3
    io.github.mr-kaynak.sedef-amp.midi-au
    io.github.mr-kaynak.sedef-amp.midi-instrument
    io.github.mr-kaynak.sedef-amp.midi-clap
)
if [ $midi_only -eq 1 ]; then
    product="Sedef MIDI"
    bundles=("${midi_bundles[@]}")
    receipts=("${midi_receipts[@]}")
else
    product="Sedef Amp"
    bundles=("${amp_bundles[@]}" "${midi_bundles[@]}")
    receipts=("${amp_receipts[@]}" "${midi_receipts[@]}")
fi
system_items=()
user_items=()
for bundle in "${bundles[@]}"; do
    system_items+=("/$bundle")
    user_items+=("$HOME/$bundle")
done

found_system=()
found_user=()
for item in "${system_items[@]}"; do [ -e "$item" ] && found_system+=("$item"); done
for item in "${user_items[@]}"; do [ -e "$item" ] && found_user+=("$item"); done
found_receipts=()
for id in "${receipts[@]}"; do
    pkgutil --pkg-info "$id" >/dev/null 2>&1 && found_receipts+=("$id")
done

if [ ${#found_system[@]} -eq 0 ] && [ ${#found_user[@]} -eq 0 ] && [ ${#found_receipts[@]} -eq 0 ]; then
    echo "$product is not installed. Nothing to do."
    exit 0
fi

echo "This removes:"
for item in ${found_system[@]+"${found_system[@]}"} ${found_user[@]+"${found_user[@]}"}; do
    echo "  $item"
done
echo
echo "Presets and settings in ~/Library/Application Support/Sedef are kept."
printf "Uninstall %s? [y/N] " "$product"
read -r answer
case "$answer" in
    y | Y | yes | YES) ;;
    *)
        echo "Cancelled."
        exit 0
        ;;
esac

status=0
for item in ${found_user[@]+"${found_user[@]}"}; do
    rm -rf "$item" || status=1
done
if [ ${#found_system[@]} -gt 0 ] || [ ${#found_receipts[@]} -gt 0 ]; then
    echo "Removing the system-wide files needs your administrator password."
    for item in ${found_system[@]+"${found_system[@]}"}; do
        sudo rm -rf "$item" || status=1
    done
    for id in ${found_receipts[@]+"${found_receipts[@]}"}; do
        sudo pkgutil --forget "$id" >/dev/null || status=1
    done
fi
killall -9 AudioComponentRegistrar 2>/dev/null || true

if [ $status -eq 0 ]; then
    echo "$product is uninstalled. Rescan plugins in your DAW."
else
    echo "Some items could not be removed; see the messages above." >&2
fi
exit $status
