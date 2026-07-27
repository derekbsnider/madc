#!/bin/bash
# RATCHET GATE — one shared balanced-delimiter tracker.
#
# The rule: `(` `[` `{` `<` nesting bookkeeping in a token or spelling scan
# belongs to DelimDepth (src/parser.cpp) and its two step helpers
# (delim_scan_step / Program::delimStepStream). NOTHING else may hand-roll it.
#
# Why a gate and not a comment: DelimDepth's own header comment has said
# "this is the single shared bookkeeping" since it was written, and eleven
# hand-rolled copies were added anyway — a comment only reaches someone
# already reading that function, which is exactly not the person writing the
# next copy. The angle-bracket disambiguation bug (a `<` counted as a template
# open inside `decltype(a < b)`, consuming a whole header and leaving its
# namespace unclosed) lived for seven weeks because the fix landed in one copy.
#
# Marker note: this counts the CONCEPT (a local delimiter-depth counter), not
# one spelling of it. Three earlier audits undercounted this family by grepping
# `++angle_depth`, which missed a counter named plain `depth`, missed
# DelimDepth's own `++angle`, and missed every paren/square/brace-only scanner.
#
# Ratchet: the count must never rise. Target is 0. Lower BASELINE whenever a
# scanner is migrated; never raise it.
set -u
cd "$(dirname "$0")/.."

BASELINE=0

# A hand-rolled tracker always declares at least one delimiter-depth local.
#
# EXCLUDED, deliberately — validate_expression_source() in madc_program.cpp is a
# raw-SOURCE mini-lexer with quote and comment states, not token-delimiter
# bookkeeping. It also tracks whether each paren level was preceded by an
# identifier, to allow commas in call args but not in grouping parens. The
# tie-breaker ("would a change to the delimiter rule require editing it?") says
# no: it would evolve with expression-validation, not with [temp.names].
# Merging it into DelimDepth would be worse than the duplication.
hits=$(grep -rnE '\bint +(angle|paren|square|brace)_depth\b' src/ include/ \
  | grep -vE '^include/doctest\.h:' \
  | grep -vE '^src/madc_program\.cpp:' )
n=$(printf '%s' "$hits" | grep -c . )

echo "one-delim-tracker ratchet: $n hand-rolled delimiter-depth locals (baseline $BASELINE, target 0)"

if [ "$n" -gt "$BASELINE" ]; then
	echo "REGRESSION — a new hand-rolled delimiter tracker was added."
	echo "Use DelimDepth + delim_scan_step()/delimStepStream() instead."
	echo "See .claude/rules/delimiter-tracking.md"
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

if [ "$n" -lt "$BASELINE" ]; then
	echo "RATCHET FORWARD — $((BASELINE - n)) scanner(s) migrated since the baseline."
	echo "Lower BASELINE in $0 to $n to lock the gain in."
	exit 1
fi

if [ "$n" -eq 0 ]; then
	echo "GREEN — DelimDepth is the only delimiter tracker."
	exit 0
fi

echo "held at baseline — $n scanner(s) still to migrate (see the rule file)."
exit 0
