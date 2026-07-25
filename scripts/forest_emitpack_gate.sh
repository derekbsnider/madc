#!/usr/bin/env bash
# S2 gate: the emitted-pack carrier (--pack-forest). A native executable
# emitted with --pack-forest=<container> must carry the container in its
# self-image carrier — ELF: appended trailer (footer at EOF, the
# --freeze-append placement); Mach-O: a __MADC,__forest section laid out by
# the fork writer INSIDE the emit-time code signature — and --dump-forest
# over the emitted image must byte-equal --dump-forest over the container
# (carrier transport is content-transparent). The ELF leg also RUNS the
# packed binary (the trailer must not disturb execution) and pins the two
# refusal arms (-c refused at the CLI; a non-container refused at emit).
# Mach-O legs run when the cross madcs exist (plain dev trees: SKIP loudly).
# Each Mach-O leg freezes its OWN container with the same cross madc that
# emits and dumps it: the context-hash pin makes cross-BINARY dump equality
# a rev-skew assertion (a dev bin/madc rebuilt after the cross madcs were
# last built froze containers they rightly reject), and the gate's claim is
# carrier transparency per binary, not build synchrony across binaries.
# AMFI acceptance of the packed Mach-O images is hardware evidence, not
# this gate.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null
mkdir -p tmp

TU=tmp/emitpack_tu.mad
CONT=tmp/emitpack.container

fail() { echo "forest_emitpack_gate: FAIL — $1"; exit 1; }

[ -x bin/madc ] || fail "missing bin/madc"

cat > "$TU" <<'EOF'
#include <stdio.h>
int main() { printf("emitpack ok\n"); return 42; }
EOF

rm -f "$CONT"
timeout 120 bin/madc --freeze="$CONT" "$TU" >/dev/null 2>&1 \
    || fail "--freeze of the payload TU failed"
d_cont=$(timeout 60 bin/madc --dump-forest="$CONT" 2>/dev/null) \
    || fail "--dump-forest over the container failed"

# ---- ELF leg: emit packed, run it, dump-forest parity ----
PROG=tmp/emitpack_prog
rm -f "$PROG"
timeout 120 bin/madc --pack-forest="$CONT" -o "$PROG" "$TU" >/dev/null 2>&1 \
    || fail "ELF --pack-forest -o emission failed"
out=$(timeout 30 "$PROG" 2>/dev/null)
rc=$?
[ "$rc" = 42 ] || fail "packed ELF executable rc=$rc (want 42)"
[ "$out" = "emitpack ok" ] || fail "packed ELF executable output '$out'"
d_elf=$(timeout 60 bin/madc --dump-forest="$PROG" 2>/dev/null) \
    || fail "--dump-forest over the packed ELF executable failed"
[ "$d_cont" = "$d_elf" ] \
    || fail "ELF carrier not content-transparent (dump mismatch)"

# ---- refusal arms (CLI chokepoint + emit-time container validation) ----
if timeout 60 bin/madc --pack-forest="$CONT" -c "$TU" >/dev/null 2>&1; then
    fail "--pack-forest -c was not refused"
fi
if timeout 60 bin/madc --pack-forest="$TU" -o tmp/emitpack_bad "$TU" \
       >/dev/null 2>&1; then
    fail "--pack-forest with a non-container payload was not refused"
fi
rm -f tmp/emitpack_bad

# ---- Mach-O legs: cross emit; dump-forest reads the __MADC,__forest
# section via the file probe's Mach-O arm (host-neutral byte parse) ----
for arch in arm64 x86-64; do
    X=bin/madc-$arch-macos
    if [ ! -x "$X" ]; then
        echo "forest_emitpack_gate: $X absent — Mach-O $arch leg SKIPPED"
        continue
    fi
    CONT_A=tmp/emitpack_$arch.container
    rm -f "$CONT_A"
    timeout 120 "$X" --freeze="$CONT_A" "$TU" >/dev/null 2>&1 \
        || fail "Mach-O $arch --freeze of the payload TU failed"
    d_cont_a=$(timeout 60 "$X" --dump-forest="$CONT_A" 2>/dev/null) \
        || fail "Mach-O $arch --dump-forest over the container failed"
    MP=tmp/emitpack_prog_$arch
    rm -f "$MP"
    timeout 120 "$X" --pack-forest="$CONT_A" -o "$MP" "$TU" >/dev/null 2>&1 \
        || fail "Mach-O $arch --pack-forest -o emission failed"
    d_macho=$(timeout 60 "$X" --dump-forest="$MP" 2>/dev/null) \
        || fail "Mach-O $arch --dump-forest over the packed image failed"
    [ "$d_cont_a" = "$d_macho" ] \
        || fail "Mach-O $arch carrier not content-transparent (dump mismatch)"
    echo "forest_emitpack_gate: Mach-O $arch leg OK"
done

echo "forest_emitpack_gate: OK"
exit 0
