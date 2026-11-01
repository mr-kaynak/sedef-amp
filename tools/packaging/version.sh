#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Prints the project version from the top-level CMakeLists.txt (project(... VERSION x.y.z)).
#
#   version.sh                  print the version
#   version.sh --check-tag TAG  fail unless TAG is v<version> or v<version>-<suffix>

set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
version="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n 1)"
if [[ -z "$version" ]]; then
    echo "error: no 'VERSION x.y.z' line in $root/CMakeLists.txt" >&2
    exit 1
fi

if [[ "${1:-}" == "--check-tag" ]]; then
    tag="${2:?usage: version.sh --check-tag TAG}"
    case "$tag" in
        "v$version" | "v$version"-*) ;;
        *)
            echo "error: tag $tag does not match the project version $version in CMakeLists.txt" >&2
            exit 1
            ;;
    esac
fi

echo "$version"
