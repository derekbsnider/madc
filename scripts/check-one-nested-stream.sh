#!/bin/bash
# DRIFT-PREVENTION GATE -- a parse over its own token run has ONE owner,
# Program::NestedTokenStream (include/madc.h). It installs the run (Isolated:
# TokenStream::swap_in; Injected: ahead of the live stream) and, on every
# exit, returns the outer stream (swap_back, or a drain back to the base) AND
# the read context the outer parse resumes with: curToken / prevToken
# (isUnaryPosition reads prevToken) and ParsePosition.
#
# The swap was hand-rolled at 24 sites and the injection at 7; twelve swaps
# and six injections returned the stream but not the context. A stale
# context reads the next operator wrong. On exit, the outer parse inherited a
# probe's `;` sentinel as its prevToken: `declval<F>()(args)` lost its call
# (testexplicitpack), and the first `std::move(x) + 0` read a unary plus (B46:
# the SFINAE check of a dependent return type). On entry, a probe inherited
# the outer token: an NSDMI's `{-1}` read a binary minus. Each was fixed by
# one more hand copy at one site.
#
# Rule, over src/*.cpp and include/*.h: no call of TokenStream::swap_in /
# swap_back, no TokenStream::State holder, and no drain loop back to a base
# (`while ( tokens.size() > base )`) outside the owner (its lines are marked
# `// allowed-exception: the owner`) and TokenStream's own definitions. A
# comment line is skipped.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	{
		if (!code($0) || $0 ~ /allowed-exception: the owner/)
			next
		if ($0 ~ /^[[:space:]]*(State swap_in\(|void swap_back\(State )/)
			next
		if ($0 ~ /[.>]swap_in\(|[.>]swap_back\(|TokenStream::State/)
			print FILENAME ":" FNR ": hand-rolled nested token stream"
		else if ($0 ~ /while[[:space:]]*\([[:space:]]*([A-Za-z_]+[.])?tokens[.]size\(\)[[:space:]]*>/)
			print FILENAME ":" FNR ": hand-rolled injected-run drain"
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
    TokenStream::State saved_tokens = tokens.swap_in(std::move(body));
	pgm.tokens.swap_back(std::move(saved));
    TokenStream::State saved;
    // a comment line is skipped: tokens.swap_in(seq)
    NestedTokenStream nested(*this, std::move(body));
	  : pgm(p), outer(p.tokens.swap_in(std::move(seq))),	// allowed-exception: the owner
    State swap_in(std::vector<TokenBase *> seq)
    void swap_back(State prev)
    nested.close();
    while ( pgm.tokens.size() > replay_base )
	while ( tokens.size() > pre_size )
		while ( pgm.tokens.size() > base )	// allowed-exception: the owner
    while ( tokens.empty() )
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 5 ]; then
	echo "check-one-nested-stream: NEGATIVE CONTROL FAILED -- matched $c of 5"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "nested token streams outside Program::NestedTokenStream: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> parse the sequence inside a Program::NestedTokenStream scope"
	echo "     (include/madc.h): it swaps the stream and restores curToken,"
	echo "     prevToken and ParsePosition on every exit, a throw included."
	exit 1
fi
echo "GREEN -- a nested token stream has one owner."
