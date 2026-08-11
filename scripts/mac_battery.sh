#!/bin/bash
# mac_battery.sh — the Mac-side evidence run for a packed darwin madc
# (macos-release-lane plan W2). The build container cannot execute darwin
# binaries, so THIS script, run on a Mac against the exact release artifact,
# is the release checklist's "mac battery log" evidence.
#
#   bash mac_battery.sh <path-to-madc>       (e.g. bin/madc from the tarball)
#
# Self-contained: probes are written to a temp dir; nothing else from the
# repo is needed. Works on the stock macOS bash 3.2. Paste the full output
# back to the build side.
#
# What it proves, in order:
#   1  the binary runs at all and its baked version prints (AMFI accepted
#      the ad-hoc signature — the first probe would otherwise be "Killed: 9")
#   2  C surface from the embedded prelude (no CLT needed)
#   3  C++ surface from the packed forest groves (string/vector/map/sstream/
#      iostream — on a header-less Mac these can ONLY come from the forest)
#   3b cout << "hi" as its OWN probe — the darwin sret arc's driving symptom;
#      unmaskable by defects earlier in the chained groves probe
#   4  the madc::value intrinsic, include-free
#   5  exec:// channels end-to-end (spawn /usr/bin/sort, value carriers)
#   6  AOT object round-trip (-c then run the .o through MIR's loader) —
#      known-unsupported on darwin until the Mach-O object writer lands; the
#      leg asserts a LOUD decline at whichever stage says no (today: the .o
#      LOAD declines; the -c step exits 0)
#   6b emit-C indirect return ON TARGET: --emit=c11 output compiled by the
#      HOST cc against libc++ and executed (needs cc; info-skip when absent)
#   6c emitted-C runtime archive (W3): a try/catch-entering program's emitted
#      C linked against the tarball's lib/libmadc_rt.a and executed (needs
#      cc; info-skip when absent; a tarball without the archive FAILS)
#   6d native AOT executable: madc -o -static-libmadc on a C++ program —
#      the image loads libc++ by LC_LOAD_DYLIB, binds imports flat, and
#      carries its madc runtime ledger-merged (no cc involved; a binary
#      predating the dylib-binding slice emits an image that dies at dyld)
#   7  compile latency of a <string> program (reported, not gated)

MADC="$1"
if [ -z "$MADC" ] || [ ! -x "$MADC" ]; then
    echo "usage: $0 <path-to-madc>" >&2
    exit 2
fi
MADC=$(cd "$(dirname "$MADC")" && pwd)/$(basename "$MADC")

WORK=$(mktemp -d /tmp/madc-battery.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

PASS=0
FAIL=0

now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time()*1000'; }

check() {
    # check <name> <expected-output-file> <cmd...>
    local name="$1" expect="$2"; shift 2
    local out rc
    out=$("$@" 2>&1); rc=$?
    if [ $rc -eq 0 ] && [ "$out" = "$(cat "$expect")" ]; then
        echo "ok   - $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL - $name (rc=$rc)"
        echo "--- got:";      printf '%s\n' "$out" | sed 's/^/    /'
        echo "--- expected:"; sed 's/^/    /' "$expect"
        FAIL=$((FAIL + 1))
    fi
}

echo "== madc mac battery: $MADC =="
echo "== host: $(uname -m) $(sw_vers -productVersion 2>/dev/null) =="

# --- 1. version / signature acceptance ------------------------------------
cat > version.mad <<'EOF'
#include <ns_madc>
#include <stdio.h>
int main() { printf("%s\n", madc::sys.version); return 0; }
EOF
V_OUT=$("$MADC" version.mad 2>&1); V_RC=$?
if [ $V_RC -eq 0 ] && [ -n "$V_OUT" ]; then
    echo "ok   - version ($V_OUT)"
    PASS=$((PASS + 1))
else
    echo "FAIL - version (rc=$V_RC): $V_OUT"
    echo "       (Killed: 9 here = AMFI rejected the signature)"
    FAIL=$((FAIL + 1))
fi

# --- 2. C surface (embedded prelude) ---------------------------------------
cat > chello.mad <<'EOF'
#include <stdio.h>
#include <math.h>
#include <string.h>
int main() {
    char buf[32];
    strcpy(buf, "prelude");
    printf("%s %.3f %d\n", buf, sqrt(2.0), (int)strlen(buf));
    return 0;
}
EOF
printf 'prelude 1.414 7\n' > chello.expect
check "C prelude (stdio/math/string)" chello.expect "$MADC" chello.mad

# --- 3. C++ groves (the packed forest) --------------------------------------
cat > groves.mad <<'EOF'
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>
int main() {
    std::vector<int> v;
    for (int i = 5; i > 0; --i) v.push_back(i * i);
    std::map<std::string, int> m;
    m["alpha"] = v[0];
    m["beta"] = v[4];
    std::ostringstream os;
    os << "sz=" << v.size();
    std::string s = os.str();
    std::cout << s << " a=" << m["alpha"] << " b=" << m["beta"] << std::endl;
    return 0;
}
EOF
printf 'sz=5 a=25 b=1\n' > groves.expect
check "C++ groves (string/vector/map/sstream/iostream)" groves.expect "$MADC" groves.mad

# --- 3b. cout << "hi", alone -------------------------------------------------
# The single construct that drove the darwin sret arc (sessions #76-#78), as
# its own probe. The groves leg above chains several constructs, so one
# earlier line hitting a known-open defect masks every stream operation after
# it — a leg a known-open defect can mask is not evidence for anything past
# the mask, which is exactly how the #78 fix stayed invisible to this battery
# (3 passed / 3 failed, identical composition, while direct probes all
# passed). This leg cannot be masked.
cat > couthi.mad <<'EOF'
#include <iostream>
int main() {
    std::cout << "hi" << std::endl;
    return 0;
}
EOF
printf 'hi\n' > couthi.expect
check "iostream alone (cout << \"hi\" — the darwin sret probe)" couthi.expect "$MADC" couthi.mad

# --- 4. value intrinsic, include-free ---------------------------------------
cat > val.mad <<'EOF'
int main() {
    value v = 41;
    v = v + 1;
    value s = "answer=";
    var joined = s + v;
    printf("%s\n", joined.c_str());
    return 0;
}
EOF
printf 'answer=42\n' > val.expect
check "value intrinsic (include-free)" val.expect "$MADC" val.mad

# --- 5. exec:// channel ------------------------------------------------------
cat > chan.mad <<'EOF'
#include <ns_madc>
#include <stdio.h>
int main() {
    madc::channel sorter("exec://sort");
    sorter.write("pear\n");
    sorter.write("apple\n");
    sorter.write("mango\n");
    sorter.close_write();
    value line;
    // readline() strips the trailing newline (<ns_madc> contract).
    while (sorter.readline(line))
        printf("%s\n", line.c_str());
    return 0;
}
EOF
printf 'apple\nmango\npear\n' > chan.expect
check "exec:// channel (sort round-trip)" chan.expect "$MADC" chan.mad

# --- 6. AOT object round-trip ------------------------------------------------
cat > aot.mad <<'EOF'
#include <stdio.h>
int main() { printf("aot %d\n", 6 * 7); return 0; }
EOF
printf 'aot 42\n' > aot.expect
# Mach-O relocatables are not written or loaded yet (the MIR object seam is
# ELF-only so far), so on darwin this leg is a KNOWN-UNSUPPORTED expectation,
# not a failure. It is still a real check, classified by exit status: madc
# must DECLINE LOUDLY at whichever stage says no. On the 15.3.2 arm64 Mac
# that is the RUN (the -c step exits 0; loading the .o declines) — the
# session-#78 run proved the old shape here accepted only a compile-stage
# decline and reported FAIL for exactly the behaviour this leg was written
# to accept ("read the code, not the note"). A silent nonzero, or exit 0
# with wrong output, are genuine failures — and the day the Mach-O writer
# lands, the success arm starts gating the real round-trip with no edit
# here. The decline's own words are echoed rather than matched, so the log
# records what madc actually said instead of a guess.
rm -f aot.o
if "$MADC" -c aot.mad -o aot.o > aot.compile.out 2>&1; then
    AOT_OUT=$("$MADC" aot.o 2>&1); AOT_RC=$?
    if [ $AOT_RC -eq 0 ] && [ "$AOT_OUT" = "$(cat aot.expect)" ]; then
        echo "ok   - AOT object round-trip (-c then run .o)"
        PASS=$((PASS + 1))
    elif [ $AOT_RC -ne 0 ] && [ -n "$AOT_OUT" ]; then
        echo "ok   - AOT object round-trip (known-unsupported on darwin: .o load declined)"
        echo "       declined: $(printf '%s\n' "$AOT_OUT" | head -1)"
        PASS=$((PASS + 1))
    else
        echo "FAIL - AOT object round-trip (rc=$AOT_RC)"
        if [ $AOT_RC -ne 0 ]; then
            echo "    .o load failed SILENTLY (no diagnostic)"
        else
            echo "--- got:";      printf '%s\n' "$AOT_OUT" | sed 's/^/    /'
            echo "--- expected:"; sed 's/^/    /' aot.expect
        fi
        FAIL=$((FAIL + 1))
    fi
elif [ -s aot.compile.out ] && [ ! -e aot.o ]; then
    echo "ok   - AOT object round-trip (known-unsupported on darwin: compile declined)"
    echo "       declined: $(head -1 aot.compile.out)"
    PASS=$((PASS + 1))
else
    echo "FAIL - AOT object round-trip (compile step declined badly)"
    [ -e aot.o ] && echo "    left an object behind despite failing"
    [ -s aot.compile.out ] || echo "    failed SILENTLY (no diagnostic)"
    sed 's/^/    /' aot.compile.out
    FAIL=$((FAIL + 1))
fi

# --- 6b. emit-C indirect return, ON TARGET ------------------------------------
# The emitted-C twin of the sret arc: --emit=c11 must spell a by-value class
# return so that APPLE's own toolchain compiles it to the C++ convention
# (x8 = destination address). std::locale is 8 bytes — exactly the size C
# returns in registers — so only the __madc_ret_ pad-struct shape makes this
# correct. Session #80 proved the shape executes here (specimen byte-matched
# the Apple clang++ oracle); this leg keeps that proof in the standing
# evidence. The reducer deliberately avoids stream INSERTION: inserted-through
# paths instantiate try/catch bodies that call the madc exception runtime —
# that is leg 6c's job (libmadc_rt, W3), and keeping THIS leg runtime-free
# means it still proves the sret shape on tarballs without the archive. The
# darwin flavor is libc++ by default, so no -stdlib flag (older binaries
# stay invocable).
cat > emitloc.mad <<'EOF'
#include <iostream>
#include <locale>
#include <cstdio>
int main()
{
    std::locale l = std::cout.getloc();
    printf("loc ok\n");
    return 0;
}
EOF
printf 'loc ok\n' > emitloc.expect
if ! command -v cc > /dev/null 2>&1; then
    echo "info - emit-C sret on-target: skipped (no cc on this Mac; nothing proven)"
elif ! "$MADC" --emit=c11 emitloc.mad > emitloc.c 2> emitloc.emit.err; then
    echo "FAIL - emit-C sret on-target (--emit=c11 failed)"
    head -3 emitloc.emit.err | sed 's/^/    /'
    FAIL=$((FAIL + 1))
elif ! grep -q "^extern struct __madc_ret_locale" emitloc.c; then
    echo "FAIL - emit-C sret on-target (emitted C still spells the first-argument"
    echo "       shape — this binary predates the indirect-return pass; a"
    echo "       release-macos build at/after 7e484749 clears this)"
    FAIL=$((FAIL + 1))
elif cc -std=c11 -O0 -w -o emitloc.bin emitloc.c -lc++ 2> emitloc.cc.err; then
    check "emit-C sret on-target (cc-compiled emitted C, x8 convention)" emitloc.expect ./emitloc.bin
else
    echo "FAIL - emit-C sret on-target (cc rejected the emitted C)"
    head -3 emitloc.cc.err | sed 's/^/    /'
    FAIL=$((FAIL + 1))
fi

# --- 6c. emitted-C runtime archive (libmadc_rt, W3) ----------------------------
# The gap leg 6b steps around, closed: a program whose emitted C ENTERS a
# try/catch (any cout insertion does, via libc++'s __put_character_sequence)
# calls the madc exception runtime — the 8-symbol vocabulary session #80
# measured with nm -u on exactly such a binary. The tarball ships
# lib/libmadc_rt.a for that link; this leg emits, links against the archive,
# runs, and gates the output. A tarball without the archive predates W3 and
# FAILS with the reason (release evidence names what is missing; it does not
# skip it).
RTLIB="$(dirname "$MADC")/../lib/libmadc_rt.a"
cat > emitrt.mad <<'EOF'
#include <iostream>
int main()
{
    std::cout << "rt ok" << std::endl;
    return 0;
}
EOF
printf 'rt ok\n' > emitrt.expect
if ! command -v cc > /dev/null 2>&1; then
    echo "info - emitted-C runtime archive: skipped (no cc on this Mac; nothing proven)"
elif [ ! -f "$RTLIB" ]; then
    echo "FAIL - emitted-C runtime archive (no lib/libmadc_rt.a beside this binary —"
    echo "       the tarball predates W3; a release-macos build at/after the"
    echo "       libmadc_rt change ships it)"
    FAIL=$((FAIL + 1))
elif ! "$MADC" --emit=c11 emitrt.mad > emitrt.c 2> emitrt.emit.err; then
    echo "FAIL - emitted-C runtime archive (--emit=c11 failed)"
    head -3 emitrt.emit.err | sed 's/^/    /'
    FAIL=$((FAIL + 1))
elif cc -std=c11 -O0 -w -o emitrt.bin emitrt.c "$RTLIB" -lc++ 2> emitrt.cc.err; then
    check "emitted-C runtime archive (try/catch-entering program + libmadc_rt)" emitrt.expect ./emitrt.bin
else
    echo "FAIL - emitted-C runtime archive (cc rejected the emitted C or the link)"
    head -3 emitrt.cc.err | sed 's/^/    /'
    FAIL=$((FAIL + 1))
fi

# --- 6d. native AOT executable (madc -o, C++ world) ---------------------------
# The dylib-binding slice's on-target proof, and the strongest single AOT
# probe: madc emits a signed Mach-O executable DIRECTLY (no cc), whose C++
# imports bind flat-namespace against the LC_LOAD_DYLIB'd libc++ and whose
# madc runtime (try/catch via cout's __put_character_sequence) rides
# ledger-merged inside the image. A binary predating the slice emits an
# image whose C++ binds die at dyld (missing __ZNSt3__14coutE) — the run
# check catches and prints that.
cat > aotcpp.mad <<'EOF'
#include <iostream>
int main()
{
    std::cout << "aot c++ ok" << std::endl;
    return 0;
}
EOF
printf 'aot c++ ok\n' > aotcpp.expect
if ! "$MADC" -static-libmadc -o aotcpp.bin aotcpp.mad 2> aotcpp.emit.err; then
    echo "FAIL - native AOT executable (-static-libmadc emit failed)"
    head -3 aotcpp.emit.err | sed 's/^/    /'
    FAIL=$((FAIL + 1))
else
    check "native AOT executable (madc -o, C++ world, flat dylib binds)" aotcpp.expect ./aotcpp.bin
fi

# --- 7. compile latency (report only) ----------------------------------------
T0=$(now_ms)
"$MADC" groves.mad > /dev/null 2>&1
T1=$(now_ms)
echo "info - <string>+groves program wall time: $((T1 - T0)) ms (packed forest engaged if ~100ms-scale)"

echo "== battery: $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
