#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Renders the committed packaging images from their sources with headless Chrome (macOS):
#   icon.svg                  -> icon-1024.png, icon-256.png, icon.ico
#   installer-background.svg  -> installer-background.png (200 x 200 pt at 2x)
#   dmg-background.html       -> dmg-background.png (720 x 440) and dmg-background@2x.png
#
# Run it after editing a source and commit the PNGs; the release builds use the PNGs as they are.
# Set CHROME to the browser binary if it isn't in the default place.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
chrome="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
if [[ ! -x "$chrome" ]]; then
    echo "error: Chrome not found at '$chrome' (set CHROME)" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# shoot <url> <width> <height> <scale> <output>
# Chrome sometimes exits non-zero while tearing down after writing the file, so the check is on
# the file itself.
shoot() {
    rm -f "$5"
    "$chrome" --headless --disable-gpu --hide-scrollbars --no-first-run \
        --user-data-dir="$work/profile" \
        --default-background-color=00000000 \
        --allow-file-access-from-files \
        --virtual-time-budget=2000 \
        --window-size="$2,$3" --force-device-scale-factor="$4" \
        --screenshot="$5" "$1" >/dev/null 2>&1 || true
    [[ -s "$5" ]] || { echo "error: Chrome did not write $5" >&2; exit 1; }
}

shoot "file://$here/icon.svg" 1024 1024 1 "$here/icon-1024.png"
sips -z 256 256 "$here/icon-1024.png" --out "$here/icon-256.png" >/dev/null
sips -s format ico "$here/icon-256.png" --out "$here/icon.ico" >/dev/null

shoot "file://$here/installer-background.svg" 200 200 2 "$here/installer-background.png"
sips -s dpiWidth 144 -s dpiHeight 144 "$here/installer-background.png" >/dev/null

shoot "file://$here/dmg-background.html" 720 440 1 "$here/dmg-background.png"
shoot "file://$here/dmg-background.html" 720 440 2 "$here/dmg-background@2x.png"
# Finder sizes the background by its DPI: 72 for 1x, 144 for 2x.
sips -s dpiWidth 144 -s dpiHeight 144 "$here/dmg-background@2x.png" >/dev/null

echo "Rendered:"
for f in icon-1024.png icon-256.png icon.ico installer-background.png dmg-background.png \
    dmg-background@2x.png; do
    printf '  %-24s %s\n' "$f" "$(sips -g pixelWidth -g pixelHeight "$here/$f" | awk '/pixel/ {printf "%s ", $2}')"
done
