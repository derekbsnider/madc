#!/bin/bash
# DRIFT-PREVENTION GATE -- "is this declared parameter spelling a FORWARDING
# reference" ([temp.deduct.call]/3: an rvalue reference to a cv-unqualified
# type parameter of the function template itself) has ONE test,
# spelling_is_forwarding_reference() in include/spelling_delim.h, over the one
# spelled-reference reader spelled_reference().
#
# Three copies grew: best_deduced_fn_template's lambda compared
# spelled_reference(...).referent to each type parameter, FuncDef::
# is_concrete_rvalue_ref_param stripped the pack and the `&&` by hand and
# compared the remainder, and the identity-return recorder needed the same
# answer (B120, B128).
#
# Two rules over src/*.cpp and include/*.h:
#   1. no SpelledReference `.referent` equality against a type parameter (the
#      line, or one of the 3 code lines before it, names typeparams /
#      template_param_names / tparam) outside the owner, which is marked
#      `// allowed-exception: the owner`;
#   2. no type-parameter equality (`typeparams[..]` / `template_param_names[..]`
#      against a name) whose name is the variable a two-character suffix strip
#      (`x = s.substr(0, s.size() - 2)`, `s.erase(s.size() - 2)`) produced
#      within the 8 code lines before it.
# Two-sided: the negative control proves both rules still bite.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	# The last identifier of s.
	function lastident(s,   m) {
		if (match(s, /[A-Za-z_][A-Za-z_0-9]*[^A-Za-z_0-9]*$/) == 0)
			return ""
		m = substr(s, RSTART)
		sub(/[^A-Za-z_0-9].*$/, "", m)
		return m
	}
	function hasword(s, w) {
		return s ~ ("(^|[^A-Za-z_0-9])" w "([^A-Za-z_0-9]|$)")
	}
	FNR == 1 { n = 0; sname = ""; sline = -100; split("", ctx) }
	{
		if (!code($0)) next
		n++
		ctx[n % 4] = $0
		if ($0 ~ /size\(\) - 2\)/) {
			if (match($0, /[A-Za-z_][A-Za-z_0-9]*\.erase\(/)) {
				sname = substr($0, RSTART, RLENGTH - 7)
				sline = n
			} else if (match($0, /[^=!<>]=[^=]/)) {
				sname = lastident(substr($0, 1, RSTART))
				sline = n
			}
		}
		tp = $0 ~ /typeparams|template_param_names|tparam/
		if (!tp)
			for (k = 1; k <= 3; k++)
				if (ctx[(n - k) % 4] ~ /typeparams|template_param_names|tparam/)
					tp = 1
		ref = $0 ~ /\.referent[[:space:]]*[!=]=|[!=]=[[:space:]]*[A-Za-z_][A-Za-z_0-9]*\.referent([^A-Za-z_0-9]|$)/
		if (ref && tp && $0 !~ /allowed-exception: the owner/)
			print FILENAME ":" FNR ": hand-composed forwarding test (spelled referent vs a type parameter)"
		else if (sname != "" && n - sline <= 8 && $0 ~ /[!=]=/ \
			 && $0 ~ /(typeparams|template_param_names)\[/ && hasword($0, sname))
			print FILENAME ":" FNR ": hand-stripped forwarding test (&& suffix vs a type parameter)"
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	if ( r.rvalue && !r.referent_const )
	    for ( const std::string &tp : typeparams[k] )
		if ( r.referent == tp )
		    return 0;
	std::string base = sp.substr(0, sp.size() - 2);
	while ( !base.empty() && base[base.size() - 1] == ' ' )
	    base.erase(base.size() - 1);
	for ( size_t t = 0; t < template_param_names.size(); t++ )
	    if ( template_param_names[t] == base )
		return false;
	// a comment line is skipped: r.referent == typeparams[i]
		if ( r.referent == typeparams[i] )	// allowed-exception: the owner
	if ( spelling_is_forwarding_reference(declared[k][i], typeparams[k]) )
	    return 0;
	DataDef *base = resolve_flat_return_name(pgm, nm.substr(0, nm.size() - 2));
	return base ? (DataDef *)pgm.getReferenceType(base, true) : NULL;
	if ( td.typeparams[tp] == name )
	    return tp;
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 2 ]; then
	echo "check-one-forwarding-test: NEGATIVE CONTROL FAILED -- matched $c of 2"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "forwarding-reference tests outside spelling_is_forwarding_reference: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> ask spelling_is_forwarding_reference(spelling, typeparams)"
	echo "     (include/spelling_delim.h): it reads the reference through"
	echo "     spelled_reference and refuses a cv-qualified referent."
	exit 1
fi
echo "GREEN -- the forwarding-reference test has one owner."
