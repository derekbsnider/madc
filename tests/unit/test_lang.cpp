// Unit battery for madc::lang — the language services over a document
// (split arc 1C.11; design docs/plans/2026-10-10-language-services-design.md).
// ORACLE: clangd 18's LSP answers for the same C text, positionEncoding
// utf-8 (recorded: definition 1:4-1:9 — the NAME; references
// 7:11-7:16, 7:17-7:22, 7:29-7:34, the declaration 1:4-1:9; hover 7:11-7:16;
// documentSymbol f 1:0-1:27 / 1:6-1:7, twice 3:0-6:1 / 3:4-3:9, as 1-based
// line:byte here) and gcc's diagnostic anchor (3:12, the undeclared
// identifier's first byte). Coordinates: a 1-based line, a 0-based byte
// column.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/hub.h"
#include "madc_lang.h"
#include "madc/bits/file_kinds"

using madc::hub::world;
using madc::hub::entity_id;
namespace lang = madc::lang;

static entity_id open_doc(world &w, const std::string &text,
			  int64_t kind = madc::fkC17,
			  const std::string &path = "a.c")
{
    entity_id e = w.create("document");
    REQUIRE(w.doc_attach(e, path, kind, text));
    return e;
}

static lang::point at(int64_t line, int64_t column)
{
    lang::point p;
    p.line = line;
    p.column = column;
    return p;
}

// The text a single-line range covers.
static std::string covered(const world &w, entity_id e, const lang::range &r)
{
    REQUIRE(r.from.line == r.to.line);
    size_t off = 0, len = 0;
    REQUIRE(w.text_of(e)->line_span((size_t)r.from.line, off, len));
    return w.text_of(e)->text().substr(off + (size_t)r.from.column,
				       (size_t)(r.to.column - r.from.column));
}

static const char *TWO_FUNCS =
    "float f(void) { return 1; }\n"	// 1
    "\n"				// 2
    "int twice(int x)\n"		// 3
    "{\n"				// 4
    "    return x * 2;\n"		// 5
    "}\n";				// 6

TEST_CASE("document_handle: one handle, re-parsed only when the version moves")
{
    world w;
    entity_id e = open_doc(w, "int main(void) { return 0; }\n");
    int64_t h = lang::document_handle(w, e);
    REQUIRE(h != 0);
    CHECK(w.doc_of(e)->parse_handle() == h);
    CHECK(w.doc_of(e)->parse_version() == w.text_version(e));
    CHECK(lang::document_handle(w, e) == h);

    REQUIRE(w.text_insert(e, 0, "int g;\n"));
    CHECK(w.doc_of(e)->parse_version() != w.text_version(e));
    CHECK(lang::document_handle(w, e) == h);		// refreshed in place
    CHECK(w.doc_of(e)->parse_version() == w.text_version(e));

    entity_id txt = open_doc(w, "just words\n", madc::fkTEXT, "notes.txt");
    CHECK(lang::document_handle(w, txt) == 0);		// not the compiler's
    CHECK(lang::document_handle(w, (entity_id)9999) == 0);
}

TEST_CASE("diagnostics: the range is the word at the anchor, in byte columns")
{
    world w;
    entity_id e = open_doc(w, "int main(void)\n{\n    return undefined_name;\n}\n");
    std::vector<lang::diagnostic> ds;
    REQUIRE(lang::diagnostics(w, e, ds));
    REQUIRE(!ds.empty());
    CHECK(ds[0].severity == madc::diag_severity::error);
    CHECK(ds[0].at.from.line == 3);
    CHECK(ds[0].at.from.column == 11);		// gcc: 3:12, 1-based
    CHECK(covered(w, e, ds[0].at) == "undefined_name");
    CHECK(ds[0].message.find("undefined_name") != std::string::npos);

    entity_id clean = open_doc(w, TWO_FUNCS);
    REQUIRE(lang::diagnostics(w, clean, ds));
    CHECK(ds.empty());
}

TEST_CASE("outline: each definition's lines and its NAME as the selection")
{
    world w;
    entity_id e = open_doc(w, TWO_FUNCS);
    std::vector<lang::symbol> syms;
    REQUIRE(lang::outline(w, e, syms));
    REQUIRE(syms.size() == 2u);

    CHECK(syms[0].name == "f");
    CHECK(syms[0].kind == lang::symbol_kind::function);
    CHECK(syms[0].extent.from.line == 1);
    CHECK(syms[0].extent.to.line == 1);
    CHECK(syms[0].extent.to.column == 27);		// clangd: 1:0-1:27
    CHECK(syms[0].selection.from.column == 6);	// not the `f` in `float`
    CHECK(covered(w, e, syms[0].selection) == "f");

    CHECK(syms[1].name == "twice");
    CHECK(syms[1].extent.from.line == 3);
    CHECK(syms[1].extent.from.column == 0);
    CHECK(syms[1].extent.to.line == 6);
    CHECK(syms[1].extent.to.column == 1);		// the closing brace's line, whole
    CHECK(covered(w, e, syms[1].selection) == "twice");
    CHECK(std::string(lang::symbol_kind_name(syms[1].kind)) == "function");
}

TEST_CASE("enclosing: the definition around a point, its first byte included")
{
    world w;
    entity_id e = open_doc(w, TWO_FUNCS);
    lang::symbol s;
    REQUIRE(lang::enclosing(w, e, at(5, 4), s));
    CHECK(s.name == "twice");
    REQUIRE(lang::enclosing(w, e, at(3, 0), s));	// the definition's first byte
    CHECK(s.name == "twice");
    REQUIRE(lang::enclosing(w, e, at(1, 0), s));
    CHECK(s.name == "f");
    CHECK_FALSE(lang::enclosing(w, e, at(2, 0), s));	// between definitions
}

TEST_CASE("spans: the parse's classes as enums, in byte columns")
{
    world w;
    entity_id e = open_doc(w, "int x = 42; // c\nint twice(int v) { return v * 2; }\n");
    std::vector<madc::highlight_span> sp;
    REQUIRE(lang::spans(w, e, sp));
    bool number = false, comment = false, function = false;
    for ( size_t i = 0; i < sp.size(); ++i )
    {
	if ( sp[i].line == 1 && sp[i].column == 8 && sp[i].length == 2 )
	    number = sp[i].cls == HighlightClass::hcNumber;
	if ( sp[i].line == 1 && sp[i].column == 12 )
	    comment = sp[i].cls == HighlightClass::hcComment;
	if ( sp[i].line == 2 && sp[i].column == 4 && sp[i].length == 5 )
	    function = sp[i].cls == HighlightClass::hcFunction;
    }
    CHECK(number);
    CHECK(comment);
    CHECK(function);

    entity_id txt = open_doc(w, "words\n", madc::fkTEXT, "n.txt");
    CHECK_FALSE(lang::spans(w, txt, sp));
    CHECK(sp.empty());
}

