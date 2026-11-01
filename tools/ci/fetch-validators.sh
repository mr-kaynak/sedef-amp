#!/usr/bin/env bash
# Downloads pinned pluginval and clap-validator builds for this runner into <dir> and exports
# PLUGINVAL / CLAP_VALIDATOR through $GITHUB_ENV for tools/ci/validate-plugins.sh.
#
# On Linux, pluginval is built from the same tag's source with a JUCE fix applied (see
# build_pluginval below), which needs git, CMake, a C++ compiler, JUCE's Linux dependencies and
# ladspa.h (ladspa-sdk).
#
#   GH_TOKEN=... tools/ci/fetch-validators.sh "$RUNNER_TEMP/validators"
set -euo pipefail

PLUGINVAL_TAG=v1.0.4
CLAP_VALIDATOR_TAG=0.4.1

dir=${1:?usage: fetch-validators.sh <dir>}
mkdir -p "$dir"
patch="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/patches/pluginval-juce-vst3-runloop.patch"
cd "$dir"

# The pluginval release's Linux binary is built with a JUCE whose VST3 host can crash when the last
# plug-in instance is destroyed (a use after free in its run loop; the patch explains). The crash
# is intermittent and can come after every test passed. So on Linux the same tag is built from
# source with JUCE's fix applied. <dir>/pluginval-linux keeps the result for as long as the tag
# and the patch stay the same (CI caches that folder). Runs in <dir>.
build_pluginval() {
    local out=pluginval-linux src=pluginval-src stamp generator=()
    stamp="$PLUGINVAL_TAG $(sha256sum "$patch" | cut -d ' ' -f 1)"
    if [[ -x "$out/pluginval" && "$(cat "$out/stamp" 2>/dev/null)" == "$stamp" ]]; then
        echo "pluginval $PLUGINVAL_TAG with the JUCE fix: reusing $dir/$out"
        return
    fi
    if command -v ninja > /dev/null; then generator=(-G Ninja); fi
    rm -rf "$src" "$out"
    git clone --quiet --depth 1 --branch "$PLUGINVAL_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/Tracktion/pluginval "$src"
    git -C "$src/modules/juce" apply "$patch"
    cmake -S "$src" -B "$src/build" "${generator[@]}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$src/build" --target pluginval --parallel "$(nproc)"
    mkdir -p "$out"
    cp "$src/build/pluginval_artefacts/Release/pluginval" "$out/"
    echo "$stamp" > "$out/stamp"
    rm -rf "$src"
}

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
        pv_asset= # built from source
        cv_asset='*ubuntu-22.04.zip'
        pv_bin=pluginval-linux/pluginval
        ;;
esac

if [[ -n "$pv_asset" ]]; then
    gh release download "$PLUGINVAL_TAG" --repo Tracktion/pluginval --pattern "$pv_asset" --clobber
else
    build_pluginval
fi
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
