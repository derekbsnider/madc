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
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "ns_common.h"
#include "madc_lang.h"
#include "madc_file_kinds.h"	// the kind's default standard
#include "madcdis/verbs.h"	// mutation_context: the document's parse slot

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

int64_t document_handle(hub::world &w, hub::entity_id e, value *checked)
{
    const hub::document *d = w.doc_of(e);
    const text_buffer *b = w.text_of(e);
    if ( !d || !b )
	return 0;
    if ( d->kind() != fkUNKNOWN && file_kind_default_standard(d->kind()) == 0 )
	return 0;		// a kind the compiler does not parse
    // The handle adopts the KIND (split arc 1A.4): a family head is its
    // default, as the CLI compiles the same file (Program::adopt_file_standard).
    int64_t standard = d->kind() == fkUNKNOWN ? 0 : d->kind();
    int64_t handle = d->parse_handle();
    uint64_t version = b->version();
    if ( handle && d->parse_version() == version )
    {
	if ( checked )
	    madc_parse_diagnostics(checked, handle);
	return handle;
    }
    std::string text = b->text();
    if ( !handle )
    {
	std::string name = d->path().empty() ? "untitled" : d->path();
	handle = madc_parse_open(&text, &name, standard);
	if ( !handle )
	    return 0;
	if ( checked )
	    madc_parse_diagnostics(checked, handle);
    }
    else if ( checked )
	madc_parse_refresh_checked(checked, handle, &text);
    else
	madc_parse_refresh(handle, &text);
    hub::mutation_context mc(w);
    mc.doc_set_parse(e, handle, version);
    return handle;
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

} // namespace lang
} // namespace madc
