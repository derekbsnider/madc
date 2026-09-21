#!/usr/bin/env bash
# Cross-TU freeze-consumer gate (#20): freeze a SMALL multi-header C++
# corpus, then compile SEPARATE consumer TUs against it. The other forest
# gates freeze each test's OWN TU, which misses the entire class of
# producer/consumer identity splits: a frozen corpus whose restored
# products must serve a consumer that never parsed the headers live.
#
# This is the gate the release-lane freeze regression (#20) lacked. Its
# consumers pin the specific lanes that broke:
#   A — the defect-P reducer (tmp/fmin_use.mad): a <vector> consumer whose
#       push_back identity crossed the int/int32_t flavor split and whose
#       emitted module declared `int32_t *` in a TU that never emits that
#       typedef ("unknown type int32_t").
#   B — a <memory>-only consumer: binding <memory>'s chain restores pmr
#       basic_string products whose method default-args (`= _Alloc()`)
#       name types the closure filter dropped; the flush's re-parse must
#       skip them, never SIGABRT the bind.
#   C — a bare-script .mad consumer: auto-include's synthetic
#       `#include <string>` must bind its frozen unit BEFORE the one-shot
#       decl restore (it used to inject at parse() start — too late under
#       a bind — leaving `string` undeclared only when packed).
#   D — the RANKER'S INPUTS survive the freeze (LOADED == parsed on the
#       overload set): an explicit-template-argument call into a restored
#       std::min set. Every candidate both runs know must record the SAME
#       template-argument identities, and the bound run must not re-mint
#       (a __oN twin) an instance it restored — before the declaration
#       identity rode the DK_FUNC record, a restored member ranked blank
#       and the consumer instantiated a third std::min<uint64_t>.
# Every leg pins BIND ENGAGEMENT (-v shows "bound to grove unit"), so a
# silent live fall-through cannot false-green it, and output parity
# against a --no-forest-bind live parse.
set -u
set -o pipefail
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

BIN=bin/madc
if [ ! -x "$BIN" ]; then
    echo "forest_crosstu_gate: missing $BIN"
    exit 1
fi

D=tmp/xtugate
rm -rf "$D"
mkdir -p "$D"

fail() { echo "forest_crosstu_gate: $1"; exit 1; }

# A copy of the dev binary is the bind subject (its sidecar path is
# $D/madc.forest — never beside the real bin/madc).
cp "$BIN" "$D/madc"
# thin-CLI subject loader (PK2): the copy's $ORIGIN/../lib rpath misses from
# this depth — hand the LINKER the build tree's lib so the subject can LOAD;
# the dev libmadc.so is unpacked, so forest-discovery arms are unaffected.
export LD_LIBRARY_PATH="$(pwd)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cat > "$D/producer.cpp" <<'EOF'
#include <string>
#include <vector>
#include <memory>
int main() { return 0; }
EOF

# Consumer A — the defect-P reducer, verbatim.
cat > "$D/use_vector.mad" <<'EOF'
#include <vector>
int main() { std::vector<int> v; v.push_back(7); __builtin_printf("%d %zu\n", v[0], v.size()); return 0; }
EOF

# Consumer B — <memory>-only: the bind itself replays the corpus's
# restored default-arg runs (the pmr SIGABRT lane).
cat > "$D/use_memory.mad" <<'EOF'
#include <memory>
int main() { std::allocator<int> a; int *q = a.allocate(1); *q = 42; __builtin_printf("%d\n", *q); a.deallocate(q, 1); return 0; }
EOF

# Consumer C — zero-ceremony script: `string` arrives via auto-include.
cat > "$D/use_script.mad" <<'EOF'
string s = "xtu";
__builtin_printf("%s %zu\n", s.c_str(), s.length());
EOF

timeout 120 "$BIN" --freeze="$D/corpus.msnap" "$D/producer.cpp" >/dev/null 2>&1 \
    || fail "corpus --freeze failed"
cp "$D/corpus.msnap" "$D/madc.forest"

check_leg() {
    local name="$1" src="$2" want="$3"
    local live bound out
    live=$(env -u MADC_FOREST timeout 120 "$D/madc" --no-forest-bind "$src" 2>/dev/null) \
        || fail "[$name] live reference run failed"
    [ "$live" = "$want" ] || fail "[$name] live reference output wrong: '$live'"
    bound=$(env -u MADC_FOREST timeout 120 "$D/madc" -v "$src" 2>&1 | tr -d '\0') \
        || fail "[$name] bound compile failed"
    grep -q <<<"$bound" 'bound to grove unit' || fail "[$name] did not bind"
    out=$(env -u MADC_FOREST timeout 120 "$D/madc" "$src" 2>/dev/null) \
        || fail "[$name] bound run failed"
    [ "$out" = "$want" ] || fail "[$name] bound output '$out' != live '$want'"
}

# Consumer D — an explicit-template-argument call into a restored set.
cat > "$D/use_min.mad" <<'EOF'
#include <vector>
int main() {
    std::vector<int> v; v.push_back(7); v.push_back(9);
    unsigned long a = v.size(), b = 5;
    unsigned long m = std::min<unsigned long>(a, b);
    __builtin_printf("%lu %d\n", m, v[1]);
    return 0;
}
EOF

# The ranker's inputs, live vs bound: MADC_OVL_PROBE2=<substr> prints one
# `[ovl2] cand=<symbol> targs='..', expl='..',` line per candidate of an
# explicit-template-argument call. Membership may legitimately differ (the
# corpus froze more headers than the consumer includes), so the pin is per
# candidate BOTH runs know — and there must be at least one.
check_ranker_inputs() {
    local name="$1" src="$2" probe="$3" live bound common=0 line cand bline
    live=$(env -u MADC_FOREST MADC_OVL_PROBE2="$probe" timeout 120 "$D/madc" --no-forest-bind "$src" 2>&1 >/dev/null \
           | tr -d '\0' | grep -a '^\[ovl2\]' | sort -u)
    bound=$(env -u MADC_FOREST MADC_OVL_PROBE2="$probe" timeout 120 "$D/madc" "$src" 2>&1 >/dev/null \
           | tr -d '\0' | grep -a '^\[ovl2\]' | sort -u)
    [ -n "$live" ] || fail "[$name] the live run printed no ranker inputs (probe dead?)"
    [ -n "$bound" ] || fail "[$name] the bound run printed no ranker inputs"
    while IFS= read -r line; do
        cand=$(sed -n 's/^\[ovl2\] cand=\([^ ]*\) .*/\1/p' <<<"$line")
        [ -n "$cand" ] || continue
        bline=$(grep -a -F "cand=$cand " <<<"$bound")
        if [ -n "$bline" ]; then
            common=$((common + 1))
            [ "$line" = "$bline" ] \
                || fail "[$name] ranker inputs differ for $cand: live '$line' bound '$bline'"
        fi
        grep -a -q -F "cand=${cand}__o" <<<"$bound" \
            && fail "[$name] the bound run re-instantiated a restored instance: ${cand}__oN"
    done <<<"$live"
    [ "$common" -ge 1 ] || fail "[$name] no candidate known to both runs (probe substring '$probe' matched nothing shared)"
}

check_leg "vector"  "$D/use_vector.mad"  "7 1"
check_leg "memory"  "$D/use_memory.mad"  "42"
check_leg "script"  "$D/use_script.mad"  "xtu 3"
check_leg "min"     "$D/use_min.mad"     "2 9"
check_ranker_inputs "min" "$D/use_min.mad" "min"

echo "forest_crosstu_gate: OK (corpus freeze + 4 cross-TU consumers bind with live parity; the ranker's inputs LOADED == parsed)"
