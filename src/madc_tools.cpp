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
#include "madcdis/hub.h"		// the served document's text
#include "madcdis/verbs.h"		// mutation_context: an edit lands through it

namespace madc {
namespace tools {

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

value property(const char *type, const char *description)
{
    fields f;
    f["type"] = value(type);
    f["description"] = value(description);
    return value::make_object(f);
}

value schema_of(shape s)
{
    fields p;
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
    fields f;
    f["type"] = value("object");
    f["properties"] = value::make_object(p);
    return value::make_object(f);
}

const value *arg(const value &args, const char *name)
{
    if ( !args.is_object() )
	return 0;
    const fields &f = args.as_object();
    fields::const_iterator i = f.find(name);
    return i == f.end() || i->second.is_null() ? 0 : &i->second;
}

int64_t int_arg(const value &args, const char *name, int64_t dflt = 0)
{
    const value *a = arg(args, name);
    return a && a->is_integer() ? a->as_integer() : dflt;
}

std::string text_arg(const value &args, const char *name)
{
    const value *a = arg(args, name);
    return a && a->is_string() ? a->as_string() : std::string();
}

bool has_error(const value &v)
{
    return v.is_object() && v.as_object().count("error") != 0;
}

bool graph_read(verb v, const context &c, const value &args, value &result)
{
    int64_t h = lang::document_handle(c.w, c.doc);
    if ( !h )
    {
	result = refusal("the compiler does not parse the served document");
	return false;
    }
    switch ( v )
    {
    case verb::symbols:
	madc_graph_symbols(&result, h);
	break;
    case verb::definition:
    {
	std::string name = text_arg(args, "name");
	madc_graph_definition(&result, h, &name);
	break;
    }
    case verb::node:
	madc_graph_node(&result, h, int_arg(args, "id"));
	break;
    case verb::type_of:
	madc_graph_type_of(&result, h, int_arg(args, "id"));
	break;
    case verb::members:
	madc_graph_members(&result, h, int_arg(args, "id"));
	break;
    case verb::bases:
	madc_graph_bases(&result, h, int_arg(args, "id"));
	break;
    case verb::enclosing:
	madc_graph_enclosing(&result, h, int_arg(args, "line"), int_arg(args, "column"));
	break;
    case verb::body:
	madc_graph_body(&result, h, int_arg(args, "func"), int_arg(args, "depth", -1));
	break;
    case verb::children:
	madc_graph_children(&result, h, int_arg(args, "id"), int_arg(args, "depth", 1));
	break;
    case verb::callees:
	madc_graph_callees(&result, h, int_arg(args, "func"));
	break;
    case verb::callers:
	madc_graph_callers(&result, h, int_arg(args, "func"));
	break;
    case verb::references:
	madc_graph_references(&result, h, int_arg(args, "func"));
	break;
    case verb::search:
    {
	std::string kind = text_arg(args, "kind"), name = text_arg(args, "name");
	madc_graph_search(&result, h, &kind, &name);
	break;
    }
    case verb::impact:
	madc_graph_impact(&result, h, int_arg(args, "id"));
	break;
    case verb::span:
	madc_graph_span(&result, h, int_arg(args, "id"));
	break;
    case verb::at:
	madc_graph_at(&result, h, int_arg(args, "line"), int_arg(args, "column"));
	break;
    }
    return !has_error(result);
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
    fields f;
    f["type"] = value("object");
    f["properties"] = value::make_object(p);
    return value::make_object(f);
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
    const std::string src = text_arg(args, "src");
    const std::string where = text_arg(args, "where");
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
    madc_graph_span(&sp, h, int_arg(args, "id"));
    const value *span = arg(sp, "span");
    if ( !span )
    {
	why = has_error(sp) ? text_arg(sp, "error") : "the node has no source extent";
	return false;
    }
    const int64_t l0 = int_arg(*span, "line"), c0 = int_arg(*span, "column");
    const int64_t l1 = int_arg(*span, "end_line"), c1 = int_arg(*span, "end_column");
    size_t s0 = 0, n0 = 0, s1 = 0, n1 = 0;
    if ( l0 < 1 || l1 < l0 || !b.line_span((size_t)l0, s0, n0)
	 || !b.line_span((size_t)l1, s1, n1) )
    {
	why = "the node's extent is outside the text";
	return false;
    }
    const size_t at = s0 + (size_t)c0, end = s1 + (size_t)c1;
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

// The touched lines, now and after (a proposal's preview).
value edit_preview(const hub::text_buffer &b, const std::string &t,
		   const std::string &cand, const edit_plan &p)
{
    const size_t line0 = b.line_of(p.at);
    const size_t line1 = b.line_of(p.del ? p.at + p.del - 1 : p.at);
    size_t s0 = 0, n0 = 0, s1 = 0, n1 = 0;
    b.line_span(line0, s0, n0);
    if ( !b.line_span(line1, s1, n1) )
	s1 = t.size(), n1 = 0;
    const size_t end = std::max(s1 + n1, p.at + p.del);
    const size_t after_end = end + p.ins.size() - p.del;
    fields f;
    f["line0"] = value((int64_t)line0);
    f["line1"] = value((int64_t)line1);
    f["before"] = value(t.substr(s0, end - s0));
    f["after"] = value(cand.substr(s0, after_end - s0));
    return value::make_object(f);
}

value op_value(edit_verb v, const std::string &verb_name, const value &args,
	       const edit_plan &p)
{
    fields splice;
    splice["at"] = value((int64_t)p.at);
    splice["del"] = value((int64_t)p.del);
    splice["ins"] = value(p.ins);
    fields f;
    f["verb"] = value(verb_name);
    f["where"] = value(text_arg(args, "where"));
    f["target"] = p.target;
    f["text"] = value(p.text);
    f["src"] = value(text_arg(args, "src"));
    f["splice"] = value::make_object(splice);
    f["line"] = value(p.line);
    f["column"] = value(p.column);
    (void)v;
    return value::make_object(f);
}

// One edit: planned, its candidate validated by the compiler, then applied
// as one undo step — or stored as a proposal.
bool graph_edit(edit_verb v, const char *verb_name, const registry &r,
		const context &c, const value &args, value &result)
{
    const hub::text_buffer *b = c.w.text_of(c.doc);
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
    int64_t h = lang::document_handle(c.w, c.doc);
    if ( !h )
    {
	result = refusal("the compiler does not parse the served document");
	return false;
    }
    const std::string t = b->text();
    edit_plan p;
    std::string why;
    if ( !plan_edit(v, *b, t, h, args, p, why) )
    {
	result = refusal(why);
	return false;
    }
    std::string cand = t;
    cand.replace(p.at, p.del, p.ins);
    const value *pv = arg(args, "propose");
    const bool propose = c.caller == tier::proposer
	|| (pv && pv->is_boolean() && pv->as_boolean());
    value diags;
    std::string cand_c = cand;
    if ( !madc_parse_would_accept(&diags, h, &cand_c) )
    {
	result = refusal(std::string(propose ? "proposal" : "edit")
			 + " rejected: the result introduces a parse/semantic"
			 " error (nothing changed)");
	result.object()["diagnostics"] = diags;
	return false;
    }
    const value op = op_value(v, verb_name, args, p);
    if ( propose )
    {
	if ( !r.memory() )
	{
	    result = refusal("no project memory holds proposals here");
	    return false;
	}
	fields prop;
	prop["ops"] = value::make_array(std::vector<value>(1, op));
	prop["preview"] = edit_preview(*b, t, cand, p);
	prop["diagnostics"] = diags;
	return r.memory()->propose(c, value::make_object(prop), result);
    }
    hub::mutation_context mc(c.w);
    fields meta;
    meta["kind"] = value("nodeop");
    meta["verb"] = value(verb_name);
    mc.doc_checkpoint(c.doc, value::make_object(meta));	// ONE undo step
    if ( !mc.text_replace(c.doc, p.at, p.del, p.ins) )
    {
	result = refusal("the document refused the edit");
	return false;
    }
    h = lang::document_handle(c.w, c.doc);		// the edited text's parse
    if ( r.memory() )
    {
	fields rec;
	rec["verb"] = value(verb_name);
	rec["target"] = p.target;
	rec["src"] = value(text_arg(args, "src"));
	r.memory()->applied(c, value::make_object(rec));
    }
    value node = value::make_object();
    if ( v != edit_verb::remove && h )
	madc_graph_at(&node, h, p.line, p.column);
    fields f;
    f["ok"] = value(true);
    f["node"] = node;
    f["splice_at"] = value((int64_t)p.at);
    f["splice_del"] = value((int64_t)p.del);
    result = value::make_object(f);
    return true;
}

} // namespace

void add_graph_tools(registry &r)
{
    for ( size_t i = 0; i < sizeof(verb_rows) / sizeof(verb_rows[0]); ++i )
    {
	const verb v = verb_rows[i].v;
	tool t;
	t.name = verb_rows[i].name;
	t.description = verb_rows[i].description;
	t.input_schema = schema_of(verb_rows[i].s);
	t.least = tier::observer;
	t.run = [v](const context &c, const value &args, value &result) {
	    return graph_read(v, c, args, result);
	};
	r.add(t);
    }
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
	t.run = [v, name, reg](const context &c, const value &args, value &result) {
	    return graph_edit(v, name, *reg, c, args, result);
	};
	r.add(t);
    }
}

} // namespace tools
} // namespace madc
