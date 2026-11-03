#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Installs the package that was just built through the in-app updater's own code
# (sedef-update-check --install, plugin/src/update/Installer.cpp), the way "Update now" does, and
# checks the result. Runs in .github/workflows/release.yml on CI machines only: it installs into
# the system's plug-in folders, with sudo on macOS (passwordless on GitHub's runners) and elevated
# on Windows. Never run it on your own computer.
#
#   tools/ci/install-test.sh <dist dir> <JUCE artefacts dir> <sedef-update-check>
#
#   macOS    installs over the real $OLD_TAG release (all four formats), checks that every bundle
#            was replaced and has the new version, then removes the AU and installs again to
#            check that it stays removed.
#   Windows  installs $OLD_TAG with VST3 and CLAP only and with the update's switches (which that
#            setup does not know), then the new setup through the updater, waiting for a dummy
#            process like it waits for the DAW. Checks the wait, the files and that the standalone
#            app stays uninstalled (Setup reuses the previous formats).
#   Linux    installs $OLD_TAG for the user, then the new archive through the updater.
#
# Needs GH_TOKEN for `gh release download`.

set -euo pipefail

dist="$(cd "${1:?usage: install-test.sh <dist> <artefacts> <sedef-update-check>}" && pwd)"
artefacts="$(cd "${2:?}" && pwd)"
tool="${3:?}"
OLD_TAG="${OLD_TAG:-v0.3.0}"
repo="${GITHUB_REPOSITORY:-mr-kaynak/sedef-amp}"
here="$(cd "$(dirname "$0")" && pwd)"
version="$("$here/../packaging/version.sh")"
work="$(mktemp -d)"
failures=0

say() { printf '\n== %s\n' "$*"; }
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}
check_equal() { # <what> <expected> <actual>
    if [[ "$2" == "$3" ]]; then
        echo "ok: $1 = $3"
    else
        fail "$1: expected '$2', got '$3'"
    fi
}
old() { # <asset>: downloads it from the old release
    gh release download "$OLD_TAG" --repo "$repo" --pattern "$1" --dir "$work/old" --clobber
    echo "$work/old/$1"
}

echo "Installing Sedef Amp $version over $OLD_TAG with $tool"

case "$(uname -s)" in
Darwin)
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
    bundle_version() { plutil -extract CFBundleShortVersionString raw "$1/Contents/Info.plist" 2>/dev/null || echo missing; }
    bundles=("/Applications/Sedef Amp.app" "/Library/Audio/Plug-Ins/VST3/Sedef Amp.vst3"
        "/Library/Audio/Plug-Ins/Components/Sedef Amp.component" "/Library/Audio/Plug-Ins/CLAP/Sedef Amp.clap")
    payloads=(app vst3 au clap)
    cache="$HOME/Library/Caches/Sedef/Updates"

    say "1. $OLD_TAG, installed as its read-me says"
    hdiutil attach -nobrowse -readonly -mountpoint "$work/old-image" "$(old Sedef-Amp-macOS.dmg)" >/dev/null
    sudo /usr/sbin/installer -pkg "$work/old-image/Install Sedef Amp.pkg" -target /
    hdiutil detach "$work/old-image" >/dev/null
    for b in "${bundles[@]}"; do
        echo "  $b: $(bundle_version "$b")"
    done

    say "2. What the new pkg carries"
    hdiutil attach -nobrowse -readonly -mountpoint "$work/new-image" "$dist/Sedef-Amp-macOS.dmg" >/dev/null
    pkgutil --expand-full "$work/new-image/Install Sedef Amp.pkg" "$work/expanded"
    hdiutil detach "$work/new-image" >/dev/null
    expected=()
    for i in "${!bundles[@]}"; do
        name="$(basename "${bundles[$i]}")"
        expected+=("$(sha "$work/expanded/sedef-amp-${payloads[$i]}.pkg/Payload/$name/Contents/MacOS/Sedef Amp")")
    done

    check_bundles() { # <index to expect missing, or -1>
        for i in "${!bundles[@]}"; do
            b="${bundles[$i]}"
            if [[ "$i" == "$1" ]]; then
                [[ -e "$b" ]] && fail "$b came back" || echo "ok: $b stays removed"
                continue
            fi
            check_equal "$b version" "$version" "$(bundle_version "$b")"
            check_equal "$b binary" "${expected[$i]}" "$(sha "$b/Contents/MacOS/Sedef Amp")"
        done
        if hdiutil info | grep -F "$cache" >/dev/null; then
            fail "a disk image is still attached under $cache"
        else
            echo "ok: no disk image left attached"
        fi
        [[ -e "$cache/choices.xml" ]] && fail "choices.xml was left in the cache" || true
    }

    say "3. Update now, over $OLD_TAG"
    "$tool" --install "$dist/Sedef-Amp-macOS.dmg" --expect "$version" || fail "the install failed"
    check_bundles -1

    say "4. Without the AU: it stays removed"
    sudo rm -rf "${bundles[2]}"
    "$tool" --install "$dist/Sedef-Amp-macOS.dmg" --expect "$version" | tee "$work/second.log" ||
        fail "the second install failed"
    grep -q '^formats: app, vst3, clap$' "$work/second.log" && echo "ok: formats app, vst3, clap" ||
        fail "the second install chose other formats"
    check_bundles 2
    ;;

MINGW* | MSYS* | CYGWIN*)
    sha() { sha256sum "$1" | cut -d' ' -f1; }
    common="/c/Program Files/Common Files"
    vst3="$common/VST3/Sedef Amp.vst3/Contents/x86_64-win/Sedef Amp.vst3"
    clap="$common/CLAP/Sedef Amp.clap"
    app="/c/Program Files/Sedef Amp/Sedef Amp.exe"
    # A process to wait for, as Setup waits for the DAW: its Windows process id.
    dummy() { # <seconds>
        powershell -NoProfile -Command "(Start-Process -PassThru -WindowStyle Hidden powershell -ArgumentList '-NoProfile','-Command','Start-Sleep -Seconds $1').Id" | tr -d '\r'
    }
    alive() { powershell -NoProfile -Command "if (Get-Process -Id $1 -ErrorAction SilentlyContinue) { 'yes' } else { 'no' }" | tr -d '\r'; }

    say "1. $OLD_TAG with VST3 and CLAP only, given the update's switches it does not know"
    setup_old="$(cygpath -w "$(old Sedef-Amp-Windows-Setup.exe)")"
    pid="$(dummy 90)"
    started=$SECONDS
    MSYS2_ARG_CONV_EXCL='*' "$setup_old" /SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS \
        /RESTARTEXITCODE=3010 "/WAITPID=$pid" /RELAUNCH '/COMPONENTS=vst3,clap' \
        "/LOG=$(cygpath -w "$work/old.log")" || fail "the $OLD_TAG setup failed with $?"
    echo "  took $((SECONDS - started)) s; the dummy is alive: $(alive "$pid")"
    [[ "$(alive "$pid")" == yes ]] && echo "ok: $OLD_TAG ignored /WAITPID" ||
        fail "$OLD_TAG waited for /WAITPID"
    powershell -NoProfile -Command "Stop-Process -Id $pid -ErrorAction SilentlyContinue" || true
    [[ -e "$vst3" && -e "$clap" && ! -e "$app" ]] && echo "ok: $OLD_TAG installed VST3 and CLAP" ||
        fail "$OLD_TAG did not install VST3 and CLAP only"

    say "2. Update now: Setup waits for the dummy, then installs"
    pid="$(dummy 20)"
    started=$SECONDS
    "$tool" --install "$dist/Sedef-Amp-Windows-Setup.exe" --expect "$version" --waitpid "$pid" \
        --log "$work/new.log" || fail "the install failed"
    took=$((SECONDS - started))
    echo "  took $took s"
    [[ "$(alive "$pid")" == no ]] && echo "ok: the dummy had exited" || fail "Setup did not wait"
    ((took >= 15)) && echo "ok: Setup waited ($took s)" || fail "Setup finished after $took s, before the dummy"
    grep -a "Waiting for process $pid" "$work/new.log" >/dev/null && echo "ok: the log shows the wait" ||
        fail "the setup log does not show the wait"
    check_equal "VST3 binary" "$(sha "$artefacts/VST3/Sedef Amp.vst3/Contents/x86_64-win/Sedef Amp.vst3")" "$(sha "$vst3")"
    check_equal "CLAP binary" "$(sha "$artefacts/CLAP/Sedef Amp.clap")" "$(sha "$clap")"
    [[ ! -e "$app" ]] && echo "ok: the standalone app stays uninstalled (previous formats kept)" ||
        fail "silent Setup installed the standalone app the user had left out"
    if ((failures > 0)); then
        echo "--- setup log"
        tail -n 60 "$work/new.log" || true
    fi
    ;;

*)
    sha() { sha256sum "$1" | cut -d' ' -f1; }
    vst3="$HOME/.vst3/Sedef Amp.vst3"
    so="$vst3/Contents/x86_64-linux/Sedef Amp.so"
    module_version() { sed -n 's/.*"Version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1/Contents/Resources/moduleinfo.json" | head -n 1; }

    say "1. $OLD_TAG for this user"
    mkdir -p "$work/old-unpacked"
    tar -xzf "$(old Sedef-Amp-Linux-x86_64.tar.gz)" -C "$work/old-unpacked"
    "$work"/old-unpacked/*/install.sh >/dev/null
    echo "  $vst3: $(module_version "$vst3")"

    say "2. Update now"
    mkdir -p "$work/new-unpacked"
    tar -xzf "$dist/Sedef-Amp-Linux-x86_64.tar.gz" -C "$work/new-unpacked"
    new="$(echo "$work"/new-unpacked/*)"
    "$tool" --install "$dist/Sedef-Amp-Linux-x86_64.tar.gz" --expect "$version" || fail "the install failed"
    check_equal "VST3 version" "$version" "$(module_version "$vst3")"
    check_equal "VST3 binary" "$(sha "$new/VST3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so")" "$(sha "$so")"
    check_equal "CLAP binary" "$(sha "$new/CLAP/Sedef Amp.clap")" "$(sha "$HOME/.clap/Sedef Amp.clap")"
    check_equal "standalone binary" "$(sha "$new/Standalone/Sedef Amp")" "$(sha "$HOME/.local/bin/sedef-amp")"
    [[ -e "${XDG_CACHE_HOME:-$HOME/.cache}/sedef/updates/unpacked" ]] &&
        fail "the unpacked archive was left in the cache" || echo "ok: the cache holds no unpacked files"
    ;;
esac

rm -rf "$work" 2>/dev/null || true
if ((failures > 0)); then
    echo
    echo "$failures check(s) failed" >&2
    exit 1
fi
echo
echo "Install test passed"
