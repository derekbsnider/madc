// madcide_markdown.inc — the ONE Markdown renderer of madcide (plan
// docs/plans/2026-10-03-chthonia-windows-macos.md §7d): a markdown:: tree
// (the madcmark module, <ns_markdown>) as a list of ROWS, one text line each,
// for the panes that show Markdown — Help's topics (madcide_help.inc) and the
// Markdown Preview of a buffer (madcide_preview.inc).
//
// A row is {content, line} plus, where it applies, `target` (a link's URL: a
// link is a row of its own) and `heading` (a heading's level). `line` is the
// source line the row came from (1-based; cmark's source positions): Help
// ignores it, the Preview moves the editor to it. Blocks read as text: a
// heading or a paragraph is one row, a list item hangs under its bullet (•,
// a number, a task box), a code block keeps its lines indented four columns,
// a block quote leads with a bar, a table pads its cells to their columns
// with a rule under the header, a thematic break is a rule. Inline HTML and
// HTML blocks show nothing.
//
// Thread contract: pure functions of their tree; the session's thread.

// A node's inline text, appended to `out`: its leaves' text, a line break a
// space; inline HTML is left out.
void md_inline(var &out, var &n)
{
    var kids;
    keyed_get(kids, n, "children");
    if ( kids.is_null() )
	return;
    for ( var c : kids )
    {
	if ( c["kind"] == markdown::node_kind::text || c["kind"] == markdown::node_kind::code )
	    out = format("{}{}", out, c["text"]);
	else if ( c["kind"] == markdown::node_kind::softbreak
	       || c["kind"] == markdown::node_kind::linebreak )
	    out = format("{} ", out);
	else if ( !(c["kind"] == markdown::node_kind::html_inline) )
	    md_inline(out, c);
    }
}

// The links under a node, in order: {text, url, line}.
void md_links(var &out, var &n)
{
    var kids;
    keyed_get(kids, n, "children");
    if ( kids.is_null() )
	return;
    for ( var c : kids )
    {
	if ( c["kind"] == markdown::node_kind::link )
	{
	    var t = "";
	    md_inline(t, c);
	    out[] = { "text": t, "url": c["url"], "line": c["line"] };
	}
	else
	    md_links(out, c);
    }
}

// The paragraph's one link when it holds nothing else (a contents line).
bool md_sole_link(var &out, var &p)
{
    var kids;
    keyed_get(kids, p, "children");
    if ( kids.is_null() || php::count(kids) != 1
      || !(kids[0]["kind"] == markdown::node_kind::link) )
	return false;
    out = kids[0];
    return true;
}

// A table's rows, each cell padded to its column's width, the columns
// joined by a bar; a rule under the header row.
void md_table_rows(var &rows, var &t, const char *indent0)
{
    var indent = indent0;	// ring discipline: own the text first
    var cells = {};
    var widths = {};
    for ( var r : t["children"] )
    {
	var line = {};
	long c = 0;
	for ( var cell : r["children"] )
	{
	    var txt = "";
	    md_inline(txt, cell);
	    line[] = txt;
	    long n = strlen(txt.c_str());
	    if ( c >= php::count(widths) )
		widths[] = n;
	    else if ( n > widths[c].as_integer() )
		widths[c] = n;
	    c = c + 1;
	}
	cells[] = line;
    }
    long ri = 0;
    for ( var r : t["children"] )
    {
	var text = format("{}", indent);
	long c = 0;
	for ( var txt : cells[ri] )
	{
	    if ( c > 0 )
		text = format("{} │ ", text);
	    text = format("{}{}", text, php::str_pad(txt, widths[c].as_integer(), " "));
	    c = c + 1;
	}
	rows[] = { "content": php::rtrim(text), "line": r["line"] };
	if ( r["header"].as_boolean() )
	{
	    long width = 0;
	    for ( var wd : widths )
		width = width + wd.as_integer() + 3;
	    rows[] = { "content": format("{}{}", indent, php::str_repeat("─", width > 3 ? width - 3 : 1)),
		       "line": r["line"] };
	}
	ri = ri + 1;
    }
}

void md_blocks_rows(var &rows, var &blocks, const char *lead0, const char *indent0);

// One block's rows: `lead` before its first row, `indent` before the rest (a
// list item's bullet and its hanging indent, a quote's bar).
void md_block_rows(var &rows, var &b, const char *lead0, const char *indent0)
{
    var lead = lead0;		// ring discipline: own the text first
    var indent = indent0;
    if ( b["kind"] == markdown::node_kind::heading || b["kind"] == markdown::node_kind::paragraph )
    {
	var link;
	if ( b["kind"] == markdown::node_kind::paragraph && md_sole_link(link, b) )
	{
	    var lt = "";
	    md_inline(lt, link);
	    rows[] = { "content": format("{}{}", lead, lt), "target": link["url"],
		       "line": b["line"] };
	    return;
	}
	var text = "";
	md_inline(text, b);
	var row = { "content": format("{}{}", lead, text), "line": b["line"] };
	if ( b["kind"] == markdown::node_kind::heading )
	    row["heading"] = b["level"];
	rows[] = row;
	var links = {};
	md_links(links, b);
	for ( var l : links )
	    rows[] = { "content": format("{}→ {}", indent, l["text"]), "target": l["url"],
		       "line": l["line"] };
    }
    else if ( b["kind"] == markdown::node_kind::list )
    {
	bool ordered = b["list"] == markdown::list_kind::ordered;
	long num = b["start"].as_integer();
	for ( var item : b["children"] )
	{
	    // The bullet, and the blank run as wide as it on screen that hangs
	    // the item's later rows under its text (• is one column).
	    var bullet = "• ";
	    long cols = 2;
	    if ( ordered )
	    {
		bullet = format("{}. ", num);
		cols = strlen(bullet.c_str());
	    }
	    if ( item["task"].as_boolean() )
	    {
		if ( item["checked"].as_boolean() )
		    bullet = format("{}[x] ", bullet);
		else
		    bullet = format("{}[ ] ", bullet);
		cols = cols + 4;
	    }
	    var first = format("{}{}", indent, bullet);
	    var hang = format("{}{}", indent, php::str_repeat(" ", cols));
	    var iblocks = item["children"];
	    md_blocks_rows(rows, iblocks, first.c_str(), hang.c_str());
	    num = num + 1;
	}
    }
    else if ( b["kind"] == markdown::node_kind::code_block )
    {
	// A fenced block's first line is its fence; its code starts below.
	var code = b["text"];
	array lines;
	php::explode(lines, "\n", php::rtrim(code));
	long ln = b["line"].as_integer();
	if ( b["fenced"].as_boolean() )
	    ln = ln + 1;
	for ( var l : lines )
	{
	    rows[] = { "content": format("{}    {}", indent, l), "line": ln };
	    ln = ln + 1;
	}
    }
    else if ( b["kind"] == markdown::node_kind::block_quote )
    {
	var ql = format("{}│ ", lead);
	var qi = format("{}│ ", indent);
	var qblocks = b["children"];
	md_blocks_rows(rows, qblocks, ql.c_str(), qi.c_str());
    }
    else if ( b["kind"] == markdown::node_kind::table )
	md_table_rows(rows, b, indent.c_str());
    else if ( b["kind"] == markdown::node_kind::thematic_break )
	rows[] = { "content": format("{}{}", indent, php::str_repeat("─", 24)), "line": b["line"] };
    // an HTML block shows nothing; a footnote's definition reads as its blocks
    else if ( b["kind"] == markdown::node_kind::footnote_definition )
    {
	var fblocks = b["children"];
	md_blocks_rows(rows, fblocks, lead.c_str(), indent.c_str());
    }
}

// A run of blocks: the first block's first row after `lead`, every other row
// after `indent`; a blank row between top-level blocks (lead "").
void md_blocks_rows(var &rows, var &blocks, const char *lead0, const char *indent0)
{
    var lead = lead0;		// ring discipline: own the text first
    var indent = indent0;
    if ( blocks.is_null() )
	return;
    bool first = true;
    for ( var b : blocks )
    {
	if ( !first && strlen(indent.c_str()) == 0 )
	    rows[] = { "content": "" };
	if ( first )
	    md_block_rows(rows, b, lead.c_str(), indent.c_str());
	else
	    md_block_rows(rows, b, indent.c_str(), indent.c_str());
	first = false;
    }
}

// A Markdown text's rows: its document's blocks through the renderer above.
// False when the madcmark module is absent (the caller says so).
bool md_text_rows(var &rows, const char *text0)
{
    var text = text0;		// ring discipline: own the text first
    if ( !markdown::available() )
	return false;
    var doc;
    markdown::parse(doc, text.c_str());
    var blocks = doc["children"];
    md_blocks_rows(rows, blocks, "", "");
    return true;
}
