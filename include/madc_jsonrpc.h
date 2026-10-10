//////////////////////////////////////////////////////////////////////////
//									//
// madc::jsonrpc — JSON-RPC 2.0's envelopes and a message's members, the	//
// ONE spelling madc's protocol servers share (the LSP, src/madc_lsp.cpp;	//
// MCP, src/madc_mcp.cpp). JSON text converts through the one		//
// value<->JSON bridge (madcdis/world_text.h); the encoding replaces a	//
// string's bytes that are not UTF-8, never throws.			//
//									//
// Thread contract: pure functions over their arguments.		//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_JSONRPC_H
#define __MADC_JSONRPC_H 1

#include <cstdint>
#include <string>

namespace madc {

class value;

namespace jsonrpc {

// JSON-RPC's error codes, and the LSP's two in its reserved range.
enum class error_code : int64_t
{
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    server_not_initialized = -32002,	// LSP
    request_failed = -32803		// LSP
};

value response(const value &id, const value &result);
value error_response(const value &id, error_code code, const std::string &message);
value notification(const char *method, const value &params);
// A request a server sends; its id is the server's own.
value request(int64_t id, const char *method, const value &params);

// A message's members: NULL / "" / `dflt` / a null value when absent or of
// another type.
const value *field(const value &v, const char *name);
std::string text_field(const value &v, const char *name);
int64_t int_field(const value &v, const char *name, int64_t dflt = 0);
const value &object_field(const value &v, const char *name);

// One message's JSON text to a value; false when it is not JSON.
bool decode(const std::string &text, value &out);
// A value as one line of JSON text.
std::string encode(const value &v);

} // namespace jsonrpc
} // namespace madc

#endif // __MADC_JSONRPC_H
