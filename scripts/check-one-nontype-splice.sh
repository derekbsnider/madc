#!/bin/bash
# DRIFT-PREVENTION GATE -- a non-type template argument is spliced into a
# cloned token run by ONE owner, splice_nontype_template_arg (src/parser.cpp):
# the argument's tokens, GROUPED when the body applies a postfix step to the
# parameter ([temp.param]/8 -- the parameter names ONE prvalue operand).
#
# Three copies of the splice pasted the raw tokens: the class-template body
# clone, the out-of-line member clone and clone_template_tokens_with_type_subst.
# `P()` over the argument `&g` read `&(g())` -- "expecting addressable
# expression after '&'" once the `&` reader became the cast-expression owner --
# and `O->m` over `&obj` read `&(obj->m)`.
#
# Rule: after a lookup into a NON-TYPE token map (token_subst / toksubst), the
# hit is spliced through the owner; a clone_origin() push before the owner call
# (up to the arm's `continue;`) is a hand-rolled copy. (The alias-template
# tok_subst map carries TYPE arguments as tokens too -- `T(int)`, `T[3]` must
# never group -- so it is deliberately outside this rule.)
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

# hand_rolled FILE... : every lookup into a non-type token map whose hit is
# pushed token-by-token instead of through splice_nontype_template_arg.
hand_rolled() {
	awk '
		/(token_subst|toksubst)(\.|->)find\(/ && $0 !~ /^[ \t]*\/\// {
			open = FNR; at = FILENAME ":" FNR; owner = 0; next
		}
		open {
			if ( $0 ~ /splice_nontype_template_arg\(/ ) owner = 1
			if ( $0 ~ /clone_origin\(\)/ && !owner ) { print at ": " $0; open = 0; next }
			if ( $0 ~ /continue;/ || FNR - open > 12 ) open = 0
		}' "$@"
}

ctl=$(mktemp -d)
trap 'rm -rf "$ctl"' EXIT
cat > "$ctl/bad.cpp" <<'CTL'
	    std::map<std::string, std::vector<TokenBase *> >::iterator nti =
		token_subst.find(s);
	    if ( nti != token_subst.end() )
	    {
		for ( size_t ni = 0; ni < nti->second.size(); ++ni )
		    inj.push_back(nti->second[ni]->clone_origin());
		continue;
	    }
CTL
cat > "$ctl/good.cpp" <<'CTL'
		std::map<std::string, std::vector<TokenBase *> >::iterator ti =
		    toksubst.find(s);
		if ( ti != toksubst.end() )
		{
		    splice_nontype_template_arg(sub, ti->second, bt, next);
		    continue;
		}
		sub.push_back(bt->clone_origin());
CTL
cb=$(hand_rolled "$ctl/bad.cpp" | grep -c .)
cg=$(hand_rolled "$ctl/good.cpp" | grep -c .)
if [ "$cb" -ne 1 ] || [ "$cg" -ne 0 ]; then
	echo "check-one-nontype-splice: NEGATIVE CONTROL FAILED -- a hand-rolled"
	echo "  splice matched $cb of 1, an owner splice $cg of 0"
	exit 1
fi

hr=$(hand_rolled src/parser.cpp)
n=$(printf '%s' "$hr" | grep -c . || true)
owners=$(grep -c 'splice_nontype_template_arg(' src/parser.cpp)
echo "hand-rolled non-type argument splices: $n (target 0); owner call sites: $((owners - 1))"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hr"
	echo "  -> splice through splice_nontype_template_arg (it groups before a"
	echo "     postfix step)."
	exit 1
fi
echo "GREEN -- a non-type template argument is spliced by one owner."
