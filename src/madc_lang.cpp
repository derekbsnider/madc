//////////////////////////////////////////////////////////////////////////
//									//
// madc::lang — the language services over a document (split arc	//
// 1C.11; include/madc_lang.h, design docs/plans/2026-10-10-language-	//
// services-design.md). Each answer composes the parse handle's reads	//
// (the madc_parse_* / madc_graph_* bridges, ns_common.h) and converts	//
// their rows ONCE into the one coordinate: a 1-based line and a	//
// 0-based byte column. The compiler's 1-based stamps (a diagnostic's	//
// column, a definition's) convert here, never in a consumer.		//
//									//
//////////////////////////////////////////////////////////////////////////

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ns_common.h"
#include "madc_lang.h"
#include "madc_file_kinds.h"	// the kind's default standard
#include "madcdis/verbs.h"	// mutation_context: the document's parse slot
#include "rt/rt_task.h"		// __madc_yield: a caller waits out a parse in flight

namespace madc {
namespace lang {

const char *symbol_kind_name(symbol_kind k)
{
    switch ( k )
    {
    case symbol_kind::function:
	return "function";
    }
    return "";
}

namespace {

typedef hub::text_buffer text_buffer;
typedef std::map<std::string, value> row_fields;

int64_t row_int(const value &row, const char *name)
{
    if ( !row.is_object() )
	return 0;
    const row_fields &f = row.as_object();
    row_fields::const_iterator i = f.find(name);
    return i == f.end() || !i->second.is_integer() ? 0 : i->second.as_integer();
}

std::string row_text(const value &row, const char *name)
{
    if ( !row.is_object() )
	return std::string();
    const row_fields &f = row.as_object();
    row_fields::const_iterator i = f.find(name);
    return i == f.end() || !i->second.is_string() ? std::string()
						  : i->second.as_string();
}

const std::vector<value> &rows_of(const value &v)
{
    static const std::vector<value> none;
    return v.is_array() ? v.as_array() : none;
}

point make_point(int64_t line, int64_t column)
{
    point p;
    p.line = line;
    p.column = column;
    return p;
}

range make_range(point from, point to)
{
    range r;
    r.from = from;
    r.to = to;
    return r;
}

// Lines `from`..`to`, whole: from the first one's start to the last one's
// end (its newline excluded). A line the text does not have collapses onto
// the first line's start.
range whole_lines(const text_buffer &b, int64_t from, int64_t to)
{
    if ( from < 1 )
	from = 1;
    if ( to < from )
	to = from;
    size_t off = 0, len = 0;
    if ( !b.line_span((size_t)to, off, len) )
    {
	to = from;
	if ( !b.line_span((size_t)from, off, len) )
	    len = 0;
    }
    return make_range(make_point(from, 0), make_point(to, (int64_t)len));
}

// The whole word containing byte `off` (the buffer's ONE word rule,
// text_buffer::word_left_in / word_right_in): false when `off` is in no
// word.
bool word_span(const std::string &t, size_t off, size_t &from, size_t &to)
{
    size_t e = text_buffer::word_right_in(t, off);
    if ( e == 0 )
	return false;
    size_t s = text_buffer::word_left_in(t, e);
    if ( s >= e || text_buffer::word_right_in(t, s) != e )
	return false;				// not one whole word
    if ( s > off || off > e )
	return false;				// `off` is not in it
    from = s;
    to = e;
    return true;
}

// `name` as a whole word on 1-based `line`, searching from byte column
// `start`: its first occurrence there that is not part of a longer word.
bool name_on_line(const text_buffer &b, const std::string &t, int64_t line,
		  int64_t start, const std::string &name, range &out)
{
    size_t off = 0, len = 0;
    if ( name.empty() || line < 1 || !b.line_span((size_t)line, off, len) )
	return false;
    size_t end = off + len;
    size_t at = off + (start > 0 && (size_t)start < len ? (size_t)start : 0);
    while ( at + name.size() <= end )
    {
	size_t hit = t.find(name, at);
	if ( hit == std::string::npos || hit + name.size() > end )
	    return false;
	size_t s = 0, e = 0;
	if ( word_span(t, hit, s, e) && s == hit && e == hit + name.size() )
	{
	    out = make_range(make_point(line, (int64_t)(hit - off)),
			     make_point(line, (int64_t)(e - off)));
	    return true;
	}
	at = hit + 1;
    }
    return false;
}

// The byte offset of a point (its column clamped to its line); false when
// the text has no such line.
bool offset_of(const text_buffer &b, point p, size_t &off)
{
    size_t start = 0, len = 0;
    if ( p.line < 1 || !b.line_span((size_t)p.line, start, len) )
	return false;
    off = start + (p.column < 0 ? 0
		   : (size_t)p.column > len ? len : (size_t)p.column);
    return true;
}

// The word a point names: the one containing it, else — a caret resting
// just past a word — the one ending there.
bool word_under(const text_buffer &b, const std::string &t, point p,
		std::string &word, range &r)
{
    size_t off = 0, s = 0, e = 0;
    if ( !offset_of(b, p, off) )
	return false;
    if ( !word_span(t, off, s, e) && (off == 0 || !word_span(t, off - 1, s, e)) )
	return false;
    word = t.substr(s, e - s);
    r = make_range(make_point(p.line, p.column - (int64_t)(off - s)),
		   make_point(p.line, p.column + (int64_t)(e - off)));
    return true;
}

// `name` as a whole word on 1-based `line`, the LAST occurrence starting at
// or before byte column `anchor` (a use site's anchor).
bool name_before(const text_buffer &b, const std::string &t, int64_t line,
		 int64_t anchor, const std::string &name, range &out)
{
    size_t off = 0, len = 0;
    if ( name.empty() || line < 1 || anchor < 0
	 || !b.line_span((size_t)line, off, len) )
	return false;
    size_t last = off + ((size_t)anchor < len ? (size_t)anchor : len);
    size_t at = last + 1;
    while ( at > off )
    {
	size_t hit = t.rfind(name, at - 1);
	if ( hit == std::string::npos || hit < off )
	    return false;
	size_t s = 0, e = 0;
	if ( hit + name.size() <= off + len && word_span(t, hit, s, e)
	     && s == hit && e == hit + name.size() )
	{
	    out = make_range(make_point(line, (int64_t)(hit - off)),
			     make_point(line, (int64_t)(e - off)));
	    return true;
	}
	at = hit;
    }
    return false;
}

// The graph's definition nodes for the word a point names (empty: no word,
// or none the TU defines).
std::vector<value> definition_nodes(int64_t handle, const std::string &word)
{
    value snap;
    madc_graph_definition(&snap, handle, (void *)&word);
    std::vector<value> nodes;
    if ( snap.is_object() )
    {
	const row_fields &f = snap.as_object();
	row_fields::const_iterator n = f.find("nodes");
	if ( n != f.end() )
	    nodes = rows_of(n->second);
    }
    return nodes;
}

// A definition row's NAME ({name, line, column, ...}: an outline row or a
// graph node; the column a token's 1-based stamp at the declaration's
// start): the name searched from that column, else the row's line.
range name_range(const text_buffer &b, const std::string &t, const value &row)
{
    int64_t line = row_int(row, "line");
    range r;
    if ( !name_on_line(b, t, line, row_int(row, "column") - 1,
		       row_text(row, "name"), r) )
	r = whole_lines(b, line, line);
    return r;
}

// An outline row ({kind, name, line, column, end_line}) as a symbol: its
// lines, and its name.
symbol symbol_of(const text_buffer &b, const std::string &t, const value &row)
{
    symbol s;
    s.name = row_text(row, "name");
    s.kind = symbol_kind::function;
    s.extent = whole_lines(b, row_int(row, "line"), row_int(row, "end_line"));
    s.selection = name_range(b, t, row);
    return s;
}

// A diagnostic row's range: the word STARTING at its 1-based column, else
// the one byte there; a column outside its line covers the line, a line
// outside the text the first line.
range diagnostic_range(const text_buffer &b, const std::string &t,
		       const value &row)
{
    int64_t line = row_int(row, "line");
    if ( line < 1 )
	line = 1;
    size_t off = 0, len = 0;
    if ( !b.line_span((size_t)line, off, len) )
	return whole_lines(b, 1, 1);
    int64_t col = row_int(row, "column") - 1;	// the row's is 1-based
    if ( col < 0 || (size_t)col >= len )
	return whole_lines(b, line, line);
    size_t at = off + (size_t)col;
    size_t s = 0, e = 0;
    if ( !word_span(t, at, s, e) || s != at )
	e = at + 1;
    return make_range(make_point(line, col),
		      make_point(line, (int64_t)(e - off)));
}

const text_buffer *text_of(hub::world &w, hub::entity_id e)
{
    return w.text_of(e);
}

} // namespace

// The document once no parse of it is in flight: a parse yields between
// slices (stage 2), so a caller arriving mid-parse — another parse, a close,
// a new TU or standard, each of which closes the handle — waits for it and
// finds the slot as that parse left it. NULL = no such document (or it
// closed while the caller waited).
static const hub::document *settled_doc(hub::world &w, hub::entity_id e)
{
    const hub::document *d = w.doc_of(e);
    while ( d && d->parse_busy() )
    {
	__madc_yield();
	d = w.doc_of(e);
    }
    return d;
}

// The slot's handle closed (its next document_handle opens under the
// document's current options).
static void drop_parse(hub::world &w, hub::entity_id e, const hub::document &d)
{
    if ( !d.parse_handle() )
	return;
    madc_parse_close(d.parse_handle());
    hub::mutation_context(w).doc_set_parse(e, 0, 0);
}

int64_t document_handle(hub::world &w, hub::entity_id e, value *checked)
{
    const hub::document *d = settled_doc(w, e);
    const text_buffer *b = w.text_of(e);
    if ( !d || !b )
	return 0;
    if ( d->kind() != fkUNKNOWN && file_kind_default_standard(d->kind()) == 0 )
	return 0;		// a kind the compiler does not parse
    // The handle adopts the KIND (split arc 1A.4): a family head is its
    // default, as the CLI compiles the same file (Program::adopt_file_standard)
    // — unless the document names its own standard.
    int64_t standard = d->parse_standard() ? d->parse_standard()
			: d->kind() == fkUNKNOWN ? 0 : d->kind();
    const std::string name = d->path().empty() ? "untitled" : d->path();
    int64_t handle = d->parse_handle();
    if ( handle && (d->parsed_standard() != standard || d->parsed_name() != name) )
    {
	drop_parse(w, e, *d);		// opened as another unit: a new standard, a Save As
	handle = 0;
    }
    uint64_t version = b->version();
    if ( handle && d->parse_version() == version )
    {
	if ( checked )
	    madc_parse_diagnostics(checked, handle);
	return handle;
    }
    std::string text = b->text();
    hub::mutation_context mc(w);
    mc.doc_set_parse_busy(e, true);
    const bool opening = !handle;
    if ( opening )
    {
	std::string unit = name;
	handle = d->parse_tu() ? madc_parse_open_tu(&text, &unit, standard, d->parse_tu())
			       : madc_parse_open(&text, &unit, standard);
	if ( handle && checked )
	    madc_parse_diagnostics(checked, handle);
    }
    else if ( checked )
	madc_parse_refresh_checked(checked, handle, &text);
    else
	madc_parse_refresh(handle, &text);
    if ( !w.doc_of(e) )
    {
	// Closed while the parse yielded (a close waits for a parse in
	// flight, so only a detach that bypasses the door gets here).
	if ( handle )
	    madc_parse_close(handle);
	return 0;
    }
    mc.doc_set_parse_busy(e, false);
    if ( !handle )
	return 0;
    mc.doc_set_parse(e, handle, version);
    if ( opening )
	mc.doc_set_parse_opened(e, standard, name);
    return handle;
}

bool document_set_tu(hub::world &w, hub::entity_id e, const ProjectTU &tu)
{
    const hub::document *d = settled_doc(w, e);
    if ( !d )
	return false;
    drop_parse(w, e, *d);
    return hub::mutation_context(w).doc_set_parse_tu(e, tu);
}

bool document_set_standard(hub::world &w, hub::entity_id e, int64_t standard)
{
    // The door compares: a handle opened under another standard reopens.
    return hub::mutation_context(w).doc_set_parse_standard(e, standard);
}

void close_world_parses(hub::world &w)
{
    std::vector<hub::entity_id> docs = w.docs();
    for ( size_t i = 0; i < docs.size(); ++i )
	if ( const hub::document *d = w.doc_of(docs[i]) )
	    drop_parse(w, docs[i], *d);
}

int64_t document_parsed(const hub::world &w, hub::entity_id e)
{
    const hub::document *d = w.doc_of(e);
    return d ? d->parse_handle() : 0;
}

bool document_close(hub::world &w, hub::entity_id e)
{
    const hub::document *d = settled_doc(w, e);
    if ( !d )
	return false;
    drop_parse(w, e, *d);
    return hub::mutation_context(w).doc_detach(e);
}

bool diagnostics(hub::world &w, hub::entity_id e,
		 std::vector<diagnostic> &out)
{
    out.clear();
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    value rows;
    madc_parse_diagnostics(&rows, handle);
    const text_buffer &b = *text_of(w, e);
    std::string t = b.text();
    const std::vector<value> &r = rows_of(rows);
    for ( size_t i = 0; i < r.size(); ++i )
    {
	diagnostic d;
	d.at = diagnostic_range(b, t, r[i]);
	d.severity = (diag_severity)row_int(r[i], "severity_code");
	d.phase = (diag_phase)row_int(r[i], "phase_code");
	d.message = row_text(r[i], "message");
	out.push_back(d);
    }
    return true;
}

bool outline(hub::world &w, hub::entity_id e, std::vector<symbol> &out)
{
    out.clear();
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    value rows;
    madc_parse_outline(&rows, handle);
    const text_buffer &b = *text_of(w, e);
    std::string t = b.text();
    const std::vector<value> &r = rows_of(rows);
    for ( size_t i = 0; i < r.size(); ++i )
	out.push_back(symbol_of(b, t, r[i]));
    return true;
}

bool enclosing(hub::world &w, hub::entity_id e, point at, symbol &out)
{
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    value row;
    // parse_enclosing compares against the definitions' 1-based stamps.
    madc_parse_enclosing(&row, handle, at.line, at.column + 1);
    if ( !row.is_object() )
	return false;
    const text_buffer &b = *text_of(w, e);
    out = symbol_of(b, b.text(), row);
    return true;
}

bool spans(hub::world &w, hub::entity_id e, std::vector<highlight_span> &out)
{
    out.clear();
    int64_t handle = document_handle(w, e);
    return handle && parse_classes(handle, out);
}

bool definition(hub::world &w, hub::entity_id e, point at,
		std::vector<range> &out)
{
    out.clear();
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    const text_buffer &b = *text_of(w, e);
    std::string t = b.text();
    std::string word;
    range wr;
    if ( !word_under(b, t, at, word, wr) )
	return true;
    std::vector<value> nodes = definition_nodes(handle, word);
    for ( size_t i = 0; i < nodes.size(); ++i )
	if ( row_int(nodes[i], "line") > 0 )
	    out.push_back(name_range(b, t, nodes[i]));
    return true;
}

bool references(hub::world &w, hub::entity_id e, point at,
		bool include_declaration, std::vector<range> &out)
{
    out.clear();
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    const text_buffer &b = *text_of(w, e);
    std::string t = b.text();
    std::string word;
    range wr;
    if ( !word_under(b, t, at, word, wr) )
	return true;
    std::vector<value> defs = definition_nodes(handle, word);
    if ( defs.empty() )
	return true;
    value snap;
    madc_graph_references(&snap, handle, row_int(defs[0], "id"));
    if ( snap.is_object() )
    {
	const row_fields &f = snap.as_object();
	row_fields::const_iterator n = f.find("nodes");
	row_fields::const_iterator g = f.find("edges");
	const std::vector<value> &nodes = n == f.end() ? rows_of(value())
						       : rows_of(n->second);
	const std::vector<value> &edges = g == f.end() ? rows_of(value())
						       : rows_of(g->second);
	// An edge's `from` names its use site among `nodes` (the subject
	// rides in `nodes` too).
	std::map<int64_t, const value *> by_id;
	for ( size_t i = 0; i < nodes.size(); ++i )
	    by_id[row_int(nodes[i], "id")] = &nodes[i];
	for ( size_t i = 0; i < edges.size(); ++i )
	{
	    std::map<int64_t, const value *>::const_iterator site =
		by_id.find(row_int(edges[i], "from"));
	    if ( site == by_id.end() )
		continue;
	    int64_t line = row_int(*site->second, "line");
	    if ( line < 1 )
		continue;
	    std::string name = row_text(*site->second, "name");
	    range r;
	    // The site's column is a token's 1-based stamp.
	    if ( !name_before(b, t, line, row_int(*site->second, "column") - 1,
			      name.empty() ? word : name, r) )
		r = whole_lines(b, line, line);
	    out.push_back(r);
	}
    }
    if ( include_declaration && row_int(defs[0], "line") > 0 )
	out.push_back(name_range(b, t, defs[0]));
    return true;
}

bool hover(hub::world &w, hub::entity_id e, point at, hover_info &out)
{
    out = hover_info();
    out.has_range = false;
    int64_t handle = document_handle(w, e);
    if ( !handle )
	return false;
    const text_buffer &b = *text_of(w, e);
    std::string t = b.text();
    std::string word;
    range wr;
    if ( word_under(b, t, at, word, wr) )
    {
	std::vector<value> defs = definition_nodes(handle, word);
	if ( !defs.empty() )
	{
	    out.text = row_text(defs[0], "kind") + " " + row_text(defs[0], "name");
	    int64_t line = row_int(defs[0], "line");
	    if ( line > 0 )
		out.text += " — defined on line " + std::to_string(line);
	    out.has_range = true;
	    out.at = wr;
	    return true;
	}
    }
    symbol s;
    if ( !enclosing(w, e, at, s) )
	return false;
    out.text = "in " + s.name;
    return true;
}

} // namespace lang
} // namespace madc
