//////////////////////////////////////////////////////////////////////////
//									//
// madc::tool_host — the tools a madc process serves (split arc 1D, plan	//
// 2026-10-09-madc-madcide-split-arc.md item 19): ONE registry over a	//
// hub world's documents holding the code-graph tools, the project	//
// memory and its tools (the registry's edit memory), and every loaded	//
// module's (madcgit: the version history). `madc --mcp` serves one; a	//
// dialect host (madcide) builds one over its ui:: world, adds its own	//
// commands as tools, calls them, answers MCP messages over its seats and	//
// reads the memory's change log.					//
//									//
// Thread contract (C++ standard library convention): a host is used	//
// from its world's thread; a script tool runs on that thread, inside	//
// the call that names it.						//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_TOOL_HOST_H
#define __MADC_TOOL_HOST_H 1

#include <cstdint>

#include "libmadc/value.h"

class Program;

namespace madc {

class channel;
namespace hub { class world; }
namespace tools { class registry; }
namespace memory { class project; }

class tool_host
{
public:
    // Over `w`'s documents, with the modules `prog` loaded (none for 0).
    // Construct it before any document opens: the memory's baseline is a
    // document's text as loaded.
    tool_host(hub::world &w, Program *prog);
    ~tool_host();
    tools::registry &registry();
    memory::project &memory();

    // ---- the dialect face (<ns_madc>) ---------------------------------
    // Declaration-only there; resolved mangled-direct here. LAYOUT
    // CONTRACT with <ns_madc>: a single pointer member, append-only. A
    // tier is a <bits/tool_enums> tool_tier code; `actor` the caller's id
    // (-1 = the local operator).

    // Over a ui:: world handle's documents, with the running program's
    // modules. An unknown handle builds a host that refuses every call.
    explicit tool_host(int64_t w);
    // The memory's file (a manifest's <base>.events): refused, with `why`
    // (bound to no file then). unbind: written to no file from here.
    bool bind(const char *events_path, value &why);
    void unbind();
    void set_manifest(const char *path);
    // A host tool: `fn` answers into `result` — false refuses, `result`
    // then {error}; `name` is the tool called (one function may serve a
    // family). `input_schema` a JSON schema object ({} = no arguments).
    // False when the name is empty or taken.
    typedef bool (*script_tool)(value &result, const char *name, int64_t w, int64_t doc,
				value &args, int64_t caller, int64_t actor);
    bool add(const char *name, const char *description, value &input_schema,
	     int64_t least, script_tool fn);
    // [{name, description, inputSchema, least}] in registration order.
    value &list(value &out);
    bool call(value &result, int64_t doc, const char *name, value &args,
	      int64_t caller, int64_t actor);
    // One decoded MCP message for `doc`: true when answered (`reply`); a
    // notification is not.
    bool mcp_handle(value &reply, value &message, int64_t doc, int64_t caller,
		    int64_t actor);
    // The MCP line loop over `io` until the client goes away (0).
    int64_t serve_mcp(channel &io, int64_t doc, int64_t caller);
    // The change log: its head seq, the records after `since` (a document's
    // `doc` is the writing session's entity), a document's text at `upto`.
    int64_t head();
    value &records(value &out, int64_t since);
    // An application's record into the change log (memory::project::note):
    // its seq, 0 refused.
    int64_t note(value &record, int64_t doc);
    value &replay(value &out, int64_t doc, int64_t upto);

private:
    struct impl;
    impl *_impl;
    // _impl while its world is open: a handle-built host (the dialect's)
    // asks madc_ui_world at every use, so a closed world refuses (NULL).
    impl *live();
    tool_host(const tool_host &);
    tool_host &operator=(const tool_host &);
};

} // namespace madc

#endif // __MADC_TOOL_HOST_H
