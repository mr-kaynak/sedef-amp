#!/usr/bin/env bash
# Runs the plugin validators against a folder of built plugins.
#
#   tools/ci/validate-plugins.sh build/release/plugin/SedefAmp_artefacts/Release
#
# Needs pluginval and clap-validator on PATH (or PLUGINVAL / CLAP_VALIDATOR pointing at them).
# On macOS the AU is copied to ~/Library/Audio/Plug-Ins/Components so auval can find it.
set -euo pipefail

dir=${1:?usage: validate-plugins.sh <artefacts dir>}
pluginval=${PLUGINVAL:-pluginval}
clap_validator=${CLAP_VALIDATOR:-clap-validator}

echo "::group::pluginval (VST3)"
"$pluginval" --strictness-level 8 --validate-in-process --skip-gui-tests \
    --timeout-ms 600000 "$dir/VST3/Sedef Amp.vst3"
echo "::endgroup::"

echo "::group::clap-validator"
"$clap_validator" validate "$dir/CLAP/Sedef Amp.clap"
echo "::endgroup::"

if [[ "$(uname)" == "Darwin" ]]; then
    echo "::group::auval"
    components="$HOME/Library/Audio/Plug-Ins/Components"
    mkdir -p "$components"
    rm -rf "$components/Sedef Amp.component"
    cp -R "$dir/AU/Sedef Amp.component" "$components/"
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    auval -strict -v aufx Sda1 Sedf
    echo "::endgroup::"
fi
