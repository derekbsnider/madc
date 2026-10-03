#!/bin/bash
# DRIFT-PREVENTION GATE -- a parser rewind has ONE owner: Program::mark_stream /
# rewind_stream (include/madc.h). The mark holds the cursor (TokenStream::Pos,
# the pushback LIFO included) AND the read context the stream had there --
# curToken / prevToken (isUnaryPosition and isMemberAccessPosition read
# prevToken once the next token is consumed) and ParsePosition (a token made
# afterwards takes it, and so does a diagnostic); the rewind returns all of
# them.
#
# The rewind was hand-rolled at 26 sites: a TokenStream::savepos() mark,
# restored with `tokens = saved` or `tokens.restore(saved)`. Fourteen restored
# the cursor without the context, and the twelve that saved it disagreed on
# how (cur/prv only, cur/prv + ParsePosition::set_from(curToken), cur/prv +
# a ParsePosition snapshot). Four of them were injected runs ended by a
# rewind; those are Program::NestedTokenStream's (check-one-nested-stream.sh),
# whose Injected close is now this rewind.
#
# Rule, over src/*.cpp and include/*.h: no call of TokenStream::restore
# outside the owner (its line is marked `// allowed-exception: the owner`) and
# TokenStream's own definition. TokenStream has no `operator=(const Pos &)`
# any more, so the `tokens = saved` spelling does not compile. A mark that is
# never rewound (consumed_since, a cursor read) needs no owner. A comment line
# is skipped.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	{
		if (!code($0) || $0 ~ /allowed-exception: the owner/)
			next
		if ($0 ~ /^[[:space:]]*void restore\(const Pos &/)
			next
		if ($0 ~ /tokens[.]restore\(/)
			print FILENAME ":" FNR ": hand-rolled token stream rewind"
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
    tokens.restore(saved);
	pgm.tokens.restore(saved_tokens);
	    p->tokens.restore(pos);
    // a comment line is skipped: tokens.restore(saved)
	tokens.restore(m.pos);	// allowed-exception: the owner
    void restore(const Pos &p) { _cursor = p.cursor; _pushback = p.pushback; }
    saved_line_pos.restore();
    rewind_stream(saved);
    TokenStream::Pos before = pgm.tokens.savepos();
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 3 ]; then
	echo "check-one-stream-rewind: NEGATIVE CONTROL FAILED -- matched $c of 3"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "token stream rewinds outside Program::rewind_stream: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> mark with Program::mark_stream() and rewind with"
	echo "     Program::rewind_stream() (include/madc.h): it returns the"
	echo "     cursor AND curToken, prevToken and ParsePosition."
	exit 1
fi
echo "GREEN -- a token stream rewind has one owner."
