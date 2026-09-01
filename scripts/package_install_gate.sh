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
#            => RESCUE banner].
#   tar      same probes with NO LD_LIBRARY_PATH at all (env -u) — the
#            run-time $ORIGIN proof (the packager's ldd check is static;
#            this one executes) [control: hide lib/libmadc.so.0 => madc
#            must fail to run — proving the pass wasn't served by some
#            system copy].
#   winzip   unzip; the zipped madc.exe under wine compiles a
#            runtime-needing probe with -o INTO bin/ (PE binding is
#            adjacency) and the emitted exe runs (output asserted);
#            zipped madcide.exe prints its usage line [control: hide
#            bin/libmadc-0.dll => both must fail].
#
# The macOS tarball is NOT gateable here — the container cannot execute
# darwin binaries; its install proof is the mac_battery on owner hardware.
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
    mv "$pdir" "$pdir.hidden"
    out=$( ( ulimit -t 120; timeout 90 "${runenv[@]}" \
             python3 scripts/install_gate_pty.py "$madcide" "$probe" pk4probe ) 2>&1 )
    mv "$pdir.hidden" "$pdir"
    case "$out" in
        RESCUE\ *) ok "$kind" "negative control: hidden profiles => RESCUE banner ($out)" ;;
        *) fail "$kind" "negative control broken: profiles hidden but no RESCUE (got: $out)" ;;
    esac
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

    # 3. the zipped madcide.exe loads, binds, and runs (usage line)
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine madcide.exe ) 2> /dev/null | tr -d '\r' )
    case "$out" in
        *"usage: madcide"*) ok winzip "zipped madcide.exe prints its usage line" ;;
        *) fail winzip "zipped madcide.exe usage smoke failed (got: $out)" ;;
    esac

    # 4. negative control: hide the engine DLL => both must fail
    #    (proves the green runs above were bound by adjacency to the
    #    zipped libmadc-0.dll, not something on WINEPATH).
    mv "$bindir/libmadc-0.dll" "$bindir/libmadc-0.dll.hidden"
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine ./pk4hello.exe ) 2>&1 | tr -d '\r' )
    case "$out" in
        *"$MARKER"*) mv "$bindir/libmadc-0.dll.hidden" "$bindir/libmadc-0.dll"
                     fail winzip "negative control broken: emitted exe ran without libmadc-0.dll" ;;
    esac
    out=$( ( cd "$bindir" && ulimit -t 120 && timeout 60 wine madcide.exe ) 2> /dev/null | tr -d '\r' )
    mv "$bindir/libmadc-0.dll.hidden" "$bindir/libmadc-0.dll"
    case "$out" in
        *"usage: madcide"*) fail winzip "negative control broken: madcide.exe ran without libmadc-0.dll" ;;
        *) ok winzip "negative control: hidden libmadc-0.dll => neither binary runs" ;;
    esac
    echo "package_install_gate: PASS winzip ($artifact)"
}

# ---------- dispatch ----------
mode="${1:-}"
write_probes
case "$mode" in
    deb)    gate_deb    "${2:?usage: package_install_gate.sh deb <artifact>}" ;;
    rpm)    gate_rpm    "${2:?usage: package_install_gate.sh rpm <artifact>}" ;;
    tar)    gate_tar    "${2:?usage: package_install_gate.sh tar <artifact>}" ;;
    winzip) gate_winzip "${2:?usage: package_install_gate.sh winzip <artifact>}" ;;
    all)
        VER=$(cat VERSION)
        found=0
        for deb in dist/madc_${VER}-*_amd64.deb;        do [ -f "$deb" ] && { gate_deb "$deb"; found=1; }; done
        for rpm in dist/madc-${VER}-*.x86_64.rpm;       do [ -f "$rpm" ] && { gate_rpm "$rpm"; found=1; }; done
        tarball="dist/madc-${VER}-linux-x86_64.tar.gz"
        [ -f "$tarball" ] && { gate_tar "$tarball"; found=1; }
        winzip="dist/madc-${VER}-windows-x86_64.zip"
        [ -f "$winzip" ] && { gate_winzip "$winzip"; found=1; }
        [ "$found" = 1 ] || fail all "no gateable ${VER} artifacts in dist/"
        echo "package_install_gate: PASS all (version ${VER})"
        ;;
    *)
        echo "usage: package_install_gate.sh <deb|rpm|tar|winzip> <artifact> | all" >&2
        exit 2
        ;;
esac
