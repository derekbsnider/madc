#!/bin/bash
# run_tests.sh — run Chthonia's tests with a madc.
#
#   scripts/run_tests.sh [--gui] [NAME...]
#
#   MADC             the madc to run them with, as a command (default: madc
#                    on PATH)
#   MADCIDE_INCLUDE  madcide's public headers (default: the installed ones,
#                    <prefix>/share/madcide/include, beside MADC)
#   --gui            the window tests (tests/gui/), for a display: run this
#                    under xvfb-run where there is none
#   NAME...          only these tests (base names, no .mad)
#
# Each test is a program that includes <madcide/harness> and links
# libmadcide. It passes when it exits 0 and every non-empty line of its
# NAME.expect appears in its standard output. Beside it, NAME.env holds NAME=value
# words for its environment and NAME.timeout its limit in seconds (default
# 120). The tests run from Chthonia's top directory, and what they create
# goes under tmp/.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
madc=${MADC:-madc}
dir=tests
if [ "${1:-}" = "--gui" ]; then
	dir=tests/gui
	shift
fi
inc=${MADCIDE_INCLUDE:-}
if [ -z "$inc" ]; then
	bin=$(command -v "${madc%% *}" || true)
	if [ -z "$bin" ]; then
		echo "run_tests.sh: no madc ($madc) — set MADC" >&2
		exit 1
	fi
	inc=$(cd "$(dirname "$bin")/.." && pwd)/share/madcide/include
fi
inc=$(cd "$inc" 2>/dev/null && pwd) || { echo "run_tests.sh: no directory $inc" >&2; exit 1; }
if [ ! -f "$inc/madcide/harness" ]; then
	echo "run_tests.sh: no <madcide/harness> in $inc — set MADCIDE_INCLUDE" >&2
	exit 1
fi
case "$madc" in
/*|*" "*) ;;
*/*) madc=$(cd "$(dirname "$madc")" && pwd)/$(basename "$madc") ;;
esac
cd "$here" || exit 1
mkdir -p tmp

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
	for t in "$dir"/*.mad; do
		[ -e "$t" ] && names+=("$(basename "$t" .mad)")
	done
fi
pass=0
fail=0
for name in ${names[@]+"${names[@]}"}; do
	t=$dir/$name.mad
	envs=()
	if [ -f "$dir/$name.env" ]; then
		read -r -a envs < "$dir/$name.env"
	fi
	limit=120
	[ -f "$dir/$name.timeout" ] && limit=$(cat "$dir/$name.timeout")
	# shellcheck disable=SC2086
	out=$(env ${envs[@]+"${envs[@]}"} timeout "$limit" $madc -I "$inc" -lmadcide "$t" 2> tmp/run_tests.err)
	rc=$?
	why=
	if [ $rc -ne 0 ]; then
		why="exit status $rc"
	elif [ -f "$dir/$name.expect" ]; then
		while IFS= read -r line; do
			[ -z "$line" ] && continue
			if ! grep -qF -- "$line" <<< "$out"; then
				why="missing: $line"
				break
			fi
		done < "$dir/$name.expect"
	fi
	if [ -z "$why" ]; then
		pass=$((pass + 1))
		echo "PASS $name"
	else
		fail=$((fail + 1))
		echo "FAIL $name ($why)"
		{ printf '%s\n' "$out"; cat tmp/run_tests.err; } | tail -20 | sed 's/^/    /'
	fi
done
rm -f tmp/run_tests.err
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
