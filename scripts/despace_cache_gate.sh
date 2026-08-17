#!/bin/bash
# despace_cache_gate.sh — prove the despaced-canonical cache cannot go stale.
#
# StructRegistry files each struct_map entry under despace(strip_ns(canonical
# spelling)). That key is a pure function of the dd's own spelling, so it is
# cached on the DataDef and reused across index rebuilds (a rebuild used to
# re-derive ~1,600 keys because ONE dd's rewrite invalidates the whole index).
#
# The failure mode is SILENT: a spelling rewritten outside
# DataDef::set_canonical_spelling leaves a cached key that still looks
# plausible, and find_despaced then answers with the WRONG DataDef rather than
# failing. MADC_DESPACE_VERIFY re-derives every cached key during the sweep and
# aborts on mismatch; this gate runs the template-heavy tests under it.
#
# NON-VACUITY: a verification that silently does not run is worse than none, so
# the gate also asserts the probe reports verify=1 AND swept>0. If the env var
# is ever renamed or the sweep stops running, this fails instead of passing
# quietly.
#
# Usage: bash scripts/despace_cache_gate.sh
# Exit:  0 = every workload verified clean, with the check provably engaged.

set -u
cd "$(dirname "$0")/.." || exit 1

MADC="${MADC:-bin/madc}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-60}"
MADC_CPU_LIMIT="${MADC_CPU_LIMIT:-60}"

# Workloads that actually populate struct_map with templated spellings, each
# VERIFIED to sweep a non-trivial number of entries (testsubscript 29k,
# testvector 14k, testmap 13k, testset 12k, testtemplate 8k). An unrelated
# test would pass this gate while checking nothing, which is why swept>0 is
# asserted per workload rather than only in aggregate — several plausible
# candidates (testclasspatternbasic, testclasspatternvector) sweep ZERO and
# would have made the gate quietly hollow.
WORKLOADS="
tests/testsubscript.mad
tests/testtemplate.mad
tests/testvector.mad
tests/testset.mad
tests/testmap.mad
"

[ -x "$MADC" ] || { echo "despace_cache_gate: missing $MADC — build with: make -C src" >&2; exit 1; }

rc=0
ran=0
for w in $WORKLOADS; do
	[ -f "$w" ] || continue		# suite layout differs by branch; skip absent
	ran=$((ran + 1))
	err=$(env MADC_DESPACE_PROBE=1 MADC_DESPACE_VERIFY=1 MADC_CPU_LIMIT="$MADC_CPU_LIMIT" \
		timeout "$PER_TEST_TIMEOUT" "$MADC" "$w" 2>&1 >/dev/null)
	wrc=$?

	if [ "$wrc" -eq 134 ] || echo "$err" | grep -q "DESPACECACHE STALE"; then
		echo "despace_cache_gate: STALE CACHE on $w" >&2
		echo "$err" | grep -m3 "DESPACECACHE STALE" >&2
		rc=1
		continue
	fi
	if [ "$wrc" -ne 0 ]; then
		echo "despace_cache_gate: $w exited $wrc under verify (not a stale-cache abort)" >&2
		rc=1
		continue
	fi

	# A run dumps a probe line PER StructRegistry, and the first one is an
	# empty registry that never swept anything — reading only that line
	# reports "vacuous" for a run that verified thousands of entries. Take
	# the busiest registry.
	probe=$(echo "$err" | grep "DESPACEPROBE")
	if [ -z "$probe" ] || ! echo "$probe" | grep -q "verify=1"; then
		echo "despace_cache_gate: $w did not report verify=1 — the check did NOT run" >&2
		rc=1
		continue
	fi
	swept=$(echo "$probe" | sed -n 's/.*swept=\([0-9]*\).*/\1/p' | sort -n | tail -1)
	if [ -z "$swept" ] || [ "$swept" -eq 0 ]; then
		echo "despace_cache_gate: $w swept 0 entries — gate is vacuous here" >&2
		rc=1
		continue
	fi
	echo "despace_cache_gate: $w OK (swept=$swept verified)"
done

if [ "$ran" -eq 0 ]; then
	echo "despace_cache_gate: no workloads found — refusing to pass vacuously" >&2
	exit 1
fi
if [ "$rc" -eq 0 ]; then
	echo "despace_cache_gate: OK — $ran workloads, every cached key re-derived and matched"
else
	echo "despace_cache_gate: FAILED" >&2
fi
exit "$rc"
