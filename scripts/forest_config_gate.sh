#!/usr/bin/env bash
# Forest-carriers S6 gate: madc.ini — the configuration file and the
# precedence rule it completes, CLI > environment > madc.ini > baked defaults.
#
# Every setting leg is paired with a BASELINE leg that proves the assertion
# would fail without the config file: a "the ini worked" check that also passes
# with no ini present is worthless. The forest key is discovery ARM 5, so it
# also gets an arm-ORDER leg (a valid $MADC_FOREST must win before arm 5 is
# probed at all) and the strictness legs pin that a bad config REFUSES rather
# than half-applying.
#
# The dev binary is the vehicle: it is unpacked, so the discovery chain reaches
# the configured arms. Runs from the repo root (fulltest does); fixtures land
# in tmp/ (gitignored) and the ./madc.ini legs run with tmp/cfggate as the CWD
# so no config file is ever created in the repo root.
set -u
# pipefail so a madc failure inside a $(... | tr) capture trips the || fail
# guards. The greps below therefore read via here-strings, NOT pipes: `echo big
# | grep -q` dies of EPIPE when grep -q exits at the first match, and pipefail
# would turn that into a false failure.
set -o pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

ulimit -t 300 2>/dev/null

BIN=bin/madc
if [ ! -x "$BIN" ]; then
    echo "forest_config_gate: missing $BIN"
    exit 1
fi

D=tmp/cfggate
rm -rf "$D"
mkdir -p "$D/inc"

fail() { echo "forest_config_gate: $1"; exit 1; }

# A copy of the dev binary is the subject: its self-exe path is $D/madc, so no
# sidecar or config fixture can leak into another test run.
cp "$BIN" "$D/madc"
MADC=$ROOT/$D/madc

cat > "$D/consumer.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("cfggate ok\n"); return 0; }
EOF
cat > "$D/inc/cfgonly.h" <<'EOF'
#define CFGONLY_MARK 4242
EOF
cat > "$D/needsinc.c" <<'EOF'
#include <stdio.h>
#include <cfgonly.h>
int main(void) { printf("inc %d\n", CFGONLY_MARK); return 0; }
EOF

# The container the forest legs bind (freeze modes live-parse, so this is a
# plain live parse of a <stdio.h> user).
timeout 120 "$BIN" --freeze="$ROOT/$D/stdio.msnap" "$D/consumer.c" >/dev/null 2>&1 \
    || fail "--freeze failed"
echo "not a container" > "$D/junk.txt"

# ---- dialect: baked default vs the ini `std` key vs a CLI --std= -----------
# -dM is the observable: the selected dialect decides __STDC_VERSION__, and the
# baked default for a direct .c compile defines it not at all. So the baseline
# is the ABSENCE of the macro and each configured dialect is a distinct value —
# no leg can pass without the setting actually taking effect.
std_of() {   # $1.. = extra args; runs in $D so ./madc.ini is in scope
    ( cd "$D" && env -u MADC_FOREST timeout 120 ./madc -dM "$@" consumer.c 2>/dev/null )
}

base=$(std_of --no-config) || fail "[baseline-std] -dM run failed"
grep -q <<<"$base" '__STDC_VERSION__' \
    && fail "[baseline-std] the baked default already defines __STDC_VERSION__ (fixture is not a baseline)"

cat > "$D/madc.ini" <<'EOF'
# madc.ini fixture (forest-carriers S6 gate)
[madc]
std = c99
EOF
out=$(std_of) || fail "[ini-std] -dM run failed"
grep -q <<<"$out" '__STDC_VERSION__ 199901L' \
    || fail "[ini-std] the ini std key did not apply: $(grep <<<"$out" __STDC_VERSION__)"

out=$(std_of --std=c11) || fail "[cli-beats-ini] -dM run failed"
grep -q <<<"$out" '__STDC_VERSION__ 201112L' \
    || fail "[cli-beats-ini] the ini overrode the command line: $(grep <<<"$out" __STDC_VERSION__)"

out=$(std_of --no-config) || fail "[no-config] -dM run failed"
grep -q <<<"$out" '__STDC_VERSION__' \
    && fail "[no-config] --no-config still read the ini: $(grep <<<"$out" __STDC_VERSION__)"

# ---- include dirs: the ini adds them, and only the ini can reach this one ---
rm -f "$D/madc.ini"
( cd "$D" && env -u MADC_FOREST timeout 120 ./madc needsinc.c >/dev/null 2>&1 ) \
    && fail "[baseline-include] compiled without the ini include dir (fixture is not exclusive)"

cat > "$D/madc.ini" <<'EOF'
include = inc
EOF
out=$( cd "$D" && env -u MADC_FOREST timeout 120 ./madc needsinc.c 2>/dev/null ) \
    || fail "[ini-include] compile failed with the ini include dir"
[ "$out" = "inc 4242" ] || fail "[ini-include] wrong output: '$out'"

# ---- --config=<file>: the whole search, and paths resolve against the FILE --
# Run from the REPO ROOT with an absolute --config: `include = inc` must resolve
# against the config file's own directory, not this CWD.
out=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/madc.ini" "$D/needsinc.c" 2>/dev/null) \
    || fail "[explicit-config] compile failed"
[ "$out" = "inc 4242" ] \
    || fail "[explicit-config] relative include did not resolve against the config file dir: '$out'"

err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/absent.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[explicit-miss] a named config that does not exist was accepted"
grep -q <<<"$err" "cannot read config file" \
    || fail "[explicit-miss] missing the loud diagnostic: $err"

# ---- strictness: a bad config REFUSES, it never half-applies ---------------
# The comment + blank line are deliberate: the reported line number must be the
# real file line (3), not a count of the lines that carried settings.
cat > "$D/bad.ini" <<'EOF'
# a comment

std = c99
frost = /tmp/typo.msnap
EOF
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/bad.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[unknown-key] an unknown key was tolerated"
grep -q <<<"$err" "unknown key 'frost'" || fail "[unknown-key] did not name the key: $err"
grep -q <<<"$err" "bad.ini:4" || fail "[unknown-key] did not name file:line: $err"
grep -q <<<"$err" "accepted: std, stdlib, forest, include, cpu-limit, mem-limit" \
    || fail "[unknown-key] did not list the accepted keys: $err"

printf 'std c99\n' > "$D/malformed.ini"
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/malformed.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[malformed] a line with no '=' was tolerated"
grep -q <<<"$err" "expected 'key = value'" || fail "[malformed] wrong diagnostic: $err"

printf 'mem-limit = 8G\n' > "$D/badint.ini"
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/badint.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[bad-int] '8G' was accepted as a megabyte count"
grep -q <<<"$err" "mem-limit needs a whole number" || fail "[bad-int] wrong diagnostic: $err"

printf '[nope]\nstd = c99\n' > "$D/badsection.ini"
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/badsection.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[bad-section] a foreign section was tolerated"
grep -q <<<"$err" "unknown section \[nope\]" || fail "[bad-section] wrong diagnostic: $err"

# ---- forest key = discovery arm 5 ------------------------------------------
live=$(env -u MADC_FOREST timeout 120 "$MADC" --no-config --no-forest-bind "$D/consumer.c" 2>/dev/null) \
    || fail "[live-ref] live-parse reference run failed"
[ "$live" = "cfggate ok" ] || fail "[live-ref] wrong reference output: '$live'"

cat > "$D/forest.ini" <<EOF
forest = stdio.msnap
EOF
v=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/forest.ini" -v "$D/consumer.c" 2>&1 | tr -d '\0') \
    || fail "[ini-forest] compile failed"
grep -q <<<"$v" 'bound to grove unit' || fail "[ini-forest] arm 5 did not bind"
out=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/forest.ini" "$D/consumer.c" 2>/dev/null)
[ "$out" = "$live" ] || fail "[ini-forest] bind output != live parse output"

# Arm ORDER: a usable $MADC_FOREST wins BEFORE arm 5 is probed, so the junk
# path in the ini must never produce a not-a-container notice.
cat > "$D/junkforest.ini" <<EOF
forest = junk.txt
EOF
v=$(MADC_FOREST="$ROOT/$D/stdio.msnap" timeout 120 "$MADC" --config="$ROOT/$D/junkforest.ini" -v "$D/consumer.c" 2>&1 | tr -d '\0') \
    || fail "[arm-order] compile failed"
grep -q <<<"$v" 'bound to grove unit' || fail "[arm-order] the env container did not bind"
grep -q <<<"$v" 'no forest container found' \
    && fail "[arm-order] arm 5 was probed even though \$MADC_FOREST bound"

# And with nothing in the environment, that same junk ini path IS reached and
# is loud (a configured container that is not one must never be skipped
# silently) — the pair proves the arm exists AND sits last.
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/junkforest.ini" "$D/consumer.c" 2>&1 >/dev/null) \
    || fail "[ini-forest-junk] compile failed (fallback broken)"
grep -q <<<"$err" 'no forest container found' \
    || fail "[ini-forest-junk] junk at the configured path was skipped silently"

# ---- resource guards: ini value reaches the guard, environment beats it ----
# A tiny address-space ceiling makes the guard trip, and the message names the
# EFFECTIVE value — which is the assertion that the ini number got there.
printf 'mem-limit = 24\n' > "$D/mem.ini"
err=$(env -u MADC_FOREST timeout 120 "$MADC" --config="$ROOT/$D/mem.ini" "$D/consumer.c" 2>&1 >/dev/null)
[ $? -ne 0 ] || fail "[ini-memlimit] a 24 MB address-space ceiling did not trip the guard"
grep -q <<<"$err" 'MADC_MEM_LIMIT=24' \
    || fail "[ini-memlimit] the guard did not report the ini value: $err"
out=$(MADC_MEM_LIMIT=4096 timeout 120 "$MADC" --config="$ROOT/$D/mem.ini" "$D/consumer.c" 2>/dev/null) \
    || fail "[env-beats-ini] the environment did not override the ini mem-limit"
[ "$out" = "cfggate ok" ] || fail "[env-beats-ini] wrong output: '$out'"

echo "forest_config_gate: OK (madc.ini: keys apply, CLI/env precedence pinned, arm 5 last, bad configs refuse)"
