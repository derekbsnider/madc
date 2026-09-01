#!/bin/bash
# perf_pack_shapes.sh — PK0 (packaging arc, docs/plans/2026-09-01-packaging-arc.md):
# cold-start A/B, packed monolithic CLI vs thin CLI + shared libmadc carrying
# the forest. Runs ON THE CONTAINER from the repo root (QNAP never builds).
# Interleaved A/B (alternate shapes inside one loop); wall time paired with
# callgrind Ir when valgrind is present. Restores the monolithic tree shape
# on exit, whatever happens.
set -u
cd "$(dirname "$0")/.."
OUT=tmp/pk0
mkdir -p "$OUT"
ITER=${PK0_ITER:-21}
log() { echo "pk0: $*"; }

[ -x bin/madc-release ] || { log "no bin/madc-release monolithic baseline — make -C src release first"; exit 1; }
if ldd bin/madc-release | grep -q libmadc; then
	log "bin/madc-release is ALREADY the shared shape — need the monolithic baseline first"
	exit 1
fi
cp bin/madc-release "$OUT/madc-mono"

SAVED="$OUT/config.mk.pk0saved"
cp config.mk "$SAVED"
restore() {
	log "restoring tree shape (monolithic)"
	cp "$SAVED" config.mk
	make -C src -j20 release > /dev/null 2>&1
	make -C src -j20 > /dev/null 2>&1
}
trap restore EXIT

log "== building the shared shape (--enable-shared) =="
./configure --enable-shared > /dev/null || exit 1
make -C src -j20 release || exit 1

ldd bin/madc-release | grep libmadc || { log "FAIL: thin CLI not linked against libmadc"; exit 1; }
if ldd bin/madc-release | grep -q "not found"; then
	log "FAIL: unresolved shared library on the thin CLI"
	exit 1
fi

log "== artifact sizes (bytes) =="
stat -c '%s  %n' "$OUT/madc-mono" bin/madc-release lib/release/libmadc.so* 2>/dev/null

printf 'println("pk0");\n' > "$OUT/hello.mad"

"$OUT/madc-mono" "$OUT/hello.mad" > /dev/null || { log "FAIL: mono hello run"; exit 1; }
bin/madc-release "$OUT/hello.mad" > /dev/null || { log "FAIL: shared hello run"; exit 1; }

# Forest evidence, best effort: -v lines naming the forest/pack source. If the
# shared shape silently fell back to live parse, the timings below are NOT the
# packaged product — the evidence files say which arm actually served.
bin/madc-release -v "$OUT/hello.mad" 2>&1 | grep -i -m5 -e forest -e pack > "$OUT/shared_forest_evidence.txt" || true
"$OUT/madc-mono" -v "$OUT/hello.mad" 2>&1 | grep -i -m5 -e forest -e pack > "$OUT/mono_forest_evidence.txt" || true
log "-- shared-shape forest evidence --"
cat "$OUT/shared_forest_evidence.txt"

run_ns() {
	local t0 t1
	t0=$(date +%s%N)
	( ulimit -t 10; timeout 10 "$@" > /dev/null 2>&1 )
	t1=$(date +%s%N)
	echo $((t1 - t0))
}

TSV="$OUT/pk0_results.tsv"
: > "$TSV"
log "== interleaved timing (n=$ITER per shape per case) =="
for c in hello testint; do
	case $c in
		hello)   set -- "$OUT/hello.mad";;
		testint) set -- tests/testint.mad;;
	esac
	for i in $(seq "$ITER"); do
		a=$(run_ns "$OUT/madc-mono" "$@")
		b=$(run_ns bin/madc-release "$@")
		printf '%s\t%s\tmono\t%s\n'   "$c" "$i" "$a" >> "$TSV"
		printf '%s\t%s\tshared\t%s\n' "$c" "$i" "$b" >> "$TSV"
	done
done

python3 - "$TSV" << 'PY'
import sys, statistics as st, collections
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    c, i, s, ns = line.split()
    rows[(c, s)].append(int(ns))
print(f"{'case':10s} {'shape':7s} {'n':>3s} {'min ms':>9s} {'median ms':>10s} {'p90 ms':>9s}")
for (c, s), v in sorted(rows.items()):
    v.sort()
    p90 = v[min(len(v) - 1, int(len(v) * 0.9))]
    print(f"{c:10s} {s:7s} {len(v):3d} {v[0]/1e6:9.2f} {st.median(v)/1e6:10.2f} {p90/1e6:9.2f}")
PY

if command -v valgrind > /dev/null 2>&1; then
	log "== callgrind Ir (hello, one shot per shape — the wall-time cross-check) =="
	valgrind --tool=callgrind --callgrind-out-file="$OUT/cg.mono" "$OUT/madc-mono" "$OUT/hello.mad" > /dev/null 2>&1
	valgrind --tool=callgrind --callgrind-out-file="$OUT/cg.shared" bin/madc-release "$OUT/hello.mad" > /dev/null 2>&1
	for s in mono shared; do
		ir=$(grep -m1 '^summary:' "$OUT/cg.$s" | awk '{print $2}')
		echo "callgrind Ir $s: $ir"
	done
else
	log "valgrind not present — Ir cross-check skipped"
fi

log "done — raw TSV at $OUT/pk0_results.tsv (tree restored to monolithic on exit)"
