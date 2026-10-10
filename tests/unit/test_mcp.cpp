// Unit battery for madc::mcp::server and the tool registry (split arc 1C;
// include/madc_mcp.h, include/madc_tools.h). Driven through handle(), one
// decoded message at a time. ORACLE: the MCP specification (2025-06-18:
// initialize answers the client's protocolVersion when the server speaks it,
// else its own; tools/list rows {name, description, inputSchema}; tools/call
// answers content with isError for a tool's own failure, and the JSON-RPC
// error -32602 for an unknown tool); JSON-RPC 2.0 for -32600 / -32601 and an
// unanswered notification; madcide's testmadcide_serve_graph record for the
// code-graph results' shapes (Function / Struct nodes).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madc_mcp.h"
#include "madc_tools.h"
#include "madc_documents.h"
#include "madcdis/hub.h"
#include "madcdis/verbs.h"		// mutation_context: the document's text
#include "madcdis/world_text.h"	// the one value<->JSON bridge

using nlohmann::json;
using madc::hub::detail::wt_json_to_value;
using madc::hub::detail::wt_value_to_json;

namespace {

const char *CALC =
    "struct Point { int x; int y; };\n"		// 1
    "\n"					// 2
    "long add(long a, long b)\n"		// 3
    "{\n"					// 4
    "    return a + b;\n"			// 5
    "}\n"					// 6
    "\n"					// 7
    "int main(void)\n"				// 8
    "{\n"					// 9
    "    return (int)add(2, 3);\n"		// 10
    "}\n";					// 11

// A server over one C document, the code-graph tools registered.
struct fixture
{
    madc::hub::world w;
    madc::tools::registry tools;
    madc::hub::entity_id doc;
    madc::mcp::server srv;
    long next_id;
    explicit fixture(madc::tools::tier caller = madc::tools::tier::owner,
		     const char *path = "/mcp-unit/calc.c")
	: doc(madc::document_new(w, path)),
	  srv(tools, w, doc, caller), next_id(1)
    {
	madc::hub::mutation_context mc(w);
	mc.text_load(doc, CALC);
	madc::tools::add_graph_tools(tools);
    }
    json send(const json &m)
    {
	madc::value reply;
	return srv.handle(wt_json_to_value(m), reply) ? wt_value_to_json(reply) : json();
    }
    json request(const std::string &method, const json &params)
    {
	return send({ { "jsonrpc", "2.0" }, { "id", next_id++ }, { "method", method },
		      { "params", params } });
    }
    // tools/call: the result, with the content's text decoded.
    json call(const std::string &name, const json &args, bool *is_error = 0)
    {
	json r = request("tools/call", { { "name", name }, { "arguments", args } });
	if ( is_error )
	    *is_error = r["result"]["isError"].get<bool>();
	return json::parse(r["result"]["content"][0]["text"].get<std::string>());
    }
};

bool has_node(const json &result, const char *kind, const char *name)
{
    for ( const json &n : result["nodes"] )
	if ( n["kind"] == kind && n["name"] == name )
	    return true;
    return false;
}

} // namespace

TEST_CASE("initialize answers the client's protocol version when it is spoken")
{
    fixture f;
    json init = f.request("initialize", { { "protocolVersion", "2024-11-05" },
					  { "capabilities", json::object() } });
    CHECK(init["result"]["protocolVersion"] == "2024-11-05");
    CHECK(init["result"]["serverInfo"]["name"] == "madc");
    CHECK(init["result"]["capabilities"]["tools"]["listChanged"] == false);
    CHECK(f.request("initialize", { { "protocolVersion", "1999-01-01" } })
	  ["result"]["protocolVersion"] == madc::mcp::server::latest_version());
    CHECK(f.send({ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } })
	  .is_null());					// a notification: no reply
    CHECK(f.request("ping", json::object())["result"] == json::object());
    CHECK(f.request("resources/list", json::object())["error"]["code"] == -32601);
    CHECK(f.send(json::array())["error"]["code"] == -32600);
}

TEST_CASE("tools/list advertises the registry's rows")
{
    fixture f;
    json tools = f.request("tools/list", json::object())["result"]["tools"];
    REQUIRE(tools.is_array());
    CHECK(tools.size() == f.tools.all().size());
    bool symbols = false;
    for ( const json &t : tools )
	if ( t["name"] == "graph.definition" )
	{
	    CHECK(t["inputSchema"]["type"] == "object");
	    CHECK(t["inputSchema"]["properties"]["name"]["type"] == "string");
	    CHECK(t["description"].get<std::string>().find("requires observer")
		  != std::string::npos);
	}
	else if ( t["name"] == "graph.symbols" )
	    symbols = true;
    CHECK(symbols);
}

TEST_CASE("tools/call runs a code-graph read over the served document")
{
    fixture f;
    bool err = true;
    json syms = f.call("graph.symbols", json::object(), &err);
    CHECK_FALSE(err);
    CHECK(has_node(syms, "Function", "add"));
    CHECK(has_node(syms, "Function", "main"));

    json def = f.call("graph.definition", { { "name", "Point" } }, &err);
    CHECK_FALSE(err);
    CHECK(has_node(def, "Struct", "Point"));

    json add = f.call("graph.definition", { { "name", "add" } });
    REQUIRE(add["nodes"].size() >= 1u);
    json span = f.call("graph.span", { { "id", add["nodes"][0]["id"] } }, &err);
    CHECK_FALSE(err);
    CHECK(span["span"]["line"] == 3);
    CHECK(span["span"]["end_line"] == 6);

    json callers = f.call("graph.callers", { { "func", add["nodes"][0]["id"] } });
    CHECK(has_node(callers, "Function", "main"));

    CHECK(f.request("tools/call", { { "name", "graph.bogus" },
				    { "arguments", json::object() } })
	  ["error"]["code"] == -32602);
}

TEST_CASE("a tool above the caller's tier is refused as isError content")
{
    fixture f(madc::tools::tier::observer);
    madc::tools::tool t;
    t.name = "test.edit";
    t.description = "an edit";
    t.input_schema = madc::value::make_object();
    t.least = madc::tools::tier::editor;
    t.run = [](const madc::tools::context &, const madc::value &, madc::value &r) {
	r = madc::value("edited");
	return true;
    };
    REQUIRE(f.tools.add(t));
    CHECK_FALSE(f.tools.add(t));			// the name is taken
    bool err = false;
    json r = f.call("test.edit", json::object(), &err);
    CHECK(err);
    CHECK(r["error"].get<std::string>().find("requires editor (you are observer)")
	  != std::string::npos);
}

TEST_CASE("a document the compiler does not parse refuses the graph tools")
{
    fixture f(madc::tools::tier::owner, "/mcp-unit/notes.txt");
    bool err = false;
    json r = f.call("graph.symbols", json::object(), &err);
    CHECK(err);
    CHECK(r["error"].get<std::string>().find("does not parse") != std::string::npos);
}
