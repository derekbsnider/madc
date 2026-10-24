// madc_regex.cpp — madc's ONE regex engine over PCRE2 (split arc 1A.3;
// design docs/plans/2026-10-10-pcre2-design.md; the contract is
// include/madc_regex.h). The only file that includes pcre2.h (gate:
// scripts/check-one-regex-engine.sh). No JIT: a pattern is compiled once per
// call or per ex command, and PCRE2's interpreter is enough.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>


#include "datadef.h"		// madc_verbose (the DBG contract)
#include "madc_regex.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace madc {

namespace {

// PCRE2's text for an error code.
std::string pcre2_message(int code)
{
    PCRE2_UCHAR buf[256];
    int n = pcre2_get_error_message(code, buf, sizeof(buf));
    return n < 0 ? std::string("unknown PCRE2 error")
		 : std::string((const char *)buf, (size_t)n);
}

// The byte after the code point at `at` (a UTF-8 continuation byte is never
// a code point's start; an invalid lead byte counts as one).
size_t next_code_point(const std::string &s, size_t at)
{
    ++at;
    while ( at < s.size() && ((unsigned char)s[at] & 0xC0) == 0x80 )
	++at;
    return at;
}

// Match data sized for the pattern, freed on scope exit.
struct match_data
{
    pcre2_match_data *md;
    explicit match_data(const pcre2_code *code)
	: md(pcre2_match_data_create_from_pattern(code, NULL)) {}
    ~match_data() { pcre2_match_data_free(md); }
};

} // namespace

regex::regex(const std::string &pattern)
    : code(0), text(pattern), offset(0)
{
    int err = 0;
    PCRE2_SIZE at = 0;
    code = pcre2_compile((PCRE2_SPTR)pattern.data(), pattern.size(),
			 PCRE2_UTF | PCRE2_MATCH_INVALID_UTF, &err, &at, NULL);
    if ( !code )
    {
	message = pcre2_message(err);
	offset = (size_t)at;
    }
}

regex::~regex()
{
    pcre2_code_free(code);
}

std::string regex::refusal(const char *who) const
{
    return std::string(who ? who : "regex") + ": invalid pattern \"" + text
	 + "\": " + message + " at offset " + std::to_string(offset);
}

bool regex::find(const std::string &subject, size_t from, unsigned options,
		 span &found, std::vector<span> *groups) const
{
    if ( !code || from > subject.size() )
	return false;
    match_data m(code);
    if ( !m.md )
	return false;
    int rc = pcre2_match(code, (PCRE2_SPTR)subject.data(), subject.size(),
			 from, options, m.md, NULL);
    if ( rc < 0 )
	return false;
    const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(m.md);
    found.begin = (size_t)ov[0];
    found.end = (size_t)ov[1];
    if ( groups )
    {
	// Pairs past rc are unset; PCRE2_UNSET marks an unset one within.
	const uint32_t pairs = pcre2_get_ovector_count(m.md);
	groups->assign(pairs, span());
	for ( uint32_t k = 0; k < pairs; ++k )
	{
	    const bool set = (int)k < rc && ov[2 * k] != PCRE2_UNSET;
	    (*groups)[k].begin = set ? (size_t)ov[2 * k] : std::string::npos;
	    (*groups)[k].end = set ? (size_t)ov[2 * k + 1] : std::string::npos;
	}
    }
    return true;
}

bool regex::match(const std::string &subject) const
{
    span s;
    return find(subject, 0, PCRE2_ANCHORED | PCRE2_ENDANCHORED, s);
}

bool regex::search(const std::string &subject) const
{
    span s;
    return find(subject, 0, 0, s);
}

std::vector<regex::span> regex::each(const std::string &subject) const
{
    return walk(subject, 0);
}

std::vector<std::vector<regex::span> > regex::each_groups(const std::string &subject) const
{
    std::vector<std::vector<span> > groups;
    walk(subject, &groups);
    return groups;
}

std::vector<regex::span> regex::walk(const std::string &subject,
				     std::vector<std::vector<span> > *groups) const
{
    std::vector<span> out;
    std::vector<span> found_groups;
    size_t from = 0;
    bool after_empty = false;
    while ( from <= subject.size() )
    {
	span s;
	const unsigned options = after_empty ? (PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED) : 0;
	if ( !find(subject, from, options, s, groups ? &found_groups : 0) )
	{
	    // After an empty match: no non-empty one here either, so the
	    // next search starts a code point on.
	    if ( !after_empty || from >= subject.size() )
		break;
	    from = next_code_point(subject, from);
	    after_empty = false;
	    continue;
	}
	out.push_back(s);
	if ( groups )
	    groups->push_back(found_groups);
	from = s.end;
	after_empty = s.begin == s.end;
    }
    return out;
}

bool regex::replace(const std::string &subject, const std::string &replacement,
		    std::string &out, std::string *why) const
{
    out = subject;
    if ( !code )
    {
	if ( why )
	    *why = message;
	return false;
    }
    const uint32_t options = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_UNSET_EMPTY
			   | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    std::string buf(subject.size() + replacement.size() + 64, '\0');
    for ( int pass = 0; pass < 2; ++pass )
    {
	PCRE2_SIZE len = buf.size();
	int rc = pcre2_substitute(code, (PCRE2_SPTR)subject.data(), subject.size(), 0,
				  options, NULL, NULL,
				  (PCRE2_SPTR)replacement.data(), replacement.size(),
				  (PCRE2_UCHAR *)&buf[0], &len);
	if ( rc == PCRE2_ERROR_NOMEMORY && pass == 0 )
	{
	    buf.assign(len, '\0');	// OVERFLOW_LENGTH: the size it needs
	    continue;
	}
	if ( rc < 0 )
	{
	    if ( why )
		*why = pcre2_message(rc);
	    return false;
	}
	out.assign(buf, 0, len);
	return true;
    }
    return false;
}

std::vector<std::string> regex::split(const std::string &subject) const
{
    std::vector<std::string> fields;
    if ( !code )
	return fields;
    size_t start = 0;
    for ( const span &s : each(subject) )
    {
	if ( s.begin == s.end && (s.begin == start || s.begin >= subject.size()) )
	    continue;	// Perl: no empty field from an empty match here
	fields.push_back(subject.substr(start, s.begin - start));
	start = s.end;
    }
    fields.push_back(subject.substr(start));
    while ( !fields.empty() && fields.back().empty() )
	fields.pop_back();	// Perl drops trailing empty fields
    return fields;
}

} // namespace madc
