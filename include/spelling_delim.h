#ifndef __SPELLING_DELIM_H
#define __SPELLING_DELIM_H 1

// THE char-level balanced-delimiter tracker for C++ type SPELLINGS.
//
// The token-level rule lives in `DelimDepth` (src/parser.cpp); this is the same
// rule over a different alphabet — characters of a rendered type name rather
// than lexed tokens. Both are gated by scripts/check-one-delim-tracker.sh.
//
// Hand-rolled `int depth` loops over a spelling were the char-level half of the
// angle-bracket duplication family: five copies across parser.cpp,
// cir_builder.cpp and madc_mangle.cpp, none of which guarded `<` against
// appearing inside `(...)`/`[...]` or against following a non-name character.
// See docs/rules/delimiter-tracking.md.

#include <cctype>
#include <string>
#include <vector>

struct SpellingDelimDepth
{
	int paren = 0, square = 0, brace = 0, angle = 0;
	char prev = '\0';
	static bool name_char(char c)
	{ return isalnum((unsigned char)c) || c == '_'; }
	bool top() const { return !paren && !square && !brace && !angle; }
	void update(char c)
	{
		switch ( c )
		{
		    case '(': ++paren; break;
		    case ')': if ( paren > 0 )  --paren;  break;
		    case '[': ++square; break;
		    case ']': if ( square > 0 ) --square; break;
		    case '{': ++brace; break;
		    case '}': if ( brace > 0 )  --brace;  break;
		    // A `<` opens a template-argument list only after a NAME and
		    // outside every other nesting — `operator<`, `a < b` and a
		    // `<` inside `(...)` are not template brackets.
		    case '<':
			if ( !paren && !square && !brace
			  && (prev == '\0' || name_char(prev)) )
			    ++angle;
			break;
		    case '>':
			if ( angle > 0 && !paren && !square && !brace )
			    --angle;
			break;
		    default: break;
		}
		if ( c != ' ' )
			prev = c;
	}
};

// Trim leading/trailing spaces from a spelling fragment.
inline std::string spelling_trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t");
	if ( a == std::string::npos )
		return std::string();
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

// What a trailing remainder after the template-id's closing '>' means.
//
// `A<B>::C<D>` is NOT the template-id `A<B>` — its primary template-id is
// `C<D>`. Callers that must not be fooled by that pass Reject. The
// overload-matching callers historically ignored the tail; that policy is kept
// but is now WRITTEN DOWN at the call site instead of being a silent property
// of a second copy of this function.
enum class SpellingTail { Reject, Ignore };

// Split `head<a,b,c>` into `head` plus its TOP-LEVEL arguments, preserving any
// nesting inside each argument. Returns false when there is no top-level '<'
// (head is still set to the whole string), when the list is unterminated, or
// when `tail` is Reject and text follows the closing '>'.
inline bool split_template_id_parts(const std::string &s, std::string &head,
				    std::vector<std::string> &args,
				    SpellingTail tail = SpellingTail::Reject)
{
	SpellingDelimDepth d;
	size_t lt = std::string::npos;
	for ( size_t i = 0; i < s.size(); ++i )
	{
		d.update(s[i]);
		if ( s[i] == '<' && d.angle == 1 ) { lt = i; break; }
	}
	if ( lt == std::string::npos )
	{
		head = spelling_trim(s);
		return false;
	}
	head = spelling_trim(s.substr(0, lt));

	SpellingDelimDepth a;
	for ( size_t i = 0; i <= lt; ++i )
		a.update(s[i]);			// a.angle == 1 at the open
	size_t start = lt + 1;
	size_t close = std::string::npos;
	for ( size_t i = lt + 1; i < s.size(); ++i )
	{
		char c = s[i];
		int before = a.angle;
		a.update(c);
		if ( c == '>' && before == 1 && a.angle == 0 )
		{
			args.push_back(spelling_trim(s.substr(start, i - start)));
			close = i;
			break;
		}
		if ( c == ',' && a.angle == 1 && !a.paren && !a.square && !a.brace )
		{
			args.push_back(spelling_trim(s.substr(start, i - start)));
			start = i + 1;
		}
	}
	if ( close == std::string::npos )
		return false;			// unterminated argument list
	if ( tail == SpellingTail::Reject
	  && !spelling_trim(s.substr(close + 1)).empty() )
		return false;
	return true;
}

// Split a qualified spelling at TOP-LEVEL "::". Each component may carry its
// own `<...>`, which is why this cannot be a plain string split.
inline std::vector<std::string> split_scope_spelling(const std::string &s)
{
	std::vector<std::string> out;
	SpellingDelimDepth d;
	size_t start = 0;
	for ( size_t i = 0; i + 1 < s.size(); ++i )
	{
		d.update(s[i]);
		if ( s[i] == ':' && s[i + 1] == ':' && d.top() )
		{
			out.push_back(s.substr(start, i - start));
			d.update(s[++i]);
			start = i + 1;
		}
	}
	out.push_back(s.substr(start));
	return out;
}

// Split a template-argument list body (the text BETWEEN the outer angles) at
// top-level commas.
inline std::vector<std::string> split_template_args_spelling(const std::string &s)
{
	std::vector<std::string> out;
	SpellingDelimDepth d;
	size_t start = 0;
	for ( size_t i = 0; i < s.size(); ++i )
	{
		d.update(s[i]);
		if ( s[i] == ',' && d.top() )
		{
			out.push_back(spelling_trim(s.substr(start, i - start)));
			start = i + 1;
		}
	}
	std::string last = spelling_trim(s.substr(start));
	if ( !last.empty() )
		out.push_back(last);
	return out;
}

#endif
