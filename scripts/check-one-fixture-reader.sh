#!/bin/bash
# check-one-fixture-reader.sh — a script that enumerates the suite's tests
# (tests/*.mad, "$TEST_DIR"/*.mad, "$TESTS"/*.mad) reads their run fixtures
# (.flags .argv .env .input .timeout) ONLY through scripts/test_fixtures.sh's
# fixture_load (.claude/rules/test-fixtures.md).
#
# Hand-copies of that reading drifted twice: six scripts each excluded the
# include helper by name until the .helper marker, then warn_census.sh read
# every fixture but .env and replayed testrepl_opencmd without EDITOR=echo —
# the REPL's %edit started the real vi on the test's input.
#
# A hand read is a fixture extension appended to a variable (`$base.flags`,
# `${base%.mad}.input`, `$dir/$name.env`) on a non-comment line of such a
# script. Negative control: a synthetic enumerator that reads `.flags` and
# `.env` by hand must FAIL; the same script through fixture_load must PASS.
#
#   scripts/check-one-fixture-reader.sh [DIR]   (DIR: the scripts to scan)
set -u
cd "$(dirname "$0")/.." || exit 2

enumerates_tests='(tests|TEST_DIR\}?|TESTS\}?)"?/\*\.mad'
hand_read='\$\{?[A-Za-z_][A-Za-z0-9_]*([%#][^}]*)?\}?\.(flags|argv|env|input|timeout)\b'

# Prints the hand reads in the scripts under $1; returns 0 when there are none.
scan() {
	local dir=$1 f hits found=0
	for f in "$dir"/*.sh; do
		[ -f "$f" ] || continue
		case "$(basename "$f")" in
			test_fixtures.sh|check-one-fixture-reader.sh) continue ;;
		esac
		grep -v -E '^[[:space:]]*#' "$f" | grep -q -E "$enumerates_tests" || continue
		hits=$(grep -n -E "$hand_read" "$f" | grep -v -E '^[0-9]+:[[:space:]]*#')
		if [ -n "$hits" ]; then
			found=1
			printf '%s\n' "$hits" | sed "s|^|  $f:|"
		fi
	done
	return $found
}

# Negative control, both ways.
ctl=$(mktemp -d tmp/fixture-reader-ctl.XXXXXX 2>/dev/null) || { mkdir -p tmp; ctl=$(mktemp -d tmp/fixture-reader-ctl.XXXXXX); }
trap 'rm -rf "$ctl"' EXIT
mkdir -p "$ctl/bad" "$ctl/good"
cat > "$ctl/bad/enum.sh" <<'EOF'
for t in tests/*.mad; do
	base=$(basename "$t" .mad)
	[ -f "tests/$base.flags" ] && read -r -a flags < "tests/$base.flags"
	[ -f "tests/${base}.env" ] && read -r -a envs < "tests/${base}.env"
done
EOF
cat > "$ctl/good/enum.sh" <<'EOF'
. scripts/test_fixtures.sh
for t in tests/*.mad; do
	fixture_load tests "$(basename "$t" .mad)"
done
EOF
if scan "$ctl/bad" > /dev/null; then
	echo "check-one-fixture-reader: CONTROL FAILED — a hand read of .flags/.env went undetected" >&2
	exit 1
fi
if ! scan "$ctl/good" > /dev/null; then
	echo "check-one-fixture-reader: CONTROL FAILED — fixture_load was flagged" >&2
	exit 1
fi

out=$(scan "${1:-scripts}")
if [ -n "$out" ]; then
	echo "check-one-fixture-reader: a script reads a test's fixtures by hand —" >&2
	echo "use fixture_load (scripts/test_fixtures.sh):" >&2
	printf '%s\n' "$out" >&2
	exit 1
fi
echo "check-one-fixture-reader: OK (one reader; control both ways)"
