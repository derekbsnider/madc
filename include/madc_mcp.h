//////////////////////////////////////////////////////////////////////////
//									//
// madc::mcp — the Model Context Protocol (split arc 1C, plan		//
// 2026-10-09-madc-madcide-split-arc.md item 13). JSON-RPC 2.0, one	//
// message per line (MCP's stdio transport) — a server, and a client of	//
// another program's server (nexus.sync). A server serves a tool		//
// registry (include/madc_tools.h) over a document of a hub world (a	//
// project's: the one a call's `file` names):				//
// tools/list advertises the registry's rows, tools/call runs one at	//
// the caller's tier and answers its result as JSON text content (a	//
// refusal is isError content). handle() answers one decoded message and	//
// knows no transport; serve() is the line loop.			//
//									//
// Thread contract (C++ standard library convention): one server, used	//
// from one thread, over a registry and a world that thread owns; one	//
// client per channel, used from one thread.				//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_MCP_H
#define __MADC_MCP_H 1

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "madc_tools.h"

namespace madc {

class value;
class channel;

namespace mcp {

class server
{
public:
    // `tools` and `w` outlive the server; `doc` is the document tools act
    // on; `caller` the client's tier and `actor` who it is (the id its
    // writes are recorded under; -1 = the local operator).
    server(tools::registry &tools, hub::world &w, hub::entity_id doc,
	   tools::tier caller, int64_t actor = -1);

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
    int64_t _actor;
    documents _documents;
};

// A client of one MCP server over `io` (one message per line — a child's
// stdio through exec://): each request waits at most `deadline_ms` for its
// reply, matched by id; a notification, another id's message and a line
// that is not JSON are skipped, never trusted. A refused call answers
// {error, code?} in `result` (a gone server's exit status is the channel's
// once its owner closes it).
class client
{
public:
    explicit client(channel &io) : _io(io), _next(1) {}
    // initialize (this client's protocol revision and info), then the
    // initialized notification.
    bool start(value &result, int64_t deadline_ms);
    bool call(const char *method, const value &params, value &result,
	      int64_t deadline_ms);
    void notify(const char *method, const value &params);
    // A tool result's ROWS: its first text content read as JSON — an array,
    // or an object's "rows"; none otherwise.
    static std::vector<value> rows_of(const value &tool_result);

private:
    channel &_io;
    int64_t _next;
};

} // namespace mcp
} // namespace madc

#endif // __MADC_MCP_H
