//////////////////////////////////////////////////////////////////////////
//									//
// madc_tools.cpp — the tool registry and madc's code-graph tools (split	//
// arc 1C; contract in include/madc_tools.h). The graph tools are the	//
// madc_graph_* reads over the served document's parse handle		//
// (madc::lang::document_handle, the one door): this file converts a	//
// tool's arguments to the read's and nothing else.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <map>
#include <string>
#include <vector>

#include "ns_common.h"		// the madc_graph_* reads
#include "madc_tools.h"
#include "madc_lang.h"		// document_handle: the served document's parse

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
}

} // namespace tools
} // namespace madc
