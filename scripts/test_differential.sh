#!/usr/bin/env bash
# test_differential.sh OLD_BIN NEW_BIN [GLOB...] — the suite's programs, old
# binary against new: every tests/*.mad matching a GLOB (default: all) runs
# under OLD twice and NEW once, the way scripts/run_tests.sh runs its JIT
# pass (from the repo root, the hermetic flags, the test's fixtures through
# scripts/test_fixtures.sh's fixture_load), and the tests whose exit status,
# stdout or stderr differ are reported. A test whose two OLD runs already
# differ is listed apart as nondeterministic, never as a difference.
#
# A change meant to keep every program's behaviour (a restructured bind, a
# lookup moved behind a miss) is proven with this BEFORE it commits; every
# reported difference is classified as intended or fixed. Build both
# binaries with the same configure, on the build container.
#
#   OUT=dir (default tmp/test_differential): report.txt + per-test outputs.
set -u
[ $# -ge 2 ] || { echo "usage: $0 OLD_BIN NEW_BIN [GLOB...]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
absolute() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
for b in "$1" "$2"; do
	[ -f "$b" ] && [ -x "$b" ] || { echo "$0: not an executable: $b" >&2; exit 2; }
done
OLD="$(absolute "$1")" NEW="$(absolute "$2")"
shift 2
GLOBS=("$@")
[ ${#GLOBS[@]} -gt 0 ] || GLOBS=('*')
. "$ROOT/scripts/test_fixtures.sh"
cd "$ROOT" || exit 2
OUT="${OUT:-$ROOT/tmp/test_differential}"
rm -rf "$OUT"; mkdir -p "$OUT/run"
report="$OUT/report.txt"; : > "$report"
export MADC_MEM_LIMIT="${MADC_MEM_LIMIT:-auto}"
n=0 same=0 diff=0 flaky=0 flaky_list=""

# Binary paths and addresses vary between builds and runs; nothing else is
# normalized — a changed number in a program's output is a difference.
norm()
{
	sed -E -e "s|$OLD|madc|g" -e "s|$NEW|madc|g" -e 's/0x[0-9a-f]{6,}/0xADDR/g' "$1"
}

run_one()
{
	local bin="$1" t="$2" d="$3"
	if [ -n "$fx_input" ]; then
		( ulimit -t 120; env "${fx_envs[@]}" timeout "$fx_timeout" "$bin" $FIXTURE_HERMETIC_FLAGS \
			"${fx_flags[@]}" "$t" "${fx_args[@]}" < "$fx_input" > "$d.stdout" 2> "$d.stderr"; echo $? > "$d.rc" )
	else
		( ulimit -t 120; env "${fx_envs[@]}" timeout "$fx_timeout" "$bin" $FIXTURE_HERMETIC_FLAGS \
			"${fx_flags[@]}" "$t" "${fx_args[@]}" < /dev/null > "$d.stdout" 2> "$d.stderr"; echo $? > "$d.rc" )
	fi
}

differs()
{
	cmp -s "$1.rc" "$2.rc" && cmp -s <(norm "$1.stdout") <(norm "$2.stdout") \
		&& cmp -s <(norm "$1.stderr") <(norm "$2.stderr") && return 1
	return 0
}

for t in tests/*.mad; do
	[ -f "$t" ] || continue
	base=$(basename "$t" .mad)
	[ -f "tests/$base.helper" ] && continue
	[ -f "tests/$base.mir_skip" ] && continue
	keep=0
	for g in "${GLOBS[@]}"; do
		case "$base" in $g) keep=1; break ;; esac
	done
	[ $keep -eq 1 ] || continue
	fixture_load tests "$base"
	n=$((n + 1))
	run_one "$OLD" "$t" "$OUT/run/$base.old"
	run_one "$OLD" "$t" "$OUT/run/$base.old2"
	run_one "$NEW" "$t" "$OUT/run/$base.new"
	if differs "$OUT/run/$base.old" "$OUT/run/$base.old2"; then
		flaky=$((flaky + 1)); flaky_list="$flaky_list $base"
		continue
	fi
	if ! differs "$OUT/run/$base.old" "$OUT/run/$base.new"; then
		same=$((same + 1))
		rm -f "$OUT/run/$base".*
		continue
	fi
	diff=$((diff + 1))
	o="$OUT/run/$base.old" w="$OUT/run/$base.new"
	{
		echo "=== $base — rc $(cat "$o.rc") -> $(cat "$w.rc")"
		diff <(norm "$o.stdout") <(norm "$w.stdout") | head -8 | sed 's/^/  out| /'
		diff <(norm "$o.stderr") <(norm "$w.stderr") | head -8 | sed 's/^/  err| /'
	} >> "$report"
done
[ -n "$flaky_list" ] && echo "nondeterministic (OLD differs from itself):$flaky_list" >> "$report"
echo "test_diff: $n tests, $same identical, $diff differ, $flaky nondeterministic — $report"
[ $diff -eq 0 ]
