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

check_leg "vector"  "$D/use_vector.mad"  "7 1"
check_leg "memory"  "$D/use_memory.mad"  "42"
check_leg "script"  "$D/use_script.mad"  "xtu 3"

echo "forest_crosstu_gate: OK (corpus freeze + 3 cross-TU consumers bind with live parity)"
