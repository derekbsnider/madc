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

// ---- highlighting: a Markdown buffer's spans (plan §7d, consumer 2) --------
// The rows madcide_core.inc's spans_to_hspans converts for every buffer —
// {line, column, length, class}, column 0-based, one row per source line —
// from the tree's source positions (cmark's: 1-based lines and columns, the
// end column inclusive). The class is a theme word: a heading and strong
// text `keyword`, a code span and a code block `string`, a link and an image
// `type`, HTML and a thematic break `comment`, a list item's marker
// `number`. A styled node is one span and its children are not walked, so
// spans never overlap; an unstyled node's children are.

// The theme class of a node's span; "" = none (its children are walked).
const char *md_span_class(var &n)
{
    if ( n["kind"] == markdown::node_kind::heading || n["kind"] == markdown::node_kind::strong )
	return "keyword";
    if ( n["kind"] == markdown::node_kind::code || n["kind"] == markdown::node_kind::code_block )
	return "string";
    if ( n["kind"] == markdown::node_kind::link || n["kind"] == markdown::node_kind::image )
	return "type";
    if ( n["kind"] == markdown::node_kind::html_block || n["kind"] == markdown::node_kind::html_inline
      || n["kind"] == markdown::node_kind::thematic_break )
	return "comment";
    return "";
}

// One span row per source line of [l1:c1 .. l2:c2] (1-based, c2 inclusive)
// over the text's `lines`.
void md_range_spans(var &rows, array &lines, long l1, long c1, long l2, long c2,
		    const char *cls0)
{
    var cls = cls0;		// ring discipline: own the text first
    long nl = php::count(lines);
    for ( long l = l1; l <= l2; l = l + 1 )
    {
	if ( l < 1 || l > nl )
	    continue;
	var ln = lines[l - 1];
	long len = strlen(ln.c_str());
	long s = 0;
	if ( l == l1 )
	    s = c1 - 1;
	long e = len;
	if ( l == l2 && c2 < len )
	    e = c2;
	if ( e > s )
	    rows[] = { "line": l, "column": s, "length": e - s, "class": cls };
    }
}

// A code span's range covers its content only (cmark): widen it over the
// backtick runs just outside, on its one line.
void md_code_span(var &rows, array &lines, var &n)
{
    long l = n["line"].as_integer();
    if ( l < 1 || l > php::count(lines) || n["end_line"].as_integer() != l )
    {
	md_range_spans(rows, lines, l, n["col"].as_integer(), n["end_line"].as_integer(),
		       n["end_col"].as_integer(), "string");
	return;
    }
    var ln = lines[l - 1];
    const char *p = ln.c_str();
    long len = strlen(p);
    long s = n["col"].as_integer() - 1;
    long e = n["end_col"].as_integer();
    while ( s > 0 && p[s - 1] == '`' )
	s = s - 1;
    while ( e < len && p[e] == '`' )
	e = e + 1;
    if ( e > s )
	rows[] = { "line": l, "column": s, "length": e - s, "class": "string" };
}

// A list item's marker (`-`, `1.`, with a task box `- [ ]`): from the item's
// column to its first block's, on the item's first line.
void md_item_marker(var &rows, array &lines, var &item)
{
    long l = item["line"].as_integer();
    if ( l < 1 || l > php::count(lines) )
	return;
    var ln = lines[l - 1];
    const char *p = ln.c_str();
    long len = strlen(p);
    long s = item["col"].as_integer() - 1;
    long e = s + 1;
    var kids = item["children"];
    if ( !kids.is_null() && php::count(kids) > 0 && kids[0]["line"].as_integer() == l )
	e = kids[0]["col"].as_integer() - 1;
    if ( e > len )
	e = len;
    while ( e > s + 1 && p[e - 1] == ' ' )
	e = e - 1;
    if ( e > s )
	rows[] = { "line": l, "column": s, "length": e - s, "class": "number" };
}

void md_node_spans(var &rows, array &lines, var &n)
{
    var cls = md_span_class(n);
    if ( strlen(cls.c_str()) > 0 )
    {
	if ( n["kind"] == markdown::node_kind::code )
	    md_code_span(rows, lines, n);
	else
	    md_range_spans(rows, lines, n["line"].as_integer(), n["col"].as_integer(),
			   n["end_line"].as_integer(), n["end_col"].as_integer(), cls.c_str());
	return;
    }
    if ( n["kind"] == markdown::node_kind::item )
	md_item_marker(rows, lines, n);
    var kids;
    keyed_get(kids, n, "children");
    if ( kids.is_null() )
	return;
    for ( var c : kids )
	md_node_spans(rows, lines, c);
}

// A Markdown text's span rows. False when the madcmark module is absent.
bool md_spans(var &rows, const char *text0)
{
    var text = text0;		// ring discipline: own the text first
    if ( !markdown::available() )
	return false;
    var doc;
    markdown::parse(doc, text.c_str());
    array lines;
    php::explode(lines, "\n", text.c_str());
    md_node_spans(rows, lines, doc);
    return true;
}

// ---- the concealing lens: a Markdown text as it reads (plan §7d, consumer 3)
// The formatted view of a Markdown buffer: its text with the formatting
// characters CONCEALED, and the coordinate map back to the stored text
// (madcdis/doc_lens.h: copy segments {disp, stored, len}; a stored byte in no
// segment is concealed). What is concealed comes from the tree's source
// positions (cmark's: exact for blocks and for an inline's delimiters):
//  - an emphasis, strong, strikethrough, link or image: its range minus its
//    children's — `**`, `_`, `~~`, `[` and `](url "title")`, `![`, `<` `>`;
//  - a code span: the backtick runs just outside its content;
//  - a heading: the `#`s before its text (and closing ones, or a setext
//    underline, after it);
//  - a block quote: each line's leading `>` prefix;
//  - a fenced code block: its fence lines, with their newlines;
//  - a text run: an escape's backslash (`\*` reads `*`).
// An entity stays as written (showing `&` for `&amp;` would need synthetic
// display bytes, not a concealment). List markers, tables, HTML and rules
// stay: they read as text.

// Each line's starting byte offset (line 1 at index 0).
void text_line_starts(var &out, var &text)
{
    out = {};
    out[] = 0;
    const char *p = text.c_str();
    long n = strlen(p);
    for ( long i = 0; i < n; i = i + 1 )
	if ( p[i] == '\n' )
	    out[] = i + 1;
}

// A 1-based (line, column) as a byte offset; -1 off the text.
long md_off(var &starts, long line, long col)
{
    if ( line < 1 || line > php::count(starts) )
	return -1;
    return starts[line - 1].as_integer() + col - 1;
}

// One concealed range [s, e) — ignored when empty or off the text. A range
// is ONE integer, s * 2^32 + e, so the ranges sort as numbers (php::sort).
void md_cut(var &cut, long s, long e)
{
    if ( s >= 0 && e > s )
	cut[] = s * 4294967296 + e;
}

bool md_ascii_punct(long c)
{
    const char *pc = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    for ( long i = 0; pc[i] != 0; i = i + 1 )
	if ( pc[i] == c )
	    return true;
    return false;
}

// One construct a put must keep whole or drop whole: its range [s, e), its
// visible text [cs, ce) and whether its delimiters are LINE structure (a
// heading's `#`s: a deletion that joins the line takes them) rather than a
// pair around inline text (kept unless the text all goes).
void md_wrap(var &wraps, long s, long e, long cs, long ce, bool line)
{
    if ( s >= 0 && e > s )
	wraps[] = { "s": s, "e": e, "cs": cs, "ce": ce, "line": line };
}

void md_conceal_node(var &cut, var &wraps, var &starts, var &text, var &n)
{
    long n_len = strlen(text.c_str());
    long ns = md_off(starts, n["line"].as_integer(), n["col"].as_integer());
    long ne = md_off(starts, n["end_line"].as_integer(), n["end_col"].as_integer()) + 1;
    var kids;
    keyed_get(kids, n, "children");
    // the children's extent: the first and the last child with a position
    // (a soft break reports none)
    long cs = -1;
    long ce = -1;
    if ( !kids.is_null() )
	for ( var c : kids )
	{
	    if ( c["line"].as_integer() < 1 )
		continue;
	    long s = md_off(starts, c["line"].as_integer(), c["col"].as_integer());
	    long e = md_off(starts, c["end_line"].as_integer(), c["end_col"].as_integer()) + 1;
	    if ( cs < 0 )
		cs = s;
	    ce = e;
	}
    var k = n["kind"];
    if ( k == markdown::node_kind::emph || k == markdown::node_kind::strong
      || k == markdown::node_kind::strikethrough || k == markdown::node_kind::link
      || k == markdown::node_kind::image || k == markdown::node_kind::heading )
    {
	if ( cs >= 0 && ns >= 0 )
	{
	    md_cut(cut, ns, cs);
	    md_cut(cut, ce, ne);
	    md_wrap(wraps, ns, ne, cs, ce, k == markdown::node_kind::heading);
	}
    }
    else if ( k == markdown::node_kind::code )
    {
	// the content's range; the backtick runs sit just outside it
	const char *p = text.c_str();
	long s = ns;
	long e = ne;
	while ( s > 0 && p[s - 1] == '`' )
	    s = s - 1;
	while ( e < n_len && p[e] == '`' )
	    e = e + 1;
	md_cut(cut, s, ns);
	md_cut(cut, ne, e);
	md_wrap(wraps, s, e, ns, ne, false);
	return;
    }
    else if ( k == markdown::node_kind::block_quote )
    {
	// every line's quote prefix — its `>`s, each with one space — from the
	// quote's column on its first line (a list marker before it stays) and
	// past the indentation on the rest (which stays); a lazy line has none
	const char *p = text.c_str();
	long first = n["line"].as_integer();
	long last = n["end_line"].as_integer();
	for ( long l = first; l <= last; l = l + 1 )
	{
	    long o = md_off(starts, l, l == first ? n["col"].as_integer() : 1);
	    if ( o < 0 )
		break;
	    long q = o;
	    long from = -1;
	    long end = q;
	    while ( q < n_len )
	    {
		while ( q < n_len && p[q] == ' ' )
		    q = q + 1;
		if ( q >= n_len || p[q] != '>' )
		    break;
		if ( from < 0 )
		    from = q;
		q = q + 1;
		if ( q < n_len && p[q] == ' ' )
		    q = q + 1;
		end = q;
	    }
	    if ( from >= 0 )
		md_cut(cut, from, end);
	}
    }
    else if ( k == markdown::node_kind::code_block )
    {
	if ( !n["fenced"].as_boolean() )
	    return;
	long first = n["line"].as_integer();
	long last = n["end_line"].as_integer();
	long nl = php::count(starts);
	// the opening fence line, its newline with it: from the block's column,
	// back over the indentation before it (a list marker stays)
	const char *p = text.c_str();
	long o1 = md_off(starts, first, n["col"].as_integer());
	long lo = md_off(starts, first, 1);
	while ( o1 > lo && p[o1 - 1] == ' ' )
	    o1 = o1 - 1;
	long e1 = first < nl ? starts[first].as_integer() : n_len;
	md_cut(cut, o1, e1);
	// the opening fence: its character and its length
	long f = o1;
	while ( f < e1 && p[f] == ' ' )
	    f = f + 1;
	long fch = f < n_len ? (long)p[f] : 0;
	long flen = 0;
	while ( f + flen < e1 && p[f + flen] == fch )
	    flen = flen + 1;
	// the closing fence line when there is one (an unclosed block runs on
	// to its container's end): container prefix, a run of the opening
	// fence's character at least as long, spaces — the whole line goes
	if ( last > first && flen >= 3 )
	{
	    long o2 = md_off(starts, last, 1);
	    long e2 = last < nl ? starts[last].as_integer() : n_len;
	    long q = o2;
	    while ( q < e2 && (p[q] == ' ' || p[q] == '>') )
		q = q + 1;
	    long run = 0;
	    while ( q < e2 && p[q] == fch )
	    {
		q = q + 1;
		run = run + 1;
	    }
	    while ( q < e2 && (p[q] == ' ' || p[q] == '\n') )
		q = q + 1;
	    if ( run >= flen && q == e2 )
		md_cut(cut, o2, e2);
	}
	return;
    }
    else if ( k == markdown::node_kind::text )
    {
	// an escape's backslash, inside the text run's source
	const char *p = text.c_str();
	long i = ns;
	while ( i >= 0 && i + 1 < ne && i + 1 < n_len )
	{
	    if ( p[i] == '\\' && md_ascii_punct((long)(unsigned char)p[i + 1]) )
	    {
		md_cut(cut, i, i + 1);
		md_wrap(wraps, i, i + 2, i + 1, i + 2, false);	// `\*`: one construct
		i = i + 2;
	    }
	    else
		i = i + 1;
	}
	return;
    }
    if ( !kids.is_null() )
	for ( var c : kids )
	    md_conceal_node(cut, wraps, starts, text, c);
}

// A text's concealed ranges (`cut`: s * 2^32 + e keys, in stored order —
// they may overlap) and its constructs (`wraps`, md_wrap's rows). False when
// the madcmark module is absent.
bool md_concealed(var &cut, var &wraps, var &text)
{
    cut = {};
    wraps = {};
    if ( !markdown::available() )
	return false;
    var doc;
    markdown::parse(doc, text.c_str());
    var starts;
    text_line_starts(starts, text);
    md_conceal_node(cut, wraps, starts, text, doc);
    php::sort(cut);
    return true;
}

// The display text and its map: the stored text's visible runs, in order.
// False when the madcmark module is absent.
bool md_conceal(var &display, var &map, const char *text0)
{
    var text = text0;		// ring discipline: own the text first
    var cut;
    var wraps;
    if ( !md_concealed(cut, wraps, text) )
	return false;
    // overlapping ranges merge as the walk below skips them
    long nc = php::count(cut);
    long n = strlen(text.c_str());
    var segs = {};
    var shown = "";
    long pos = 0;
    long disp = 0;
    for ( long i = 0; i <= nc; i = i + 1 )
    {
	long s = n;
	long e = n;
	if ( i < nc )
	{
	    long key = cut[i].as_integer();
	    s = key / 4294967296;
	    e = key % 4294967296;
	}
	if ( s > pos )
	{
	    segs[] = { "disp": disp, "stored": pos, "len": s - pos };
	    shown = format("{}{}", shown, perl::substr(text, pos, s - pos));
	    disp = disp + s - pos;
	}
	if ( e > pos )
	    pos = e;
    }
    display = shown;
    map = segs;
    return true;
}

// ---- the concealing lens's put (plan §7d, slice 2): an edit through the
// formatted view runs on the stored text; this repairs what it deleted.
// The edit replaced before[p, e) with `ins`. A deletion takes only the
// VISIBLE bytes of its range and the line structure inside it (a quote's
// `>`s, a heading's `#`s, fence lines: the lines it joins); the concealed
// delimiters of an inline construct stay paired — unless the deletion took
// ALL of that construct's visible text, when the construct goes whole
// (`**b**` with its `b` deleted leaves nothing, never a bare `****`).
// Out: the repaired text and the caret just after `ins` in it. False =
// nothing to repair (the edit's own text stands).
bool md_put_repair(var &out, long &caret, var &before, long p, long e, var &ins)
{
    if ( e <= p )
	return false;
    var cut;
    var wraps;
    if ( !md_concealed(cut, wraps, before) )
	return false;
    long nw = php::count(wraps);
    // the constructs that go whole: each one whose visible text lies inside
    // the deletion grown by the constructs already going (nested ones go
    // with their parent: `***a***` loses both)
    var gone = {};
    for ( long i = 0; i < nw; i = i + 1 )
	gone[] = false;
    long lo = p;
    long hi = e;
    bool grew = true;
    while ( grew )
    {
	grew = false;
	for ( long i = 0; i < nw; i = i + 1 )
	{
	    if ( gone[i].as_boolean() )
		continue;
	    var wr = wraps[i];
	    long cs = wr["cs"].as_integer();
	    long ce = wr["ce"].as_integer();
	    if ( ce <= cs || cs < lo || ce > hi )
		continue;
	    gone[i] = true;
	    grew = true;
	    if ( wr["s"].as_integer() < lo )
		lo = wr["s"].as_integer();
	    if ( wr["e"].as_integer() > hi )
		hi = wr["e"].as_integer();
	}
    }
    // what [lo, hi) keeps: the inline delimiters inside [p, e) of the
    // constructs that stay (a gone construct's bytes all go; outside
    // [p, e), [lo, hi) holds only gone constructs)
    var kept = "";
    bool repaired = lo < p || hi > e;
    for ( long i = p; i < e; i = i + 1 )
    {
	bool keep = false;
	for ( long j = 0; j < nw && !keep; j = j + 1 )
	{
	    if ( gone[j].as_boolean() )
		continue;
	    var wr = wraps[j];
	    if ( wr["line"].as_boolean() )
		continue;
	    long ws = wr["s"].as_integer();
	    long we = wr["e"].as_integer();
	    long cs = wr["cs"].as_integer();
	    long ce = wr["ce"].as_integer();
	    if ( (i >= ws && i < cs) || (i >= ce && i < we) )
		keep = true;
	}
	if ( keep )
	{
	    kept = format("{}{}", kept, perl::substr(before, i, 1));
	    repaired = true;
	}
    }
    if ( !repaired )
	return false;
    long n = strlen(before.c_str());
    out = format("{}{}{}{}", perl::substr(before, 0, lo), ins, kept,
		 perl::substr(before, hi, n - hi));
    caret = lo + strlen(ins.c_str());
    return true;
}
