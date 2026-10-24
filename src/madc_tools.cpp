//////////////////////////////////////////////////////////////////////////
//									//
// madc_tools.cpp — the tool registry and madc's code-graph tools (split	//
// arc 1C; contract in include/madc_tools.h). The graph tools are the	//
// madc_graph_* reads over the served document's parse handle		//
// (madc::lang::document_handle, the one door): this file converts a	//
// tool's arguments to the read's and nothing else.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "ns_common.h"		// the madc_graph_* reads
#include "madc_tools.h"
#include "madc_lang.h"		// document_handle: the served document's parse
#include "madc_jsonrpc.h"	// a message's members: the arguments
#include "madcdis/hub.h"		// the served document's text
#include "madcdis/verbs.h"		// mutation_context: an edit lands through it

namespace madc {
namespace tools {

using jsonrpc::field;
using jsonrpc::object_field;
using jsonrpc::text_field;
using jsonrpc::int_field;

namespace {

typedef std::map<std::string, value> fields;

const char *const tier_names[] = { "observer", "proposer", "editor", "owner" };

} // namespace

const char *tier_name(tier t)
{
    return tier_names[(size_t)t];
}

value refusal(const std::string &why)
{
    fields f;
    f["error"] = value(why);
    return value::make_object(f);
}

bool refused(const value &result)
{
    return result.is_object() && result.as_object().count("error") != 0;
}

value property(const char *type, const char *description)
{
    fields f;
    f["type"] = value(type);
    f["description"] = value(description);
    return value::make_object(f);
}

value object_schema(const std::map<std::string, value> &properties)
{
    fields f;
    f["type"] = value("object");
    f["properties"] = value::make_object(properties);
    return value::make_object(f);
}

std::string served_path(const context &c)
{
    const hub::document *d = c.w.doc_of(c.doc);
    return d ? d->path() : std::string();
}

bool span_extent(const hub::text_buffer &b, const value &span, size_t &at, size_t &end)
{
    const int64_t l0 = int_field(span, "line"), l1 = int_field(span, "end_line");
    size_t s0 = 0, n0 = 0, s1 = 0, n1 = 0;
    if ( l0 < 1 || l1 < l0 || !b.line_span((size_t)l0, s0, n0)
	 || !b.line_span((size_t)l1, s1, n1) )
	return false;
    at = s0 + (size_t)int_field(span, "column");
    end = s1 + (size_t)int_field(span, "end_column");
    return at <= end && end <= b.size();
}

// ---- a document's past revisions ---------------------------------------

namespace {
const size_t REVISIONS_PER_DOCUMENT = 8;	// madcide's REV_HANDLES_MAX
}

revisions::~revisions()
{
    for ( const entry &e : _open )
	madc_parse_close(e.handle);
}

bool revisions::open(history_source &h, hub::entity_id doc, const std::string &path,
		     const std::string &spec, entry &out, std::string &why)
{
    value shown;
    if ( !h.show(path, spec, shown) )
    {
	why = text_field(shown, "error");
	return false;
    }
    const std::string sha = text_field(shown, "sha");
    for ( size_t i = 0; i < _open.size(); ++i )
	if ( _open[i].doc == doc && _open[i].sha == sha )
	{
	    out = _open[i];
	    _open.erase(_open.begin() + i);
	    _open.push_back(out);		// now the most recently used
	    return true;
	}
    std::string text = text_field(shown, "text"), name = path;
    const int64_t handle = madc_parse_open_tagged(&text, &name);
    if ( handle <= 0 )
    {
	why = "the revision's text did not open as a parse handle";
	return false;
    }
    size_t held = 0;
    for ( const entry &e : _open )
	held += e.doc == doc;
    for ( size_t i = 0; held >= REVISIONS_PER_DOCUMENT && i < _open.size(); )
	if ( _open[i].doc == doc )
	{
	    madc_parse_close(_open[i].handle);	// its ids route nowhere from now on
	    _open.erase(_open.begin() + i);
	    --held;
	}
	else
	    ++i;
    std::shared_ptr<hub::text_buffer> tb(new hub::text_buffer());
    tb->load(text);
    entry e;
    e.doc = doc;
    e.sha = sha;
    e.tag = madc_parse_generation(handle);
    e.handle = handle;
    e.text = tb;
    _open.push_back(e);
    out = e;
    return true;
}

bool revisions::of_handle(int64_t handle, entry &out) const
{
    for ( const entry &e : _open )
	if ( e.handle == handle )
	{
	    out = e;
	    return true;
	}
    return false;
}

std::vector<revisions::entry> revisions::of(hub::entity_id doc) const
{
    std::vector<entry> out;
    for ( const entry &e : _open )
	if ( e.doc == doc )
	    out.push_back(e);
    return out;
}

bool registry::add(const tool &t)
{
    if ( t.name.empty() || find(t.name) || !t.run )
	return false;
    _tools.push_back(t);
    return true;
}

const tool *registry::find(const std::string &name) const
{
    for ( size_t i = 0; i < _tools.size(); ++i )
	if ( _tools[i].name == name )
	    return &_tools[i];
    return 0;
}

bool registry::call(const context &c, const std::string &name, const value &args,
		    value &result) const
{
    const tool *t = find(name);
    if ( !t )
    {
	result = refusal("no tool is named '" + name + "'");
	return false;
    }
    if ( c.caller < t->least )
    {
	result = refusal("'" + name + "' requires " + tier_name(t->least)
			 + " (you are " + tier_name(c.caller) + ")");
	return false;
    }
    return t->run(c, args, result);
}

// ---- the code-graph tools ---------------------------------------------

namespace {

enum class verb : unsigned char
{
    symbols, definition, node, type_of, members, bases, enclosing, body,
    children, callees, callers, references, search, impact, span, at
};

// A verb's arguments, by shape: the schema advertised and the read made
// from it are one table's.
enum class shape : unsigned char
{
    none, id, name, position, body, children, func, def, search, at
};

struct verb_row
{
    const char *name;
    verb v;
    shape s;
    const char *description;
};

const verb_row verb_rows[] = {
    { "graph.symbols", verb::symbols, shape::none,
      "the TU's own functions as graph nodes {id,kind,name,span}" },
    { "graph.definition", verb::definition, shape::name,
      "resolve a name to its definition node(s)" },
    { "graph.node", verb::node, shape::id, "the node for a node id" },
    { "graph.type_of", verb::type_of, shape::id,
      "the type node of a node (HAS_TYPE)" },
    { "graph.members", verb::members, shape::id,
      "a struct/class's members with their types (MEMBER_OF)" },
    { "graph.bases", verb::bases, shape::id,
      "a class's direct base types (INHERITS)" },
    { "graph.enclosing", verb::enclosing, shape::position,
      "the innermost enclosing function at a position (ENCLOSES)" },
    { "graph.body", verb::body, shape::body,
      "a function's body subtree {nodes,edges:CONTAINS} over the live parse tree" },
    { "graph.children", verb::children, shape::children,
      "structural descent from any node (a body id or a function id)" },
    { "graph.callees", verb::callees, shape::func,
      "the functions a function calls (CALLS out-edges)" },
    { "graph.callers", verb::callers, shape::func,
      "the functions that call a function (CALLS in-edges)" },
    { "graph.references", verb::references, shape::def,
      "every use-site of a function or global (REFERENCES + enclosing)" },
    { "graph.search", verb::search, shape::search,
      "find decl nodes by kind + name substring (v1: functions, globals)" },
    { "graph.impact", verb::impact, shape::id,
      "batched callers + references + enclosing for a function or global" },
    { "graph.span", verb::span, shape::id,
      "a node's exact source extent {span:{line,column,end_line,end_column}}"
      " (statement / function / global)" },
    { "graph.at", verb::at, shape::at,
      "the editable node whose span starts at a position (1-based line,"
      " 0-based column)" },
};

value schema_of(shape s)
{
    fields p;
    if ( s == shape::none || s == shape::name || s == shape::position || s == shape::search
	 || s == shape::at )
	p["rev"] = property("string", "read the file at this git revision (rev-parse spec or"
			    " sha; optional — \"\" = the live text; needs the version"
			    " history); its ids route themselves to it");
    switch ( s )
    {
    case shape::none:
	break;
    case shape::id:
	p["id"] = property("integer", "a node id from a graph.* result (a type-id,"
			   " a global decl-id or a body node id)");
	break;
    case shape::name:
	p["name"] = property("string", "a symbol name");
	break;
    case shape::position:
	p["line"] = property("integer", "1-based line");
	p["column"] = property("integer", "1-based column");
	break;
    case shape::body:
	p["func"] = property("integer", "a function node id (type-id)");
	p["depth"] = property("integer", "subtree depth (optional; default: full)");
	break;
    case shape::children:
	p["id"] = property("integer", "a node id from a graph.* result (a type-id,"
			   " a global decl-id or a body node id)");
	p["depth"] = property("integer", "subtree depth (optional; default 1)");
	break;
    case shape::func:
	p["func"] = property("integer", "a function node id (type-id)");
	break;
    case shape::def:
	p["func"] = property("integer", "a function type-id or a global decl-id");
	break;
    case shape::search:
	p["kind"] = property("string", "a node kind filter (optional; v1:"
			     " Function, Global)");
	p["name"] = property("string", "a name substring to match");
	break;
    case shape::at:
	p["line"] = property("integer", "1-based line");
	p["column"] = property("integer", "0-based byte column (the span"
			       " convention: where the node's text starts)");
	break;
    }
    return object_schema(p);
}

// The handle `rev` names for the served document: "" = the live parse; 0
// with the refusal in `result`.
int64_t revision_handle(const registry &r, const context &c, int64_t live,
			const std::string &rev, value &result)
{
    if ( rev.empty() )
	return live;
    if ( !r.history() )
    {
	result = refusal("no version history here (the git module is not imported)");
	return 0;
    }
    revisions::entry e;
    std::string why;
    if ( !r.past().open(*r.history(), c.doc, served_path(c), rev, e, why) )
    {
	result = refusal(why);
	return 0;
    }
    return e.handle;
}

// The handle a read runs against (item 14): an id names its own — the live
// parse's or an open revision's (madc_graph_route; 0, a stale or unknown id:
// the live handle, whose read refuses it) — and an id-less read may name a
// revision (`rev`). 0 with the refusal in `result`.
int64_t read_handle(const registry &r, const context &c, const value &args, value &result)
{
    const int64_t live = lang::document_handle(c.w, c.doc);
    if ( !live )
    {
	result = refusal("the compiler does not parse the served document");
	return 0;
    }
    const value *id = field(args, "id") ? field(args, "id") : field(args, "func");
    if ( id && id->is_integer() )
    {
	const int64_t h = madc_graph_route(live, id->as_integer());
	return h ? h : live;
    }
    return revision_handle(r, c, live, text_field(args, "rev"), result);
}

// The text a handle was parsed from: an open revision's (held by `keep`),
// else the served document's.
const hub::text_buffer *handle_text(const registry &r, const context &c, int64_t h,
				    revisions::entry &keep)
{
    return r.past().of_handle(h, keep) ? keep.text.get() : c.w.text_of(c.doc);
}

bool graph_read(verb v, const registry &r, const context &c, const value &args,
		value &result)
{
    const int64_t h = read_handle(r, c, args, result);
    if ( !h )
	return false;
    switch ( v )
    {
    case verb::symbols:
	madc_graph_symbols(&result, h);
	break;
    case verb::definition:
    {
	std::string name = text_field(args, "name");
	madc_graph_definition(&result, h, &name);
	break;
    }
    case verb::node:
	madc_graph_node(&result, h, int_field(args, "id"));
	break;
    case verb::type_of:
	madc_graph_type_of(&result, h, int_field(args, "id"));
	break;
    case verb::members:
	madc_graph_members(&result, h, int_field(args, "id"));
	break;
    case verb::bases:
	madc_graph_bases(&result, h, int_field(args, "id"));
	break;
    case verb::enclosing:
	madc_graph_enclosing(&result, h, int_field(args, "line"), int_field(args, "column"));
	break;
    case verb::body:
	madc_graph_body(&result, h, int_field(args, "func"), int_field(args, "depth", -1));
	break;
    case verb::children:
	madc_graph_children(&result, h, int_field(args, "id"), int_field(args, "depth", 1));
	break;
    case verb::callees:
	madc_graph_callees(&result, h, int_field(args, "func"));
	break;
    case verb::callers:
	madc_graph_callers(&result, h, int_field(args, "func"));
	break;
    case verb::references:
	madc_graph_references(&result, h, int_field(args, "func"));
	break;
    case verb::search:
    {
	std::string kind = text_field(args, "kind"), name = text_field(args, "name");
	madc_graph_search(&result, h, &kind, &name);
	break;
    }
    case verb::impact:
	madc_graph_impact(&result, h, int_field(args, "id"));
	break;
    case verb::span:
	madc_graph_span(&result, h, int_field(args, "id"));
	break;
    case verb::at:
	madc_graph_at(&result, h, int_field(args, "line"), int_field(args, "column"));
	break;
    }
    return !refused(result);
}

// graph.source: a node's text — its span's bytes in the text ITS handle was
// parsed from (the live document's, or a revision's).
bool node_source(const registry &r, const context &c, int64_t h, int64_t id,
		 value &node, std::string &text, value &result)
{
    madc_graph_span(&node, h, id);
    const value *span = field(node, "span");
    if ( !span )
    {
	result = refused(node) ? node : refusal("the node has no source extent");
	return false;
    }
    revisions::entry keep;
    const hub::text_buffer *b = handle_text(r, c, h, keep);
    size_t at = 0, end = 0;
    if ( !b || !span_extent(*b, *span, at, end) )
    {
	result = refusal("the node's extent is outside the text");
	return false;
    }
    text = b->slice(at, end - at);
    return true;
}

bool graph_source(const registry &r, const context &c, const value &args, value &result)
{
    const int64_t h = read_handle(r, c, args, result);
    value node;
    std::string text;
    if ( !h || !node_source(r, c, h, int_field(args, "id"), node, text, result) )
	return false;
    fields f;
    f["node"] = node;
    f["text"] = value(text);
    result = value::make_object(f);
    return true;
}

// ---- the history reads (add_history_tools) ----------------------------

enum class history_verb : unsigned char { commits, revision, diff };

struct history_row
{
    const char *name;
    history_verb v;
    const char *description;
};

const history_row history_rows[] = {
    { "graph.commits", history_verb::commits,
      "the commits touching the served file, newest first" },
    { "graph.revision", history_verb::revision,
      "open the served file at a git revision as a read-only handle; its ids route"
      " themselves to it" },
    { "graph.diff", history_verb::diff,
      "declaration-level diff between two revisions (\"\" = the live text): the"
      " symbols added, removed and changed" },
};

value history_schema(history_verb v)
{
    fields p;
    const char *rev = "a git revision (rev-parse spec or sha); \"\" = the live text";
    switch ( v )
    {
    case history_verb::commits:
	p["limit"] = property("integer", "at most this many rows (optional; default 20)");
	break;
    case history_verb::revision:
	p["rev"] = property("string", rev);
	break;
    case history_verb::diff:
	p["a"] = property("string", rev);
	p["b"] = property("string", rev);
	break;
    }
    return object_schema(p);
}

// A handle's symbols keyed by kind and name, in the handle's order.
void symbol_index(int64_t h, std::vector<std::string> &keys, std::map<std::string, value> &nodes)
{
    value sy;
    madc_graph_symbols(&sy, h);
    const value &list = object_field(sy, "nodes");
    if ( !list.is_array() )
	return;
    for ( const value &n : list.as_array() )
    {
	const std::string key = text_field(n, "kind") + "\t" + text_field(n, "name");
	if ( nodes.insert(std::make_pair(key, n)).second )
	    keys.push_back(key);
    }
}

// graph.diff at declaration granularity: changed = the node's text differs.
bool graph_diff(const registry &r, const context &c, int64_t live, const value &args,
		value &result)
{
    const int64_t ha = revision_handle(r, c, live, text_field(args, "a"), result);
    const int64_t hb = ha ? revision_handle(r, c, live, text_field(args, "b"), result) : 0;
    if ( !ha || !hb )
	return false;
    std::vector<std::string> ka, kb;
    std::map<std::string, value> na, nb;
    symbol_index(ha, ka, na);
    symbol_index(hb, kb, nb);
    std::vector<value> added, removed, changed;
    for ( const std::string &k : ka )
    {
	std::map<std::string, value>::const_iterator xb = nb.find(k);
	const value &xa = na[k];
	if ( xb == nb.end() )
	{
	    removed.push_back(xa);
	    continue;
	}
	value spa, spb, why;
	std::string ta, tb;
	node_source(r, c, ha, int_field(xa, "id"), spa, ta, why);
	node_source(r, c, hb, int_field(xb->second, "id"), spb, tb, why);
	if ( ta != tb )
	{
	    fields pair;
	    pair["a"] = xa;
	    pair["b"] = xb->second;
	    changed.push_back(value::make_object(pair));
	}
    }
    for ( const std::string &k : kb )
	if ( !na.count(k) )
	    added.push_back(nb[k]);
    fields f;
    f["added"] = value::make_array(added);
    f["removed"] = value::make_array(removed);
    f["changed"] = value::make_array(changed);
    result = value::make_object(f);
    return true;
}

bool history_read(history_verb v, const registry &r, const context &c, const value &args,
		  value &result)
{
    const int64_t live = lang::document_handle(c.w, c.doc);
    if ( !live )
    {
	result = refusal("the compiler does not parse the served document");
	return false;
    }
    switch ( v )
    {
    case history_verb::commits:
    {
	const int64_t limit = int_field(args, "limit", 20);
	return r.history()->commits(served_path(c), limit > 0 ? limit : 20, result);
    }
    case history_verb::revision:
    {
	const std::string rev = text_field(args, "rev");
	if ( rev.empty() )
	{
	    result = refusal("graph.revision needs rev: a rev-parse spec or a sha");
	    return false;
	}
	revisions::entry e;
	std::string why;
	if ( !r.past().open(*r.history(), c.doc, served_path(c), rev, e, why) )
	{
	    result = refusal(why);
	    return false;
	}
	fields f;
	f["sha"] = value(e.sha);
	f["tag"] = value(e.tag);
	result = value::make_object(f);
	return true;
    }
    case history_verb::diff:
	return graph_diff(r, c, live, args, result);
    }
    return false;
}

// ---- the edits --------------------------------------------------------

enum class edit_verb : unsigned char { insert, replace, remove };

struct edit_row
{
    const char *name;
    edit_verb v;
    const char *description;
};

const edit_row edit_rows[] = {
    { "graph.insert", edit_verb::insert,
      "insert src before|after a node (proposer tier stores a proposal; editor"
      " tier applies: validated, undoable)" },
    { "graph.replace", edit_verb::replace,
      "replace a node's source with src (proposer tier stores a proposal;"
      " editor tier applies: validated, undoable)" },
    { "graph.delete", edit_verb::remove,
      "delete a node (proposer tier stores a proposal; editor tier applies:"
      " validated, undoable)" },
};

value edit_schema(edit_verb v)
{
    fields p;
    p["id"] = property("integer", "a node id from a graph.* result (a type-id,"
		       " a global decl-id or a body node id)");
    if ( v != edit_verb::remove )
	p["src"] = property("string", "source text: the statement(s) or definition"
			    " to place (validated by the compiler before commit)");
    if ( v == edit_verb::insert )
	p["where"] = property("string", "before | after (relative to the target node)");
    p["propose"] = property("boolean", "store the edit as a PROPOSAL for an editor"
			    " to accept instead of applying it (the proposer tier"
			    " always does; optional)");
    return object_schema(p);
}

// One planned edit: the target node (graph.span's answer), its bytes now,
// the splice {at, del, ins}, and where the edited node starts afterwards.
struct edit_plan
{
    value target;
    std::string text;
    size_t at, del;
    std::string ins;
    int64_t line, column;
};

// Only blanks in [from, to) of `t`?
bool blanks(const std::string &t, size_t from, size_t to)
{
    return to <= from || t.find_first_not_of(" \t", from) >= to;
}

// The splice for `v` on the node `args` names, with the line rules: a node
// that owns its line(s) — nothing but blanks beside it — is deleted whole
// and gets an insert on a line of its own at its indent; a node sharing its
// line is edited at its exact boundary.
bool plan_edit(edit_verb v, const hub::text_buffer &b, const std::string &t,
	       int64_t h, const value &args, edit_plan &p, std::string &why)
{
    const std::string src = text_field(args, "src");
    const std::string where = text_field(args, "where");
    const bool before = where == "before";
    if ( v == edit_verb::insert && !before && where != "after" )
    {
	why = "graph.insert needs where = before | after";
	return false;
    }
    if ( v != edit_verb::remove && src.empty() )
    {
	why = "src is empty (use graph.delete to remove a node)";
	return false;
    }
    value sp;
    madc_graph_span(&sp, h, int_field(args, "id"));
    const value *span = field(sp, "span");
    if ( !span )
    {
	why = refused(sp) ? text_field(sp, "error") : "the node has no source extent";
	return false;
    }
    size_t at = 0, end = 0;
    if ( !span_extent(b, *span, at, end) )
    {
	why = "the node's extent is outside the text";
	return false;
    }
    const int64_t l0 = int_field(*span, "line"), c0 = int_field(*span, "column");
    const int64_t l1 = int_field(*span, "end_line"), c1 = int_field(*span, "end_column");
    size_t s0 = 0, n0 = 0, s1 = 0, n1 = 0;
    b.line_span((size_t)l0, s0, n0);			// in the text: span_extent read them
    b.line_span((size_t)l1, s1, n1);
    const size_t eol = s1 + n1;			// line l1's end, before its '\n'
    const size_t past = eol < t.size() ? eol + 1 : t.size();	// past its '\n'
    const size_t ind = std::min(t.find_first_not_of(" \t", s0), s0 + n0);
    const std::string indent = t.substr(s0, ind - s0);
    const bool owns = blanks(t, s0, at) && blanks(t, end, eol);
    p.target = sp;
    p.text = t.substr(at, end - at);
    p.at = at;
    p.del = 0;
    p.line = l0;
    p.column = c0;
    switch ( v )
    {
    case edit_verb::replace:
	p.del = end - at;
	p.ins = src;
	break;
    case edit_verb::remove:
	if ( owns )
	{
	    p.at = s0;				// the line(s) whole, newline included
	    p.del = past - s0;
	}
	else
	    p.del = end - at;
	break;
    case edit_verb::insert:
	if ( !owns )
	{
	    // The target shares its line: one space apart at its own
	    // boundary, never at the line's edge beside a neighbour.
	    if ( before )
		p.ins = src + " ";
	    else
	    {
		p.at = end;
		p.ins = " " + src;
		p.line = l1;
		p.column = c1 + 1;
	    }
	    break;
	}
	p.column = (int64_t)indent.size();
	if ( before )
	{
	    p.at = s0;
	    p.ins = indent + src + "\n";
	}
	else
	{
	    p.at = past;
	    p.line = l1 + 1;
	    // A target that ends the text with no newline: the fragment still
	    // gets a line of its own.
	    p.ins = past == eol ? "\n" + indent + src + "\n" : indent + src + "\n";
	}
	break;
    }
    return true;
}

value op_value(const std::string &verb_name, const value &args, const edit_plan &p)
{
    fields splice;
    splice["at"] = value((int64_t)p.at);
    splice["del"] = value((int64_t)p.del);
    splice["ins"] = value(p.ins);
    fields f;
    f["verb"] = value(verb_name);
    f["where"] = value(text_field(args, "where"));
    f["target"] = p.target;
    f["text"] = value(p.text);
    f["src"] = value(text_field(args, "src"));
    f["splice"] = value::make_object(splice);
    f["line"] = value(p.line);
    f["column"] = value(p.column);
    return value::make_object(f);
}

const edit_row *edit_row_of(const std::string &verb)
{
    for ( size_t i = 0; i < sizeof(edit_rows) / sizeof(edit_rows[0]); ++i )
	if ( verb == edit_rows[i].name )
	    return &edit_rows[i];
    return 0;
}

// An op's splice.
void splice_of(const value &op, size_t &at, size_t &del, std::string &ins)
{
    const value &s = object_field(op, "splice");
    at = (size_t)int_field(s, "at");
    del = (size_t)int_field(s, "del");
    ins = text_field(s, "ins");
}

// The ops' order of application: highest offset first, so an earlier splice
// never moves a later target.
std::vector<size_t> splice_order(const value &ops)
{
    std::vector<size_t> order;
    const std::vector<value> &v = ops.as_array();
    for ( size_t i = 0; i < v.size(); ++i )
	order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&v](size_t a, size_t b) {
	return int_field(object_field(v[a], "splice"), "at")
	    > int_field(object_field(v[b], "splice"), "at");
    });
    return order;
}

// The text with every op's splice applied.
std::string candidate_of(const std::string &t, const value &ops)
{
    std::string cand = t;
    std::vector<size_t> order = splice_order(ops);
    for ( size_t i = 0; i < order.size(); ++i )
    {
	size_t at, del;
	std::string ins;
	splice_of(ops.as_array()[order[i]], at, del, ins);
	if ( at <= cand.size() )
	    cand.replace(at, std::min(del, cand.size() - at), ins);
    }
    return cand;
}

// The touched lines, now and after (a proposal's preview): the first and
// last line any splice touches, and the same region in the candidate.
value edit_preview(const hub::text_buffer &b, const std::string &t,
		   const std::string &cand, const value &ops)
{
    size_t lo = t.size(), hi = 0;
    int64_t delta = 0;
    for ( const value &op : ops.as_array() )
    {
	size_t at, del;
	std::string ins;
	splice_of(op, at, del, ins);
	lo = std::min(lo, at);
	hi = std::max(hi, at + del);
	delta += (int64_t)ins.size() - (int64_t)del;
    }
    const size_t line0 = b.line_of(lo);
    const size_t line1 = b.line_of(hi > lo ? hi - 1 : lo);
    size_t s0 = 0, n0 = 0, s1 = 0, n1 = 0;
    b.line_span(line0, s0, n0);
    if ( !b.line_span(line1, s1, n1) )
	s1 = t.size(), n1 = 0;
    const size_t end = std::max(s1 + n1, hi);
    fields f;
    f["line0"] = value((int64_t)line0);
    f["line1"] = value((int64_t)line1);
    f["before"] = value(t.substr(s0, end - s0));
    f["after"] = value(cand.substr(s0, (size_t)((int64_t)end + delta) - s0));
    return value::make_object(f);
}

// Every edit's preconditions: the served document has text, is not
// read-only, and parses (`h`, its current parse).
bool edit_ready(const context &c, const hub::text_buffer *&b, int64_t &h, value &result)
{
    b = c.w.text_of(c.doc);
    if ( !b )
    {
	result = refusal("the served document has no text");
	return false;
    }
    if ( c.w.doc_of(c.doc) && c.w.doc_of(c.doc)->read_only() )
    {
	result = refusal("the document is read-only");
	return false;
    }
    h = lang::document_handle(c.w, c.doc);
    if ( !h )
    {
	result = refusal("the compiler does not parse the served document");
	return false;
    }
    return true;
}

// The candidate's verdict: false, with the refusal and the candidate's
// diagnostics in `result`, when it adds an error.
bool edit_accepted(int64_t h, const std::string &cand, const char *what,
		   value &diags, value &result)
{
    std::string text = cand;
    if ( madc_parse_would_accept(&diags, h, &text) )
	return true;
    result = refusal(std::string(what) + " rejected: the result introduces a"
		     " parse/semantic error (nothing changed)");
    result.object()["diagnostics"] = diags;
    return false;
}

// One edit tool call: the op planned, then landed — or, for a proposer (or
// a caller asking `propose`), validated and stored as a proposal.
bool graph_edit(const char *verb_name, const registry &r, const context &c,
		const value &args, value &result)
{
    value op;
    std::string why;
    if ( !plan_op(c, verb_name, args, op, why) )
    {
	result = refusal(why);
	return false;
    }
    const value ops = value::make_array(std::vector<value>(1, op));
    const value *pv = field(args, "propose");
    if ( c.caller != tier::proposer && !(pv && pv->is_boolean() && pv->as_boolean()) )
	return land_ops(r, c, ops, 0, result);
    if ( !r.memory() )
    {
	result = refusal("no project memory holds proposals here");
	return false;
    }
    const hub::text_buffer *b = 0;
    int64_t h = 0;
    if ( !edit_ready(c, b, h, result) )
	return false;
    const std::string t = b->text();
    const std::string cand = candidate_of(t, ops);
    value diags;
    if ( !edit_accepted(h, cand, "proposal", diags, result) )
	return false;
    fields prop;
    prop["ops"] = ops;
    prop["preview"] = edit_preview(*b, t, cand, ops);
    prop["diagnostics"] = diags;
    return r.memory()->propose(c, value::make_object(prop), result);
}

} // namespace

bool plan_op(const context &c, const std::string &verb, const value &args,
	     value &op, std::string &why)
{
    const edit_row *row = edit_row_of(verb);
    if ( !row )
    {
	why = "no edit is named '" + verb + "'";
	return false;
    }
    const hub::text_buffer *b = 0;
    int64_t h = 0;
    value refused;
    if ( !edit_ready(c, b, h, refused) )
    {
	why = text_field(refused, "error");
	return false;
    }
    const int64_t routed = madc_graph_route(h, int_field(args, "id"));
    if ( routed && routed != h )
    {
	why = "a revision's nodes are read-only: edit the live document's";
	return false;
    }
    const std::string t = b->text();
    edit_plan p;
    if ( !plan_edit(row->v, *b, t, h, args, p, why) )
	return false;
    op = op_value(row->name, args, p);
    return true;
}

bool land_ops(const registry &r, const context &c, const value &ops,
	      int64_t proposal, value &result)
{
    if ( !ops.is_array() || ops.as_array().empty() )
    {
	result = refusal("no edit to land");
	return false;
    }
    const hub::text_buffer *b = 0;
    int64_t h = 0;
    if ( !edit_ready(c, b, h, result) )
	return false;
    const std::string t = b->text();
    value diags;
    if ( !edit_accepted(h, candidate_of(t, ops), "edit", diags, result) )
	return false;
    hub::mutation_context mc(c.w);
    fields meta;
    meta["kind"] = value("nodeop");
    meta["verb"] = object_field(ops.as_array()[0], "verb");
    mc.doc_checkpoint(c.doc, value::make_object(meta));	// ONE undo step
    std::vector<size_t> order = splice_order(ops);
    for ( size_t i = 0; i < order.size(); ++i )
    {
	const value &op = ops.as_array()[order[i]];
	size_t at, del;
	std::string ins;
	splice_of(op, at, del, ins);
	if ( r.memory() )
	{
	    fields rec;
	    rec["verb"] = object_field(op, "verb");
	    rec["target"] = object_field(op, "target");
	    rec["src"] = object_field(op, "src");
	    if ( proposal > 0 )
		rec["proposal"] = value(proposal);
	    r.memory()->applied(c, value::make_object(rec));
	}
	if ( !mc.text_replace(c.doc, at, del, ins) )
	{
	    result = refusal("the document refused the edit");
	    return false;
	}
    }
    h = lang::document_handle(c.w, c.doc);		// the edited text's parse
    const value &first = ops.as_array()[0];
    value node = value::make_object();
    if ( text_field(first, "verb") != "graph.delete" && h )
	madc_graph_at(&node, h, int_field(first, "line"), int_field(first, "column"));
    size_t at, del;
    std::string ins;
    splice_of(first, at, del, ins);
    fields f;
    f["ok"] = value(true);
    f["node"] = node;
    f["splice_at"] = value((int64_t)at);
    f["splice_del"] = value((int64_t)del);
    result = value::make_object(f);
    return true;
}

void add_graph_tools(registry &r)
{
    const registry *reg = &r;
    for ( size_t i = 0; i < sizeof(verb_rows) / sizeof(verb_rows[0]); ++i )
    {
	const verb v = verb_rows[i].v;
	tool t;
	t.name = verb_rows[i].name;
	t.description = verb_rows[i].description;
	t.input_schema = schema_of(verb_rows[i].s);
	t.least = tier::observer;
	t.run = [v, reg](const context &c, const value &args, value &result) {
	    return graph_read(v, *reg, c, args, result);
	};
	r.add(t);
    }
    tool src;
    src.name = "graph.source";
    src.description = "a node's source text (a live or a revision's id)";
    src.input_schema = schema_of(shape::id);
    src.least = tier::observer;
    src.run = [reg](const context &c, const value &args, value &result) {
	return graph_source(*reg, c, args, result);
    };
    r.add(src);
    for ( size_t i = 0; i < sizeof(edit_rows) / sizeof(edit_rows[0]); ++i )
    {
	const edit_verb v = edit_rows[i].v;
	const char *name = edit_rows[i].name;
	const registry *reg = &r;
	tool t;
	t.name = name;
	t.description = edit_rows[i].description;
	t.input_schema = edit_schema(v);
	t.least = tier::proposer;
	t.run = [name, reg](const context &c, const value &args, value &result) {
	    return graph_edit(name, *reg, c, args, result);
	};
	r.add(t);
    }
}

void add_history_tools(registry &r, history_source &h)
{
    r.set_history(&h);
    const registry *reg = &r;
    for ( const history_row &row : history_rows )
    {
	const history_verb v = row.v;
	tool t;
	t.name = row.name;
	t.description = row.description;
	t.input_schema = history_schema(v);
	t.least = tier::observer;
	t.run = [v, reg](const context &c, const value &args, value &result) {
	    return history_read(v, *reg, c, args, result);
	};
	r.add(t);
    }
}

} // namespace tools
} // namespace madc
