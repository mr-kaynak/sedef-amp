#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Installs the package that was just built through the in-app updater's own code
# (sedef-update-check --install, plugin/src/update/Installer.cpp), the way "Update now" does, and
# checks the result. Runs in .github/workflows/release.yml on CI machines only: it installs into
# the system's plug-in folders, as root through sudo (passwordless on GitHub's runners) on macOS
# and Linux, and elevated on Windows. Never run it on your own computer.
#
#   tools/ci/install-test.sh <dist dir> <JUCE artefacts dir> <sedef-update-check>
#
#   macOS    installs over the real $OLD_TAG release (all four formats) and checks that every
#            bundle was replaced and has the new version; then removes the AU, puts $OLD_TAG's
#            VST3 back and installs again: the VST3 is replaced and the AU stays removed. The
#            install script leaves no folder and no attached image behind.
#   Windows  installs $OLD_TAG with VST3 and CLAP only and with the update's switches (which that
#            setup does not know), then the new setup through the updater, waiting for a dummy
#            process like it waits for the DAW, while nothing can change the setup file. Checks the
#            wait, the files and that the standalone app stays uninstalled (/COMPONENTS). Then runs
#            the setup alone: it holds its own file while it waits, and does not wait for a process
#            whose id now belongs to a process created at another time.
#   Linux    installs $OLD_TAG for the user and system-wide, then the new archive through the
#            updater, in your home folder and then system-wide, through the root script.
#
# Needs GH_TOKEN for `gh release download`.

set -euo pipefail

dist="$(cd "${1:?usage: install-test.sh <dist> <artefacts> <sedef-update-check>}" && pwd)"
artefacts="$(cd "${2:?}" && pwd)"
tool="${3:?}"
repo="${GITHUB_REPOSITORY:-mr-kaynak/sedef-amp}"
# The release users update from: the latest one published.
OLD_TAG="${OLD_TAG:-$(gh release view --repo "$repo" --json tagName --jq .tagName)}"
here="$(cd "$(dirname "$0")" && pwd)"
version="$("$here/../packaging/version.sh")"
work="$(mktemp -d)"
failures=0
dummies=()

cleanup() {
    case "$(uname -s)" in
    Darwin)
        for image in "$work/old-image" "$work/new-image"; do
            if [[ -d "$image" ]]; then hdiutil detach "$image" -force >/dev/null 2>&1 || true; fi
        done
        ;;
    MINGW* | MSYS* | CYGWIN*)
        for pid in "${dummies[@]+"${dummies[@]}"}"; do
            powershell -NoProfile -Command "Stop-Process -Id $pid -ErrorAction SilentlyContinue" || true
        done
        ;;
    esac
    rm -rf "$work" 2>/dev/null || true
}
trap cleanup EXIT

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
check_differ() { # <what> <a> <b>
    if [[ "$2" != "$3" ]]; then echo "ok: $1 differ"; else fail "$1 are the same ($2)"; fi
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
    binary() { echo "$1/Contents/MacOS/Sedef Amp"; }
    payload() { # <expanded pkg> <index>
        echo "$1/sedef-amp-${payloads[$2]}.pkg/Payload/$(basename "${bundles[$2]}")"
    }
    leftovers() {
        if hdiutil info | grep -F "/sedef-update." >/dev/null; then
            fail "an install script left a disk image attached"
        else
            echo "ok: no disk image left attached"
        fi
        if compgen -G "/private/var/tmp/sedef-update.*" >/dev/null; then
            fail "an install script left its folder: $(echo /private/var/tmp/sedef-update.*)"
        else
            echo "ok: no install folder left"
        fi
    }

    say "1. $OLD_TAG, installed as its read-me says"
    hdiutil attach -nobrowse -readonly -mountpoint "$work/old-image" "$(old Sedef-Amp-macOS.dmg)" >/dev/null
    sudo /usr/sbin/installer -pkg "$work/old-image/Install Sedef Amp.pkg" -target /
    pkgutil --expand-full "$work/old-image/Install Sedef Amp.pkg" "$work/old-expanded"
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
        expected+=("$(sha "$(binary "$(payload "$work/expanded" "$i")")")")
    done

    check_bundles() { # <index to expect missing, or -1>
        for i in "${!bundles[@]}"; do
            b="${bundles[$i]}"
            if [[ "$i" == "$1" ]]; then
                [[ -e "$b" ]] && fail "$b came back" || echo "ok: $b stays removed"
                continue
            fi
            check_equal "$b version" "$version" "$(bundle_version "$b")"
            check_equal "$b binary" "${expected[$i]}" "$(sha "$(binary "$b")")"
        done
        leftovers
    }

    say "3. Update now, over $OLD_TAG"
    "$tool" --install "$dist/Sedef-Amp-macOS.dmg" --expect "$version" || fail "the install failed"
    check_bundles -1

    say "4. $OLD_TAG's VST3 back, the AU removed: the VST3 is replaced, the AU stays removed"
    old_vst3="$(payload "$work/old-expanded" 1)"
    check_differ "$OLD_TAG's and the new VST3 binaries" "$(sha "$(binary "$old_vst3")")" "${expected[1]}"
    sudo rm -rf "${bundles[1]}" "${bundles[2]}"
    sudo ditto "$old_vst3" "${bundles[1]}"
    check_equal "the VST3 put back" "$(sha "$(binary "$old_vst3")")" "$(sha "$(binary "${bundles[1]}")")"
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
    created() { powershell -NoProfile -Command "(Get-Process -Id $1).StartTime.ToFileTimeUtc()" | tr -d '\r'; }
    stop() { powershell -NoProfile -Command "Stop-Process -Id $1 -ErrorAction SilentlyContinue" || true; }
    held() { # <file>: nothing can write or move it
        if (echo x >>"$1") 2>/dev/null; then
            fail "$1 could be written while it is held"
        else
            echo "ok: $1 can't be written"
        fi
        if mv "$1" "$1.moved" 2>/dev/null; then
            mv "$1.moved" "$1"
            fail "$1 could be moved while it is held"
        else
            echo "ok: $1 can't be moved"
        fi
    }
    wait_for_log() { # <log> <text> <seconds>
        for _ in $(seq 1 "$3"); do
            grep -aq "$2" "$1" 2>/dev/null && return 0
            sleep 1
        done
        return 1
    }
    setup_args=(/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010)

    say "1. $OLD_TAG with VST3 and CLAP only, given the update's switches it does not know"
    setup_old="$(cygpath -w "$(old Sedef-Amp-Windows-Setup.exe)")"
    pid="$(dummy 90)"
    dummies+=("$pid")
    started=$SECONDS
    MSYS2_ARG_CONV_EXCL='*' "$setup_old" "${setup_args[@]}" "/WAITPID=$pid" /WAITPIDTIME=1 /RELAUNCH \
        '/COMPONENTS=vst3,clap' "/LOG=$(cygpath -w "$work/old.log")" || fail "the $OLD_TAG setup failed with $?"
    echo "  took $((SECONDS - started)) s; the dummy is alive: $(alive "$pid")"
    [[ "$(alive "$pid")" == yes ]] && echo "ok: $OLD_TAG ignored /WAITPID" ||
        fail "$OLD_TAG waited for /WAITPID"
    stop "$pid"
    [[ -e "$vst3" && -e "$clap" && ! -e "$app" ]] && echo "ok: $OLD_TAG installed VST3 and CLAP" ||
        fail "$OLD_TAG did not install VST3 and CLAP only"
    check_differ "$OLD_TAG's and the new VST3 binaries" "$(sha "$vst3")" \
        "$(sha "$artefacts/VST3/Sedef Amp.vst3/Contents/x86_64-win/Sedef Amp.vst3")"

    say "2. Update now: Setup waits for the dummy while nothing can change its file, then installs"
    # A copy, as the plug-in installs from its cache: the checks below try to write to it.
    mkdir -p "$work/update"
    setup="$work/update/Sedef-Amp-Windows-Setup.exe"
    cp "$dist/Sedef-Amp-Windows-Setup.exe" "$setup"
    pid="$(dummy 20)"
    dummies+=("$pid")
    started=$SECONDS
    "$tool" --install "$setup" --expect "$version" --waitpid "$pid" --log "$work/new.log" \
        >"$work/tool.log" 2>&1 &
    tool_pid=$!
    if wait_for_log "$work/new.log" "Waiting for process $pid" 15; then
        held "$setup"
    else
        fail "Setup was not waiting after 15 s"
    fi
    wait "$tool_pid" || fail "the install failed"
    cat "$work/tool.log"
    took=$((SECONDS - started))
    echo "  took $took s"
    [[ "$(alive "$pid")" == no ]] && echo "ok: the dummy had exited" || fail "Setup did not wait"
    ((took >= 15)) && echo "ok: Setup waited ($took s)" || fail "Setup finished after $took s, before the dummy"
    grep -q '^components: vst3, clap' "$work/tool.log" && echo "ok: components vst3, clap" ||
        fail "the updater chose other components"
    grep -aq "Holding " "$work/new.log" && echo "ok: Setup held its file" || fail "Setup did not hold its file"
    grep -a "Waiting for process $pid" "$work/new.log" >/dev/null && echo "ok: the log shows the wait" ||
        fail "the setup log does not show the wait"
    check_equal "VST3 binary" "$(sha "$artefacts/VST3/Sedef Amp.vst3/Contents/x86_64-win/Sedef Amp.vst3")" "$(sha "$vst3")"
    check_equal "CLAP binary" "$(sha "$artefacts/CLAP/Sedef Amp.clap")" "$(sha "$clap")"
    [[ ! -e "$app" ]] && echo "ok: the standalone app stays uninstalled" ||
        fail "the update installed the standalone app the user had left out"

    say "3. Setup alone holds its own file while it waits"
    mkdir -p "$work/alone"
    cp "$setup" "$work/alone/Setup.exe"
    pid="$(dummy 120)"
    dummies+=("$pid")
    MSYS2_ARG_CONV_EXCL='*' "$(cygpath -w "$work/alone/Setup.exe")" "${setup_args[@]}" \
        '/COMPONENTS=vst3,clap' "/WAITPID=$pid" "/WAITPIDTIME=$(created "$pid")" \
        "/LOG=$(cygpath -w "$work/alone.log")" &
    setup_pid=$!
    if wait_for_log "$work/alone.log" "Waiting for process $pid" 30; then
        grep -aq "Holding " "$work/alone.log" && echo "ok: Setup holds its file" || fail "Setup does not hold its file"
        held "$work/alone/Setup.exe"
    else
        fail "Setup was not waiting after 30 s"
    fi
    stop "$pid"
    wait "$setup_pid" && echo "ok: Setup finished once the dummy was gone" || fail "Setup failed"

    say "4. A process created at another time than /WAITPIDTIME says is not waited for"
    pid="$(dummy 120)"
    dummies+=("$pid")
    started=$SECONDS
    MSYS2_ARG_CONV_EXCL='*' "$(cygpath -w "$work/alone/Setup.exe")" "${setup_args[@]}" \
        '/COMPONENTS=vst3,clap' "/WAITPID=$pid" /WAITPIDTIME=1 "/LOG=$(cygpath -w "$work/reused.log")" ||
        fail "Setup failed"
    echo "  took $((SECONDS - started)) s"
    [[ "$(alive "$pid")" == yes ]] && echo "ok: Setup did not wait for it" || fail "the dummy is gone"
    grep -aq "was created at another time" "$work/reused.log" && echo "ok: the log says why" ||
        fail "the setup log does not say why it did not wait"
    stop "$pid"
    if ((failures > 0)); then
        for log in new alone reused; do
            echo "--- $log.log"
            tail -n 40 "$work/$log.log" || true
        done
    fi
    ;;

*)
    sha() { sha256sum "$1" | cut -d' ' -f1; }
    module_version() { sed -n 's/.*"Version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1/Contents/Resources/moduleinfo.json" | head -n 1; }
    leftovers() {
        if sudo sh -c 'ls -d /var/tmp/sedef-update.* 2>/dev/null' | grep -q .; then
            fail "an install script left its folder"
        else
            echo "ok: no install folder left"
        fi
    }

    say "1. $OLD_TAG for this user and for every user"
    mkdir -p "$work/old-unpacked"
    tar -xzf "$(old Sedef-Amp-Linux-x86_64.tar.gz)" -C "$work/old-unpacked"
    "$work"/old-unpacked/*/install.sh >/dev/null
    sudo "$work"/old-unpacked/*/install.sh --system >/dev/null
    echo "  $HOME/.vst3/Sedef Amp.vst3: $(module_version "$HOME/.vst3/Sedef Amp.vst3")"
    echo "  /usr/lib/vst3/Sedef Amp.vst3: $(module_version "/usr/lib/vst3/Sedef Amp.vst3")"

    mkdir -p "$work/new-unpacked"
    tar -xzf "$dist/Sedef-Amp-Linux-x86_64.tar.gz" -C "$work/new-unpacked"
    new="$(echo "$work"/new-unpacked/*)"
    so="VST3/Sedef Amp.vst3/Contents/x86_64-linux/Sedef Amp.so"
    check_differ "$OLD_TAG's and the new VST3 binaries" "$(sha "$(echo "$work"/old-unpacked/*)/$so")" \
        "$(sha "$new/$so")"
    check_scope() { # <vst3 dir> <clap dir> <bin dir>
        check_equal "$1 version" "$version" "$(module_version "$1/Sedef Amp.vst3")"
        check_equal "$1 binary" "$(sha "$new/$so")" "$(sha "$1/${so#VST3/}")"
        check_equal "$2 binary" "$(sha "$new/CLAP/Sedef Amp.clap")" "$(sha "$2/Sedef Amp.clap")"
        check_equal "$3 binary" "$(sha "$new/Standalone/Sedef Amp")" "$(sha "$3/sedef-amp")"
        leftovers
    }

    say "2. Update now, in your home folder"
    "$tool" --install "$dist/Sedef-Amp-Linux-x86_64.tar.gz" --expect "$version" || fail "the install failed"
    check_scope "$HOME/.vst3" "$HOME/.clap" "$HOME/.local/bin"

    say "3. Update now, for every user (the root script: through sudo here, pkexec in the plug-in)"
    "$tool" --install "$dist/Sedef-Amp-Linux-x86_64.tar.gz" --expect "$version" --system ||
        fail "the system-wide install failed"
    check_scope /usr/lib/vst3 /usr/lib/clap /usr/local/bin
    check_equal "the system VST3's owner" root "$(stat -c %U "/usr/lib/vst3/${so#VST3/}")"
    for f in "/usr/lib/vst3/${so#VST3/}" "/usr/lib/clap/Sedef Amp.clap" /usr/local/bin/sedef-amp; do
        sudo -n -u nobody test -r "$f" && echo "ok: every user can read $f" ||
            fail "another user can't read $f"
    done
    ;;
esac

if ((failures > 0)); then
    echo
    echo "$failures check(s) failed" >&2
    exit 1
fi
echo
echo "Install test passed"
