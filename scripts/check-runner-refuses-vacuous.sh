#!/bin/bash
# check-runner-refuses-vacuous.sh — scripts/run_tests.sh must exit nonzero
# when it executed no test. "0 passed, 0 failed" with rc 0 reads as green to
# every caller: win_suite.sh once reported "native Windows suite OK" for a
# filter given as ONE argument ('testui_* testrepl_*') that matched nothing.
# The refusal lives in the runner, once, for every lane.
#
# Cases (a stub madc that exits 0 stands in for the binary):
#   a filter matching no test          -> must FAIL with "VACUOUS RUN"
#   the selection all skipped          -> must FAIL with "VACUOUS RUN"
#   one test, selected                 -> must PASS (the positive control)
set -u
cd "$(dirname "$0")/.." || exit 2

mkdir -p tmp
work=$(mktemp -d tmp/vacuous-gate.XXXXXX) || exit 2
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/one"
printf 'int main() { return 0; }\n' > "$work/one/vacuous_probe.mad"
printf 'int main() { return 0; }\n' > "$work/one/vacuous_skipped.mad"
echo "the gate's all-skipped case" > "$work/one/vacuous_skipped.vacuousgate_skip"
printf '#!/bin/sh\nexit 0\n' > "$work/stub-madc"
chmod +x "$work/stub-madc"

fail=0
# $1 = case label, $2 = expected outcome (fail|pass), $3 = test dir, rest = runner args
expect_run() {
	local label=$1 want=$2 dir=$3
	shift 3
	local out rc
	out=$(env -u MADC_WRAPPER MADC_SKIP_EXT="${MADC_SKIP_EXT:-}" MADC_BIN="$work/stub-madc" MADC_TEST_DIR="$dir" \
		bash scripts/run_tests.sh "$@" 2>&1)
	rc=$?
	if [ "$want" = fail ]; then
		if [ $rc -eq 0 ] || ! grep -q "VACUOUS RUN" <<< "$out"; then
			echo "check-runner-refuses-vacuous: $label — expected a VACUOUS RUN refusal, got rc=$rc" >&2
			printf '%s\n' "$out" | tail -5 >&2
			fail=1
		fi
	elif [ $rc -ne 0 ]; then
		echo "check-runner-refuses-vacuous: $label — expected rc 0, got rc=$rc" >&2
		printf '%s\n' "$out" | tail -5 >&2
		fail=1
	fi
}

expect_run "filter matching nothing" fail "$work/one" 'no_such_test_*'
MADC_SKIP_EXT=vacuousgate expect_run "selection all skipped" fail "$work/one" 'vacuous_skipped'
expect_run "one test selected (positive control)" pass "$work/one" 'vacuous_probe'

[ $fail -eq 0 ] || exit 1
echo "check-runner-refuses-vacuous: OK (2 refusals, 1 positive control)"
