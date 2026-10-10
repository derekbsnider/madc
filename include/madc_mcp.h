//////////////////////////////////////////////////////////////////////////
//									//
// madc::mcp — the Model Context Protocol server (split arc 1C, plan	//
// 2026-10-09-madc-madcide-split-arc.md item 13). JSON-RPC 2.0, one	//
// message per line (MCP's stdio transport). A server serves a tool	//
// registry (include/madc_tools.h) over a document of a hub world (a	//
// project's: the one a call's `file` names):				//
// tools/list advertises the registry's rows, tools/call runs one at	//
// the caller's tier and answers its result as JSON text content (a	//
// refusal is isError content). handle() answers one decoded message and	//
// knows no transport; serve() is the line loop.			//
//									//
// Thread contract (C++ standard library convention): one server, used	//
// from one thread, over a registry and a world that thread owns.	//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_MCP_H
#define __MADC_MCP_H 1

#include <cstdint>
#include <functional>
#include <string>

#include "madc_tools.h"

namespace madc {

class value;
class channel;

namespace mcp {

class server
{
public:
    // `tools` and `w` outlive the server; `doc` is the document tools act on.
    server(tools::registry &tools, hub::world &w, hub::entity_id doc,
	   tools::tier caller);

    // One decoded message. True when it is answered (`reply`); a
    // notification is never answered.
    bool handle(const value &message, value &reply);

    // The line loop over `io`: read one message, answer it, write the reply
    // and a newline — until the client goes away. A line that is not JSON
    // answers -32700. Returns 0.
    int serve(channel &io);

    // The protocol revision answered when the client asks for one this
    // server does not speak.
    static const char *latest_version();

    // A project's documents (madc --mcp --project): a call's `file`
    // argument to its document (the project's TU, opened on first use); 0,
    // with `why`, refused. Set, every tool advertises `file`, and a call
    // naming one acts on that document — naming none, on the served one.
    typedef std::function<hub::entity_id(const std::string &file, std::string &why)>
	documents;
    void set_documents(const documents &d) { _documents = d; }

private:
    tools::registry &_tools;
    hub::world &_w;
    hub::entity_id _doc;
    tools::tier _caller;
    documents _documents;
};

} // namespace mcp
} // namespace madc

#endif // __MADC_MCP_H
