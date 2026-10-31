#!/usr/bin/env bash
# Downloads pinned pluginval and clap-validator builds for this runner into <dir> and exports
# PLUGINVAL / CLAP_VALIDATOR through $GITHUB_ENV for tools/ci/validate-plugins.sh.
#
#   GH_TOKEN=... tools/ci/fetch-validators.sh "$RUNNER_TEMP/validators"
set -euo pipefail

PLUGINVAL_TAG=v1.0.4
CLAP_VALIDATOR_TAG=0.4.1

dir=${1:?usage: fetch-validators.sh <dir>}
mkdir -p "$dir"
cd "$dir"

case "${RUNNER_OS:-$(uname)}" in
    macOS | Darwin)
        pv_asset=pluginval_macOS.zip
        cv_asset='*macos-universal.zip'
        pv_bin=pluginval.app/Contents/MacOS/pluginval
        ;;
    Windows)
        pv_asset=pluginval_Windows.zip
        cv_asset='*windows.zip'
        pv_bin=pluginval.exe
        ;;
    *)
        pv_asset=pluginval_Linux.zip
        cv_asset='*ubuntu-22.04.zip'
        pv_bin=pluginval
        ;;
esac

gh release download "$PLUGINVAL_TAG" --repo Tracktion/pluginval --pattern "$pv_asset" --clobber
gh release download "$CLAP_VALIDATOR_TAG" --repo free-audio/clap-validator --pattern "$cv_asset" --clobber

for archive in *.zip; do unzip -q -o "$archive"; done
# The clap-validator zips wrap a tarball (binaries/clap-validator).
for archive in *.tar.gz; do
    [[ -e "$archive" ]] && tar -xzf "$archive"
done

cv_bin=$(find "$dir" -type f \( -name clap-validator -o -name clap-validator.exe \) | head -n 1)
[[ -n "$cv_bin" ]] || { echo "clap-validator binary not found" >&2; exit 1; }
chmod +x "$dir/$pv_bin" "$cv_bin" 2>/dev/null || true

if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "PLUGINVAL=$dir/$pv_bin" >> "$GITHUB_ENV"
    echo "CLAP_VALIDATOR=$cv_bin" >> "$GITHUB_ENV"
else
    echo "PLUGINVAL=$dir/$pv_bin"
    echo "CLAP_VALIDATOR=$cv_bin"
fi
