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

result="$(mktemp)"
trap 'rm -f "$result"' EXIT

echo "Notarizing $(basename "$file")"
# notarytool exits non-zero for a rejected submission; carry on so Apple's log gets printed.
xcrun notarytool submit "$file" "${credentials[@]}" --wait --timeout 1h \
    --output-format json >"$result" || true
status="$(plutil -extract status raw -o - "$result" 2>/dev/null || echo "unknown")"
submission="$(plutil -extract id raw -o - "$result" 2>/dev/null || true)"
echo "  submission ${submission:-(none)}: $status"

if [[ "$status" != "Accepted" ]]; then
    cat "$result" >&2
    if [[ -n "$submission" ]]; then
        xcrun notarytool log "$submission" "${credentials[@]}" || true
    fi
    echo "error: notarization of $file failed ($status)" >&2
    exit 1
fi

xcrun stapler staple "$file"
xcrun stapler validate "$file"
