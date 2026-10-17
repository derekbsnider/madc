#!/bin/bash
# package_install_gate.sh — PK4: install-then-run validation of the SHIPPED
# artifact bytes (packaging arc, docs/plans/2026-09-01-packaging-arc.md).
#
# The packagers' in-flight smokes prove the STAGE; this gate re-proves the
# ARTIFACT — extract the actual .deb/.rpm/.tar.gz/.zip a user downloads
# into a scratch root and run the installed binaries from there. Runs on
# the build container, wired into the packagers (NOT into fulltest:
# packaging is a release ceremony, not a per-merge-wave event).
#
#   bash scripts/package_install_gate.sh deb    dist/madc_<ver>-<rel>_amd64.deb
#   bash scripts/package_install_gate.sh rpm    dist/madc-<ver>-<rel>.x86_64.rpm
#   bash scripts/package_install_gate.sh tar    dist/madc-<ver>-linux-x86_64.tar.gz
#   bash scripts/package_install_gate.sh winzip dist/madc-<ver>-windows-x86_64.zip
#   bash scripts/package_install_gate.sh mactar dist/madc-<ver>-macos-<arch>.tar.gz   (darwin host)
#   bash scripts/package_install_gate.sh all    # every gateable artifact for VERSION
#   xvfb-run -a bash scripts/package_install_gate.sh apparmor   # the INSTALLED 24.04 .deb's
#            windows under AppArmor's user-namespace restriction (sudo; release.yml)
#
# Per-artifact probes (each asserts its failure mode loudly, and every
# positive probe has a NEGATIVE CONTROL proving the gate can fail):
#   every    the shipped notices: each carrier packaging/notices.tsv names for
#            the artifact's platform is in it, and so is its notice — a file
#            check, so mactar runs it on Linux too [control: hide the last
#            row's notice => reported missing]. Except winzip: every entry
#            of the artifact's listing is readable by every user, every
#            directory searchable [control: a 0640 entry => reported].
#   deb/rpm  dpkg -x / rpm2cpio|cpio extract; installed madc runs a probe
#            program (output asserted) with LD_LIBRARY_PATH=<root libdir>
#            (an extracted root has no ldconfig — the real install
#            registers the .so); `madc -v` over a probe that includes a
#            system header (pk4forest.c) must say
#            `forest-bind: [library-image] opened container` — the frozen
#            header forest served from the INSTALLED libmadc.so.0 (a program
#            that includes nothing binds nothing and never opens it)
#            [control: the non-verbose run must NOT match the pattern].
#   tar      same probes with NO LD_LIBRARY_PATH at all (env -u) — the
#            run-time $ORIGIN proof (the packager's ldd check is static;
#            this one executes) [control: hide lib/libmadc.so.0 => madc
#            must fail to run — proving the pass wasn't served by some
#            system copy].
#   winzip   unzip; the zipped madc.exe under wine compiles a
#            runtime-needing probe with -o INTO bin/ (PE binding is
#            adjacency) and the emitted exe runs (output asserted)
#            [control: hide bin/libmadc-0.dll => the exe must fail].
#
#   mactar   (darwin host only — the release.yml mac jobs; the container
#            cannot execute darwin binaries and prints a stated SKIP) tar
#            extract; the shipped bin/madc runs the probe (output asserted);
#            `madc -v` over pk4forest.c says
#            `forest-bind: [library-image] opened container`
#            — the forest served from the shipped lib/libmadc-0.dylib, the
#            thin CLI carrying none [control: hide lib/libmadc-0.dylib =>
#            madc must fail to run];
#            scripts/mac_battery.sh against the extracted tarball layout
#            holds the PASS floor (MAC_BATTERY_FLOOR, default 8 = the
#            owner-hardware baseline) and prints every FAIL line [control:
#            hide lib/libmadc_rt.a => battery leg 6c must FAIL — proving
#            the battery exercised THIS tarball's archive]. Host arch only:
#            a tarball for the other arch is refused (no Rosetta proof).
#
# The IDE, madcide, is its own packages, with their own installed checks
# (github.com/derekbsnider/madcide: scripts/check_install.sh).
set -u
cd "$(dirname "$0")/.."

MARKER="pk4-install-gate-alive"
GATE_TMP="tmp/install_gate"

fail() {
    echo "package_install_gate: FAIL [$1] $2" >&2
    exit 1
}
ok() {
    echo "package_install_gate: ok   [$1] $2"
}

# The probe, generated fresh per run (scratch-files rule: tmp/ only).
# pk4hello: runtime-needing madc-dialect program (println pulls the madc
# runtime, so the AOT form binds libmadc — the binding is what's under
# test; a plain-C hello would be runtime-free and prove nothing).
write_probes() {
    mkdir -p "$GATE_TMP"
    {
        printf 'var greeting = "%s";\n' "$MARKER"
        printf 'println("{}", greeting);\n'
    } > "$GATE_TMP/pk4hello.mad"
    # pk4forest: includes a system header, so its compile binds the frozen
    # forest (a C file through the pack's source arm) — the forest check's
    # probe; pk4hello includes nothing, binds nothing, and never opens it.
    {
        printf '#include <stdio.h>\n'
        printf 'int main(void) { printf("%%s\\n", "%s"); return 0; }\n' "$MARKER"
    } > "$GATE_TMP/pk4forest.c"
}

# ---------- shipped notices (packaging/notices.tsv) ----------
# A static check — file names only — so it runs on every artifact, mactar
# included where the container cannot execute darwin binaries.
NOTICES=packaging/notices.tsv

# notices_missing <platform> <root>: one line per row whose carrier or notice
# is absent from the extracted artifact (by file name, anywhere under root).
notices_missing() {
    local platform="$1" root="$2" p carrier notice rest
    while IFS=$'\t' read -r p carrier notice rest; do
        case "$p" in ''|'#'*) continue ;; esac
        [ "$p" = "$platform" ] || continue
        [ -n "$(find "$root" -name "$carrier" -print -quit)" ] || echo "carrier $carrier"
        [ -n "$(find "$root" -name "$notice" -print -quit)" ] || echo "notice $notice (for $carrier)"
    done < "$NOTICES"
}

# check_notices <kind> <platform> <root>: every carrier the platform's rows
# name ships, and so does its notice [control: hide the last row's notice —
# the check must report it].
check_notices() {
    local kind="$1" platform="$2" root="$3" rows missing last hidden
    rows=$(awk -F'\t' -v p="$platform" '$1 == p' "$NOTICES" | wc -l)
    [ "$rows" -gt 0 ] || fail "$kind" "$NOTICES has no $platform rows"
    missing=$(notices_missing "$platform" "$root")
    [ -z "$missing" ] || fail "$kind" "missing from the artifact ($NOTICES): $(echo $missing)"
    ok "$kind" "every carrier ships its notice ($rows rows, $NOTICES)"
    last=$(awk -F'\t' -v p="$platform" '$1 == p { n = $3 } END { print n }' "$NOTICES")
    hidden=$(find "$root" -name "$last" -print -quit)
    mv "$hidden" "$hidden.hidden"
    missing=$(notices_missing "$platform" "$root")
    mv "$hidden.hidden" "$hidden"
    case "$missing" in
        *"notice $last "*) ok "$kind" "negative control: hidden $last => reported missing" ;;
        *) fail "$kind" "negative control broken: hidden $last was not reported" ;;
    esac
}

# check_modes <kind> <long listing>: every entry of the artifact (its own
# listing, so no extraction's umask intervenes) is readable by every user and
# every directory searchable [control: a 0640 entry => reported].
modes_wrong() {
    awk 'substr($1, 8, 1) != "r" || (substr($1, 1, 1) == "d" && substr($1, 10, 1) !~ /[xt]/)' <<< "$1"
}
check_modes() {
    local kind="$1" wrong
    wrong=$(modes_wrong "$2")
    [ -z "$wrong" ] || fail "$kind" "entries another user cannot read: $(echo "$wrong" | head -5 | tr '\n' ';')"
    ok "$kind" "every entry is readable, and every directory searchable, by every user"
    wrong=$(modes_wrong "-rw-r----- root/root 1 2026-10-05 12:00 ./usr/pk4modes")
    case "$wrong" in
        *pk4modes*) ok "$kind" "negative control: a 0640 entry => reported" ;;
        *) fail "$kind" "negative control broken: a 0640 entry was not reported" ;;
    esac
}

# ---------- the linux probe battery ----------
# run_linux <kind> <root> <bindir> <libdir-or-"">
#   libdir nonempty => run with LD_LIBRARY_PATH=<libdir> (deb/rpm roots)
#   libdir empty    => run with NO LD_LIBRARY_PATH at all (tarball $ORIGIN)
run_linux() {
    local kind="$1" root="$2" bindir="$3" libdir="$4"
    local madc="$bindir/madc"
    local hello out
    hello=$(readlink -f "$GATE_TMP/pk4hello.mad")
    [ -x "$madc" ] || fail "$kind" "no executable $madc in the artifact"

    local -a runenv
    if [ -n "$libdir" ]; then
        runenv=(env "LD_LIBRARY_PATH=$libdir")
    else
        runenv=(env -u LD_LIBRARY_PATH)
    fi

    # 1. installed madc runs a program
    out=$( ( ulimit -t 120; timeout 60 "${runenv[@]}" "$madc" "$hello" ) 2>&1 )
    case "$out" in
        *"$MARKER"*) ok "$kind" "installed madc runs (JIT output asserted)" ;;
        *) fail "$kind" "installed madc did not produce '$MARKER' (got: $out)" ;;
    esac

    # 2. the forest is served from the INSTALLED library image
    # (all arm lines: the search prints '[<arm>] no image at ...' for the
    # arms it walks past before the one that opens)
    out=$( ( ulimit -t 120; timeout 60 "${runenv[@]}" "$madc" -v \
             "$(readlink -f "$GATE_TMP/pk4forest.c")" ) 2>&1 | grep 'forest-bind:' )
    case "$out" in
        *"forest-bind: [library-image] opened container"*)
            ok "$kind" "forest served from the installed libmadc.so.0 ([library-image])" ;;
        *) fail "$kind" "-v never said 'forest-bind: [library-image] opened container' (got: $out)" ;;
    esac
    # negative control for the grep: without -v the line must be ABSENT
    # (proves the pattern needs the real evidence, not a vacuous match).
    out=$( ( ulimit -t 120; timeout 60 "${runenv[@]}" "$madc" "$hello" ) 2>&1 )
    case "$out" in
        *"forest-bind:"*) fail "$kind" "negative control broken: non-verbose run mentions forest-bind" ;;
        *) ok "$kind" "negative control: forest-bind evidence absent without -v" ;;
    esac
}

gate_deb() {
    local artifact="$1" root="$GATE_TMP/deb"
    [ -f "$artifact" ] || fail deb "artifact not found: $artifact"
    rm -rf "$root"; mkdir -p "$root"
    dpkg -x "$artifact" "$root" || fail deb "dpkg -x refused $artifact"
    root=$(readlink -f "$root")
    check_notices deb linux "$root"
    check_modes deb "$(dpkg-deb -c "$artifact")"
    run_linux deb "$root" "$root/usr/bin" "$root/usr/lib/x86_64-linux-gnu"
    echo "package_install_gate: PASS deb ($artifact)"
}

gate_rpm() {
    local artifact="$1" root="$GATE_TMP/rpm" abs
    [ -f "$artifact" ] || fail rpm "artifact not found: $artifact"
    abs=$(readlink -f "$artifact")
    rm -rf "$root"; mkdir -p "$root"
    ( cd "$root" && rpm2cpio "$abs" | cpio -idm --quiet ) \
        || fail rpm "rpm2cpio|cpio refused $artifact"
    root=$(readlink -f "$root")
    check_notices rpm linux "$root"
    check_modes rpm "$(rpm -qlvp "$artifact")"
    run_linux rpm "$root" "$root/usr/bin" "$root/usr/lib64"
    echo "package_install_gate: PASS rpm ($artifact)"
}

gate_tar() {
    local artifact="$1" scratch="$GATE_TMP/tar" root out
    [ -f "$artifact" ] || fail tar "artifact not found: $artifact"
    rm -rf "$scratch"; mkdir -p "$scratch"
    tar -C "$scratch" -xzf "$artifact" || fail tar "tar refused $artifact"
    root=$(echo "$scratch"/madc-*-linux-x86_64)
    [ -d "$root" ] || fail tar "expected one madc-*-linux-x86_64 root in $artifact"
    root=$(readlink -f "$root")
    check_notices tar linux "$root"
    check_modes tar "$(tar -tzvf "$artifact")"
    run_linux tar "$root" "$root/bin" ""
    # tarball-only negative control: hide the shipped library — the run
    # must FAIL, proving the green run above bound THIS lib via $ORIGIN
    # and not some system copy.
    mv "$root/lib/libmadc.so.0" "$root/lib/libmadc.so.0.hidden"
    out=$( ( ulimit -t 120; timeout 60 env -u LD_LIBRARY_PATH \
             "$root/bin/madc" "$(readlink -f "$GATE_TMP/pk4hello.mad")" ) 2>&1 )
    mv "$root/lib/libmadc.so.0.hidden" "$root/lib/libmadc.so.0"
    case "$out" in
        *"$MARKER"*) fail tar "negative control broken: madc ran with lib/libmadc.so.0 hidden — \$ORIGIN was not the binding" ;;
        *) ok tar "negative control: hidden lib/libmadc.so.0 => madc cannot run" ;;
    esac
    echo "package_install_gate: PASS tar ($artifact)"
}

gate_winzip() {
    local artifact="$1" scratch="$GATE_TMP/winzip" root bindir out
    [ -f "$artifact" ] || fail winzip "artifact not found: $artifact"
    command -v wine > /dev/null 2>&1 || fail winzip "wine not available on this host"
    rm -rf "$scratch"; mkdir -p "$scratch"
    unzip -q "$artifact" -d "$scratch" || fail winzip "unzip refused $artifact"
    root=$(echo "$scratch"/madc-*-windows-x86_64)
    [ -d "$root" ] || fail winzip "expected one madc-*-windows-x86_64 root in $artifact"
    check_notices winzip windows "$root"
    bindir=$(readlink -f "$root/bin")
    export WINEDEBUG=-all
    # Adjacency is the binding under test: a caller's WINEPATH (the Windows
    # packager points it at bin/release-windows for its own builds) would
    # serve libmadc-0.dll from elsewhere, and the negative control below
    # would find the exe still running.
    unset WINEPATH
    wineserver -p 2> /dev/null || true

    # 1. the zipped madc.exe compiles a runtime-needing program with -o,
    #    emitting INTO bin/ — adjacency is PE's binding rule, for the
    #    toolchain's own DLLs and for what it emits next to them.
    cp "$GATE_TMP/pk4hello.mad" "$bindir/pk4hello.mad"
    ( cd "$bindir" && ulimit -t 600 && \
      timeout 600 wine madc.exe -o pk4hello.exe pk4hello.mad ) \
        || fail winzip "zipped madc.exe could not compile the probe (-o under wine)"
    [ -f "$bindir/pk4hello.exe" ] || fail winzip "-o reported success but emitted no pk4hello.exe"
    ok winzip "zipped madc.exe compiled the probe (-o under wine)"

    # 2. the emitted exe runs, binding libmadc-0.dll by adjacency
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine ./pk4hello.exe ) 2>&1 | tr -d '\r' )
    case "$out" in
        *"$MARKER"*) ok winzip "emitted exe runs beside the zipped DLLs (output asserted)" ;;
        *) fail winzip "emitted pk4hello.exe did not produce '$MARKER' (got: $out)" ;;
    esac

    # 3. negative control: hide the engine DLL => the exe must fail
    #    (proves the green run above was bound by adjacency to the
    #    zipped libmadc-0.dll, not something on WINEPATH).
    mv "$bindir/libmadc-0.dll" "$bindir/libmadc-0.dll.hidden"
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine ./pk4hello.exe ) 2>&1 | tr -d '\r' )
    mv "$bindir/libmadc-0.dll.hidden" "$bindir/libmadc-0.dll"
    case "$out" in
        *"$MARKER"*) fail winzip "negative control broken: emitted exe ran without libmadc-0.dll" ;;
        *) ok winzip "negative control: hidden libmadc-0.dll => the emitted exe does not run" ;;
    esac
    echo "package_install_gate: PASS winzip ($artifact)"
}

# ---------- the macOS tarball (darwin host) ----------
gate_mactar() {
    local artifact="$1" scratch="$GATE_TMP/mactar" root out host want passed floor tmo
    [ -f "$artifact" ] || fail mactar "artifact not found: $artifact"
    rm -rf "$scratch"; mkdir -p "$scratch"
    tar -C "$scratch" -xzf "$artifact" || fail mactar "tar refused $artifact"
    root=$(echo "$scratch"/madc-*-macos-*)
    [ -d "$root" ] || fail mactar "expected one madc-*-macos-<arch> root in $artifact"
    root=$(cd "$root" && pwd)
    check_notices mactar macos "$root"
    check_modes mactar "$(tar -tzvf "$artifact")"
    host=$(uname -s)
    if [ "$host" != Darwin ]; then
        echo "package_install_gate: SKIP mactar's run legs ($artifact — darwin binaries do not execute on $host; the release.yml mac job runs them)"
        return 0
    fi
    tmo=$(command -v timeout || command -v gtimeout || true)
    [ -n "$tmo" ] || fail mactar "no timeout/gtimeout on this host (brew coreutils)"
    want=$(uname -m)
    case "$root" in
        *"-macos-$want") ;;
        *) fail mactar "$artifact is not a $want tarball — the gate runs the host arch only (no Rosetta proof)" ;;
    esac
    [ -x "$root/bin/madc" ]        || fail mactar "no executable bin/madc in the artifact"
    [ -f "$root/lib/libmadc_rt.a" ] || fail mactar "no lib/libmadc_rt.a in the artifact"
    [ -f "$root/lib/madc/modules/ui_web.dylib" ] || fail mactar "no lib/madc/modules/ui_web.dylib in the artifact"
    [ -f "$root/lib/libmadc-0.dylib" ] || fail mactar "no lib/libmadc-0.dylib in the artifact"

    # 1. the shipped madc runs a program
    out=$( ( ulimit -t 120; "$tmo" 60 "$root/bin/madc" "$PWD/$GATE_TMP/pk4hello.mad" ) 2>&1 )
    case "$out" in
        *"$MARKER"*) ok mactar "shipped madc runs (JIT output asserted)" ;;
        *) fail mactar "shipped madc did not produce '$MARKER' (got: $out)" ;;
    esac

    # 1b. the forest is served from the shipped library image (the thin CLI
    # carries none); the grep's control is the non-verbose run above, which
    # asserted the marker and so ran with no -v evidence lines.
    out=$( ( ulimit -t 120; "$tmo" 60 "$root/bin/madc" -v "$PWD/$GATE_TMP/pk4forest.c" ) 2>&1 \
           | grep 'forest-bind:' )
    case "$out" in
        *"forest-bind: [library-image] opened container"*)
            ok mactar "forest served from the shipped lib/libmadc-0.dylib ([library-image])" ;;
        *) fail mactar "-v never said 'forest-bind: [library-image] opened container' (got: $out)" ;;
    esac
    # control: hide the library => the thin CLI cannot run at all (proves
    # the pass was served by THIS tarball's library, not some other copy).
    mv "$root/lib/libmadc-0.dylib" "$root/lib/libmadc-0.dylib.hidden"
    out=$( ( ulimit -t 120; "$tmo" 60 "$root/bin/madc" "$PWD/$GATE_TMP/pk4hello.mad" ) 2>&1 )
    mv "$root/lib/libmadc-0.dylib.hidden" "$root/lib/libmadc-0.dylib"
    case "$out" in
        *"$MARKER"*) fail mactar "negative control broken: lib/libmadc-0.dylib hidden but madc still ran" ;;
        *) ok mactar "negative control: hidden lib/libmadc-0.dylib => madc does not run" ;;
    esac

    # 2. the Mac battery against the extracted tarball layout (bin/madc +
    # lib/libmadc_rt.a beside it = leg 6c's shape). The full output is the
    # release evidence — print it; gate the PASS count.
    floor="${MAC_BATTERY_FLOOR:-8}"
    out=$( ( ulimit -t 600; "$tmo" 900 bash scripts/mac_battery.sh "$root/bin/madc" ) 2>&1 )
    printf '%s\n' "$out" | sed 's/^/    | /'
    passed=$(printf '%s\n' "$out" | sed -n 's/^== battery: \([0-9]*\) passed.*/\1/p')
    [ -n "$passed" ] || fail mactar "mac_battery printed no '== battery:' summary"
    if [ "$passed" -ge "$floor" ]; then
        ok mactar "mac battery: $passed passed (floor $floor); FAIL lines: $(printf '%s\n' "$out" | grep -c '^FAIL')"
    else
        fail mactar "mac battery: $passed passed < floor $floor"
    fi

    # 3. negative control: hide the shipped runtime archive => leg 6c FAILS.
    command -v cc >/dev/null 2>&1 || fail mactar "negative control needs cc (leg 6c info-skips without it)"
    mv "$root/lib/libmadc_rt.a" "$root/lib/libmadc_rt.a.hidden"
    out=$( ( ulimit -t 600; "$tmo" 900 bash scripts/mac_battery.sh "$root/bin/madc" ) 2>&1 )
    mv "$root/lib/libmadc_rt.a.hidden" "$root/lib/libmadc_rt.a"
    case "$out" in
        *"FAIL - emitted-C runtime archive"*) ok mactar "negative control: hidden lib/libmadc_rt.a => battery leg 6c FAILS" ;;
        *) fail mactar "negative control broken: lib/libmadc_rt.a hidden but leg 6c did not FAIL" ;;
    esac
    echo "package_install_gate: PASS mactar ($artifact)"
}

# ---------- the installed .deb's window under AppArmor (Ubuntu 24.04+) ----------
# gate_apparmor: madc's .deb INSTALLED (apt), the user-namespace restriction
# ON as a 24.04 desktop has it, a display (xvfb-run), sudo. By the profile
# the .deb installs, /usr/bin/madc runs a ui:: program to its first rendered
# page (tests/gui/ui_web_hello.mad's GUI_SNAPSHOT: WebKit's web process
# runs only in its bwrap sandbox); [control: the profile unloaded, the same
# program dies before its page — LP: #2046844].
gate_apparmor() {
    local prof=/etc/apparmor.d/madc prog=tests/gui/ui_web_hello.mad out rc
    [ -f "$prof" ] || fail apparmor "no $prof (the .deb installs it on Ubuntu 24.04 and later)"
    [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null)" = 1 ] \
        || fail apparmor "the user-namespace restriction is off; this gate needs it on"
    sudo grep -q "^madc " /sys/kernel/security/apparmor/profiles \
        || fail apparmor "the madc profile is not loaded (the .deb's postinst loads it)"
    ok apparmor "the .deb's madc profile is loaded"
    out=$( ( ulimit -t 120 -c 0; timeout 60 /usr/bin/madc "$prog" ) 2>&1 )
    rc=$?
    case "$rc:$out" in
        0:*GUI_SNAPSHOT*) ok apparmor "/usr/bin/madc runs a ui:: window to its rendered page" ;;
        *) fail apparmor "/usr/bin/madc's ui:: window did not render (rc $rc: $(echo "$out" | tail -3 | tr '\n' ' '))" ;;
    esac
    sudo apparmor_parser -R "$prof" || fail apparmor "could not unload $prof"
    out=$( ( ulimit -t 120 -c 0; timeout 60 /usr/bin/madc "$prog" ) 2>&1 )
    rc=$?
    sudo apparmor_parser -r -T -W "$prof" || fail apparmor "could not load $prof again"
    case "$out" in
        *GUI_SNAPSHOT*) fail apparmor "control broken: the profile unloaded, the window still rendered" ;;
        *) ok apparmor "control: the profile unloaded, the window died before its page (rc $rc)" ;;
    esac
    echo "package_install_gate: PASS apparmor"
}

# ---------- dispatch ----------
mode="${1:-}"
write_probes
case "$mode" in
    deb)    gate_deb    "${2:?usage: package_install_gate.sh deb <artifact>}" ;;
    rpm)    gate_rpm    "${2:?usage: package_install_gate.sh rpm <artifact>}" ;;
    tar)    gate_tar    "${2:?usage: package_install_gate.sh tar <artifact>}" ;;
    winzip) gate_winzip "${2:?usage: package_install_gate.sh winzip <artifact>}" ;;
    mactar) gate_mactar "${2:?usage: package_install_gate.sh mactar <artifact>}" ;;
    apparmor) gate_apparmor ;;
    all)
        VER=$(cat VERSION)
        found=0
        for deb in dist/madc_${VER}-*_amd64.deb;        do [ -f "$deb" ] && { gate_deb "$deb"; found=1; }; done
        for rpm in dist/madc-${VER}-*.x86_64.rpm;       do [ -f "$rpm" ] && { gate_rpm "$rpm"; found=1; }; done
        tarball="dist/madc-${VER}-linux-x86_64.tar.gz"
        [ -f "$tarball" ] && { gate_tar "$tarball"; found=1; }
        winzip="dist/madc-${VER}-windows-x86_64.zip"
        [ -f "$winzip" ] && { gate_winzip "$winzip"; found=1; }
        # the mac tarball of THIS host's arch, on a darwin host only
        if [ "$(uname -s)" = Darwin ]; then
            mactar="dist/madc-${VER}-macos-$(uname -m).tar.gz"
            [ -f "$mactar" ] && { gate_mactar "$mactar"; found=1; }
        fi
        [ "$found" = 1 ] || fail all "no gateable ${VER} artifacts in dist/"
        echo "package_install_gate: PASS all (version ${VER})"
        ;;
    *)
        echo "usage: package_install_gate.sh <deb|rpm|tar|winzip|mactar> <artifact> | apparmor | all" >&2
        exit 2
        ;;
esac
