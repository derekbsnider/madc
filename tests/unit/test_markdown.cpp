// test_markdown.cpp — madc::markdown_tree (the ONE cmark-gfm caller, the
// madcmark module) over a README-shaped sample: the tree's kinds, the
// kind-specific fields and the source positions. The oracle is cmark-gfm
// itself: a probe walking the same sample with CMARK_OPT_SOURCEPOS and the
// table, strikethrough, autolink and tasklist extensions (cmark_iter over
// every node, cmark_node_get_start_line / _column / end_line / end_column)
// printed the positions asserted below (cmark-gfm 0.29.0.gfm.6, the system
// library; unchanged at the pinned 0.29.0.gfm.13 the module links).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/markdown.h"

#include <string>
#include <vector>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

using madc::value;

namespace {

const char *sample =
	"# Title *one*\n"
	"\n"
	"Some **bold\n"
	"across** a line, `code`, and a [link](http://x.org \"t\").\n"
	"\n"
	"- [x] done\n"
	"- [ ] todo\n"
	"\n"
	"3. three\n"
	"4. four\n"
	"\n"
	"| a | b |\n"
	"|:--|--:|\n"
	"| *1* | 2 |\n"
	"\n"
	"```c\n"
	"int x;\n"
	"```\n"
	"> quote &amp; \\* escape\n"
	"\n"
	"~~gone~~\n";

const value &field(const value &n, const char *key)
{
	return n.as_object().at(key);
}

const value &child(const value &n, size_t i)
{
	return field(n, "children").as_array().at(i);
}

size_t count(const value &n)
{
	return field(n, "children").as_array().size();
}

markdown::node_kind kind(const value &n)
{
	return (markdown::node_kind)field(n, "kind").as_integer();
}

// line:col-end_line:end_col, the probe's spelling.
std::string span(const value &n)
{
	return std::to_string(field(n, "line").as_integer()) + ":"
	     + std::to_string(field(n, "col").as_integer()) + "-"
	     + std::to_string(field(n, "end_line").as_integer()) + ":"
	     + std::to_string(field(n, "end_col").as_integer());
}

std::string text(const value &n)
{
	return field(n, "text").as_string();
}

} // namespace

TEST_CASE("markdown_tree: the document's blocks, in order, with their positions")
{
	value doc = madc::markdown_tree(sample);
	CHECK(kind(doc) == markdown::node_kind::document);
	CHECK(span(doc) == "1:1-21:8");
	REQUIRE(count(doc) == 8);
	CHECK(kind(child(doc, 0)) == markdown::node_kind::heading);
	CHECK(kind(child(doc, 1)) == markdown::node_kind::paragraph);
	CHECK(kind(child(doc, 2)) == markdown::node_kind::list);
	CHECK(kind(child(doc, 3)) == markdown::node_kind::list);
	CHECK(kind(child(doc, 4)) == markdown::node_kind::table);
	CHECK(kind(child(doc, 5)) == markdown::node_kind::code_block);
	CHECK(kind(child(doc, 6)) == markdown::node_kind::block_quote);
	CHECK(kind(child(doc, 7)) == markdown::node_kind::paragraph);
}

TEST_CASE("markdown_tree: inlines — emphasis across a line break, code, a link")
{
	value doc = madc::markdown_tree(sample);
	const value &h = child(doc, 0);
	CHECK(field(h, "level").as_integer() == 1);
	CHECK(span(h) == "1:1-1:13");
	CHECK(text(child(h, 0)) == "Title ");
	CHECK(kind(child(h, 1)) == markdown::node_kind::emph);
	CHECK(span(child(h, 1)) == "1:9-1:13");		// the delimiters included

	const value &p = child(doc, 1);
	REQUIRE(count(p) == 7);
	const value &strong = child(p, 1);
	CHECK(kind(strong) == markdown::node_kind::strong);
	CHECK(span(strong) == "3:6-4:8");		// across the line break
	REQUIRE(count(strong) == 3);
	CHECK(text(child(strong, 0)) == "bold");
	CHECK(kind(child(strong, 1)) == markdown::node_kind::softbreak);
	CHECK(span(child(strong, 1)) == "0:0-0:0");	// a soft break has no position
	CHECK(text(child(strong, 2)) == "across");
	const value &code = child(p, 3);
	CHECK(kind(code) == markdown::node_kind::code);
	CHECK(text(code) == "code");
	CHECK(span(code) == "4:19-4:22");		// the content; the backticks are outside
	const value &link = child(p, 5);
	CHECK(kind(link) == markdown::node_kind::link);
	CHECK(field(link, "url").as_string() == "http://x.org");
	CHECK(field(link, "title").as_string() == "t");
	CHECK(span(link) == "4:32-4:55");
	CHECK(text(child(link, 0)) == "link");
}

TEST_CASE("markdown_tree: task lists and ordered lists")
{
	value doc = madc::markdown_tree(sample);
	const value &tasks = child(doc, 2);
	CHECK(field(tasks, "list").as_integer() == (int64_t)markdown::list_kind::bullet);
	CHECK(field(tasks, "tight").as_boolean());
	REQUIRE(count(tasks) == 2);
	CHECK(kind(child(tasks, 0)) == markdown::node_kind::item);
	CHECK(field(child(tasks, 0), "task").as_boolean());
	CHECK(field(child(tasks, 0), "checked").as_boolean());
	CHECK(field(child(tasks, 1), "task").as_boolean());
	CHECK_FALSE(field(child(tasks, 1), "checked").as_boolean());
	CHECK(span(tasks) == "6:1-8:0");		// a list ends at the next line, column 0

	const value &numbered = child(doc, 3);
	CHECK(field(numbered, "list").as_integer() == (int64_t)markdown::list_kind::ordered);
	CHECK(field(numbered, "start").as_integer() == 3);
	CHECK_FALSE(field(child(numbered, 0), "task").as_boolean());
	CHECK(text(child(child(child(numbered, 1), 0), 0)) == "four");
}

TEST_CASE("markdown_tree: a table's columns, header row and cells")
{
	value doc = madc::markdown_tree(sample);
	const value &t = child(doc, 4);
	CHECK(span(t) == "12:1-14:11");
	const std::vector<value> &al = field(t, "align").as_array();
	REQUIRE(al.size() == 2);
	CHECK(al[0].as_integer() == (int64_t)markdown::column_align::left);
	CHECK(al[1].as_integer() == (int64_t)markdown::column_align::right);
	REQUIRE(count(t) == 2);
	CHECK(kind(child(t, 0)) == markdown::node_kind::table_row);
	CHECK(field(child(t, 0), "header").as_boolean());
	CHECK_FALSE(field(child(t, 1), "header").as_boolean());
	const value &cell = child(child(t, 1), 0);
	CHECK(kind(cell) == markdown::node_kind::table_cell);
	CHECK(span(cell) == "14:2-14:6");
	CHECK(kind(child(cell, 0)) == markdown::node_kind::emph);
}

TEST_CASE("markdown_tree: a fenced block, a quote's decoded text, strikethrough")
{
	value doc = madc::markdown_tree(sample);
	const value &cb = child(doc, 5);
	CHECK(field(cb, "info").as_string() == "c");
	CHECK(field(cb, "fenced").as_boolean());
	CHECK(text(cb) == "int x;\n");
	CHECK(span(cb) == "16:1-18:3");

	const value &q = child(child(doc, 6), 0);
	CHECK(text(child(q, 0)) == "quote & * escape");	// entity and escape decoded, one node
	CHECK(span(child(q, 0)) == "19:3-19:23");

	const value &s = child(child(doc, 7), 0);
	CHECK(kind(s) == markdown::node_kind::strikethrough);
	CHECK(span(s) == "21:1-21:8");
	CHECK(text(child(s, 0)) == "gone");
}

TEST_CASE("markdown_tree: empty text is an empty document; autolinks are links")
{
	value doc = madc::markdown_tree("");
	CHECK(kind(doc) == markdown::node_kind::document);
	CHECK(count(doc) == 0);
	value al = madc::markdown_tree("see https://example.com now\n");
	const value &p = child(al, 0);
	REQUIRE(count(p) == 3);
	CHECK(kind(child(p, 1)) == markdown::node_kind::link);
	CHECK(field(child(p, 1), "url").as_string() == "https://example.com");
}
