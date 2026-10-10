#ifndef __MADC_REGEX_H
#define __MADC_REGEX_H 1

// madc_regex.h — madc's ONE regex engine (split arc 1A.3; design
// docs/plans/2026-10-10-pcre2-design.md): a pattern compiled once by PCRE2,
// then matched, searched, replaced or split as many times as asked. Every
// regex in madc goes through this class — madc::regex_*, perl::grep,
// perl::split, and the ex core's :s / :g / /re/ — and nothing else includes
// pcre2.h (gate: scripts/check-one-regex-engine.sh).
//
// The syntax is PCRE2's own (Perl / PHP: unescaped `+ ? | ( )`, `\d`, lazy
// quantifiers, lookaround). Patterns and subjects are UTF-8: `.` is a code
// point, and invalid UTF-8 in a subject never faults (it cannot match a UTF
// construct). A replacement is PCRE2's: `$0` the whole match, `$1` / `${1}`
// a group, `$$` a dollar; an unset group is empty.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a regex is
// immutable once constructed. Concurrent calls on one object are safe (each
// call makes its own match data); distinct objects are independent. No
// global state.

#include <cstddef>
#include <string>
#include <vector>

struct pcre2_real_code_8;	// pcre2.h's compiled pattern (pcre2_code_8)

namespace madc {

class regex
{
public:
    // Compile `pattern`. ok() says whether it compiled; when not, error()
    // and error_offset() say why and where (a byte offset into the pattern).
    explicit regex(const std::string &pattern);
    ~regex();

    bool ok() const { return code != 0; }
    const std::string &pattern() const { return text; }
    const std::string &error() const { return message; }
    size_t error_offset() const { return offset; }
    // The one refusal line a function says on stderr for a pattern that did
    // not compile: `who: invalid pattern "p": message at offset N`.
    std::string refusal(const char *who) const;

    // The whole subject matches (anchored at both ends).
    bool match(const std::string &subject) const;
    // A match anywhere in the subject.
    bool search(const std::string &subject) const;

    // Every match, left to right, as byte offsets [begin, end). An empty
    // match is reported once per position; after one, the next match must
    // be non-empty at that position or start a code point later (PCRE2's
    // own global-match loop).
    struct span
    {
	size_t begin;
	size_t end;
    };
    std::vector<span> each(const std::string &subject) const;

    // Every match replaced by `replacement` into `out`. False when the
    // replacement is malformed (an unknown group, a stray `$`); `why`, when
    // given, then says so, and `out` is the subject.
    bool replace(const std::string &subject, const std::string &replacement,
		 std::string &out, std::string *why = 0) const;

    // Perl's split: the fields between matches. A positive-width match at
    // the start keeps an empty leading field; an empty match never makes an
    // empty field where the previous field ended, nor at the subject's end
    // (so an empty pattern splits into code points); TRAILING empty fields
    // are removed.
    std::vector<std::string> split(const std::string &subject) const;

private:
    // The match at or after `from` under PCRE2 match `options`; false when
    // there is none.
    bool find(const std::string &subject, size_t from, unsigned options,
	      span &found) const;

    pcre2_real_code_8 *code;	// NULL when the pattern did not compile
    std::string text;
    std::string message;
    size_t offset;

    regex(const regex &);
    regex &operator=(const regex &);
};

} // namespace madc

#endif // __MADC_REGEX_H
