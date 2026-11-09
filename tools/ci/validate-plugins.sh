#!/usr/bin/env bash
# Runs the plugin validators against a folder of built plugins.
#
#   tools/ci/validate-plugins.sh build/release/plugin/SedefAmp_artefacts/Release
#   tools/ci/validate-plugins.sh build/release/plugin/SedefMidi_artefacts/Release "Sedef MIDI" aufx Sdm1
#
# The product name (default "Sedef Amp") names the bundles; the AU type and subtype (default
# aufx Sda1, manufacturer Sedf) are what auval checks. Needs pluginval and clap-validator on PATH
# (or PLUGINVAL / CLAP_VALIDATOR pointing at them). On macOS the AU is copied to
# ~/Library/Audio/Plug-Ins/Components so auval can find it. SKIP_CLAP_TESTS lists clap-validator
# tests to leave out (comma-separated), for checks a kind of plug-in cannot meet.
set -euo pipefail

dir=${1:?usage: validate-plugins.sh <artefacts dir> [product name] [AU type] [AU subtype]}
product=${2:-Sedef Amp}
au_type=${3:-aufx}
au_subtype=${4:-Sda1}
pluginval=${PLUGINVAL:-pluginval}
clap_validator=${CLAP_VALIDATOR:-clap-validator}

if [[ -d "$dir/VST3/$product.vst3" ]]; then
    echo "::group::pluginval (VST3, $product)"
    "$pluginval" --strictness-level 8 --validate-in-process --skip-gui-tests \
        --timeout-ms 600000 "$dir/VST3/$product.vst3"
    echo "::endgroup::"
fi

if [[ -e "$dir/CLAP/$product.clap" ]]; then
    echo "::group::clap-validator ($product)"
    clap_args=(validate)
    if [[ -n "${SKIP_CLAP_TESTS:-}" ]]; then
        # clap-validator takes a regex of the tests to run; --invert-filter runs the others.
        clap_args+=(--test-filter "^(${SKIP_CLAP_TESTS//,/|})\$" --invert-filter)
    fi
    # clap-validator writes the states it compares under the temp folder; CI keeps them on failure.
    if [[ -n "${VALIDATOR_OUTPUT:-}" ]]; then
        mkdir -p "$VALIDATOR_OUTPUT"
        TMPDIR="$VALIDATOR_OUTPUT" TMP="$VALIDATOR_OUTPUT" TEMP="$VALIDATOR_OUTPUT" \
            "$clap_validator" "${clap_args[@]}" "$dir/CLAP/$product.clap"
    else
        "$clap_validator" "${clap_args[@]}" "$dir/CLAP/$product.clap"
    fi
    echo "::endgroup::"
fi

if [[ "$(uname)" == "Darwin" && -d "$dir/AU/$product.component" ]]; then
    echo "::group::auval ($product)"
    components="$HOME/Library/Audio/Plug-Ins/Components"
    mkdir -p "$components"
    rm -rf "$components/$product.component"
    cp -R "$dir/AU/$product.component" "$components/"
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    auval -strict -v "$au_type" "$au_subtype" Sedf
    echo "::endgroup::"
fi
