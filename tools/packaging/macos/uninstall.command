#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Uninstall Sedef Amp: removes the app and plugins put in place by "Install Sedef Amp.pkg", and
# copies installed by hand in your home folder (the 0.1.0 instructions). Your presets and
# settings in ~/Library/Application Support/Sedef are kept.
#
# Double-click it in Finder, or run it in Terminal: bash "Uninstall Sedef Amp.command"

set -u

system_items=(
    "/Applications/Sedef Amp.app"
    "/Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3"
    "/Library/Audio/Plug-Ins/Components/Sedef Amp.component"
    "/Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap"
)
user_items=(
    "$HOME/Applications/Sedef Amp.app"
    "$HOME/Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3"
    "$HOME/Library/Audio/Plug-Ins/Components/Sedef Amp.component"
    "$HOME/Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap"
)
receipts=(
    io.github.mr-kaynak.sedef-amp.app
    io.github.mr-kaynak.sedef-amp.vst3
    io.github.mr-kaynak.sedef-amp.au
    io.github.mr-kaynak.sedef-amp.clap
)

found_system=()
found_user=()
for item in "${system_items[@]}"; do [ -e "$item" ] && found_system+=("$item"); done
for item in "${user_items[@]}"; do [ -e "$item" ] && found_user+=("$item"); done
found_receipts=()
for id in "${receipts[@]}"; do
    pkgutil --pkg-info "$id" >/dev/null 2>&1 && found_receipts+=("$id")
done

if [ ${#found_system[@]} -eq 0 ] && [ ${#found_user[@]} -eq 0 ] && [ ${#found_receipts[@]} -eq 0 ]; then
    echo "Sedef Amp is not installed. Nothing to do."
    exit 0
fi

echo "This removes:"
for item in ${found_system[@]+"${found_system[@]}"} ${found_user[@]+"${found_user[@]}"}; do
    echo "  $item"
done
echo
echo "Presets and settings in ~/Library/Application Support/Sedef are kept."
printf "Uninstall Sedef Amp? [y/N] "
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
    echo "Sedef Amp is uninstalled. Rescan plugins in your DAW."
else
    echo "Some items could not be removed; see the messages above." >&2
fi
exit $status
