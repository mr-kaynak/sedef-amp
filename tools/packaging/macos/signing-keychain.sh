#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Sets up (or removes) a temporary keychain with the Developer ID certificates for a CI build.
#
#   signing-keychain.sh create   import the certificates, export the identities
#   signing-keychain.sh delete   remove the keychain again
#
# create reads these (repository secrets in CI; see docs/releasing.md):
#   MACOS_DEVELOPER_ID_APP_P12        base64 of the "Developer ID Application" .p12
#   MACOS_DEVELOPER_ID_INSTALLER_P12  base64 of the "Developer ID Installer" .p12
#   MACOS_P12_PASSWORD                the password both .p12 files were exported with
#   APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD   used later by notarize.sh
# With none of them set it does nothing, and the build is ad hoc signed and not notarized.
# With only some set it fails, so a half-configured release can't ship unsigned by accident.
#
# It exports MACOS_SIGN_APP_IDENTITY, MACOS_SIGN_INSTALLER_IDENTITY and MACOS_KEYCHAIN to
# $GITHUB_ENV, or prints them as export lines outside GitHub Actions.

set -euo pipefail

keychain="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
keychain="${keychain%/}/sedef-signing.keychain-db"

case "${1:-}" in
    delete)
        if [[ -f "$keychain" ]]; then
            security delete-keychain "$keychain"
            echo "Deleted $keychain"
        fi
        exit 0
        ;;
    create) ;;
    *)
        echo "usage: $0 create|delete" >&2
        exit 2
        ;;
esac

required=(MACOS_DEVELOPER_ID_APP_P12 MACOS_DEVELOPER_ID_INSTALLER_P12 MACOS_P12_PASSWORD
    APPLE_ID APPLE_TEAM_ID APPLE_APP_PASSWORD)
missing=()
for name in "${required[@]}"; do
    [[ -n "${!name:-}" ]] || missing+=("$name")
done
if [[ ${#missing[@]} -eq ${#required[@]} ]]; then
    echo "No signing secrets: the bundles get ad hoc signatures and the installer is not signed."
    exit 0
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: signing is half configured; missing: ${missing[*]}" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

keychain_password="$(uuidgen)"
# A keychain left over from an interrupted run would make create-keychain fail.
security delete-keychain "$keychain" 2>/dev/null || true
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"

printf '%s' "$MACOS_DEVELOPER_ID_APP_P12" | base64 --decode >"$work/app.p12"
printf '%s' "$MACOS_DEVELOPER_ID_INSTALLER_P12" | base64 --decode >"$work/installer.p12"
for p12 in "$work/app.p12" "$work/installer.p12"; do
    security import "$p12" -k "$keychain" -f pkcs12 -P "$MACOS_P12_PASSWORD" \
        -T /usr/bin/codesign -T /usr/bin/productbuild -T /usr/bin/productsign -T /usr/bin/pkgbuild
done
# Let the signing tools use the keys without a password prompt.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" \
    "$keychain" >/dev/null

# Put the keychain first in the search list, keeping the existing ones.
search_list=("$keychain")
while IFS= read -r line; do
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line#\"}"
    line="${line%\"}"
    [[ -n "$line" && "$line" != "$keychain" ]] && search_list+=("$line")
done < <(security list-keychains -d user)
security list-keychains -d user -s "${search_list[@]}"

app_identity="$(security find-identity -v -p codesigning "$keychain" |
    sed -n 's/.*"\(Developer ID Application: .*\)"$/\1/p' | head -n 1)"
installer_identity="$(security find-identity -v "$keychain" |
    sed -n 's/.*"\(Developer ID Installer: .*\)"$/\1/p' | head -n 1)"
if [[ -z "$app_identity" || -z "$installer_identity" ]]; then
    echo "error: the certificates did not yield both identities:" >&2
    security find-identity -v "$keychain" >&2
    exit 1
fi
echo "Signing as: $app_identity"
echo "Installer:  $installer_identity"

if [[ -n "${GITHUB_ENV:-}" ]]; then
    {
        echo "MACOS_SIGN_APP_IDENTITY=$app_identity"
        echo "MACOS_SIGN_INSTALLER_IDENTITY=$installer_identity"
        echo "MACOS_KEYCHAIN=$keychain"
    } >>"$GITHUB_ENV"
else
    printf 'export MACOS_SIGN_APP_IDENTITY=%q\n' "$app_identity"
    printf 'export MACOS_SIGN_INSTALLER_IDENTITY=%q\n' "$installer_identity"
    printf 'export MACOS_KEYCHAIN=%q\n' "$keychain"
fi
