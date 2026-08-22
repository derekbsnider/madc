#!/bin/bash
# check-program-cache.sh — the transparent per-manifest program cache's gate
# (S5, docs/plans/2026-08-21-project-prelude-forest.md). The suite runners
# export MADC_NO_PROGRAM_CACHE=1 so ordinary project tests keep testing the
# LIVE compile; THIS gate owns the cached lanes end-to-end on a hermetic
# two-TU project in tmp/:
#
#   1. cold run populates __madcache__ (one container per TU), output = live
#   2. warm run is byte-identical AND refreezes nothing (mtime touchstone)
#   3. the warm fast path actually engages (MIR blob loads, -v evidence) —
#      a silently dead cache lane would pass every equality check
#   4. staleness bites (the gate's NEGATIVE CONTROL): an edited TU must
#      change the output — a stale cache would replay the old value — and
#      exactly that TU's container refreezes
#   5. a corrupted container self-heals: the run still succeeds with
#      correct output (cache = derived state, never a wrong run)
#   6. both kill switches (env + --no-program-cache) leave no cache behind
#
# Every madc invocation is capped (ulimit -t + timeout) per testing law.
set -u
cd "$(dirname "$0")/.." || exit 9
MADC=${MADC_BIN:-bin/madc}
unset MADC_NO_PROGRAM_CACHE	# this gate owns the cached lane
D=tmp/progcache_gate.$$
mkdir -p "$D"
trap 'rm -rf "$D"' EXIT

cat > "$D/band.inc" <<'EOF'
#define BAND_WORD "alpha"
EOF
cat > "$D/main.mad" <<'EOF'
extern long helper_tag();
extern long boot_tag;
#include "band.inc"

int main()
{
	println("{}", format("gate {} {} {}", helper_tag(), boot_tag, BAND_WORD));
	return 0;
}
EOF
cat > "$D/helper.mad" <<'EOF'
long helper_tag() { return 41; }
long boot_tag = helper_tag() + 1;
EOF
cat > "$D/gate.cc.json" <<'EOF'
[
  { "file": "main.mad" },
  { "file": "helper.mad" }
]
EOF

run() {	# run <outfile> [extra madc flags...]
    local out="$1"; shift
    ( ulimit -t 60; timeout 60 "$MADC" "$@" "$D/gate.cc.json" ) \
	> "$out" 2> "$out.err"
}

fail() { echo "check-program-cache: FAIL — $1" >&2; exit 1; }

# 1. Live reference, then the cold cached run.
MADC_NO_PROGRAM_CACHE=1 run "$D/live.out"
grep -q "gate 41 42 alpha" "$D/live.out" \
    || fail "live run did not produce the expected line (harness broken?)"
[ -d "$D/__madcache__" ] && fail "kill-switched live run created __madcache__"
run "$D/cold.out"
cmp -s "$D/live.out" "$D/cold.out" || fail "cold cached output != live output"
n=$(ls "$D/__madcache__"/*.forest 2>/dev/null | wc -l)
[ "$n" -eq 2 ] || fail "expected 2 cache containers, found $n"

# 2. Warm run: byte-identical, refreezes nothing.
touch "$D/stamp"
sleep 1
run "$D/warm.out"
cmp -s "$D/live.out" "$D/warm.out" || fail "warm cached output != live output"
if [ -n "$(find "$D/__madcache__" -type f -newer "$D/stamp")" ]; then
    fail "warm run refroze a container (staleness key unstable)"
fi

# 3. The fast path engages: the -v warm run must show the MIR blob loading
# for both TUs — equality checks alone cannot see a dead cache lane.
( ulimit -t 60; timeout 60 "$MADC" -v "$D/gate.cc.json" ) \
    > "$D/verbose.out" 2>&1
loads=$(grep -ac "module loaded from container" "$D/verbose.out")
[ "$loads" -ge 2 ] || fail "warm thaw did not load MIR blobs ($loads of 2)"

# 4. Staleness bites (negative control): the edited TU's NEW value must
# appear — a stale cache would replay 41/42 — and only helper refreezes.
touch "$D/stamp2"
sleep 1
sed -i 's/return 41/return 51/' "$D/helper.mad"
run "$D/edit.out"
grep -q "gate 51 52 alpha" "$D/edit.out" \
    || fail "edited TU's output did not change — STALE CACHE REPLAYED"
refroze=$(find "$D/__madcache__" -type f -newer "$D/stamp2" -name '*.forest')
case "$refroze" in
    *helper*) : ;;
    *) fail "edited helper.mad did not refreeze its container" ;;
esac
echo "$refroze" | grep -q "main" \
    && fail "unedited main.mad refroze (per-TU staleness too coarse)"

# 5. A corrupted container self-heals (derived state, never a wrong run).
hc=$(ls "$D/__madcache__"/helper-*.forest | head -1)
truncate -s 17 "$hc"
run "$D/heal.out"
grep -q "gate 51 52 alpha" "$D/heal.out" \
    || fail "corrupt container did not self-heal to a correct run"
[ -s "$hc" ] || fail "corrupt container was not regrown"
[ "$(stat -c %s "$hc")" -gt 17 ] || fail "corrupt container was not refrozen"

# 6. Kill switches: no cache read OR written.
rm -rf "$D/__madcache__"
MADC_NO_PROGRAM_CACHE=1 run "$D/off_env.out"
cmp -s "$D/off_env.out" "$D/edit.out" || fail "env kill-switch changed output"
[ -d "$D/__madcache__" ] && fail "env kill-switch still created __madcache__"
run "$D/off_flag.out" --no-program-cache
cmp -s "$D/off_flag.out" "$D/edit.out" || fail "--no-program-cache changed output"
[ -d "$D/__madcache__" ] && fail "--no-program-cache still created __madcache__"

echo "check-program-cache: cold/warm byte-identical, MIR fast path engaged, per-TU staleness bites, corruption self-heals, kill switches clean"
exit 0
