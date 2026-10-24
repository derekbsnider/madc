#!/bin/bash
# check-one-option-parser.sh — a command line is read by ONE reader,
# madc::cli::parse (include/libmadc/cli_options.h), over a table of rows
# (madc's own: src/madc_cli.cpp).
#
# Until 2026-10 madc's main() read its options with 82 strcmp/strncmp arms,
# each deciding on its own what an option looks like and where the
# program's argv begins; three gcc-parity bugs in one week came from arms
# that disagreed (-E ignored -o, a compiler line ran as a script, -Wall was
# read as a file). Design: docs/plans/2026-10-16-cli-options-design.md.
#
# Rule: outside the reader (src/cli_options.cpp), no C/C++ source in src/ or
# include/ compares an argv word with an option spelling (strcmp(argv[i],
# "-x"), strncmp(argv[i], "--x=", n)) or tests an argv word's leading dash
# (argv[i][0] == '-'). Comparing an operand with "-" (stdin) is not an
# option read and passes. include/doctest.h is third-party.
#
# Negative control: a synthetic file with each shape must FAIL and a clean
# one must PASS — else the gate itself is broken and fails loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

PATTERN='strn?cmp *\( *argv *\[[^]]*] *, *"-[^"]|argv *\[[^]]*] *\[ *0 *] *== *'"'"'-'"'"

# scan FILE... — prints each hand-rolled option read; 0 = clean.
scan() {
	local out
	out=$(grep -n -H -E "$PATTERN" "$@" 2>/dev/null)
	[ -z "$out" ] && return 0
	echo "$out"
	return 1
}

tmpd=$(mktemp -d)
printf 'void a(char **argv, int i) { if (strcmp(argv[i], "-v") == 0) return; }\n' > "$tmpd/bad1.cpp"
printf 'void b(char **argv, int i) { if (strncmp(argv[i], "--std=", 6) == 0) return; }\n' > "$tmpd/bad2.cpp"
printf "void c(char **argv, int i) { if (argv[i][0] == '-') return; }\n" > "$tmpd/bad3.cpp"
printf 'void d(char **argv, int f) { if (strcmp(argv[f], "-") == 0) return; }\n' > "$tmpd/good.cpp"
for bad in bad1 bad2 bad3; do
	if scan "$tmpd/$bad.cpp" >/dev/null; then
		rm -rf "$tmpd"
		echo "check-one-option-parser: NEGATIVE CONTROL FAILED — $bad passed" >&2
		exit 2
	fi
done
if ! scan "$tmpd/good.cpp" >/dev/null; then
	rm -rf "$tmpd"
	echo "check-one-option-parser: POSITIVE CONTROL FAILED — the stdin operand test was flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h' 'include/*.h' \
	'include/libmadc/*.h' 'include/madcdis/*.h' \
	| grep -v -E '^(src/cli_options\.cpp|include/doctest\.h)$')
# shellcheck disable=SC2086
if ! out=$(scan $files); then
	echo "check-one-option-parser: a command line read past madc::cli::parse:" >&2
	echo "$out" >&2
	echo "  -> add a row to the program's table (madc: src/madc_cli.cpp) and a case to its switch" >&2
	exit 1
fi
echo "check-one-option-parser: OK (every command line reads through madc::cli::parse)"
