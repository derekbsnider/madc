// Unit battery for madc::regex, madc's one regex engine over PCRE2 (split
// arc 1A.3; design docs/plans/2026-10-10-pcre2-design.md). ORACLE: Perl and
// PHP on the same patterns — `perl -e` for split's fields and the global
// match positions, `php -r` for preg_match / preg_replace and
// preg_last_error_msg's compile messages (PCRE2's own text) — recorded beside
// each case.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "libmadc/value.h"
#include "madc_regex.h"

using madc::regex;

static std::vector<std::string> v(std::initializer_list<const char *> l)
{
    return std::vector<std::string>(l.begin(), l.end());
}

TEST_CASE("regex: a pattern compiles once; a bad one says why and where")
{
    regex ok("hello[0-9]+");
    CHECK(ok.ok());
    CHECK(ok.error().empty());

    // php -r 'preg_match("/a(b/", "x"); echo preg_last_error_msg();' —
    // PHP warns "missing closing parenthesis at offset 3".
    regex bad("a(b");
    CHECK_FALSE(bad.ok());
    CHECK(bad.error() == "missing closing parenthesis");
    CHECK(bad.error_offset() == 3u);
    CHECK(bad.refusal("madc::regex_search")
	  == "madc::regex_search: invalid pattern \"a(b\": missing closing"
	     " parenthesis at offset 3");
    // A refused pattern matches nothing and splits into nothing.
    CHECK_FALSE(bad.search("a(b"));
    CHECK(bad.split("a b").empty());
    std::string out, why;
    CHECK_FALSE(bad.replace("abc", "x", out, &why));
    CHECK(out == "abc");
    CHECK(why == "missing closing parenthesis");

    regex quant("a{2,1}");		// "numbers out of order in {} quantifier"
    CHECK_FALSE(quant.ok());
    CHECK(quant.error_offset() == 5u);
}

TEST_CASE("regex: match is anchored at both ends, search finds anywhere")
{
    regex r("hello[0-9]+");
    CHECK(r.match("hello123"));
    CHECK_FALSE(r.match("hello123!"));		// ENDANCHORED
    CHECK_FALSE(r.match("say hello123"));	// ANCHORED
    CHECK(r.search("say hello123!"));
    CHECK_FALSE(r.search("hello"));
    regex alt("quick|brown");			// unescaped | (ERE / Perl)
    CHECK(alt.search("The brown fox"));
    regex lazy("<.+?>");			// a lazy quantifier
    std::vector<regex::span> s = lazy.each("<a><b>");
    REQUIRE(s.size() == 2u);
    CHECK(s[0].begin == 0u);
    CHECK(s[0].end == 3u);
}

TEST_CASE("regex: UTF-8 subjects — `.` is a code point, invalid bytes never fault")
{
    regex dot("^.$");
    CHECK(dot.match("\xc3\xa9"));		// é: ONE code point
    CHECK_FALSE(dot.match("ab"));
    regex any("b");
    CHECK(any.search("a\xff" "b"));		// an invalid byte before the match
    CHECK_FALSE(regex("^.$").match("\xff"));	// it is no code point
}

TEST_CASE("regex: each match, empty ones once per position (Perl's //g)")
{
    // perl -e '$_="abc"; while(/x*/g){print pos,","}' → 0,1,2,3,
    std::vector<regex::span> e = regex("x*").each("abc");
    REQUIRE(e.size() == 4u);
    for ( size_t i = 0; i < e.size(); ++i )
    {
	CHECK(e[i].begin == i);
	CHECK(e[i].end == i);
    }
    // perl -e '$_="a,b"; while(/,*/g){print "$-[0]-$+[0] "}' → 0-0 1-2 2-2 3-3
    std::vector<regex::span> c = regex(",*").each("a,b");
    REQUIRE(c.size() == 4u);
    CHECK(c[1].begin == 1u);
    CHECK(c[1].end == 2u);
    // An empty match after a multibyte code point steps over the whole of it.
    std::vector<regex::span> u = regex("").each("\xc3\xa9x");
    REQUIRE(u.size() == 3u);
    CHECK(u[1].begin == 2u);
}

TEST_CASE("regex: replace — PCRE2's $0 $1 ${1} $$, every match")
{
    std::string out;
    // php -r 'echo preg_replace("/quick|brown/", "slow", "The quick brown fox");'
    REQUIRE(regex("quick|brown").replace("The quick brown fox", "slow", out));
    CHECK(out == "The slow slow fox");
    // php -r 'echo preg_replace("/(\w+)@(\w+)/", "$2 at ${1}", "me@host");'
    REQUIRE(regex("(\\w+)@(\\w+)").replace("me@host", "$2 at ${1}", out));
    CHECK(out == "host at me");
    REQUIRE(regex("o").replace("foo", "[$0]$$", out));
    CHECK(out == "f[o]$[o]$");
    REQUIRE(regex("(a)|(b)").replace("ab", "<$2>", out));
    CHECK(out == "<><b>");			// an unset group is empty
    REQUIRE(regex("x").replace("abc", "y", out));
    CHECK(out == "abc");			// no match: the subject
    // A replacement longer than the first guess grows the buffer.
    REQUIRE(regex(".").replace("abcd", "0123456789012345678901234567890123456789", out));
    CHECK(out.size() == 160u);
    // A malformed replacement is refused: the subject comes back.
    std::string why;
    CHECK_FALSE(regex("(a)").replace("a", "$9", out, &why));
    CHECK(out == "a");
    CHECK_FALSE(why.empty());
}

TEST_CASE("regex: split — Perl's fields")
{
    // perl -e 'print join("|", split(/:+/, "one:two::three:::four"))'
    CHECK(regex(":+").split("one:two::three:::four") == v({ "one", "two", "three", "four" }));
    // perl -e 'print join("|", split(/:/, ":a:b::"))' → |a|b  (leading kept,
    // trailing dropped)
    CHECK(regex(":").split(":a:b::") == v({ "", "a", "b" }));
    // perl -e 'print join("|", split(//, "abc"))' → a|b|c
    CHECK(regex("").split("abc") == v({ "a", "b", "c" }));
    // perl -e 'print join("|", split(/,*/, "a,b"))' → a|b
    CHECK(regex(",*").split("a,b") == v({ "a", "b" }));
    // perl -e 'print scalar(split(/,/, ""))' → 0
    CHECK(regex(",").split("").empty());
    CHECK(regex(",").split("abc") == v({ "abc" }));
    CHECK(regex("").split("\xc3\xa9z") == v({ "\xc3\xa9", "z" }));
}

// The script surfaces (split arc 1A.3 commit 2): the very functions a script
// reaches — madc::regex_* mangled-direct, perl::grep / perl::split through
// their __perl_* entries — each REFUSE a pattern that does not compile with
// one stderr line naming itself, and answer neutrally.
namespace madc {
bool regex_search(const char *s, const char *pattern);
value &regex_replace(value &out, const char *s, const char *pattern,
		     const char *replacement);
}
extern "C" {
void __perl_grep(madc::value *, const char *, madc::value *);
void __perl_split(madc::value *, const char *, const char *);
}

namespace {

// std::cerr's text while `run` runs.
template <typename F>
std::string cerr_of(F run)
{
    std::ostringstream said;
    std::streambuf *was = std::cerr.rdbuf(said.rdbuf());
    run();
    std::cerr.rdbuf(was);
    return said.str();
}

} // namespace

TEST_CASE("regex: every script surface refuses a bad pattern on stderr")
{
    const std::string why = ": invalid pattern \"a(b\": missing closing parenthesis at offset 3\n";
    bool found = true;
    CHECK(cerr_of([&] { found = madc::regex_search("a(b", "a(b"); })
	  == "madc::regex_search" + why);
    CHECK_FALSE(found);

    madc::value out;
    CHECK(cerr_of([&] { madc::regex_replace(out, "xa(by", "a(b", "-"); })
	  == "madc::regex_replace" + why);
    CHECK(out.as_string() == "xa(by");
    CHECK(cerr_of([&] { madc::regex_replace(out, "a", "(a)", "$9"); })
	  .find("madc::regex_replace: invalid replacement \"$9\": ") == 0);
    CHECK(out.as_string() == "a");

    madc::value words = madc::value::make_array();
    words.array().push_back(madc::value("xa(by"));
    madc::value hits;
    CHECK(cerr_of([&] { __perl_grep(&hits, "a(b", &words); }) == "perl::grep" + why);
    REQUIRE(hits.is_array());
    CHECK(hits.as_array().empty());
    madc::value parts;
    CHECK(cerr_of([&] { __perl_split(&parts, "a(b", "xa(by"); }) == "perl::split" + why);
    REQUIRE(parts.is_array());
    CHECK(parts.as_array().empty());

    // A pattern that compiles says nothing.
    CHECK(cerr_of([&] { found = madc::regex_search("say hi", "h(i)"); }).empty());
    CHECK(found);
}
