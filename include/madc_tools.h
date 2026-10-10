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
#include <map>
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

// One call's context: the world and the document the server serves, the
// caller's tier, and who the caller is (the actor its writes are recorded
// under; -1 = the local operator).
struct context
{
    hub::world &w;
    hub::entity_id doc;
    tier caller;
    int64_t actor;
};

// A handler answers into `result`; false = refused, `result` then carries
// {error: why}.
typedef std::function<bool(const context &c, const value &args, value &result)> handler;

// The project memory a registry's edit tools report to (item 15): a
// proposer's edit is stored there as a proposal, an applied edit's node-op
// is recorded there. A registry without one refuses proposals and records
// nothing.
class edit_memory
{
public:
    virtual ~edit_memory() {}
    // `proposal` {ops, preview, diagnostics} stored; `result` answers the
    // caller ({proposal: seq, ...}). False, with {error}, when refused.
    virtual bool propose(const context &c, const value &proposal, value &result) = 0;
    // An applied edit, recorded after it landed: {verb, target, src}.
    virtual void applied(const context &c, const value &nodeop) = 0;
};

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
    // The project memory the edit tools report to (not owned); none: 0.
    void set_memory(edit_memory *m) { _memory = m; }
    edit_memory *memory() const { return _memory; }

    registry() : _memory(0) {}

private:
    std::vector<tool> _tools;
    edit_memory *_memory;
    registry(const registry &);
    registry &operator=(const registry &);
};

// The answer every refusal shares: {error: why}; refused: a result that is
// one.
value refusal(const std::string &why);
bool refused(const value &result);

// A tool's input schema: one property {type, description}; the object
// schema over named properties.
value property(const char *type, const char *description);
value object_schema(const std::map<std::string, value> &properties);

// The code-graph tools (item 14): node-addressed reads over the served
// document's parse — graph.symbols, definition, node, type_of, members,
// bases, enclosing, body, children, callees, callers, references, search,
// impact, span and at — each answering the graph's own result object; and
// the edits, graph.insert (src before | after a node), graph.replace and
// graph.delete. An edit's extent is the node's span; its candidate text is
// validated by the compiler (no new error) before anything changes. An
// editor's edit lands as ONE undo step and answers {ok, node, splice_at,
// splice_del}; a proposer's — or any caller's asking `propose` — is stored
// in the registry's memory as a proposal {ops, preview, diagnostics}.
void add_graph_tools(registry &r);

// The edit core the edit tools and a proposal's accept share. plan_op: one
// node-op against the served document's CURRENT parse — `verb` graph.insert
// / graph.replace / graph.delete with its args {id, src, where} — as
// {verb, where, target (graph.span's answer), text (the node's bytes now),
// src, splice {at, del, ins}, line, column}; false with `why`. land_ops:
// the ops landed as ONE undo step once the compiler accepts the candidate
// (every splice, highest offset first), each recorded in the registry's
// memory as applied (under `proposal` when > 0); {ok, node, splice_at,
// splice_del}, or false with {error, diagnostics?}.
bool plan_op(const context &c, const std::string &verb, const value &args,
	     value &op, std::string &why);
bool land_ops(const registry &r, const context &c, const value &ops,
	      int64_t proposal, value &result);

} // namespace tools
} // namespace madc

#endif // __MADC_TOOLS_H
