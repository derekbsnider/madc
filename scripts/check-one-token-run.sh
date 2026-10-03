#!/bin/bash
# DRIFT-PREVENTION GATE -- a run of tokens pushed ahead of the live stream has
# an owner. A run parsed as its own construct (a template-argument replay, a
# sentinel-terminated default argument or alias target, a class re-parse, a
# deferred body) goes through Program::NestedTokenStream (Injected), which
# returns the outer read context and discards what the parse left of the run.
# An unread (consumed tokens pushed back) is a Program::mark_stream /
# rewind_stream pair. A token rewrite of what is still unread is
# TokenStream::splice_front.
#
# The push was hand-rolled at 33 loops. Fourteen parsed a run to its end and
# returned no read context: the class re-parse left its `;` sentinel as the
# context of libc++'s `to_string(` declarator, the operator loop of
# finish_expression saw the context moved 92 times in 45 tests (its own save
# of the stop token masked it), and a refused declaration in a class
# re-parse left the rest of its run in the stream, where error recovery read
# it again until a timeout (ptrmem19.C; tests/testinjectedrunrecover).
# Eleven unread tokens, so the first token read again took the last token
# read as its prevToken (the `(` of `&(o.x)` after `o`). Three ran a
# whole-stream scan for their sentinel, and when it was gone consumed a live
# `;`. Three more rewrote a declaration by consuming it and pushing it back.
#
# Rule, over src/*.cpp and include/*.h: a `for` loop that calls pushToken
# within three lines of its header must be a SPLICE -- tokens that join the
# construct being read, the rest of it already consumed (a declarator's head
# handed back to parseDeclaration, `T x{v}` read as `T x = v`) -- and say so
# with `a splice:` on its header line or in the three lines above it. The
# owner's own injection loop is marked `// allowed-exception: the owner`. A
# comment line is skipped.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	function flush(   i, j, k, t, depth, marked, pushes) {
		for (i = 1; i <= n; i++) {
			if (!code(L[i]) || L[i] !~ /^[[:space:]]*for[[:space:]]*\(/ \
			  || L[i] ~ /allowed-exception: the owner/)
				continue
			# The header ends where its parentheses balance; the body is
			# the rest of that line and the lines up to its first `;`.
			depth = 0
			for (j = i; j <= n && j <= i + 4; j++) {
				t = L[j]
				depth += gsub(/\(/, "(", t) - gsub(/\)/, ")", t)
				if (depth <= 0)
					break
			}
			pushes = 0
			for (k = j; k <= n && k <= j + 3; k++) {
				if (code(L[k]) && L[k] ~ /pushToken\(/)
					pushes = 1
				if (k > j && L[k] ~ /;[[:space:]]*(\/\/.*)?$/)
					break
			}
			if (!pushes)
				continue
			marked = 0
			for (j = (i > 3 ? i - 3 : 1); j <= i; j++)
				if (L[j] ~ /a splice:/)
					marked = 1
			if (!marked)
				print F ":" i ": hand-rolled token run"
		}
		n = 0
	}
	FNR == 1 && NR > 1 { flush() }
	{ F = FILENAME; L[++n] = $0 }
	END { flush() }
	' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
    for ( std::vector<TokenBase *>::reverse_iterator it = inj.rbegin();
	  it != inj.rend(); ++it )
	pushToken(*it);
	for ( size_t k = decl.size(); k-- > 0; )
	    pgm.pushToken(decl[k]);
    // a splice: the declarator's head goes back for parseDeclaration.
    for ( size_t si = star_toks.size(); si-- > 0; )
	pushToken(star_toks[si]);
    if ( !cast_dd )	// a splice: the consumed cv run goes back
	for ( size_t qk = q.size(); qk-- > 0; )
	    pushToken(q[qk]);
    // for ( auto *t : run ) pushToken(t);
    for ( size_t i = 0; i < n; ++i )
	count += i;
    pushToken(nt);
		for ( size_t i = seq.size(); i-- > 0; )	// allowed-exception: the owner
		    pgm.pushToken(seq[i]);
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 2 ]; then
	echo "check-one-token-run: NEGATIVE CONTROL FAILED -- matched $c of 2"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "token runs pushed outside an owner: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> parse a run as its own construct inside a"
	echo "     Program::NestedTokenStream (Injected); rewind an unread with"
	echo "     Program::mark_stream / rewind_stream; replace unread tokens with"
	echo "     TokenStream::splice_front. A splice that joins the construct"
	echo "     being read says so with \`a splice:\` above its loop."
	exit 1
fi
echo "GREEN -- a pushed token run has an owner."
