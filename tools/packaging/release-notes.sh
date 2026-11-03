#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Prints the GitHub release notes: .github/release-notes.md with the download links for the tag,
# the CHANGELOG section for the version and the macOS first-launch advice.
#
#   release-notes.sh [TAG]
#
# TAG defaults to v<project version> (for dry runs). MACOS_NOTARIZED=true switches the macOS
# advice to the notarized wording.

set -euo pipefail
# Bash 5.2 turns & in the replacement of ${var//pattern/replacement} into the match; the
# substituted texts contain "Privacy & Security". Older versions have no such option.
shopt -u patsub_replacement 2>/dev/null || true

root="$(cd "$(dirname "$0")/../.." && pwd)"
tag="${1:-}"
if [[ -z "$tag" ]]; then
    tag="v$("$root/tools/packaging/version.sh")"
fi
version="${tag#v}"

changelog="$(mktemp)"
trap 'rm -f "$changelog"' EXIT
awk -v v="$version" '
    $0 ~ "^## \\[" v "\\]" { on = 1; next }
    on && (/^## \[/ || /^\[[^]]+\]: /) { exit }
    on { print }
' "$root/CHANGELOG.md" |
    # A release page resolves a relative link against /releases/, so the changelog's links into
    # the repo (docs/dsp/nam.md, docs/dsp/amps.md#...) point at the files as of this tag instead.
    sed -E "s#\]\(([^)#:/][^):]*)\)#](https://github.com/mr-kaynak/sedef-amp/blob/$tag/\1)#g" \
        >"$changelog"
if ! grep -q '[^[:space:]]' "$changelog"; then
    echo "warning: CHANGELOG.md has no section for $version" >&2
    echo "See [CHANGELOG.md](https://github.com/mr-kaynak/sedef-amp/blob/$tag/CHANGELOG.md)." \
        >"$changelog"
fi

if [[ "${MACOS_NOTARIZED:-false}" == "true" ]]; then
    macos='the installer is signed and notarized by Apple, so it opens like any other.'
else
    macos='the installer isn'"'"'t notarized by Apple yet, so the first time macOS says *"Install Sedef Amp.pkg" Not Opened*. Click **Done**, open **System Settings → Privacy & Security**, scroll down to **Security** and click **Open Anyway** next to the message about the installer, then enter your password. You only do this once: the app and plugins it installs open normally. On macOS 14 and earlier you can Control-click the installer and choose **Open** instead.'
fi

# GitHub renders every newline in a release body as a line break, so hard-wrapped paragraphs
# and list items (as in CHANGELOG.md) are joined back into single lines. Headings, tables,
# quotes and code blocks pass through unchanged.
unwrap() {
    awk '
        function flush() { if (buf != "") print buf; buf = "" }
        /^```/ { flush(); fence = !fence; print; next }
        fence { print; next }
        /^[[:space:]]*$/ || /^(#|\||>|<)/ { flush(); print; next }
        /^[[:space:]]*([-*+]|[0-9]+\.)[[:space:]]/ { flush(); buf = $0; next }
        { line = $0; sub(/^[[:space:]]+/, "", line); buf = (buf == "") ? line : buf " " line }
        END { flush() }
    '
}

# Skip the template's leading comment, then fill in the placeholders. The changelog section
# starts with a blank line and ends with one, which the template already provides.
awk '
    !started && /^<!--/ { in_comment = 1 }
    in_comment { if (/-->/) { in_comment = 0; started = 1 } next }
    { started = 1; print }
' "$root/.github/release-notes.md" | while IFS= read -r line; do
    line="${line//@TAG@/$tag}"
    if [[ "$line" == "@CHANGELOG@" ]]; then
        sed -e '/./,$!d' "$changelog" | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}'
    else
        printf '%s\n' "${line//@MACOS_FIRST_LAUNCH@/$macos}"
    fi
done | unwrap
