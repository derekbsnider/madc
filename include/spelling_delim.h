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

// A parameter's REFERENCE declarator read off its type spelling (`T&`,
// `const W<T>&&`, `_Args&&...`): the reference kind, the referent spelling,
// and whether the referent is const at TOP level — a `const` outside every
// `<...>` / `(...)` and after the last top-level `*` (`const char*&` refers to
// a mutable pointer, `W<const T>&` to a mutable W, `const W<int*>&` to a
// const W). The one owner of the spelled reference-binding facts: FuncDef's
// parameter predicates and the function-template candidate ranker read it.
struct SpelledReference
{
	bool is_ref = false;		// a trailing `&` or `&&`
	bool rvalue = false;		// `&&`
	bool referent_const = false;
	std::string referent;		// the spelling before the reference, trimmed
};

inline SpelledReference spelled_reference(const std::string &sp)
{
	SpelledReference r;
	size_t n = sp.size();
	while ( n > 0 && sp[n - 1] == ' ' )
		--n;
	if ( n >= 3 && sp.compare(n - 3, 3, "...") == 0 )
	{
		n -= 3;
		while ( n > 0 && sp[n - 1] == ' ' )
			--n;
	}
	if ( n == 0 || sp[n - 1] != '&' )
		return r;
	r.is_ref = true;
	--n;
	if ( n > 0 && sp[n - 1] == '&' )
	{
		r.rvalue = true;
		--n;
	}
	r.referent = spelling_trim(sp.substr(0, n));
	const std::string &t = r.referent;
	// The referent's own level starts after the last top-level `*`.
	size_t from = 0;
	{
		SpellingDelimDepth d;
		for ( size_t i = 0; i < t.size(); ++i )
		{
			bool top = d.top();
			d.update(t[i]);
			if ( top && t[i] == '*' )
				from = i + 1;
		}
	}
	SpellingDelimDepth d;
	for ( size_t i = 0; i < t.size(); ++i )
	{
		bool top = d.top();
		d.update(t[i]);
		if ( i < from || !top || t.compare(i, 5, "const") != 0 )
			continue;
		bool starts = i == 0 || !SpellingDelimDepth::name_char(t[i - 1]);
		bool ends = i + 5 >= t.size()
			 || !SpellingDelimDepth::name_char(t[i + 5]);
		if ( starts && ends )
		{
			r.referent_const = true;
			break;
		}
	}
	return r;
}

// A FORWARDING reference ([temp.deduct.call]/3): an rvalue reference to a
// cv-unqualified type parameter of the function template itself (`T &&`,
// `T &&...`; never `const T &&`, never `vector<T> &&`). The one test over a
// declared parameter spelling.
inline bool spelling_is_forwarding_reference(const std::string &sp,
				const std::vector<std::string> &typeparams)
{
	SpelledReference r = spelled_reference(sp);
	if ( !r.rvalue || r.referent_const )
		return false;
	for ( size_t i = 0; i < typeparams.size(); ++i )
		if ( r.referent == typeparams[i] )	// allowed-exception: the owner
			return true;
	return false;
}

#endif
