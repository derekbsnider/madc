#!/bin/bash
# DRIFT-PREVENTION GATE -- "the type a word lexes as" has ONE owner,
# Program::lexer_type_token() (src/lexer.cpp).
#
# The word lexer and PCH replay each looked the word up in datatype_map by
# hand. An interactive entry must lex like a file, knowing only madc's own
# types (plan §41.2a slice 3), and a hand lookup lexes an earlier entry's
# class as a type-name: every out-of-line member defined in a later entry was
# refused, and so was a local shadowing the class.
#
# One rule over the lexer (src/lexer*.cpp): no code line looks a name up in
# datatype_map by hand, unless it is marked `// allowed-exception: <why>` on
# the same line. Two-sided: the negative control proves the pattern bites.
set -u
cd "$(dirname "$0")/.."

SCAN='datatype_map\.find *\('

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
		if ( (bmi=datatype_map.find(word)) != datatype_map.end() )
    flat_datatype_map_iter di = datatype_map.find(word);	// allowed-exception: control
	// an older `datatype_map.find(word)` lookup (a comment: skipped)
CTL
c=$(code_lines "$SCAN" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-lexer-type-token: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

files=$(git ls-files 'src/lexer*.cpp')
un=$(code_lines "$SCAN" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
echo "hand-written datatype_map lookups in the lexer: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> ask Program::lexer_type_token(word)."
	exit 1
fi
echo "GREEN -- \"the type a word lexes as\" has one owner."
