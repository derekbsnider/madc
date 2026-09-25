#!/bin/bash
# check-one-escape-decoder.sh — ONE decoder of a literal's escape sequence
# (read_literal_escape, src/lexer.cpp), and ONE rule for a narrow character
# constant's value from its bytes (narrow_char_constant_value).
#
# The rule: the narrow string arm, the narrow character arm, the prefixed
# (L/u8/u/U) reader and the #if evaluator's character constants each carried
# their own escape switch, and they differed: `\x` stopped after two hex
# digits (L"\x1234" was three wide chars), a narrow literal kept a
# universal-character-name as its spelled characters, `\e` was a backslash
# plus 'e', and `\X` passed as a hex escape. All four now decode through the
# one template (over the live Source or captured text); a fifth switch would
# be a copy of the rule that can drift again — call the decoder instead.
#
# Marker (the concept's distinctive spelling — the decode direction of the
# simplest escape): `case 'n':` producing '\n'. The encode direction
# (`case '\n':` -> "\\n", in the emitters and dumpers) does not match.
# One permitted site: the decoder's body.
#
# Second rule: the tokenizer took a character constant's FIRST byte ('ab' was
# 97) while the #if evaluator folded the bytes but read '\xff' unsigned. Both
# now value the bytes through narrow_char_constant_value. Marker: the byte
# fold `(x << 8) |` in src/lexer.cpp, one permitted site (its body).
set -u
cd "$(dirname "$0")/.."

marker="case 'n': +[^;]*'\\\\n'"

fail=0
hits=$(grep -rnE "$marker" src include --include='*.cpp' --include='*.h' 2>/dev/null || true)
count=0
[ -n "$hits" ] && count=$(echo "$hits" | wc -l)
if [ "$count" -gt 1 ]; then
	echo "check-one-escape-decoder: escape switch outside read_literal_escape:"
	echo "$hits"
	fail=1
fi
if [ "$count" -lt 1 ]; then
	echo "check-one-escape-decoder: the decoder's own switch is missing from src/lexer.cpp"
	fail=1
fi

fold_marker='<< *8\) *\|'
folds=$(grep -nE "$fold_marker" src/lexer.cpp 2>/dev/null || true)
fold_count=0
[ -n "$folds" ] && fold_count=$(echo "$folds" | wc -l)
if [ "$fold_count" -ne 1 ]; then
	echo "check-one-escape-decoder: character-constant byte fold outside narrow_char_constant_value ($fold_count sites):"
	echo "$folds"
	fail=1
fi

# Negative control: the marker must catch the shapes the old copies used, or
# the gate is dead and its verdict means nothing.
ctrl=$(mktemp)
cat > "$ctrl" <<'CTRL'
			case 'n':  word += '\n'; break;
	    case 'n':  return '\n';
CTRL
ctrl_hits=$(grep -cE "$marker" "$ctrl")
if [ "$ctrl_hits" -ne 2 ]; then
	echo "check-one-escape-decoder: NEGATIVE CONTROL FAILED ($ctrl_hits of 2 shapes matched)"
	fail=1
fi
if ! echo '	    value = (value << 8) | (ch & 0xff);' | grep -qE "$fold_marker"; then
	echo "check-one-escape-decoder: NEGATIVE CONTROL FAILED (fold marker)"
	fail=1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-one-escape-decoder: OK (one escape decoder, one char-constant value rule, controls live)"
