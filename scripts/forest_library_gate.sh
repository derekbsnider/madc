#!/usr/bin/env bash
# Forest-carriers S4 gate: the LIBRARY-image carrier (discovery arm 2) and the
# embedding-host policy contract.
#
# The shared shape puts the container in libmadc's OWN image, so one carrier
# serves the thin CLI and every embedding host. Two vehicles exercise it:
#
#   bin/madc-thin  — the same madc.o linked against the shared libmadc (built
#                    by the Makefile; the gate copies it into a private
#                    bin/ + lib/ layout so $ORIGIN/../lib resolves to a
#                    library image the gate owns and may pack)
#   the host smoke — tests/libmadc_forest_smoke.cpp against the public API,
#                    linked to the same staged library. It carries the legs
#                    that have no CLI knob: strict_require end-to-end from a
#                    host, and enable_external_forest=false (the S3 boundary:
#                    a sandboxed host must still bind the IMAGE arms while the
#                    sidecar / MADC_FOREST arms are refused).
#
# One format, one loader, N carriers: every leg pins BIND ENGAGEMENT (a -v run
# names the arm that opened the container) and byte parity against a
# --no-forest-bind live parse — a silent live fall-through cannot false-green.
#
# Runs from the repo root (fulltest does); fixtures in tmp/ (gitignored).
set -u
# pipefail so a madc failure inside a $(... | tr) capture trips the guards.
# The greps read via here-strings, NOT pipes: `echo big | grep -q` dies of
# EPIPE when grep -q exits at the first match, and pipefail would turn that
# into a false failure.
set -o pipefail
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

BIN=bin/madc
THIN=bin/madc-thin
LIB=lib/libmadc.so
SOVER=$(cut -d. -f1 VERSION)
SONAME=libmadc.so.$SOVER
CXX=${CXX:-clang++}

fail() { echo "forest_library_gate: $1"; exit 1; }

[ -x "$BIN" ] || fail "missing $BIN"
[ -x "$THIN" ] || fail "missing $THIN (make -C src $THIN)"
[ -f "$LIB" ] || fail "missing $LIB"

D=tmp/libgate
rm -rf "$D"
mkdir -p "$D/packed/bin" "$D/packed/lib" "$D/plain/bin" "$D/plain/lib"

cat > "$D/producer.cpp" <<'EOF'
#include <stdio.h>
int main() { return 0; }
EOF
cat > "$D/consumer.cpp" <<'EOF'
#include <stdio.h>
int main() { printf("libgate ok\n"); return 0; }
EOF

# Standalone container (the sidecar/env arms bind this file directly).
timeout 180 "$BIN" --freeze="$D/stdio.msnap" "$D/producer.cpp" >/dev/null 2>&1 \
    || fail "--freeze failed"

# Two staged installations, identical except for the carrier: `packed` has the
# container inside the library image, `plain` has a bare library. Each keeps
# the real install layout (bin/ + lib/ with the SONAME) so the thin CLI's
# $ORIGIN/../lib rpath resolves to that tree's own library.
cp "$THIN" "$D/packed/bin/madc"
cp "$THIN" "$D/plain/bin/madc"
cp "$LIB" "$D/packed/lib/$SONAME"
cp "$LIB" "$D/plain/lib/$SONAME"
# Pack via the production path (--freeze-append), never a hand-rolled append.
timeout 180 "$BIN" --freeze-append="$D/packed/lib/$SONAME" "$D/producer.cpp" >/dev/null 2>&1 \
    || fail "--freeze-append into the library image failed"

# Live-parse reference output (the parity oracle for every bind leg) — also
# the thin CLI's own smoke: a shared-linked madc must behave exactly like the
# monolithic one.
live_out=$(env -u MADC_FOREST timeout 120 "$D/plain/bin/madc" --no-forest-bind "$D/consumer.cpp" 2>/dev/null) \
    || fail "thin-CLI live-parse reference run failed"
[ "$live_out" = "libgate ok" ] || fail "thin-CLI live reference output wrong: '$live_out'"

# Leg 1 — thin CLI, no container anywhere: the shared shape compiles and runs
# exactly like the monolithic binary (CLI parity, live lane).
out=$(env -u MADC_FOREST timeout 120 "$D/plain/bin/madc" "$D/consumer.cpp" 2>/dev/null) \
    || fail "[thin-live] compile failed"
[ "$out" = "$live_out" ] || fail "[thin-live] output != live parse output"

# Leg 2 — library-image arm: the container inside libmadc's image binds.
v=$(env -u MADC_FOREST timeout 120 "$D/packed/bin/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[library-image] compile failed"
grep -q <<<"$v" '\[library-image\] opened container' || fail "[library-image] arm did not open the container"
grep -q <<<"$v" 'bound to grove unit' || fail "[library-image] did not bind"
out=$(env -u MADC_FOREST timeout 120 "$D/packed/bin/madc" "$D/consumer.cpp" 2>/dev/null)
[ "$out" = "$live_out" ] || fail "[library-image] bind output != live parse output"

# Leg 3 — arm order: the library image wins BEFORE any external arm is probed
# (an <exe>.forest sidecar and a junk MADC_FOREST must go untouched).
cp "$D/stdio.msnap" "$D/packed/bin/madc.forest"
echo "not a container" > "$D/junk.txt"
v=$(MADC_FOREST="$D/junk.txt" timeout 120 "$D/packed/bin/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[order] compile failed"
grep -q <<<"$v" '\[library-image\] opened container' || fail "[order] library image did not win"
grep -q <<<"$v" '\[sidecar\]' && fail "[order] sidecar arm probed despite the library-image hit"
grep -q <<<"$v" 'no forest container found' && fail "[order] env arm probed despite the library-image hit"
rm -f "$D/packed/bin/madc.forest"

# Leg 4 — <lib>.forest sidecar arm: with a bare library image, the container
# beside the LIBRARY binds (the distro-packaging shape: one forest file for
# the CLI and every host).
cp "$D/stdio.msnap" "$D/plain/lib/$SONAME.forest"
v=$(env -u MADC_FOREST timeout 120 "$D/plain/bin/madc" -v "$D/consumer.cpp" 2>&1 | tr -d '\0') \
    || fail "[lib-sidecar] compile failed"
grep -q <<<"$v" '\[lib-sidecar\] opened container' || fail "[lib-sidecar] arm did not open the container"
grep -q <<<"$v" 'bound to grove unit' || fail "[lib-sidecar] did not bind"
out=$(env -u MADC_FOREST timeout 120 "$D/plain/bin/madc" "$D/consumer.cpp" 2>/dev/null)
[ "$out" = "$live_out" ] || fail "[lib-sidecar] bind output != live parse output"
rm -f "$D/plain/lib/$SONAME.forest"

# --- embedding host: the policy legs with no CLI knob ---------------------
# The host links the staged library directly, so dladdr resolves libmadc to
# THAT tree's image and the arms below are the staged carrier's.
build_host() {	# $1 = tree
    $CXX -std=c++11 -Wall -I include -o "$D/host-$1" tests/libmadc_forest_smoke.cpp \
	"$D/$1/lib/$SONAME" -Wl,-rpath,"$PWD/$D/$1/lib" 2>"$D/host-$1.build.log" \
	|| { cat "$D/host-$1.build.log"; fail "host smoke build failed ($1)"; }
}
build_host packed
build_host plain

# Leg 5 — THE shared-shape headline: a strict, sandboxed host (external arms
# OFF, no env, no sidecar) still binds through the LIBRARY image. Image
# carriers are the installation itself, so they are never gated.
out=$(env -u MADC_FOREST timeout 120 "$D/host-packed" strict-noext 2>"$D/h5.err"); rc=$?
[ $rc -eq 0 ] || { cat "$D/h5.err"; fail "[host/lib-image] strict+noext refused (rc=$rc): $out"; }
[ "$out" = "forest-bound" ] || fail "[host/lib-image] unexpected output '$out'"

# Leg 6 — the S3 boundary, now covered: with external arms OFF, a MADC_FOREST
# pointing at a perfectly good container is REFUSED, and strict says the chain
# ended empty (never a config-mismatch message — nothing was opened at all).
out=$(MADC_FOREST="$D/stdio.msnap" timeout 120 "$D/host-plain" strict-noext 2>"$D/h6.err"); rc=$?
[ $rc -eq 3 ] || fail "[host/noext] env container was NOT refused (rc=$rc, out='$out')"
# The host reports the diagnostic madc handed it (the engine captures madc's
# own stderr into the host's buffer, so the message arrives through the API,
# not the terminal). It must be the CHAIN-EMPTY error, never a config
# mismatch: with the external arms off, nothing was opened at all.
grep -q 'no usable container was found' <<<"$out" \
    || fail "[host/noext] wrong refusal: '$out'"
grep -q 'does not match' <<<"$out" && fail "[host/noext] refusal claims a config mismatch: '$out'"


# Leg 7 — the same host, same env, knob flipped: the external arm binds and
# the strict compile succeeds. (Leg 6 vs 7 is the whole knob, isolated.)
out=$(MADC_FOREST="$D/stdio.msnap" timeout 120 "$D/host-plain" strict 2>"$D/h7.err"); rc=$?
[ $rc -eq 0 ] || { cat "$D/h7.err"; fail "[host/ext] strict+env refused (rc=$rc): $out"; }
[ "$out" = "forest-bound" ] || fail "[host/ext] unexpected output '$out'"

# Leg 8 — strict means strict: no container anywhere is a hard error, not a
# silent slow path.
out=$(env -u MADC_FOREST timeout 120 "$D/host-plain" strict 2>"$D/h8.err"); rc=$?
[ $rc -eq 3 ] || fail "[host/strict-empty] missing container was not an error (rc=$rc, out='$out')"
grep -q 'frozen forest required' <<<"$out" \
    || fail "[host/strict-empty] wrong refusal: '$out'"


# Leg 9 — the LIBRARY default stays liberal and quiet: a host that shipped no
# container compiles, with nothing on stderr (the packaged-CLI loud notice is
# baked per product build; a library must never nag its host).
out=$(env -u MADC_FOREST timeout 120 "$D/host-plain" default 2>"$D/h9.err"); rc=$?
[ $rc -eq 0 ] || { cat "$D/h9.err"; fail "[host/default] compile failed (rc=$rc): $out"; }
[ "$out" = "forest-bound" ] || fail "[host/default] unexpected output '$out'"
# The host's OWN stderr must be clean (madc's stream is engine-captured, so
# this catches anything that escapes the library's capture).
[ -s "$D/h9.err" ] && { cat "$D/h9.err"; fail "[host/default] library default was not silent"; }

echo "forest_library_gate: OK (library-image + lib-sidecar arms bind, order pinned, host strict/sandbox contract holds)"
