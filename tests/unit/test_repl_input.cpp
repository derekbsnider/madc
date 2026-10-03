// The interactive-entry classifier (plan §41.1a, decision D11): is one REPL
// entry Complete, CompleteExtendable, Incomplete or Invalid? The corpus runs
// every entry through Program::classify_entry on a FRESH Program — the
// verdict needs neither a persistent session (§41.2) nor rollback (§41.3).
//
// The criterion is the one Julia, Python and IPython share: the first error
// decides, and an error AT the end of the entry means "keep reading". Stage 1
// (the lexer + the DelimDepth balance pass) catches the input ending inside
// a comment, a conditional group, a line splice or an open delimiter; stage
// 2 parses with the end-of-entry token appended. The REPL is a parser MODE
// (ParseMode::InteractiveEntry, off by default): the last cases pin that file
// parsing is unchanged by it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

namespace {

typedef ::Program::EntryVerdict V;

const char *verdict_name(V v)
{
    switch ( v )
    {
	case V::Complete:	    return "Complete";
	case V::CompleteExtendable: return "CompleteExtendable";
	case V::Incomplete:	    return "Incomplete";
	case V::Invalid:	    return "Invalid";
    }
    return "?";
}

::Program::EntryClassification classify(const std::string &text)
{
    ::Program p;
    return p.classify_entry(text, "<entry>");
}

struct Case { const char *text; V want; };

void check_corpus(const Case *cases, size_t n)
{
    for ( size_t i = 0; i < n; ++i )
    {
	::Program::EntryClassification r = classify(cases[i].text);
	std::string got = verdict_name(r.verdict);
	std::string want = verdict_name(cases[i].want);
	if ( got != want )
	    std::cerr << "entry [" << cases[i].text << "]: got " << got
		      << ", want " << want << " (" << r.diagnostic.message
		      << " at " << r.diagnostic.file << ':' << r.diagnostic.line
		      << ':' << r.diagnostic.column << ")\n";
	CHECK(got == want);
    }
}

} // namespace

TEST_CASE("complete entries run, with or without their final ';'") {
    const Case cases[] = {
	{ "1 + 2", V::Complete },
	{ "1 + 2;", V::Complete },
	{ "int x = 5", V::Complete },
	{ "int x = 5;", V::Complete },
	{ "int x = 5, y = 6", V::Complete },
	{ "int x", V::Complete },
	{ "int x = 5; x + 1", V::Complete },
	{ "int a[] = {1, 2, 3}", V::Complete },
	{ "\"abc\"", V::Complete },
	{ "int f(int a) { return a; }", V::Complete },
	{ "int f(int a)\n{\n  return a;\n}", V::Complete },
	{ "struct P { int x; };", V::Complete },
	{ "struct P { int x; } p", V::Complete },
	{ "enum E { A, B };", V::Complete },
	{ "enum E { A, B } e", V::Complete },
	{ "for (int i = 0; i < 3; i++) { }", V::Complete },
	{ "while (0) { }", V::Complete },
	{ "do { } while (0);", V::Complete },
	{ "template <class T> T id(T t) { return t; }", V::Complete },
	{ "#include <stdio.h>", V::Complete },
	{ "", V::Complete },
	{ "// a comment", V::Complete },
	{ "/* a comment */", V::Complete },
	{ "#define TWO 2", V::Complete },
	{ "#if 1\nint x = 1;\n#endif", V::Complete },
	{ "#define M(x) \\\n  ((x) + 1)", V::Complete },
	{ "\"a (b\"", V::Complete },
	{ "'('", V::Complete },
    };
    check_corpus(cases, sizeof(cases) / sizeof(cases[0]));
}

TEST_CASE("a finished if with no else is complete but extendable") {
    const Case cases[] = {
	{ "if (1) { }", V::CompleteExtendable },
	{ "if (1) 2", V::CompleteExtendable },
	{ "if (1) 2;", V::CompleteExtendable },
	{ "if (1) { } else if (0) { }", V::CompleteExtendable },
	{ "if (1) if (0) { }", V::CompleteExtendable },
	{ "if (1) { } else { }", V::Complete },
	{ "if (1) { } int y = 2", V::Complete },
    };
    check_corpus(cases, sizeof(cases) / sizeof(cases[0]));
}

TEST_CASE("incomplete entries keep reading") {
    const Case cases[] = {
	// stage 2: the first error is at the end of the entry
	{ "1 +", V::Incomplete },
	{ "int x =", V::Incomplete },
	{ "if (1)", V::Incomplete },
	{ "if (1) { } else", V::Incomplete },
	{ "while (1)", V::Incomplete },
	{ "for (;;)", V::Incomplete },
	{ "do { }", V::Incomplete },
	{ "do { } while (0)", V::Incomplete },
	{ "template <class T>", V::Incomplete },
	{ "int f(int a)", V::Incomplete },
	{ "struct P { int x; }", V::Incomplete },
	{ "enum E { A, B }", V::Incomplete },
	{ "typedef int T", V::Incomplete },
	{ "return", V::Incomplete },
	// stage 1b: an open delimiter
	{ "int a[] = {1,", V::Incomplete },
	{ "(1 +", V::Incomplete },
	{ "{", V::Incomplete },
	{ "int main() {", V::Incomplete },
	{ "int f(int a,", V::Incomplete },
	// stage 1a: the input ended inside a construct
	{ "/* a comment", V::Incomplete },
	{ "#if 1", V::Incomplete },
	{ "#define M(x) \\", V::Incomplete },
    };
    check_corpus(cases, sizeof(cases) / sizeof(cases[0]));
}

TEST_CASE("invalid entries fail where the grammar does") {
    const Case cases[] = {
	{ "int x = 5 5", V::Invalid },
	{ "int x = 3 4 +", V::Invalid },
	{ "1 2", V::Invalid },
	{ "y +", V::Invalid },		// madc resolves names at parse time
	{ "1 + 2)", V::Invalid },
	{ ")", V::Invalid },
	{ "(]", V::Invalid },
	{ "[(])", V::Invalid },
	{ "1) + (", V::Invalid },
	{ "\"abc", V::Invalid },		// a C string cannot continue on the next line
	{ "'a", V::Invalid },
	{ "int x; { x = 3 }", V::Invalid },
	{ "else", V::Invalid },
	{ "int int", V::Invalid },
	{ "enum E { A, B } return", V::Invalid },
	{ "struct S { enum { A } };", V::Invalid },
	{ "#include \"no_such_header_for_the_classifier.h\"", V::Invalid },
    };
    check_corpus(cases, sizeof(cases) / sizeof(cases[0]));
}

TEST_CASE("an expression entry without its ';' shows its value (D10)") {
    CHECK(classify("1 + 2").shows_value);
    CHECK(classify("int x = 5").shows_value);
    CHECK_FALSE(classify("1 + 2;").shows_value);
    CHECK_FALSE(classify("int x = 5;").shows_value);
    CHECK_FALSE(classify("int f(int a) { return a; }").shows_value);
}

TEST_CASE("an invalid entry's diagnostic is the parser's own, at its token") {
    ::Program::EntryClassification r = classify("int x = 5 5");
    CHECK(r.verdict == V::Invalid);
    CHECK(r.diagnostic.message == "expected ',' or ';' before numeric constant");
    CHECK(r.diagnostic.line == 1);
    CHECK(r.diagnostic.column == 11);
}

TEST_CASE("the lexer reports WHY it refused: the input's end is a cause") {
    // File mode too — the cause is a fact about the refusal, not a REPL rule.
    DiagnosticRenderMute mute;
    ::Program comment;
    CHECK(!comment.tokenize_buffer("int x; /* open", "<file>"));
    REQUIRE(!comment.diagnostics.empty());
    CHECK(comment.diagnostics[0].cause == ::Program::DiagnosticCause::end_of_input);

    ::Program group;
    CHECK(!group.tokenize_buffer("#ifdef X\nint x;\n", "<file>"));
    REQUIRE(!group.diagnostics.empty());
    CHECK(group.diagnostics[0].cause == ::Program::DiagnosticCause::end_of_input);

    ::Program literal;
    CHECK(!literal.tokenize_buffer("const char *s = \"open;\n", "<file>"));
    REQUIRE(!literal.diagnostics.empty());
    CHECK(literal.diagnostics[0].cause == ::Program::DiagnosticCause::none);
}

TEST_CASE("file parsing is untouched by the interactive mode") {
    // The mode is off by default: no end-of-entry token, and a statement
    // without its `;` is gcc's error, exactly as before.
    ::Program p;
    CHECK(p.parse_mode == ::Program::ParseMode::TranslationUnit);
    TokenProgram *tp = p.tokenize_buffer("int x = 5\n", "<file>");
    REQUIRE(tp);
    CHECK(!p.entry_end_token);
    {
	DiagnosticRenderMute mute;
	p.parse(tp);
    }
    REQUIRE(!p.diagnostics.empty());
    CHECK(p.diagnostics[0].message.find("expected") != std::string::npos);
    CHECK(p.diagnostics[0].cause == ::Program::DiagnosticCause::none);

    // A trailing line splice ends a FILE with a warning-free continuation
    // (gcc accepts it); only an interactive entry reads it as unfinished.
    ::Program splice;
    DiagnosticRenderMute mute;
    CHECK(splice.tokenize_buffer("int x;\n#define M 1 \\\n", "<file>"));
}
