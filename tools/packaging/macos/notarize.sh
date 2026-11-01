#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Notarizes a signed .pkg or .dmg with Apple and staples the ticket to it.
#
#   notarize.sh <file>
#
# Needs APPLE_ID, APPLE_TEAM_ID and APPLE_APP_PASSWORD (an app-specific password for the
# Apple ID; see docs/releasing.md).

set -euo pipefail

file="${1:?usage: notarize.sh <file>}"
: "${APPLE_ID:?APPLE_ID is not set}"
: "${APPLE_TEAM_ID:?APPLE_TEAM_ID is not set}"
: "${APPLE_APP_PASSWORD:?APPLE_APP_PASSWORD is not set}"

credentials=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")

echo "Notarizing $(basename "$file")"
result="$(xcrun notarytool submit "$file" "${credentials[@]}" --wait --timeout 1h \
    --output-format json)"
status="$(plutil -extract status raw -o - - <<<"$result")"
submission="$(plutil -extract id raw -o - - <<<"$result")"
echo "  submission $submission: $status"

if [[ "$status" != "Accepted" ]]; then
    xcrun notarytool log "$submission" "${credentials[@]}" || true
    echo "error: notarization of $file failed ($status)" >&2
    exit 1
fi

xcrun stapler staple "$file"
xcrun stapler validate "$file"
