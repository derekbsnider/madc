//////////////////////////////////////////////////////////////////////////
//									//
// madc_tool_host.cpp — the tools a madc process serves (contract in	//
// include/madc_tool_host.h).						//
//									//
//////////////////////////////////////////////////////////////////////////

#include <map>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"		// Program: the modules it loaded
#include "ns_common.h"		// madc_ui_world: a ui:: world handle's world
#include "madc_tool_host.h"
#include "madc_tools.h"
#include "madc_memory.h"
#include "madc_mcp.h"
#include "madc_jsonrpc.h"	// int_field: a record's seq
#include "madc_modules.h"	// madc_module_entry_point: a module's tools
#include "madc_module_tools.h"
#include "madcdis/hub.h"

namespace madc {

struct tool_host::impl
{
    hub::world &w;
    int64_t ui;				// the dialect's world handle (0: a C++ host)
    memory::project memory;
    tools::registry tools;
    impl(hub::world &world, int64_t handle) : w(world), ui(handle), memory(world) {}
    // A C++ host's world is its own; a dialect world handle may be closed
    // (ui::world_close frees it) while this host lives — asked now, as
    // madc::host_documents does.
    bool world_open() const { return !ui || madc_ui_world(ui) == &w; }
};

namespace {

void add_served_tools(tools::registry &tools, memory::project &memory, Program *prog)
{
    tools::add_graph_tools(tools);
    memory.add_tools(tools);
    if ( prog && prog->is_auto_library_loading_enabled() )
	for ( const dis::istring &spelling : prog->loaded_modules() )
	    if ( madc_module_tools_fn add = reinterpret_cast<madc_module_tools_fn>(
		     madc_module_entry_point(spelling, madc_module_entry::tools)) )
		add(tools);
}

tools::tier tier_of(int64_t code)
{
    return code <= 0 ? tools::tier::observer
	: code >= (int64_t)tools::tier::owner ? tools::tier::owner : (tools::tier)code;
}

} // namespace

tool_host::tool_host(hub::world &w, Program *prog) : _impl(new impl(w, 0))
{
    add_served_tools(_impl->tools, _impl->memory, prog);
}

tool_host::tool_host(int64_t w) : _impl(0)
{
    hub::world *world = madc_ui_world(w);
    if ( !world )
	return;
    _impl = new impl(*world, w);
    add_served_tools(_impl->tools, _impl->memory, Program::active_runtime_program());
}

tool_host::~tool_host()
{
    if ( _impl && !_impl->world_open() )
	_impl->memory.world_gone();
    delete _impl;
}

tool_host::impl *tool_host::live()
{
    return _impl && _impl->world_open() ? _impl : 0;
}

tools::registry &tool_host::registry()
{
    return _impl->tools;
}

memory::project &tool_host::memory()
{
    return _impl->memory;
}

bool tool_host::bind(const char *events_path, value &why)
{
    impl *im = live();
    std::string w;
    const bool ok = im && im->memory.bind(events_path ? events_path : "", w);
    why = value(im ? w : std::string("no world"));
    return ok;
}

void tool_host::unbind()
{
    impl *im = live();
    if ( im )
	im->memory.unbind();
}

void tool_host::set_manifest(const char *path)
{
    impl *im = live();
    if ( im )
	im->memory.set_manifest(path ? path : "");
}

bool tool_host::add(const char *name, const char *description, value &input_schema,
		    int64_t least, script_tool fn)
{
    impl *im = live();
    if ( !im || !fn )
	return false;
    tools::tool t;
    t.name = name ? name : "";
    t.description = description ? description : "";
    t.input_schema = input_schema.is_object() ? input_schema : tools::object_schema(
	std::map<std::string, value>());
    t.least = tier_of(least);
    const int64_t ui = im->ui;
    const std::string tool_name = t.name;
    t.run = [fn, ui, tool_name](const tools::context &c, const value &args, value &result) {
	value a = args;
	result = value();
	const bool ok = fn(result, tool_name.c_str(), ui, (int64_t)c.doc, a,
			   (int64_t)c.caller, c.actor);
	if ( !ok && !tools::refused(result) )
	    result = tools::refusal("refused");
	return ok;
    };
    return im->tools.add(t);
}

value &tool_host::list(value &out)
{
    impl *im = live();
    std::vector<value> rows;
    if ( im )
	for ( const tools::tool &t : im->tools.all() )
	{
	    std::map<std::string, value> r;
	    r["name"] = value(t.name);
	    r["description"] = value(t.description);
	    r["inputSchema"] = t.input_schema;
	    r["least"] = value((int64_t)t.least);
	    rows.push_back(value::make_object(r));
	}
    out = value::make_array(rows);
    return out;
}

bool tool_host::call(value &result, int64_t doc, const char *name, value &args,
		     int64_t caller, int64_t actor)
{
    impl *im = live();
    if ( !im )
    {
	result = tools::refusal("no world");
	return false;
    }
    tools::context c = { im->w, (hub::entity_id)doc, tier_of(caller), actor };
    return im->tools.call(c, name ? name : "", args, result);
}

bool tool_host::mcp_handle(value &reply, value &message, int64_t doc, int64_t caller,
			   int64_t actor)
{
    impl *im = live();
    if ( !im )
	return false;
    mcp::server s(im->tools, im->w, (hub::entity_id)doc, tier_of(caller), actor);
    return s.handle(message, reply);
}

int64_t tool_host::serve_mcp(channel &io, int64_t doc, int64_t caller)
{
    impl *im = live();
    if ( !im )
	return 1;
    mcp::server s(im->tools, im->w, (hub::entity_id)doc, tier_of(caller));
    return s.serve(io);
}

int64_t tool_host::head()
{
    impl *im = live();
    return im ? im->memory.head() : 0;
}

value &tool_host::records(value &out, int64_t since)
{
    impl *im = live();
    std::vector<value> rows;
    if ( im )
	for ( const value &rec : im->memory.records() )
	    if ( jsonrpc::int_field(rec, "seq") > since )
		rows.push_back(rec);
    out = value::make_array(rows);
    return out;
}

int64_t tool_host::note(value &record, int64_t doc)
{
    impl *im = live();
    return im ? im->memory.note(record, (hub::entity_id)doc) : 0;
}

value &tool_host::replay(value &out, int64_t doc, int64_t upto)
{
    impl *im = live();
    out = value(im ? im->memory.replay((hub::entity_id)doc, upto) : std::string());
    return out;
}

} // namespace madc
