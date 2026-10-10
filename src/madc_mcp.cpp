//////////////////////////////////////////////////////////////////////////
//									//
// madc_mcp.cpp — the Model Context Protocol server (contract in		//
// include/madc_mcp.h). The methods convert once to an enum; the tools	//
// are the registry's.							//
//									//
//////////////////////////////////////////////////////////////////////////

#include <map>
#include <string>
#include <vector>

#include "madc_mcp.h"
#include "madc_jsonrpc.h"
#include "libmadc/sysinfo.h"	// madc::sys.version: serverInfo
#include "madcdis/channel.h"	// serve: one message per line

namespace madc {
namespace mcp {

using namespace jsonrpc;

namespace {

typedef std::map<std::string, value> fields;

enum class method : unsigned char
{
    initialize, initialized, ping, tools_list, tools_call
};

struct method_row
{
    const char *name;
    method m;
};

const method_row method_rows[] = {
    { "initialize", method::initialize },
    { "notifications/initialized", method::initialized },
    { "ping", method::ping },
    { "tools/list", method::tools_list },
    { "tools/call", method::tools_call },
};

const method_row *method_of(const std::string &name)
{
    for ( size_t i = 0; i < sizeof(method_rows) / sizeof(method_rows[0]); ++i )
	if ( name == method_rows[i].name )
	    return &method_rows[i];
    return 0;
}

// The protocol revisions this server speaks, newest first: the tools
// surface is the same in each.
const char *const versions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

std::string version_for(const std::string &asked)
{
    for ( size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i )
	if ( asked == versions[i] )
	    return asked;
    return versions[0];
}

// A tool's answer as MCP content: its result as JSON text; a refusal (a
// result carrying "error") is isError.
value tool_content(const value &result, bool ok)
{
    fields item;
    item["type"] = value("text");
    item["text"] = value(encode(result));
    std::vector<value> content(1, value::make_object(item));
    fields r;
    r["content"] = value::make_array(content);
    r["isError"] = value(!ok);
    return value::make_object(r);
}

} // namespace

server::server(tools::registry &t, hub::world &w, hub::entity_id doc,
	       tools::tier caller)
    : _tools(t), _w(w), _doc(doc), _caller(caller)
{
}

const char *server::latest_version()
{
    return versions[0];
}

bool server::handle(const value &message, value &reply)
{
    if ( !message.is_object() )
    {
	reply = error_response(value(), error_code::invalid_request,
			       "a message is a JSON object");
	return true;
    }
    const value *idp = field(message, "id");
    if ( !idp )
	return false;				// a notification: no reply
    const value id = *idp;
    const std::string name = text_field(message, "method");
    if ( name.empty() )
    {
	if ( field(message, "result") || field(message, "error") )
	    return false;			// a response to nothing this server sent
	reply = error_response(id, error_code::invalid_request,
			       "a message names its method");
	return true;
    }
    const method_row *row = method_of(name);
    if ( !row )
    {
	reply = error_response(id, error_code::method_not_found,
			       "Method not found: " + name);
	return true;
    }
    const value &params = object_field(message, "params");
    switch ( row->m )
    {
    case method::initialize:
    {
	fields toolscap;
	toolscap["listChanged"] = value(false);
	fields caps;
	caps["tools"] = value::make_object(toolscap);
	fields info;
	info["name"] = value("madc");
	info["version"] = value(sys.version);
	fields r;
	r["protocolVersion"] = value(version_for(text_field(params, "protocolVersion")));
	r["capabilities"] = value::make_object(caps);
	r["serverInfo"] = value::make_object(info);
	reply = response(id, value::make_object(r));
	return true;
    }
    case method::initialized:
	return false;
    case method::ping:
	reply = response(id, value::make_object());
	return true;
    case method::tools_list:
    {
	std::vector<value> rows;
	for ( const tools::tool &t : _tools.all() )
	{
	    fields f;
	    f["name"] = value(t.name);
	    f["description"] = value(t.description + " (requires "
				     + tools::tier_name(t.least) + ")");
	    f["inputSchema"] = t.input_schema;
	    rows.push_back(value::make_object(f));
	}
	fields r;
	r["tools"] = value::make_array(rows);
	reply = response(id, value::make_object(r));
	return true;
    }
    case method::tools_call:
    {
	const std::string tool = text_field(params, "name");
	if ( !_tools.find(tool) )
	{
	    reply = error_response(id, error_code::invalid_params,
				   "Unknown tool: " + tool);
	    return true;
	}
	tools::context c = { _w, _doc, _caller, -1 };	// the lone local client
	value result;
	bool ok = _tools.call(c, tool, object_field(params, "arguments"), result);
	reply = response(id, tool_content(result, ok));
	return true;
    }
    }
    return false;
}

int server::serve(channel &io)
{
    std::string line;
    while ( io.readline(line) )
    {
	if ( line.find_first_not_of(" \t\r") == std::string::npos )
	    continue;				// a blank line carries no message
	value message, reply;
	bool answered = true;
	if ( !decode(line, message) )
	    reply = error_response(value(), error_code::parse_error,
				   "the message is not JSON");
	else
	    answered = handle(message, reply);
	if ( answered )
	{
	    std::string text = encode(reply) + "\n";
	    io.write(text);
	}
    }
    return 0;
}

} // namespace mcp
} // namespace madc
