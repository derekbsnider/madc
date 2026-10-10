//////////////////////////////////////////////////////////////////////////
//									//
// madc::tools — the tools a program serves to an agent (split arc 1C,	//
// plan 2026-10-09-madc-madcide-split-arc.md items 13-16). A tool is a	//
// row: a name, what it does, its arguments' JSON schema, the least	//
// permission tier that may call it, and its handler. A registry holds	//
// the rows a server advertises (MCP's tools/list) and runs one call	//
// (tools/call) after checking the caller's tier. The code-graph tools	//
// are madc's (add_graph_tools, over madc::lang's parse door); other	//
// families register into the same registry.				//
//									//
// Thread contract (C++ standard library convention): a registry is	//
// used from one thread; distinct registries are independent. A call	//
// runs on that thread, over the world its context names.		//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_TOOLS_H
#define __MADC_TOOLS_H 1

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libmadc/value.h"

namespace madc {

namespace hub { class world; typedef uint32_t entity_id; }

namespace tools {

// Who may call a tool, least first: an observer reads, a proposer may
// propose an edit, an editor may make one, the owner may do anything.
enum class tier : unsigned char { observer, proposer, editor, owner };
const char *tier_name(tier t);

// One call's context: the world and the document the server serves, and
// the caller's tier.
struct context
{
    hub::world &w;
    hub::entity_id doc;
    tier caller;
};

// A handler answers into `result`; false = refused, `result` then carries
// {error: why}.
typedef std::function<bool(const context &c, const value &args, value &result)> handler;

struct tool
{
    std::string name;
    std::string description;
    value input_schema;		// a JSON schema object
    tier least;			// the least tier that may call it
    handler run;
};

class registry
{
public:
    // False when the name is empty or taken.
    bool add(const tool &t);
    const tool *find(const std::string &name) const;
    const std::vector<tool> &all() const { return _tools; }
    // `name` run with `args`: false, with {error: why} in `result`, when no
    // tool has the name or the caller's tier is below the tool's.
    bool call(const context &c, const std::string &name, const value &args,
	      value &result) const;

private:
    std::vector<tool> _tools;
};

// The answer every refusal shares: {error: why}.
value refusal(const std::string &why);

// The code-graph tools (item 14): node-addressed reads over the served
// document's parse — graph.symbols, definition, node, type_of, members,
// bases, enclosing, body, children, callees, callers, references, search,
// impact, span and at. Each answers the graph's own result object.
void add_graph_tools(registry &r);

} // namespace tools
} // namespace madc

#endif // __MADC_TOOLS_H
