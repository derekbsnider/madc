#!/bin/bash
# DRIFT-PREVENTION GATE -- a string literal initializing a character array is
# read and fitted by ONE owner (src/parser.cpp): Program::string_char_array
# reads the literal and the adjacent ones after it (narrow, or a wide one in
# the target wchar_t's units) and Program::fit_char_array fits the characters
# to the array (C11 6.7.9p14; C drops an excess with a warning as gcc does,
# --std=c++NN refuses a string with no room for its NUL).
#
# Two readers did it once: the braced/designated slots truncated silently and
# the unbraced `char buf[N] = "..."` reader kept every character, so the
# capacity check refused what gcc accepts (KG DupFamily
# string_literal_char_array_fill, consolidated d39f231ea).
#
# Rule: outside the two owners and append_string_literal_chars (the one
# character copy), no code reads adjacent string-literal tokens or copies a
# literal's characters into initializer elements.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

# hand_rolled FILE... : a string-literal read or character copy outside the
# owner function bodies.
hand_rolled() {
	awk '
		/^(TokenStructLit \*Program::(string_char_array|fit_char_array)|static void append_string_literal_chars)\(/ {
			owner = 1
		}
		owner && /^}/ { owner = 0; next }
		!owner && $0 !~ /^[ \t]*\/\// \
		    && ($0 ~ /peekToken\(\)->type\(\) == TokenType::ttString/ \
			|| $0 ~ /new TokenInt\(\(int64_t\)\(unsigned char\)/) {
			print FILENAME ":" FNR ": " $0
		}' "$@"
}

ctl=$(mktemp -d)
trap 'rm -rf "$ctl"' EXIT
cat > "$ctl/bad.cpp" <<'CTL'
	    if ( peek0->type() == TokenType::ttString )
	    {
		while ( peekToken() && peekToken()->type() == TokenType::ttString )
		{
		    const std::string &s = ((TokenStr *)nextToken())->str;
		    for ( char c : s )
			init_list.push_back(new TokenInt((int64_t)(unsigned char)c));
		}
	    }
CTL
cat > "$ctl/good.cpp" <<'CTL'
static void append_string_literal_chars(TokenStructLit *slit, const std::string &s)
{
    for ( char c : s )
	slit->inits.push_back(new TokenInt((int64_t)(unsigned char)c));
}

TokenStructLit *Program::string_char_array(TokenStr *strtok, size_t count,
					   bool wide, bool pad)
{
	lit = peekToken() && peekToken()->type() == TokenType::ttString
	    ? (TokenStr *)nextToken() : NULL;
}

	    {
		TokenStructLit *chars = string_char_array((TokenStr *)nextToken(),
		    (size_t)arr_dims[0], wide_string_array_init, false);
	    }
CTL
cb=$(hand_rolled "$ctl/bad.cpp" | grep -c .)
cg=$(hand_rolled "$ctl/good.cpp" | grep -c .)
if [ "$cb" -ne 2 ] || [ "$cg" -ne 0 ]; then
	echo "check-one-string-char-fill: NEGATIVE CONTROL FAILED -- a hand-rolled"
	echo "  string fill matched $cb of 2, the owners $cg of 0"
	exit 1
fi

hr=$(hand_rolled src/parser.cpp)
n=$(printf '%s' "$hr" | grep -c . || true)
owners=$(grep -c 'string_char_array(\|fit_char_array(' src/parser.cpp)
echo "hand-rolled string-literal char fills: $n (target 0); owner references: $owners"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hr"
	echo "  -> read and fit the literal through Program::string_char_array /"
	echo "     Program::fit_char_array."
	exit 1
fi
echo "GREEN -- a string literal fills a char array through one owner."
