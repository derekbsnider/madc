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

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <queue>
#include <stack>
#include <string>
#include <vector>
#include <unistd.h>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madcdis/hub.h"
#include "madcdis/verbs.h"	// mutation_context: an edit's refresh
#include "madc_lang.h"
#include "ns_common.h"		// madc_project_open / madc_parse_*: a project TU's handle
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

TEST_CASE("document_set_standard: the document parses under the standard it names")
{
    world w;
    // `class` names a class in C++ and is an identifier in C (gcc -std=c17
    // and g++ -std=c++17 on this line): the C kind's own standard refuses
    // the text, the C++ standard the document names takes it.
    entity_id e = open_doc(w, "class K { public: int v; };\n");
    std::vector<lang::diagnostic> diags;
    REQUIRE(lang::diagnostics(w, e, diags));
    CHECK_FALSE(diags.empty());
    REQUIRE(lang::document_set_standard(w, e, madc::fkCPP17));
    REQUIRE(lang::diagnostics(w, e, diags));		// reopened under it
    CHECK(diags.empty());
    int64_t h = lang::document_parsed(w, e);
    CHECK(h != 0);
    REQUIRE(lang::document_set_standard(w, e, madc::fkCPP17));	// unchanged: the handle stays
    CHECK(lang::document_handle(w, e) == h);
    REQUIRE(lang::document_set_standard(w, e, 0));	// back to the kind's
    REQUIRE(lang::diagnostics(w, e, diags));
    CHECK_FALSE(diags.empty());
    CHECK_FALSE(lang::document_set_standard(w, (entity_id)9999, 0));
    CHECK(lang::document_parsed(w, (entity_id)9999) == 0);

    // A Save As to a C++ name: the kind moves, and the handle opened as the
    // C unit reopens as the C++ one.
    REQUIRE(madc::hub::mutation_context(w).doc_set_path(e, "a.cpp", madc::fkCPP17));
    CHECK(lang::document_parsed(w, e) != 0);		// never parsing: the C unit's, as left
    REQUIRE(lang::diagnostics(w, e, diags));
    CHECK(diags.empty());

    // The world is going: the document's parse closes with it.
    int64_t last = lang::document_parsed(w, e);
    REQUIRE(last != 0);
    lang::close_world_parses(w);
    CHECK(lang::document_parsed(w, e) == 0);
    CHECK_FALSE(madc_parse_close(last));			// already closed
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

static const char *CALLS =
    "int twice(int x)\n"				// 1
    "{\n"						// 2
    "    return x * 2;\n"				// 3
    "}\n"						// 4
    "int main(void)\n"					// 5
    "{\n"						// 6
    "    return twice(twice(1)) + twice(3);\n"		// 7
    "}\n";						// 8

static std::vector<int64_t> columns_on(const std::vector<lang::range> &rs,
				       int64_t line)
{
    std::vector<int64_t> cols;
    for ( size_t i = 0; i < rs.size(); ++i )
	if ( rs[i].from.line == line )
	    cols.push_back(rs[i].from.column);
    std::sort(cols.begin(), cols.end());
    return cols;
}

TEST_CASE("definition: the word under a point, or just before it, to its name")
{
    world w;
    entity_id e = open_doc(w, CALLS);
    std::vector<lang::range> rs;
    REQUIRE(lang::definition(w, e, at(7, 13), rs));	// inside `twice`
    REQUIRE(rs.size() == 1u);
    CHECK(rs[0].from.line == 1);		// clangd: 1:4-1:9, the name
    CHECK(rs[0].from.column == 4);
    CHECK(covered(w, e, rs[0]) == "twice");
    REQUIRE(lang::definition(w, e, at(7, 16), rs));	// the caret just past it
    CHECK(rs.size() == 1u);
    REQUIRE(lang::definition(w, e, at(7, 4), rs));	// `return`: nothing defines it
    CHECK(rs.empty());
    REQUIRE(lang::definition(w, e, at(7, 26), rs));	// a space after `)`: no word
    CHECK(rs.empty());
}

TEST_CASE("references: each use is the NAME at its site, nested calls apart")
{
    world w;
    entity_id e = open_doc(w, CALLS);
    std::vector<lang::range> rs;
    REQUIRE(lang::references(w, e, at(1, 6), false, rs));	// on the definition
    REQUIRE(rs.size() == 3u);
    std::vector<int64_t> cols = columns_on(rs, 7);
    REQUIRE(cols.size() == 3u);
    CHECK(cols[0] == 11);
    CHECK(cols[1] == 17);		// the inner call's own name
    CHECK(cols[2] == 29);
    for ( size_t i = 0; i < rs.size(); ++i )
	CHECK(covered(w, e, rs[i]) == "twice");

    REQUIRE(lang::references(w, e, at(7, 30), true, rs));	// from a use
    REQUIRE(rs.size() == 4u);
    CHECK(rs[3].from.line == 1);		// the declaration's name, last
    CHECK(rs[3].from.column == 4);
    CHECK(covered(w, e, rs[3]) == "twice");

    REQUIRE(lang::references(w, e, at(3, 6), false, rs));	// `return`: no definition
    CHECK(rs.empty());
}

TEST_CASE("hover: the definition over the word, else the enclosing one")
{
    world w;
    entity_id e = open_doc(w, CALLS);
    lang::hover_info h;
    REQUIRE(lang::hover(w, e, at(7, 13), h));
    CHECK(h.text == "Function twice \xE2\x80\x94 defined on line 1");
    CHECK(h.has_range);
    CHECK(covered(w, e, h.at) == "twice");
    CHECK(h.at.from.column == 11);

    REQUIRE(lang::hover(w, e, at(3, 4), h));		// `return` inside twice
    CHECK(h.text == "in twice");
    CHECK_FALSE(h.has_range);

    entity_id two = open_doc(w, TWO_FUNCS);
    CHECK_FALSE(lang::hover(w, two, at(2, 0), h));	// between definitions
}

TEST_CASE("a document's parse searches the include directories of its opener")
{
    // gcc -fsyntax-only -I DIR: clean; without -I DIR: lang_inc.h not found.
    char tmpl[] = "/tmp/madc_lang_XXXXXX";
    char *made = mkdtemp(tmpl);
    REQUIRE(made != NULL);
    const std::string dir = made;
    const std::string header = dir + "/lang_inc.h";
    {
	std::ofstream h(header.c_str());
	h << "#define LANG_INC_VALUE 7\n";
    }
    const char *text =
	"#include \"lang_inc.h\"\nint f(void) { return LANG_INC_VALUE; }\n";

    world w;
    std::vector<lang::diagnostic> ds;
    entity_id bare = open_doc(w, text, madc::fkC17, "/nowhere/bare.c");
    REQUIRE(lang::diagnostics(w, bare, ds));
    CHECK_FALSE(ds.empty());			// no -I: the header is not found

    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    prog->add_include_dir(dir);
    prog->push_runtime_scope();
    entity_id found = open_doc(w, text, madc::fkC17, "/nowhere/found.c");
    REQUIRE(lang::diagnostics(w, found, ds));
    CHECK(ds.empty());				// the opener's -I DIR is searched
    prog->pop_runtime_scope();

    std::remove(header.c_str());
    rmdir(dir.c_str());
}

// A project TU's handle parses under its manifest entry's options — at open
// AND at every refresh (the --project build's rule). ORACLE: gcc
// -fsyntax-only -DLANG_TU_VALUE=7 tu.c: clean; without the -D: the #error.
TEST_CASE("a project TU's handle keeps its manifest options across a refresh")
{
    char tmpl[] = "/tmp/madc_lang_XXXXXX";
    char *made = mkdtemp(tmpl);
    REQUIRE(made != NULL);
    const std::string dir = made;
    const std::string tu = dir + "/tu.c", manifest = dir + "/app.prj.json";
    std::string text = "#if LANG_TU_VALUE != 7\n#error no define\n#endif\n"
	"int f(void) { return LANG_TU_VALUE; }\n";
    {
	std::ofstream f(tu.c_str());
	f << text;
	std::ofstream m(manifest.c_str());
	m << "{\"entry\": \"f\", \"tus\": [{\"file\": \"tu.c\","
	     " \"defines\": [\"LANG_TU_VALUE=7\"]}]}\n";
    }
    std::string path = manifest;
    const int64_t project = madc_project_open(&path);
    REQUIRE(project > 0);
    madc::value tus;
    madc_project_tus(&tus, project);
    REQUIRE(tus.is_array());
    const std::vector<madc::value> &rows = tus.as_array();
    REQUIRE(rows.size() == 1u);
    const int64_t h = rows[0].as_object().at("handle").as_integer();
    REQUIRE(h > 0);
    madc::value diags;
    madc_parse_diagnostics(&diags, h);
    REQUIRE(diags.is_array());
    CHECK(diags.as_array().empty());				// open: the -D applies
    REQUIRE(madc_parse_refresh(h, &text));
    madc_parse_diagnostics(&diags, h);
    REQUIRE(diags.is_array());
    CHECK(diags.as_array().empty());				// refresh: still applies
    madc_project_close(project);
    std::remove(tu.c_str());
    std::remove(manifest.c_str());
    rmdir(dir.c_str());
}

// A served project's document parses as its TU (madc --mcp --project): its
// manifest entry's -D reaches the live text's parse, at open and refresh.
// ORACLE: gcc -fsyntax-only -DLANG_TU_VALUE=7 tu.c: clean; without: #error.
TEST_CASE("a document set to parse as a project TU takes the TU's options")
{
    const char *text = "#if LANG_TU_VALUE != 7\n#error no define\n#endif\n"
	"int f(void) { return LANG_TU_VALUE; }\n";
    world w;
    entity_id doc = open_doc(w, text, madc::fkC17, "/nowhere/tu.c");
    std::vector<lang::diagnostic> ds;
    REQUIRE(lang::diagnostics(w, doc, ds));
    CHECK_FALSE(ds.empty());				// no -D: the #error fires
    ProjectTU tu;
    tu.file = "/nowhere/tu.c";
    tu.working_dir = "/nowhere";
    tu.defines.push_back("LANG_TU_VALUE=7");
    REQUIRE(lang::document_set_tu(w, doc, tu));
    REQUIRE(lang::diagnostics(w, doc, ds));
    CHECK(ds.empty());					// the TU's -D applies
    madc::hub::mutation_context mc(w);
    REQUIRE(mc.text_replace(doc, 0, 0, "\n"));
    REQUIRE(lang::diagnostics(w, doc, ds));
    CHECK(ds.empty());					// and survives a refresh
}
