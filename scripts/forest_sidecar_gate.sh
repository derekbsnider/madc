#!/usr/bin/env bash
# Forest-carriers S3 gate: the carrier DISCOVERY CHAIN beyond the self-image —
# the <exe>.forest sidecar (arm 3) and the MADC_FOREST environment path
# (arm 4) — plus the arm ordering and the loud-not-silent failure surfaces.
# One format, one loader, N carriers: the container bytes are identical in
# every carrier, so each leg pins BIND ENGAGEMENT (a -v run shows "bound to
# grove unit" — a silent live fall-through cannot false-green it) and byte
# parity against a --no-forest-bind live parse.
#
# The dev binary is the probe vehicle: it is unpacked (self-image arm misses
# quietly), so the chain reaches the external arms. Runs from the repo root
# (fulltest does); fixtures generated into tmp/ (gitignored).
set -u
# pipefail so a madc failure inside the $(... | tr) captures trips the
# || fail guards. The greps below therefore read via here-strings, NOT
# pipes: `echo big | grep -q` dies of EPIPE when grep -q exits at the
# first match, and pipefail would turn that into a false failure.
set -o pipefail
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

BIN=bin/madc
if [ ! -x "$BIN" ]; then
    echo "forest_sidecar_gate: missing $BIN"
    exit 1
fi

D=tmp/sidegate
rm -rf "$D"
mkdir -p "$D"

fail() { echo "forest_sidecar_gate: $1"; exit 1; }

# A copy of the dev binary is the discovery subject: its self-exe path is
# $D/madc, so arm 3 probes $D/madc.forest — a sidecar placed next to the
# REAL bin/madc would leak into every other test run.
cp "$BIN" "$D/madc"

# thin-CLI subject loader (PK2): the copied subject's $ORIGIN/../lib
# rpath points at tmp/lib, which does not exist — the copy must LOAD
# before any discovery arm can be probed, so hand the LINKER the build
# tree's lib explicitly. This does not touch forest discovery: the dev
# libmadc.so is unpacked, so the library-image arm still misses quietly
# and the chain reaches the external arms this gate exists to probe.
export LD_LIBRARY_PATH="$(pwd)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cat > "$D/producer.cpp" <<'EOF'
#include <stdio.h>
int main() { return 0; }
EOF
cat > "$D/consumer.cpp" <<'EOF'
#include <stdio.h>
int main() { printf("sidegate ok\n"); return 0; }
EOF

# Freeze the producer into a standalone container (freeze modes live-parse;
# the container carries the <stdio.h> grove the consumer legs will bind).
timeout 120 "$BIN" --freeze="$D/stdio.msnap" "$D/producer.cpp" >/dev/null 2>&1 \
    || fail "--freeze failed"

# Live-parse reference output (the parity oracle for every bind leg).
live_out=$(env -u MADC_FOREST timeout 120 "$D/madc" --no-forest-bind "$D/consumer.cpp" 2>/dev/null) \
    || fail "live-parse reference run failed"
[ "$live_out" = "sidegate ok" ] || fail "live reference output wrong: '$live_out'"

# Leg 1 — sidecar arm: <exe>.forest beside the binary binds.
cp "$D/stdio.msnap" "$D/madc.forest"
v=$(env -u MADC_FOREST timeout 120 "$D/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[sidecar] compile failed"
grep -q <<<"$v" 'bound to grove unit' || fail "[sidecar] did not bind"
out=$(env -u MADC_FOREST timeout 120 "$D/madc" "$D/consumer.cpp" 2>/dev/null)
[ "$out" = "$live_out" ] || fail "[sidecar] bind output != live parse output"

# Leg 2 — --no-forest-bind beats the sidecar (the master live-parse lever).
v=$(env -u MADC_FOREST timeout 120 "$D/madc" --no-forest-bind -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[no-bind] compile failed"
grep -q <<<"$v" 'bound to grove unit' && fail "[no-bind] bound despite --no-forest-bind"

# Leg 3 — arm order: a present sidecar wins BEFORE the MADC_FOREST arm is
# probed (a junk env path must produce no not-a-container notice).
echo "not a container" > "$D/junk.txt"
v=$(MADC_FOREST="$D/junk.txt" timeout 120 "$D/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[order] compile failed"
grep -q <<<"$v" 'bound to grove unit' || fail "[order] sidecar did not bind"
grep -q <<<"$v" 'no forest container found' && fail "[order] env arm probed despite sidecar hit"

# Leg 4 — MADC_FOREST arm: with no sidecar, the env path binds.
rm -f "$D/madc.forest"
v=$(MADC_FOREST="$D/stdio.msnap" timeout 120 "$D/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[env] compile failed"
grep -q <<<"$v" 'bound to grove unit' || fail "[env] did not bind"
out=$(MADC_FOREST="$D/stdio.msnap" timeout 120 "$D/madc" "$D/consumer.cpp" 2>/dev/null)
[ "$out" = "$live_out" ] || fail "[env] bind output != live parse output"

# Leg 5 — junk sidecar is LOUD and falls back: a file that exists at the
# sidecar path but is not a container prints the notice and live-parses.
cp "$D/junk.txt" "$D/madc.forest"
err=$(env -u MADC_FOREST timeout 120 "$D/madc" "$D/consumer.cpp" 2>&1 >/dev/null) \
    || fail "[junk-sidecar] compile failed (fallback broken)"
grep -q <<<"$err" 'no forest container found' || fail "[junk-sidecar] junk was silently skipped"
out=$(env -u MADC_FOREST timeout 120 "$D/madc" "$D/consumer.cpp" 2>/dev/null)
[ "$out" = "$live_out" ] || fail "[junk-sidecar] fallback output wrong"
rm -f "$D/madc.forest"

# Leg 6 — explicit --forest-bind=<absent> is LOUD and falls back (an
# explicitly named container must never be ignored silently).
err=$(env -u MADC_FOREST timeout 120 "$D/madc" --forest-bind="$D/absent.msnap" "$D/consumer.cpp" 2>&1 >/dev/null) \
    || fail "[explicit-miss] compile failed (fallback broken)"
grep -q <<<"$err" 'did not provide a usable forest container' \
    || fail "[explicit-miss] missing the loud notice"

echo "forest_sidecar_gate: OK (sidecar + env arms bind, order pinned, failure surfaces loud)"
