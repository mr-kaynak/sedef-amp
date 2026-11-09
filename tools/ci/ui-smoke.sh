#!/usr/bin/env bash
# Loads a built VST3 in sedef-host-smoke (plugin/smoke/HostSmoke.cpp), opens its editor and
# checks that the web UI came up inside the real WebView: handshake done, controls rendered.
# Configure with -DSEDEF_BUILD_HOST_SMOKE=ON and build the sedef-host-smoke target first.
#
#   tools/ci/ui-smoke.sh build/release/plugin                          # Sedef Amp
#   tools/ci/ui-smoke.sh build/release/plugin SedefMidi "Sedef MIDI"   # Sedef MIDI
set -euo pipefail

plugin_build=${1:?usage: ui-smoke.sh <build dir of plugin/, e.g. build/release/plugin> [target] [product]}
target=${2:-SedefAmp}
product=${3:-Sedef Amp}
vst3="$plugin_build/${target}_artefacts/Release/VST3/$product.vst3"
case "$(uname -s)" in
    Darwin) host="$plugin_build/sedef-host-smoke_artefacts/Release/sedef-host-smoke.app/Contents/MacOS/sedef-host-smoke" ;;
    MINGW* | MSYS* | CYGWIN*) host="$plugin_build/sedef-host-smoke_artefacts/Release/sedef-host-smoke.exe" ;;
    *) host="$plugin_build/sedef-host-smoke_artefacts/Release/sedef-host-smoke" ;;
esac

# The host prints the editor's JSON report and exits 0 only when the UI rendered.
"$host" "$(cd "$(dirname "$vst3")" && pwd)/$(basename "$vst3")"
