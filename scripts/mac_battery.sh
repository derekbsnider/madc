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
#   4  the madc::value intrinsic, include-free
#   5  exec:// channels end-to-end (spawn /usr/bin/sort, value carriers)
#   6  AOT object round-trip (-c then run the .o through MIR's loader)
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
    while (sorter.readline(line))
        printf("%s", line.c_str());
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
if "$MADC" -c aot.mad -o aot.o > aot.compile.out 2>&1; then
    check "AOT object round-trip (-c then run .o)" aot.expect "$MADC" aot.o
else
    echo "FAIL - AOT object round-trip (compile step)"
    sed 's/^/    /' aot.compile.out
    FAIL=$((FAIL + 1))
fi

# --- 7. compile latency (report only) ----------------------------------------
T0=$(now_ms)
"$MADC" groves.mad > /dev/null 2>&1
T1=$(now_ms)
echo "info - <string>+groves program wall time: $((T1 - T0)) ms (packed forest engaged if ~100ms-scale)"

echo "== battery: $PASS passed, $FAIL failed =="
[ $FAIL -eq 0 ]
