#!/usr/bin/env bash
# conformance_measure.sh — measure madc's C23 and C++20 conformance and record
# it in the release ledger, docs/parity/conformance-ledger.tsv.
#
# Rule (.claude/rules/branching.md, owner 2026-10-16): every release raises at
# least one C23 / C++20 passing count and lowers none, against the previous
# release. This script is the ONE writer of those counts; the release gate
# scripts/check-release-conformance.sh reads them. A count is never typed by
# hand.
#
# The corpora are gcc's own tests (docs/conformance-coverage.md), driven by
# scripts/run_gcc_testsuite.py:
#   c23-compile  gcc.dg/c23-*, c2x-*: dg-do compile, clean  --std=c23    (--suite gxx:
#                                                                          parse, sema, lower)
#   c23-run      gcc.dg/c23-*, c2x-*: dg-do run             --std=c23    (compile and run)
#   torture-c23  gcc.c-torture/execute                       --std=c23    (compile and run)
#   cpp2a        g++.dg/cpp2a: dg-do compile, clean          --std=c++20  (--suite gxx)
# A test is in scope when the runner does not skip it; passing is the runner's
# passed count, and every FAIL / TIMEOUT line (FAIL(harness) included) is in
# scope and not passing.
#
# Usage (the build container, where gcc's testsuite and the archive live):
#   conformance_measure.sh --release vX.Y.Z [--madc BIN] [--root DIR] [--record]
# Without --madc it measures the release's archived binary
# (tmp/release-bins/vX.Y.Z/bin/madc-release) and its PROVENANCE names the
# commit. With --madc (the release candidate, before it is archived) the commit
# is this tree's HEAD, which must hold src/ include/ third_party/ unmodified.
# --record replaces the release's rows in the ledger; without it the rows are
# only printed.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LEDGER="$ROOT/docs/parity/conformance-ledger.tsv"
RUNNER="$ROOT/scripts/run_gcc_testsuite.py"

release="" bin="" tsroot="" record=0
while [ $# -gt 0 ]; do
	case "$1" in
		--release) release="${2:-}"; shift 2;;
		--madc) bin="${2:-}"; shift 2;;
		--root) tsroot="${2:-}"; shift 2;;
		--record) record=1; shift;;
		*) echo "conformance_measure: unknown argument '$1'" >&2; exit 2;;
	esac
done

case "$release" in
	v[0-9]*.[0-9]*.[0-9]*) ;;
	*) echo "conformance_measure: --release vX.Y.Z is required" >&2; exit 2;;
esac

if [ -z "$tsroot" ]; then
	if [ -d /workspace/gcc_testsuite ]; then
		tsroot=/workspace/gcc_testsuite
	else
		tsroot="$ROOT/gcc_testsuite"
	fi
fi
if [ ! -d "$tsroot/gcc.dg" ] || [ ! -d "$tsroot/g++.dg/cpp2a" ] \
   || [ ! -d "$tsroot/gcc.c-torture/execute" ]; then
	echo "conformance_measure: gcc's testsuite is not under $tsroot" >&2
	exit 1
fi

if [ -z "$bin" ]; then
	entry="$ROOT/tmp/release-bins/$release"
	bin="$entry/bin/madc-release"
	commit=$(sed -n 's/^commit: *//p' "$entry/PROVENANCE" 2>/dev/null | cut -c1-9)
	if [ -z "$commit" ]; then
		echo "conformance_measure: $entry/PROVENANCE names no commit" >&2
		exit 1
	fi
else
	if [ -n "$(git -C "$ROOT" status --porcelain -- src include third_party)" ]; then
		echo "conformance_measure: src/ include/ third_party/ are modified;" \
		     "a measurement names a commit, so commit first" >&2
		exit 1
	fi
	commit=$(git -C "$ROOT" rev-parse --short=9 HEAD)
fi
if [ ! -x "$bin" ]; then
	echo "conformance_measure: no madc binary at $bin" >&2
	exit 1
fi

work="$ROOT/tmp/conformance/$release"
mkdir -p "$work"

shopt -s nullglob
c23_files=("$tsroot"/gcc.dg/c23-*.c "$tsroot"/gcc.dg/c2x-*.c)
shopt -u nullglob
if [ "${#c23_files[@]}" -eq 0 ]; then
	echo "conformance_measure: no gcc.dg/c23-* tests under $tsroot" >&2
	exit 1
fi
mapfile -t c23_run < <(grep -lE 'dg-do[[:space:]]+run' "${c23_files[@]}")

# measure CORPUS STD RUNNER-ARGS... — one ledger row on stdout.
measure()
{
	local corpus="$1" std="$2" out passing failing
	shift 2
	out="$work/$corpus.log"
	python3 "$RUNNER" --root "$tsroot" --madc "$bin" --std "$std" "$@" \
		> "$out" 2>&1
	# The runner exits nonzero whenever a test fails; a run that died
	# mid-way prints no summary, and its counts are not a measurement.
	passing=$(sed -nE 's/^([0-9]+) passed, .*/\1/p' "$out" | tail -1)
	if [ -z "$passing" ]; then
		echo "conformance_measure: $corpus: the runner printed no summary;" \
		     "see $out" >&2
		return 1
	fi
	failing=$(grep -cE '^(FAIL|TIMEOUT)' "$out")
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$release" "$corpus" "$std" \
		"$passing" "$((passing + failing))" "$commit" "$(date -u +%Y-%m-%d)"
}

rows="$work/rows.tsv"
: > "$rows"
measure c23-compile c23 --suite gxx "${c23_files[@]}" >> "$rows" || exit 1
measure c23-run c23 --suite c-torture "${c23_run[@]}" >> "$rows" || exit 1
measure torture-c23 c23 --suite c-torture >> "$rows" || exit 1
measure cpp2a c++20 --suite gxx --gxx-dirs g++.dg/cpp2a >> "$rows" || exit 1

cat "$rows"

if [ "$record" -eq 1 ]; then
	tmp="$LEDGER.tmp.$$"
	awk -F'\t' -v r="$release" '$1 != r' "$LEDGER" > "$tmp"
	cat "$rows" >> "$tmp"
	mv "$tmp" "$LEDGER"
	echo "conformance_measure: recorded $release in ${LEDGER#"$ROOT"/}"
fi
exit 0
