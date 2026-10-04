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
#
# Per-artifact probes (each asserts its failure mode loudly, and every
# positive probe has a NEGATIVE CONTROL proving the gate can fail):
#   deb/rpm  dpkg -x / rpm2cpio|cpio extract; installed madc runs a probe
#            program (output asserted) with LD_LIBRARY_PATH=<root libdir>
#            (an extracted root has no ldconfig — the real install
#            registers the .so); `madc -v` must say
#            `forest-bind: [library-image] opened container` — the frozen
#            header forest served from the INSTALLED libmadc.so.0
#            [control: the non-verbose run must NOT match the pattern];
#            installed madcide on a real pty (scripts/install_gate_pty.py)
#            = NORESCUE + probe file rendered, profiles found through the
#            share/madcide layout [control: hide share/madcide/profiles
#            => RESCUE banner]; installed madcide SAVES and QUITS from a
#            foreign cwd (scripts/madcide_save_quit_pty.py: joe's keys, then
#            the rescue keys with the profiles hidden) [control: hide
#            share/madcide/verbs => it refuses to start, with the reason].
#            installed chthonia prints its usage line and `-c check`s a
#            <stdio.h> program clean from a foreign cwd [control: a syntax
#            error => 1 problem, rc 1]; its desktop entry and icon ship.
#   tar      same probes with NO LD_LIBRARY_PATH at all (env -u) — the
#            run-time $ORIGIN proof (the packager's ldd check is static;
#            this one executes) [control: hide lib/libmadc.so.0 => madc
#            must fail to run — proving the pass wasn't served by some
#            system copy].
#   winzip   unzip; the zipped madc.exe under wine compiles a
#            runtime-needing probe with -o INTO bin/ (PE binding is
#            adjacency) and the emitted exe runs (output asserted);
#            zipped `madcide.exe --help` prints its usage line; bin/verbs and
#            bin/checks sit beside it [control: hide bin/libmadc-0.dll =>
#            both must fail].
#
#   mactar   (darwin host only — the release.yml mac jobs; the container
#            cannot execute darwin binaries and prints a stated SKIP) tar
#            extract; the shipped bin/madc runs the probe (output asserted);
#            `madc -v` says `forest-bind: [library-image] opened container`
#            — the forest served from the shipped lib/libmadc-0.dylib, the
#            thin CLI carrying none [control: hide lib/libmadc-0.dylib =>
#            madc must fail to run];
#            the shipped chthonia prints its usage line and `-c check`s a
#            <stdio.h> program clean from a foreign cwd [control: a syntax
#            error => 1 problem, rc 1];
#            scripts/mac_battery.sh against the extracted tarball layout
#            holds the PASS floor (MAC_BATTERY_FLOOR, default 8 = the
#            owner-hardware baseline) and prints every FAIL line [control:
#            hide lib/libmadc_rt.a => battery leg 6c must FAIL — proving
#            the battery exercised THIS tarball's archive]. Host arch only:
#            a tarball for the other arch is refused (no Rosetta proof).
#
# The pty probe forces cwd=/tmp (see install_gate_pty.py): the shipped
# madcide's {dirname(__FILE__)}/profiles arm is baked build-tree-RELATIVE,
# so from /tmp it misses and the INSTALLED layout is what's exercised.
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

# One probe pair, generated fresh per run (scratch-files rule: tmp/ only).
# pk4hello: runtime-needing madc-dialect program (println pulls the madc
# runtime, so the AOT form binds libmadc — the binding is what's under
# test; a plain-C hello would be runtime-free and prove nothing).
write_probes() {
    mkdir -p "$GATE_TMP"
    {
        printf 'var greeting = "%s";\n' "$MARKER"
        printf 'println("{}", greeting);\n'
    } > "$GATE_TMP/pk4hello.mad"
    # The editor probe: name AND content carry the marker the pty
    # classifier looks for after first paint.
    printf '// pk4probe — package_install_gate editor probe\n' \
        > "$GATE_TMP/pk4probe.mad"
}

# ---------- the linux probe battery ----------
# run_linux <kind> <root> <bindir> <libdir-or-"">
#   libdir nonempty => run with LD_LIBRARY_PATH=<libdir> (deb/rpm roots)
#   libdir empty    => run with NO LD_LIBRARY_PATH at all (tarball $ORIGIN)
run_linux() {
    local kind="$1" root="$2" bindir="$3" libdir="$4"
    local madc="$bindir/madc" madcide="$bindir/madcide"
    local hello probe out
    hello=$(readlink -f "$GATE_TMP/pk4hello.mad")
    probe=$(readlink -f "$GATE_TMP/pk4probe.mad")
    [ -x "$madc" ]    || fail "$kind" "no executable $madc in the artifact"
    [ -x "$madcide" ] || fail "$kind" "no executable $madcide in the artifact"

    # MADCIDE_CONFIG_DIR: no ambient user settings.json or plugins/ (the
    # run_tests.sh hermeticity); the artifact's own data is what is gated.
    local -a runenv
    if [ -n "$libdir" ]; then
        runenv=(env "LD_LIBRARY_PATH=$libdir" "MADCIDE_CONFIG_DIR=$GATE_TMP/no-config")
    else
        runenv=(env -u LD_LIBRARY_PATH "MADCIDE_CONFIG_DIR=$GATE_TMP/no-config")
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
    out=$( ( ulimit -t 120; timeout 60 "${runenv[@]}" "$madc" -v "$hello" ) 2>&1 \
           | grep 'forest-bind:' )
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

    # 3. installed madcide on a real pty: profile found, file painted
    out=$( ( ulimit -t 120; timeout 90 "${runenv[@]}" \
             python3 scripts/install_gate_pty.py "$madcide" "$probe" pk4probe ) 2>&1 )
    case "$out" in
        NORESCUE\ FILESEEN\ *) ok "$kind" "installed madcide: NORESCUE + probe file rendered ($out)" ;;
        *) fail "$kind" "installed madcide pty probe expected 'NORESCUE FILESEEN' (got: $out)" ;;
    esac

    # 4. negative control: hide the installed profiles => RESCUE banner.
    local pdir="$root/${libdir:+usr/}share/madcide/profiles"
    [ -d "$pdir" ] || fail "$kind" "no profiles dir at $pdir in the artifact"
    local gdir="$root/${libdir:+usr/}share/madcide/plugins"
    [ -f "$gdir/default/default.plugin" ] \
        || fail "$kind" "no plugins/default/default.plugin at $gdir in the artifact"
    # Every shipped plugin with code carries its built library beside its
    # source (scripts/build_shipped_plugins.sh; plan §41.11a step 6).
    local pg pn
    for pg in "$gdir"/*/; do
        pg="${pg%/}"
        pn=$(basename "$pg")
        grep -q '"code"' "$pg/$pn.plugin" 2>/dev/null || continue
        [ -f "$pg/$pn.so" ] \
            || fail "$kind" "plugin '$pn' ships no library ($pn.so) at $pg in the artifact"
    done
    mv "$pdir" "$pdir.hidden"
    out=$( ( ulimit -t 120; timeout 90 "${runenv[@]}" \
             python3 scripts/install_gate_pty.py "$madcide" "$probe" pk4probe ) 2>&1 )
    mv "$pdir.hidden" "$pdir"
    case "$out" in
        RESCUE\ *) ok "$kind" "negative control: hidden profiles => RESCUE banner ($out)" ;;
        *) fail "$kind" "negative control broken: profiles hidden but no RESCUE (got: $out)" ;;
    esac

    # 5. installed madcide SAVES and QUITS from a foreign cwd (B85: its
    #    save and quit are the line editor's verbs, shipped under
    #    share/madcide/verbs): joe's ^K D, ^K Q; then with the profiles
    #    hidden, the rescue ^S, ^Q; then with the verbs hidden it must
    #    refuse to start, with the reason.
    local sq="$GATE_TMP/$kind-savequit.c"
    printf 'int main(void) { return 0; }\n' > "$sq"
    out=$( ( ulimit -t 120; timeout 120 "${runenv[@]}" \
             python3 scripts/madcide_save_quit_pty.py "$sq" 0b640b71 "$madcide" ) 2>&1 )
    case "$out" in
        "SAVED EXITED rc=0 NORESCUE VERBSOK"*) ok "$kind" "installed madcide saves and quits ($out)" ;;
        *) fail "$kind" "installed madcide did not save and quit from a foreign cwd (got: $out)" ;;
    esac
    printf 'int main(void) { return 0; }\n' > "$sq"
    mv "$pdir" "$pdir.hidden"
    out=$( ( ulimit -t 120; timeout 120 "${runenv[@]}" \
             python3 scripts/madcide_save_quit_pty.py "$sq" 1311 "$madcide" ) 2>&1 )
    mv "$pdir.hidden" "$pdir"
    case "$out" in
        "SAVED EXITED rc=0 RESCUE VERBSOK"*) ok "$kind" "hidden profiles: the rescue keys save and quit ($out)" ;;
        *) fail "$kind" "hidden profiles: the rescue keys did not save and quit (got: $out)" ;;
    esac
    local vdir="$root/${libdir:+usr/}share/madcide/verbs"
    [ -d "$vdir" ] || fail "$kind" "no verbs dir at $vdir in the artifact"
    printf 'int main(void) { return 0; }\n' > "$sq"
    mv "$vdir" "$vdir.hidden"
    out=$( ( ulimit -t 120; timeout 120 "${runenv[@]}" \
             python3 scripts/madcide_save_quit_pty.py "$sq" "" "$madcide" ) 2>&1 )
    mv "$vdir.hidden" "$vdir"
    case "$out" in
        *"EXITED rc=1 "*"VERBSMISSING"*) ok "$kind" "negative control: hidden verbs => refused to start ($out)" ;;
        *) fail "$kind" "negative control broken: verbs hidden but madcide did not refuse (got: $out)" ;;
    esac

    # 6. installed chthonia (the learning IDE on madcide's base): its usage
    #    line, then `-c check` from a foreign cwd over a C file that includes
    #    <stdio.h> — the installed verbs (share/madcide) and the forest in the
    #    installed libmadc serve it — clean; [control: a file with a syntax
    #    error reports its problem and exits 1, proving the check read it].
    #    Its desktop entry and the 256 px icon are in the artifact.
    local chthonia="$bindir/chthonia" chk="$PWD/$GATE_TMP/pk4chk.c" bad="$PWD/$GATE_TMP/pk4bad.c"
    [ -x "$chthonia" ] || fail "$kind" "no executable $chthonia in the artifact"
    [ -f "$root/${libdir:+usr/}share/applications/chthonia.desktop" ] \
        || fail "$kind" "no share/applications/chthonia.desktop in the artifact"
    [ -f "$root/${libdir:+usr/}share/icons/hicolor/256x256/apps/chthonia.png" ] \
        || fail "$kind" "no share/icons/hicolor/256x256/apps/chthonia.png in the artifact"
    out=$( ( ulimit -t 120; timeout 60 "${runenv[@]}" "$chthonia" --help ) 2>&1 )
    case "$out" in
        *"usage: chthonia"*) ok "$kind" "installed chthonia prints its usage line" ;;
        *) fail "$kind" "installed chthonia --help did not print its usage line (got: $out)" ;;
    esac
    printf '#include <stdio.h>\nint main(void) { printf("%%d\\n", 5); return 0; }\n' > "$chk"
    printf '#include <stdio.h>\nint main(void) { return 0 }\n' > "$bad"
    out=$( ( cd /tmp && ulimit -t 120 && timeout 60 "${runenv[@]}" "$chthonia" "$chk" -c check ) 2>&1 )
    local rc=$?
    case "$rc:$out" in
        0:*Problems*) ok "$kind" "installed chthonia -c check over <stdio.h> is clean (rc 0)" ;;
        *) fail "$kind" "installed chthonia -c check over <stdio.h> was not clean (rc $rc: $out)" ;;
    esac
    out=$( ( cd /tmp && ulimit -t 120 && timeout 60 "${runenv[@]}" "$chthonia" "$bad" -c check ) 2>&1 )
    rc=$?
    case "$rc:$out" in
        1:*"1 problem"*) ok "$kind" "negative control: a syntax error => chthonia -c check reports 1 problem (rc 1)" ;;
        *) fail "$kind" "negative control broken: chthonia -c check over a syntax error (rc $rc: $out)" ;;
    esac

    # 7. installed chthonia offers every shipped key style from its menu:
    #    Tools ▸ Key bindings… is on its Tools menu, and the list it opens
    #    names the six styles share/madcide/profiles carries [control: hide
    #    the profiles => the list names none of them].
    keystyle_gate "$kind" "$chthonia" "$chk" "$pdir" timeout "${runenv[@]}"
}

# The installed chthonia's key styles (probe 7, every artifact that runs it):
# `-c "menushow Tools"` lists the Key bindings… row, `-c keystyle` lists the
# styles by their display names, all six; with the profiles directory hidden
# the list names none. `tmo` is the platform's timeout command (timeout, or
# brew coreutils' gtimeout on a Mac runner); the rest is the run environment.
KEY_STYLES=("Chthonia" "VS Code" "Vim" "Emacs" "JOE" "Pico")
keystyle_gate() {
    local kind="$1" chthonia="$2" file="$3" pdir="$4" tmo="$5"
    shift 5
    local out style missing="" named=""
    out=$( ( cd /tmp && ulimit -t 120 && "$tmo" 60 "$@" "$chthonia" "$file" -c "menushow Tools" ) 2>&1 )
    case "$out" in
        *"Key bindings"*) ok "$kind" "installed chthonia's Tools menu has Key bindings…" ;;
        *) fail "$kind" "installed chthonia's Tools menu has no Key bindings… row (got: $out)" ;;
    esac
    out=$( ( cd /tmp && ulimit -t 120 && "$tmo" 60 "$@" "$chthonia" "$file" -c keystyle ) 2>&1 )
    for style in "${KEY_STYLES[@]}"; do
        case "$out" in *". $style"*) ;; *) missing="$missing [$style]" ;; esac
    done
    [ -z "$missing" ] || fail "$kind" "installed chthonia's Key bindings list lacks$missing (got: $out)"
    ok "$kind" "installed chthonia's Key bindings list names all ${#KEY_STYLES[@]} styles"
    mv "$pdir" "$pdir.hidden"
    out=$( ( cd /tmp && ulimit -t 120 && "$tmo" 60 "$@" "$chthonia" "$file" -c keystyle ) 2>&1 )
    mv "$pdir.hidden" "$pdir"
    for style in "${KEY_STYLES[@]}"; do
        case "$out" in *". $style"*) named="$named [$style]" ;; esac
    done
    [ -z "$named" ] || fail "$kind" "negative control broken: profiles hidden but the list still names$named"
    ok "$kind" "negative control: hidden profiles => the Key bindings list names no style"
}

gate_deb() {
    local artifact="$1" root="$GATE_TMP/deb"
    [ -f "$artifact" ] || fail deb "artifact not found: $artifact"
    rm -rf "$root"; mkdir -p "$root"
    dpkg -x "$artifact" "$root" || fail deb "dpkg -x refused $artifact"
    root=$(readlink -f "$root")
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
    bindir=$(readlink -f "$root/bin")
    export WINEDEBUG=-all
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

    # 3. the zipped madcide.exe loads, binds, and runs (--help prints the usage line)
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine madcide.exe --help ) 2> /dev/null | tr -d '\r' )
    case "$out" in
        *"usage: madcide"*) ok winzip "zipped madcide.exe prints its usage line" ;;
        *) fail winzip "zipped madcide.exe usage smoke failed (got: $out)" ;;
    esac
    # The line editor's verbs and checks ride beside the exe (B85: save and
    # quit are verbs; resolve_data_dir's <exedir>/verbs arm). wine has no
    # pty here, so the layout is checked; the save/quit run is the Linux
    # gate's and the genuine-Windows lane's.
    [ -f "$bindir/verbs/w.madv" ] && [ -f "$bindir/verbs/q.madv" ] \
        && [ -f "$bindir/verbs/_subject.madv" ] && [ -f "$bindir/checks/editable.madv" ] \
        || fail winzip "the zip carries no bin/verbs or bin/checks (madcide could not save or quit)"
    ok winzip "zipped madcide's verbs and checks sit beside the exe"
    [ -f "$bindir/plugins/default/default.plugin" ] \
        || fail winzip "the zip carries no bin/plugins/default/default.plugin"
    ok winzip "zipped madcide's plugins sit beside the exe"

    # 4. negative control: hide the engine DLL => both must fail
    #    (proves the green runs above were bound by adjacency to the
    #    zipped libmadc-0.dll, not something on WINEPATH).
    mv "$bindir/libmadc-0.dll" "$bindir/libmadc-0.dll.hidden"
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine ./pk4hello.exe ) 2>&1 | tr -d '\r' )
    case "$out" in
        *"$MARKER"*) mv "$bindir/libmadc-0.dll.hidden" "$bindir/libmadc-0.dll"
                     fail winzip "negative control broken: emitted exe ran without libmadc-0.dll" ;;
    esac
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine madcide.exe --help ) 2> /dev/null | tr -d '\r' )
    mv "$bindir/libmadc-0.dll.hidden" "$bindir/libmadc-0.dll"
    case "$out" in
        *"usage: madcide"*) fail winzip "negative control broken: madcide.exe ran without libmadc-0.dll" ;;
        *) ok winzip "negative control: hidden libmadc-0.dll => neither binary runs" ;;
    esac
    echo "package_install_gate: PASS winzip ($artifact)"
}

# ---------- the macOS tarball (darwin host) ----------
gate_mactar() {
    local artifact="$1" scratch="$GATE_TMP/mactar" root out host want passed floor tmo
    [ -f "$artifact" ] || fail mactar "artifact not found: $artifact"
    host=$(uname -s)
    if [ "$host" != Darwin ]; then
        echo "package_install_gate: SKIP mactar ($artifact — darwin binaries do not execute on $host; the release.yml mac job runs this leg)"
        return 0
    fi
    tmo=$(command -v timeout || command -v gtimeout || true)
    [ -n "$tmo" ] || fail mactar "no timeout/gtimeout on this host (brew coreutils)"
    rm -rf "$scratch"; mkdir -p "$scratch"
    tar -C "$scratch" -xzf "$artifact" || fail mactar "tar refused $artifact"
    root=$(echo "$scratch"/madc-*-macos-*)
    [ -d "$root" ] || fail mactar "expected one madc-*-macos-<arch> root in $artifact"
    root=$(cd "$root" && pwd)
    want=$(uname -m)
    case "$root" in
        *"-macos-$want") ;;
        *) fail mactar "$artifact is not a $want tarball — the gate runs the host arch only (no Rosetta proof)" ;;
    esac
    [ -x "$root/bin/madc" ]        || fail mactar "no executable bin/madc in the artifact"
    [ -f "$root/lib/libmadc_rt.a" ] || fail mactar "no lib/libmadc_rt.a in the artifact"
    [ -f "$root/lib/libmadcwebview.dylib" ] || fail mactar "no lib/libmadcwebview.dylib in the artifact"
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
    out=$( ( ulimit -t 120; "$tmo" 60 "$root/bin/madc" -v "$PWD/$GATE_TMP/pk4hello.mad" ) 2>&1 \
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

    # 1c. the IDE (a darwin host of this arch builds it into the tarball):
    # chthonia's usage line, then `-c check` from a foreign cwd over a
    # <stdio.h> program — share/madcide's verbs and the library's forest
    # serve it — clean [control: a syntax error => 1 problem, rc 1].
    [ -x "$root/bin/madcide" ] || fail mactar "no executable bin/madcide in the artifact"
    [ -x "$root/bin/chthonia" ] || fail mactar "no executable bin/chthonia in the artifact"
    [ -d "$root/share/madcide/verbs" ] || fail mactar "no share/madcide/verbs in the artifact"
    out=$( ( ulimit -t 120; "$tmo" 60 "$root/bin/chthonia" --help ) 2>&1 )
    case "$out" in
        *"usage: chthonia"*) ok mactar "shipped chthonia prints its usage line" ;;
        *) fail mactar "shipped chthonia --help did not print its usage line (got: $out)" ;;
    esac
    local chk="$PWD/$GATE_TMP/pk4chk.c" bad="$PWD/$GATE_TMP/pk4bad.c" rc
    printf '#include <stdio.h>\nint main(void) { printf("%%d\\n", 5); return 0; }\n' > "$chk"
    printf '#include <stdio.h>\nint main(void) { return 0 }\n' > "$bad"
    out=$( ( cd /tmp && ulimit -t 120 && "$tmo" 60 "$root/bin/chthonia" "$chk" -c check ) 2>&1 )
    rc=$?
    case "$rc:$out" in
        0:*Problems*) ok mactar "shipped chthonia -c check over <stdio.h> is clean (rc 0)" ;;
        *) fail mactar "shipped chthonia -c check over <stdio.h> was not clean (rc $rc: $out)" ;;
    esac
    out=$( ( cd /tmp && ulimit -t 120 && "$tmo" 60 "$root/bin/chthonia" "$bad" -c check ) 2>&1 )
    rc=$?
    case "$rc:$out" in
        1:*"1 problem"*) ok mactar "negative control: a syntax error => chthonia -c check reports 1 problem (rc 1)" ;;
        *) fail mactar "negative control broken: chthonia -c check over a syntax error (rc $rc: $out)" ;;
    esac
    # 1d. its key styles, the menu row and the list (keystyle_gate).
    keystyle_gate mactar "$root/bin/chthonia" "$chk" "$root/share/madcide/profiles" "$tmo"

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

# ---------- dispatch ----------
mode="${1:-}"
write_probes
case "$mode" in
    deb)    gate_deb    "${2:?usage: package_install_gate.sh deb <artifact>}" ;;
    rpm)    gate_rpm    "${2:?usage: package_install_gate.sh rpm <artifact>}" ;;
    tar)    gate_tar    "${2:?usage: package_install_gate.sh tar <artifact>}" ;;
    winzip) gate_winzip "${2:?usage: package_install_gate.sh winzip <artifact>}" ;;
    mactar) gate_mactar "${2:?usage: package_install_gate.sh mactar <artifact>}" ;;
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
        echo "usage: package_install_gate.sh <deb|rpm|tar|winzip|mactar> <artifact> | all" >&2
        exit 2
        ;;
esac
