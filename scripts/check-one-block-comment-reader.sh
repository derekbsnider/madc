#!/bin/bash
# check-one-block-comment-reader.sh — ONE reader of a block comment on the
# live lexer source (Source::consume_block_comment, src/lexer.cpp).
#
# The rule: the tokenizer, the #define body reader, the #if condition reader
# and the directive-tail skipper each carried their own `/* ... */` loop over
# the live Source. All four stopped silently at the end of input, so an
# unterminated comment (C11 6.4.9p1; gcc/clang refuse it) swallowed the rest
# of the file and the program ran. They now consume through the one owner,
# which refuses an unterminated comment at its `/*`. A fifth loop would be a
# copy of the rule without the refusal — adopt the helper instead.
#
# Markers (the concept's distinctive spellings — a comment-close test):
#   1. `== '*' && source.peek() == '/'`  (the peek-ahead close)
#   2. `prev == '*' && X == '/'`         (the previous-char close)
# One permitted site in total: the owner's body. Text-level scans over an
# already-read spelling (a std::string, not the live Source) are a different
# alphabet and do not match these markers.
set -u
cd "$(dirname "$0")/.."

marker="== '\\*' *&& *source\\.peek\\(\\) *== '/'|prev *== *'\\*' *&& *[a-z0-9_]+ *== *'/'"

fail=0
hits=$(grep -rnE "$marker" src include --include='*.cpp' --include='*.h' 2>/dev/null || true)
count=0
[ -n "$hits" ] && count=$(echo "$hits" | wc -l)
if [ "$count" -gt 1 ]; then
	echo "check-one-block-comment-reader: block-comment close test outside Source::consume_block_comment:"
	echo "$hits"
	fail=1
fi
if ! echo "$hits" | grep -q 'src/lexer.cpp'; then
	echo "check-one-block-comment-reader: the owner's close test is missing from src/lexer.cpp"
	fail=1
fi

# Negative control: each marker must catch a synthetic copy, or the gate is
# dead and its verdict means nothing.
ctrl=$(mktemp)
cat > "$ctrl" <<'EOF'
	if ( ch == '*' && source.peek() == '/' )
	if ( prev == '*' && c2 == '/' )
EOF
ctrl_hits=$(grep -cE "$marker" "$ctrl")
if [ "$ctrl_hits" -ne 2 ]; then
	echo "check-one-block-comment-reader: NEGATIVE CONTROL FAILED ($ctrl_hits of 2 markers matched)"
	fail=1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-one-block-comment-reader: OK (one owner, control live)"
