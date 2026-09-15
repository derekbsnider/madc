# Code-graph MCP L2 — derived CALLS/REFERENCES edges (functions + globals) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the L2 verb family — `graph.callees` / `graph.callers` /
`graph.references` / `graph.search` / `graph.impact` — that derives **CALLS**
and **REFERENCES** edges over a live parse handle for BOTH functions and global
variables, by walking the same live `TokenBase` forest L1b walks and reading
the callee/referent bindings the parser already resolved (every edge is
compiler ground truth, never inferred).

**Architecture:** Two derivation collectors sharing the L1b
`graph_body_children()` descent: `graph_collect_callsites()` (a call site →
its callee `FuncDef`, via the ONE overload-aware resolver
`Program::resolved_call_funcdef()`) and `graph_collect_var_uses()` (a name-use
`TokenVar` → its referent `Variable*`, by pointer identity). The five verbs
compose these. Function nodes address through the L1 type-id round-trip
(`type_id_for`/`graph_function_token`/`graph_node_value`); **global** nodes get
a new decl-id space over the live `top_decls` registry, partitioned off the
type-id and body-id ranges (`GRAPH_DECL_ID_BASE`, sibling to L1b's
`GRAPH_BODY_ID_BASE`). Edges are **computed on-demand** in v1, isolated behind
the collectors so a per-handle incremental cache is a drop-in later addition
(measured, not speculated — §10). Same five-layer plumbing and MCP seat shape
as L1/L1b.

**Tech Stack:** C++11 engine (`src/madc_program.cpp`), the C bridge
(`src/parser.cpp` + `include/ns_common.h`), the dialect wrapper
(`src/ns_madc.cpp` + `include/madc/ns_madc`), the MCP seat
(`tools/madcide/madcide_mcp.inc`), value-first `.mad` integration tests.

**Spec:** `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` (§6.2 schema,
§6.4 verb set, §6.5 result shape, §7 L2, §10 open decisions). The plan argues
from that spec; executors read both. Owner ruling 2026-09-12: L2 covers
functions AND globals (globals are live decl nodes, not an addressing barrier).

## Global Constraints

Every task's requirements implicitly include this section.

- **Substrate = the live structures only.** Function bodies: the live
  `TokenBase` AST reached from `graph_function_token`'s `TokenFunc`, descended
  with `graph_body_children()`. Global decls: the live `child.top_decls`
  (`dkGlobalVar` entries). NEVER the transient c2mir `node_t`/cir_node arena
  (freed per compile) and NEVER a frozen-forest arena.
- **Every edge is compiler ground truth (invariant §5.5).** A CALLS edge comes
  from `resolved_call_funcdef()`'s answer; a REFERENCES edge from a resolved
  binding (`&TokenVar::var == the target Variable*`, or a resolved callee).
  Never an inferred, name-matched, or heuristic edge.
- **Reuse, don't reinvent (Top-10 #4, no-parallel-implementations).** Callee
  resolver = `Program::resolved_call_funcdef()` (the ONE overload-aware
  resolver; do not re-rank in the verb). Function id round-trip =
  `child.type_id_for()`/`graph_function_token()`/`graph_node_value()`. Body
  descent = `graph_body_children()`. Use-site id space = `graph_body_intern()`
  (`GRAPH_BODY_ID_BASE`). Global storage = the existing `child.top_decls`
  (`dkGlobalVar`) + the `tkProgram` symbol table — never a new globals map.
- **Enums, not strings for edge kinds (invariant §5.4 / §6.2).** Edge kinds
  become a `GraphEdgeKind` enum → one wire-name function; the L1b `"CONTAINS"`
  literal is retrofitted through it. (Node kinds stay strings — L1's existing
  `graph_kind_name` convention; `"Global"` joins `"Function"`/`"Struct"`/…)
- **Terse by default (invariant §5.7 / §6.5).** Nodes `{id, kind, name, span}`;
  edges `{kind, from, to, at?}`. Result-capped (`truncated` flag); no unbounded
  dumps.
- **Scope (recon-settled + owner-widened).** Targets are **functions**
  (type-ids) and **global variables** (decl-ids). One handle = one TU
  (cross-TU deferred). Deferred, documented increments: type-as-reference-target;
  params/locals/members as nodes (they reuse the same registry pattern and their
  uses are within-body — already covered by L1b's CONTAINS); reference-through-
  member of a global (`g.field`).
- **Rule trailers** on every `src/`+`include/` commit. `Oracle: n/a — new
  read-only accessors, the .mad integration test is the oracle` is honest here.
- **Targeted tests per slice; the FULL battery rides the V6 arc RELEASE seam,
  never this slice** (owner law). Zero warnings (`-Wall -Werror`). No `&&`
  chains. Scratch in `tmp/`. Value-first `.mad` (`var`, bare `print`/`println`,
  out-param `madc::graph_*`). Push only to owner remotes.

---

## Verified recon appendix (facts checked against the tree at HEAD, 2026-09-12)

Full trace: `tmp/sdd-ast-graph-mcp-L2/recon-findings.md` (carries a correction
header on the globals point). Load-bearing facts re-verified directly by the
controller (not just the recon agent).

**Callee resolution — `Program::resolved_call_funcdef` (PUBLIC).**
- Decl `include/madc.h:6048`: `FuncDef *resolved_call_funcdef(class
  TokenCallFunc *tc, bool *no_winner = NULL);` — under `public:` (madc.h:4725,
  no intervening specifier verified), callable as `child.resolved_call_funcdef(tc)`.
- Def `src/parser.cpp:55963-56009` (read directly): NULL if `!tc->var.type` or
  not a function (55967); **NULL if `is_numeric()`** = a fn-ptr variable, i.e.
  an indirect call (55969-55970); returns the parse-bound `fd` for a plain
  function / a namespace set with <2 entries (55972-55979); **re-ranks
  user-written args and returns the SELECTED overload `wfd`** for a tracked set
  ≥2 or a `declaration_only` placeholder (56000-56008). Member/method calls
  (TokenMember/TokenCallMethod IS-A TokenCallFunc) are already reselected at
  parse time (`reselect_method_overload`) so `var.type` is the final method
  FuncDef and the namespace lookup misses → returned unchanged.
- **Not a byte-pure read in general** (the overload path can touch
  `activate_forest_function_family`/`getPointerType` caches). **Mitigated:** the
  identical call already ran once per node during the original parse
  (arity-check, `parser.cpp:28753-28771`); a post-parse read verb re-hits warm
  state, no new mutation for that node; handles are handed out only post-parse.
  Documented residual — the Task 9 re-query-stability test guards it. Reading
  `tc->var.type` alone is INSUFFICIENT for an overloaded free call → reuse the
  resolver uniformly.

**Function id round-trip (reused verbatim).**
- `graph_func_node_value` (`src/madc_program.cpp:5589-5599`): `id =
  child.type_id_for(tf->var.type)`. Each `FuncDef` is its own `DataDef` → id
  unique per function.
- `graph_function_token` (`5573-5583`): reverses a `DataDef*` to its `TokenFunc*`
  by pointer-identity scan over `child.pending_funcs`.
- `graph_node_value` (`5609`): a Function with a live token → full
  span+name; else terse `{id, kind, name}`. **Use for callee nodes** — a
  method/header callee with no TU-own token is still uniquely addressable by
  `type_id_for(FuncDef*)`.

**Globals are live decl nodes (owner-widened; controller-verified).**
- `child.top_decls` (`include/madc.h:4757`) is a live per-handle
  `std::vector<TopDecl>`. `struct TopDecl` (`4743-4756`): `DeclKind kind`
  (`{dkTypedef,dkStruct,dkUnion,dkEnum,dkGlobalVar}`, 4742), `std::string name`,
  `DataDef *dd`, `Variable *var` ("for global vars: the Variable"), `const char
  *file`, `int line`, `TokenBase *origin` (per-occurrence token; NULL → use
  file/line), `TokenDecl *decl` ("for global vars: the TokenDecl carrying
  initializer; NULL → no init").
- Ordinary globals push a `dkGlobalVar` TopDecl in live `parseDeclaration`
  (`src/parser.cpp:68396-68404`, `69107-69115`; comment at `25401` — "Live's
  parseDeclaration pushes the dkGlobalVar TopDecl"). So every global is a
  span-carrying live record.
- Symbol table: `TokenCpnd::variables` (`madc.h:747`) + `var_index`
  (`unordered_map<uint32_t,Variable*>` keyed by interned-name sid, `761`);
  `TokenProgram : TokenCpnd` (`1290`), so `child.tkProgram->variables` is the
  global list. `findVariable` (`697/780`) resolves a name to the one `Variable*`.
- A use-site is `new TokenVar(*var)` (`src/parser.cpp:13730`, `15892`, etc.)
  with `var = findVariable(name)`; `TokenVar(Variable &v) : var(v)`
  (`datatokens.h:277`) binds `var` BY REFERENCE. → `&use->var` equals the
  global's `top_decls[i].var` for every use-site and across functions
  (pointer identity holds; Task-9 asserts it and CB-4 confirms).
- **Why the old type-id scheme can't address a global:** `type_id_for`
  (`madc.h:2766`) keys on `DataDef*`; `int g;`'s type is the shared interned
  `int` DataDef, so two int globals share an id. Fix = a decl-id space over the
  `top_decls` index (below), NOT `type_id_for`.

**Body descent + use-site ids (reused).**
- `graph_body_children` (`6053`): total, order-correct sub-expression descent
  (covers TokenCallFunc/TokenMember + every child shape). `as_callfunc_tok()`
  (madc.h:998) matches TokenCallFunc + subclasses; `as_var_tok()`
  (datatokens.h:305, base returns NULL tokens.h:385) matches TokenVar +
  subclasses (a call node's `var` is a function, so it never false-matches a
  global target).
- `graph_body_intern` (`6023`) / `GRAPH_BODY_ID_BASE = 0x4000000000000000LL`
  (`6021`): the per-handle use-site id space; `graph_body_node_value` (`6188`)
  the terse site node.
- Edge kinds today: bare `"CONTAINS"` string (`6245`) — no enum yet.
  `GraphBodyKind` (`5870`)+`graph_body_kind_name` (`5874`) are the enum pattern
  to mirror.

**One handle = one TU:** `parse_tu_state` holds one `::Program *child`
(`4950-4963`); `top_decls`/`pending_funcs` are one per `Program`. Cross-TU
fan-out does not exist on a handle. Refresh (`internal_program_parse_refresh`)
replaces the child → `top_decls` is fresh per parse (decl-ids stable within a
snapshot, re-minted after a mutating command — matches §6.3 re-query rule).

---

## File Structure

Same surfaces as L1/L1b — L2 is additive:

- `src/madc_program.cpp` — the L2 engine: `GraphEdgeKind` enum +
  `graph_edge_kind_name`; the id-partition (`GRAPH_DECL_ID_BASE`, `graph_id_space`)
  + global helpers (`graph_global_resolve`, `graph_global_node_value`); the two
  collectors (`graph_collect_callsites`, `graph_collect_var_uses`) +
  `GRAPH_EDGE_RESULT_CAP`; `internal_program_graph_{callees,callers,references,
  search,impact}`; the L1b `"CONTAINS"` retrofit + decl-id guards in
  graph.node/type_of/members/bases/body/children.
- `src/parser.cpp` — 5 `internal_program_graph_*` forward decls + 5
  `madc_graph_*` C bridges.
- `include/ns_common.h` — 5 `madc_graph_*` prototypes.
- `src/ns_madc.cpp` — 5 `graph_*` dialect wrappers.
- `include/madc/ns_madc` — 5 declaration-only `graph_*` decls.
- `tools/madcide/madcide_mcp.inc` — 5 tool descriptors + 5 dispatch arms.
- `tests/testgraphedges.mad` (+ `.expect` + `.expect_quiet`) — NEW value-first
  fixture (functions + a global).
- `tests/testmadcide_serve_graph.mad` (+ `.expect`) — extend to drive the 5 L2
  verbs over the MCP seat.

---

### Task 1: Edge enum, id-partition, global helpers, and the two collectors

**Files:**
- Modify: `src/madc_program.cpp` — add the edge enum + name fn just below
  `graph_body_kind_name` (~5888); add the partition + global helpers + cap +
  collectors after `graph_body_walk` (~6252); retrofit the `"CONTAINS"` literal
  (~6245); add decl-id guards to the id-routing verbs (see Step 6).

**Interfaces:**
- Produces: `enum class GraphEdgeKind { Contains, Calls, References };`,
  `const char *graph_edge_kind_name(GraphEdgeKind)`; `const int64_t
  GRAPH_DECL_ID_BASE`; `enum class GraphIdSpace { Type, Global, Body };
  GraphIdSpace graph_id_space(int64_t)`; `const TopDecl *graph_global_resolve(
  ::Program &, int64_t)`; `madc::value graph_global_node_value(const TopDecl &,
  int64_t)`; `struct GraphCallSite { const TokenBase *site; ::FuncDef *callee; };`;
  `void graph_collect_callsites(::Program &, TokenFunc *,
  std::vector<GraphCallSite> &, size_t)`; `void graph_collect_var_uses(TokenFunc
  *, const Variable *, std::vector<const TokenBase *> &, size_t)`; `const size_t
  GRAPH_EDGE_RESULT_CAP`.
- Consumes: `graph_body_children`, `resolved_call_funcdef`, `as_callfunc_tok`,
  `as_var_tok`, `child.top_decls`, `GRAPH_BODY_ID_BASE`.

- [ ] **Step 1: Edge-kind enum + wire-name (below `graph_body_kind_name`)**

```cpp
// Edge kinds are enums (invariant §5.4 / §6.2), converted to a wire name ONCE.
// L1b emitted "CONTAINS" as a bare string; L2 adds CALLS/REFERENCES, so the
// enum now earns its keep — the L1b literal is retrofitted below to
// graph_edge_kind_name(GraphEdgeKind::Contains): one owner of the edge vocab.
enum class GraphEdgeKind { Contains, Calls, References };

static const char *graph_edge_kind_name(GraphEdgeKind k)
{
    switch ( k )
    {
    case GraphEdgeKind::Contains:  return "CONTAINS";
    case GraphEdgeKind::Calls:     return "CALLS";
    case GraphEdgeKind::References: return "REFERENCES";
    }
    return "CONTAINS";
}
```

- [ ] **Step 2: Retrofit the L1b CONTAINS emission (in `graph_body_walk`, ~6245)**

Replace `e["kind"] = value(std::string("CONTAINS"));` with:
```cpp
	    e["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::Contains)));
```

- [ ] **Step 3: The id partition + global decl helpers (after `graph_body_walk`)**

```cpp
// A single integer node id self-identifies its space, so it can be passed to
// any verb without ambiguity or truncation into a wrong id. Three partitions,
// each a power-of-two ceiling type-ids never reach:
//   type-id       [0, GRAPH_DECL_ID_BASE)        — L1 decl/type nodes
//   global decl   [GRAPH_DECL_ID_BASE, BODY_BASE) — a top_decls dkGlobalVar idx
//   body node     [GRAPH_BODY_ID_BASE, ...)       — L1b per-handle body handle
static const int64_t GRAPH_DECL_ID_BASE = 0x2000000000000000LL;

enum class GraphIdSpace { Type, Global, Body };
static GraphIdSpace graph_id_space(int64_t id)
{
    if ( id >= GRAPH_BODY_ID_BASE ) return GraphIdSpace::Body;
    if ( id >= GRAPH_DECL_ID_BASE ) return GraphIdSpace::Global;
    return GraphIdSpace::Type;
}

// A global's node id is GRAPH_DECL_ID_BASE + its index in child.top_decls; the
// index is stable within one parse snapshot (top_decls is append-only during a
// parse, rebuilt on refresh — §6.3). Resolves back to the TopDecl ONLY when
// that slot is a dkGlobalVar (other decl kinds — struct/typedef/... — are
// L1-addressed via type-id, not this space).
static const ::Program::TopDecl *graph_global_resolve(::Program &child, int64_t id)
{
    if ( graph_id_space(id) != GraphIdSpace::Global )
	return (const ::Program::TopDecl *)0;
    size_t idx = (size_t)(id - GRAPH_DECL_ID_BASE);
    if ( idx >= child.top_decls.size() )
	return (const ::Program::TopDecl *)0;
    const ::Program::TopDecl &td = child.top_decls[idx];
    return td.kind == ::Program::DeclKind::dkGlobalVar ? &td : (const ::Program::TopDecl *)0;
}

// A global variable's terse node: { id, kind:"Global", name, line, column? }.
// A global is not derivable from graph_kind_name (its DataDef is its TYPE, e.g.
// int) — it is a "Global" by virtue of being a dkGlobalVar decl. Span from the
// origin token when present, else the TopDecl's recorded line.
static madc::value graph_global_node_value(const ::Program::TopDecl &td, int64_t id)
{
    std::map<std::string, madc::value> f;
    f["id"]   = value(id);
    f["kind"] = value(std::string("Global"));
    if ( !td.name.empty() )
	f["name"] = value(td.name);
    if ( td.origin )
    {
	f["line"]   = value((int64_t)td.origin->line);
	f["column"] = value((int64_t)td.origin->column);
    }
    else if ( td.line )
	f["line"] = value((int64_t)td.line);
    return value::make_object(f);
}
```

> `TopDecl`, `DeclKind`, and `top_decls` are members of `class Program`
> (`include/madc.h:4742-4757`); spell them `::Program::TopDecl` /
> `::Program::DeclKind::dkGlobalVar` from these file-scope helpers. CB-5
> confirms the exact qualified spelling compiles.

- [ ] **Step 4: The result cap + the two collectors**

```cpp
// A whole-forest edge scan can span every function body; cap the RESULT so a
// graph.callers/references over a large TU stays terse (§6.5). Independent of
// the per-body GRAPH_BODY_NODE_CAP.
static const size_t GRAPH_EDGE_RESULT_CAP = 2000;

// A resolved STATIC call site inside one function body: the call token and its
// resolved callee. Indirect (fn-ptr) calls — resolved_call_funcdef -> NULL —
// are dropped (no static callee, so no ground-truth CALLS edge; §5.5).
struct GraphCallSite { const TokenBase *site; ::FuncDef *callee; };

// Collector #1 — call sites. Descends fn's body with the L1b
// graph_body_children() descent (the ONE descent owner) and resolves each
// TokenCallFunc through resolved_call_funcdef (the ONE overload-aware callee
// resolver). callees/callers/references(function)/impact(function) compose this.
static void graph_collect_callsites(::Program &child, TokenFunc *fn,
				    std::vector<GraphCallSite> &out, size_t cap)
{
    if ( !fn )
	return;
    std::vector<const TokenBase *> q;
    q.push_back(fn);
    for ( size_t qi = 0; qi < q.size(); ++qi )
    {
	if ( out.size() >= cap )
	    break;
	const TokenBase *n = q[qi];
	TokenBase *nt = const_cast<TokenBase *>(n);
	if ( TokenCallFunc *cf = nt->as_callfunc_tok() )
	{
	    ::FuncDef *callee = child.resolved_call_funcdef(cf);
	    if ( callee )
	    {
		GraphCallSite cs;
		cs.site = n;
		cs.callee = callee;
		out.push_back(cs);
	    }
	}
	std::vector<const TokenBase *> kids;
	graph_body_children(n, kids);
	for ( size_t i = 0; i < kids.size(); ++i )
	    q.push_back(kids[i]);
    }
}

// Collector #2 — name-use sites of one target Variable (a global). Same descent
// owner; matches a TokenVar whose `var` IS the target (pointer identity — a
// use-site is new TokenVar(*findVariable(name)), so &v->var == the global's
// top_decls[i].var). A call node's var is a function, never the global target,
// so it is naturally excluded. references(global)/impact(global) compose this.
static void graph_collect_var_uses(TokenFunc *fn, const Variable *target,
				   std::vector<const TokenBase *> &out, size_t cap)
{
    if ( !fn || !target )
	return;
    std::vector<const TokenBase *> q;
    q.push_back(fn);
    for ( size_t qi = 0; qi < q.size(); ++qi )
    {
	if ( out.size() >= cap )
	    break;
	const TokenBase *n = q[qi];
	TokenBase *nt = const_cast<TokenBase *>(n);
	if ( TokenVar *v = nt->as_var_tok() )
	    if ( &v->var == target )
		out.push_back(n);
	std::vector<const TokenBase *> kids;
	graph_body_children(n, kids);
	for ( size_t i = 0; i < kids.size(); ++i )
	    q.push_back(kids[i]);
    }
}
```

- [ ] **Step 5: `#include` check (CB-1)** — the verbs use `std::set<uint32_t>`
  for dedup. Confirm `<set>` (or use the already-present `<unordered_set>`) is
  available in `src/madc_program.cpp`; add the include with the other STL
  headers if missing.

- [ ] **Step 6: Decl-id guards on the L1/L1b id-routing verbs (truncation safety)**

Introducing `GRAPH_DECL_ID_BASE` means a global decl-id must not truncate via
`(uint32_t)id` into a wrong type-id. Add a `graph_id_space` guard at the top of
each id-consuming verb, in the shape each already uses:

- `internal_program_graph_node` (~5658): before `type_from_id`, add — if
  `graph_id_space(node_id) == GraphIdSpace::Global`, resolve via
  `graph_global_resolve` and return the global node (empty if not a global slot);
  a Body id keeps L1b's current behavior (graph.children owns body descent).
```cpp
    if ( graph_id_space(node_id) == GraphIdSpace::Global )
    {
	const ::Program::TopDecl *td = graph_global_resolve(*st->child, node_id);
	if ( td )
	    out = graph_global_node_value(*td, node_id);
	return true;   // valid handle; empty out for a non-global decl slot
    }
```
- `internal_program_graph_type_of`/`members`/`bases`/`body` (~5689/5773/5808/6262)
  and `graph_children`'s type-id branch (~6311): add an early
  `if ( graph_id_space(id) == GraphIdSpace::Global ) { out = <empty result of
  this verb's shape>; return true; }` — a global is not a type_of/members/bases/
  body/children target in v1 (documented). Match each verb's existing empty-out
  shape (a `{nodes:[]}` object, or `{nodes:[],edges:[]}` for body/children).

> These guards are the correct hardening for a new id partition; without them a
> decl-id silently truncates. They are behaviour-preserving for every existing
> (type-id / body-id) input. `Oracle: n/a — new partition guard; the .mad test
> exercises decl-id routing`.

---

### Task 2: `graph.callees(func)` — a function's callees

**Files:** Modify `src/madc_program.cpp` — add after `internal_program_graph_children`.

**Interfaces:**
- Produces: `bool internal_program_graph_callees(int64_t handle, int64_t
  func_id, madc::value &out)`.

- [ ] **Step 1: Implement**

```cpp
// graph.callees(handle, func_id): the functions func_id calls. func_id is a
// function type-id. Resolve to the live TokenFunc, collect its call sites (the
// ONE collector), emit one CALLS edge per site {from: func_id, to: callee-id,
// at: call-site body id} + the DISTINCT callee function nodes. A global id (or
// any non-function) yields empty (a global has no callees). Result: { nodes,
// edges:[CALLS...], truncated? }.
bool internal_program_graph_callees(int64_t handle, int64_t func_id,
				    madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;
    if ( graph_id_space(func_id) == GraphIdSpace::Type )
    {
	DataDef *dd = child.type_from_id((uint32_t)func_id);   // binds child's table (M-5)
	TokenFunc *tf = dd ? graph_function_token(child, dd) : (TokenFunc *)0;
	if ( tf )
	{
	    nodes.push_back(graph_func_node_value(child, tf));
	    std::vector<GraphCallSite> sites;
	    graph_collect_callsites(child, tf, sites, GRAPH_EDGE_RESULT_CAP);
	    std::set<uint32_t> seen_callee;
	    for ( size_t i = 0; i < sites.size(); ++i )
	    {
		if ( nodes.size() + edges.size() >= GRAPH_EDGE_RESULT_CAP )
		{ truncated = true; break; }
		uint32_t cid = child.type_id_for((DataDef *)sites[i].callee);
		if ( seen_callee.insert(cid).second )
		    nodes.push_back(graph_node_value(child, (DataDef *)sites[i].callee));
		std::map<std::string, madc::value> e;
		e["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::Calls)));
		e["from"] = value((int64_t)func_id);
		e["to"]   = value((int64_t)cid);
		e["at"]   = value(graph_body_intern(st, sites[i].site));
		edges.push_back(value::make_object(e));
	    }
	}
    }
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    r["edges"] = value::make_array(edges);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

---

### Task 3: `graph.callers(func)` — who calls a function

**Files:** Modify `src/madc_program.cpp` — add after Task 2.

**Interfaces:**
- Produces: `bool internal_program_graph_callers(int64_t handle, int64_t
  func_id, madc::value &out)`.

- [ ] **Step 1: Implement**

```cpp
// graph.callers(handle, func_id): the functions that call func_id — a
// whole-forest scan of pending_funcs (a caller is a caller regardless of TU
// origin, unfiltered, matching graph_function_token). For each caller's call
// site whose resolved callee == func_id, emit a CALLS edge {from: caller-id,
// to: func_id, at: call-site body id}; caller nodes DISTINCT. A non-function id
// yields empty. Result: { nodes, edges:[CALLS...], truncated? }.
bool internal_program_graph_callers(int64_t handle, int64_t func_id,
				    madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;
    if ( graph_id_space(func_id) == GraphIdSpace::Type )
    {
	DataDef *dd = child.type_from_id((uint32_t)func_id);
	TokenFunc *target = dd ? graph_function_token(child, dd) : (TokenFunc *)0;
	if ( target )
	{
	    nodes.push_back(graph_func_node_value(child, target));
	    std::set<uint32_t> seen_caller;
	    for ( size_t f = 0; f < child.pending_funcs.size() && !truncated; ++f )
	    {
		TokenFunc *caller = child.pending_funcs[f]
		    ? child.pending_funcs[f]->as_func_tok() : (TokenFunc *)0;
		if ( !caller )
		    continue;
		std::vector<GraphCallSite> sites;
		graph_collect_callsites(child, caller, sites, GRAPH_EDGE_RESULT_CAP);
		uint32_t caller_id = child.type_id_for(caller->var.type);
		for ( size_t i = 0; i < sites.size(); ++i )
		{
		    if ( child.type_id_for((DataDef *)sites[i].callee) != (uint32_t)func_id )
			continue;
		    if ( nodes.size() + edges.size() >= GRAPH_EDGE_RESULT_CAP )
		    { truncated = true; break; }
		    if ( seen_caller.insert(caller_id).second )
			nodes.push_back(graph_func_node_value(child, caller));
		    std::map<std::string, madc::value> e;
		    e["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::Calls)));
		    e["from"] = value((int64_t)caller_id);
		    e["to"]   = value((int64_t)func_id);
		    e["at"]   = value(graph_body_intern(st, sites[i].site));
		    edges.push_back(value::make_object(e));
		}
	    }
	}
    }
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    r["edges"] = value::make_array(edges);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

---

### Task 4: `graph.references(def)` — every use-site (function OR global)

**Files:** Modify `src/madc_program.cpp` — add after Task 3.

**Interfaces:**
- Produces: `bool internal_program_graph_references(int64_t handle, int64_t
  def_id, madc::value &out)`.

- [ ] **Step 1: Implement (routes on the id partition)**

```cpp
// graph.references(handle, def_id): every use-SITE of def_id across the TU.
//  - FUNCTION (type-id): call sites (the ONE collector), site granularity.
//  - GLOBAL (decl-id): name-use sites of the global's Variable (collector #2).
// For each site, emit the use-site body node + a REFERENCES edge {from:
// site-body-id, to: def_id} + the enclosing function node. Result: { nodes:
// [def + use-sites + enclosing funcs], edges:[REFERENCES...], truncated? }.
bool internal_program_graph_references(int64_t handle, int64_t def_id,
				       madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;

    GraphIdSpace space = graph_id_space(def_id);
    DataDef *fdd = (space == GraphIdSpace::Type)
	? child.type_from_id((uint32_t)def_id) : (DataDef *)0;
    TokenFunc *ftarget = fdd ? graph_function_token(child, fdd) : (TokenFunc *)0;
    const ::Program::TopDecl *gtd = (space == GraphIdSpace::Global)
	? graph_global_resolve(child, def_id) : (const ::Program::TopDecl *)0;
    const Variable *gvar = gtd ? gtd->var : (const Variable *)0;

    if ( ftarget )
	nodes.push_back(graph_func_node_value(child, ftarget));
    else if ( gtd )
	nodes.push_back(graph_global_node_value(*gtd, def_id));

    if ( ftarget || gvar )
    {
	std::set<uint32_t> seen_encl;
	for ( size_t f = 0; f < child.pending_funcs.size() && !truncated; ++f )
	{
	    TokenFunc *encl = child.pending_funcs[f]
		? child.pending_funcs[f]->as_func_tok() : (TokenFunc *)0;
	    if ( !encl )
		continue;
	    std::vector<const TokenBase *> sites;
	    if ( ftarget )
	    {
		std::vector<GraphCallSite> cs;
		graph_collect_callsites(child, encl, cs, GRAPH_EDGE_RESULT_CAP);
		for ( size_t i = 0; i < cs.size(); ++i )
		    if ( child.type_id_for((DataDef *)cs[i].callee) == (uint32_t)def_id )
			sites.push_back(cs[i].site);
	    }
	    else
		graph_collect_var_uses(encl, gvar, sites, GRAPH_EDGE_RESULT_CAP);

	    if ( sites.empty() )
		continue;
	    uint32_t encl_id = child.type_id_for(encl->var.type);
	    if ( seen_encl.insert(encl_id).second )
		nodes.push_back(graph_func_node_value(child, encl));
	    for ( size_t i = 0; i < sites.size(); ++i )
	    {
		if ( nodes.size() + edges.size() >= GRAPH_EDGE_RESULT_CAP )
		{ truncated = true; break; }
		int64_t sid = graph_body_intern(st, sites[i]);
		nodes.push_back(graph_body_node_value(st, sites[i]));
		std::map<std::string, madc::value> e;
		e["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::References)));
		e["from"] = value(sid);
		e["to"]   = value((int64_t)def_id);
		edges.push_back(value::make_object(e));
	    }
	}
    }
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    r["edges"] = value::make_array(edges);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

---

### Task 5: `graph.search(kind, name~)` — functions and globals by name

**Files:** Modify `src/madc_program.cpp` — add after Task 4.

**Interfaces:**
- Produces: `bool internal_program_graph_search(int64_t handle, const
  std::string &kind, const std::string &name_sub, madc::value &out)`.

- [ ] **Step 1: Implement**

```cpp
// graph.search(handle, kind, name~): decl nodes whose kind matches `kind`
// (empty = any; "Function" = functions; "Global" = globals; other kinds ->
// none in v1) and whose name CONTAINS `name~` (case-sensitive substring in v1).
// v1 covers FUNCTIONS (pending_funcs / tu_own_function — the graph.symbols
// surface) and GLOBALS (top_decls dkGlobalVar). Struct/typedef/enum search
// needs the decl-map iterators — documented next increment (CB-2). Result: {
// nodes:[matches], truncated? }.
bool internal_program_graph_search(int64_t handle, const std::string &kind,
				   const std::string &name_sub, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes;
    bool truncated = false;
    bool any = kind.empty();
    if ( any || kind == "Function" )
	for ( size_t i = 0; i < child.pending_funcs.size(); ++i )
	{
	    TokenFunc *tf = tu_own_function(child.pending_funcs[i], st->display_name);
	    if ( !tf )
		continue;
	    if ( !name_sub.empty() && tf->var.name.find(name_sub) == std::string::npos )
		continue;
	    if ( nodes.size() >= GRAPH_EDGE_RESULT_CAP )
	    { truncated = true; break; }
	    nodes.push_back(graph_func_node_value(child, tf));
	}
    if ( (any || kind == "Global") && !truncated )
	for ( size_t i = 0; i < child.top_decls.size(); ++i )
	{
	    const ::Program::TopDecl &td = child.top_decls[i];
	    if ( td.kind != ::Program::DeclKind::dkGlobalVar )
		continue;
	    if ( !name_sub.empty() && td.name.find(name_sub) == std::string::npos )
		continue;
	    if ( nodes.size() >= GRAPH_EDGE_RESULT_CAP )
	    { truncated = true; break; }
	    nodes.push_back(graph_global_node_value(td, GRAPH_DECL_ID_BASE + (int64_t)i));
	}
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

---

### Task 6: `graph.impact(id)` — batched callers + references + enclosing

**Files:** Modify `src/madc_program.cpp` — add after Task 5.

**Interfaces:**
- Produces: `bool internal_program_graph_impact(int64_t handle, int64_t id,
  madc::value &out)`.

- [ ] **Step 1: Implement (function: callers+references in one pass; global: references)**

```cpp
// graph.impact(handle, id): the one-call impact view (§6.4).
//  - FUNCTION: callers (CALLS, distinct) + references (site-granularity
//    REFERENCES) + enclosing, in ONE forest pass.
//  - GLOBAL: references (name-use sites) + enclosing — a global has no CALLS.
// Result: { nodes, edges:[CALLS + REFERENCES...], truncated? }.
bool internal_program_graph_impact(int64_t handle, int64_t id, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;

    GraphIdSpace space = graph_id_space(id);
    DataDef *fdd = (space == GraphIdSpace::Type)
	? child.type_from_id((uint32_t)id) : (DataDef *)0;
    TokenFunc *ftarget = fdd ? graph_function_token(child, fdd) : (TokenFunc *)0;
    const ::Program::TopDecl *gtd = (space == GraphIdSpace::Global)
	? graph_global_resolve(child, id) : (const ::Program::TopDecl *)0;
    const Variable *gvar = gtd ? gtd->var : (const Variable *)0;

    if ( ftarget )
	nodes.push_back(graph_func_node_value(child, ftarget));
    else if ( gtd )
	nodes.push_back(graph_global_node_value(*gtd, id));

    if ( ftarget || gvar )
    {
	std::set<uint32_t> seen_caller;
	for ( size_t f = 0; f < child.pending_funcs.size() && !truncated; ++f )
	{
	    TokenFunc *encl = child.pending_funcs[f]
		? child.pending_funcs[f]->as_func_tok() : (TokenFunc *)0;
	    if ( !encl )
		continue;
	    std::vector<const TokenBase *> sites;
	    if ( ftarget )
	    {
		std::vector<GraphCallSite> cs;
		graph_collect_callsites(child, encl, cs, GRAPH_EDGE_RESULT_CAP);
		for ( size_t i = 0; i < cs.size(); ++i )
		    if ( child.type_id_for((DataDef *)cs[i].callee) == (uint32_t)id )
			sites.push_back(cs[i].site);
	    }
	    else
		graph_collect_var_uses(encl, gvar, sites, GRAPH_EDGE_RESULT_CAP);

	    if ( sites.empty() )
		continue;
	    uint32_t encl_id = child.type_id_for(encl->var.type);
	    bool first = seen_caller.insert(encl_id).second;
	    if ( first )
	    {
		nodes.push_back(graph_func_node_value(child, encl));
		if ( ftarget )   // a CALLS in-edge only for a function target
		{
		    std::map<std::string, madc::value> ce;
		    ce["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::Calls)));
		    ce["from"] = value((int64_t)encl_id);
		    ce["to"]   = value((int64_t)id);
		    edges.push_back(value::make_object(ce));
		}
	    }
	    for ( size_t i = 0; i < sites.size(); ++i )
	    {
		if ( nodes.size() + edges.size() >= GRAPH_EDGE_RESULT_CAP )
		{ truncated = true; break; }
		int64_t sid = graph_body_intern(st, sites[i]);
		nodes.push_back(graph_body_node_value(st, sites[i]));
		std::map<std::string, madc::value> re;
		re["kind"] = value(std::string(graph_edge_kind_name(GraphEdgeKind::References)));
		re["from"] = value(sid);
		re["to"]   = value((int64_t)id);
		re["at"]   = value((int64_t)encl_id);   // the enclosing function
		edges.push_back(value::make_object(re));
	    }
	}
    }
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    r["edges"] = value::make_array(edges);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

---

### Task 7: Five-layer plumbing for the 5 verbs

**Files:**
- Modify: `src/parser.cpp` — 5 forward decls (beside 351-368) + 5 C bridges
  (beside 1176-1187).
- Modify: `include/ns_common.h` — 5 prototypes (beside 233-246).
- Modify: `src/ns_madc.cpp` — 5 wrappers (beside 252-274).
- Modify: `include/madc/ns_madc` — 5 declaration-only decls (beside 176-202).

**Interfaces:**
- Produces: `madc::graph_{callees,callers,references,search,impact}` on the
  parse-handle surface, resolved mangled-direct (no extern "C"). Signatures are
  unchanged by the global widening — a global decl-id is just another `int64_t`
  routed inside the engine.

- [ ] **Step 1: `src/parser.cpp` forward decls (after line 368)**

```cpp
// Code-graph MCP L2 (design 2026-09-12): derived CALLS/REFERENCES edges
// (functions + globals) over the live forest — see madc_program.cpp.
bool internal_program_graph_callees(int64_t handle, int64_t func_id, value &out);
bool internal_program_graph_callers(int64_t handle, int64_t func_id, value &out);
bool internal_program_graph_references(int64_t handle, int64_t def_id, value &out);
bool internal_program_graph_search(int64_t handle, const std::string &kind,
				   const std::string &name_sub, value &out);
bool internal_program_graph_impact(int64_t handle, int64_t id, value &out);
```

- [ ] **Step 2: `src/parser.cpp` C bridges (after line 1187)**

```cpp
// Code-graph MCP L2 bridges (design 2026-09-12): same thin-thunk shape as L1/L1b.
void *madc_graph_callees(void *result, int64_t handle, int64_t func_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_callees(handle, func_id, out);
    return result;
}
void *madc_graph_callers(void *result, int64_t handle, int64_t func_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_callers(handle, func_id, out);
    return result;
}
void *madc_graph_references(void *result, int64_t handle, int64_t def_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_references(handle, def_id, out);
    return result;
}
void *madc_graph_search(void *result, int64_t handle, void *kind, void *name_sub)
{
    madc::value &out = *(madc::value *)result;
    std::string k = kind ? *(std::string *)kind : std::string();
    std::string n = name_sub ? *(std::string *)name_sub : std::string();
    madc::internal_program_graph_search(handle, k, n, out);
    return result;
}
void *madc_graph_impact(void *result, int64_t handle, int64_t id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_impact(handle, id, out);
    return result;
}
```

> `madc_graph_search` passes its two string args as `std::string *` via `void
> *`, mirroring `madc_graph_definition` (parser.cpp:1148).

- [ ] **Step 3: `include/ns_common.h` prototypes (after line 246)**

```cpp
// Code-graph MCP L2 (design 2026-09-12): derived-edge verbs (functions + globals).
void *madc_graph_callees(void *result, int64_t handle, int64_t func_id);
void *madc_graph_callers(void *result, int64_t handle, int64_t func_id);
void *madc_graph_references(void *result, int64_t handle, int64_t def_id);
void *madc_graph_search(void *result, int64_t handle, void *kind, void *name_sub);
void *madc_graph_impact(void *result, int64_t handle, int64_t id);
```

- [ ] **Step 4: `src/ns_madc.cpp` wrappers (after line 274)**

```cpp
// Code-graph MCP L2 (design 2026-09-12): derived-edge verbs over the live
// parse-handle forest. func_id is a FUNCTION type-id; def_id/id may be a
// function type-id OR a global decl-id (from graph_search). Each returns
// { nodes, edges?, truncated? }.
value &graph_callees(value &out, int64_t handle, int64_t func_id)
	{ madc_graph_callees(&out, handle, func_id); return out; }
value &graph_callers(value &out, int64_t handle, int64_t func_id)
	{ madc_graph_callers(&out, handle, func_id); return out; }
value &graph_references(value &out, int64_t handle, int64_t def_id)
	{ madc_graph_references(&out, handle, def_id); return out; }
value &graph_search(value &out, int64_t handle, const char *kind, const char *name_sub)
	{ std::string k = kind ? kind : ""; std::string n = name_sub ? name_sub : "";
	  madc_graph_search(&out, handle, &k, &n); return out; }
value &graph_impact(value &out, int64_t handle, int64_t id)
	{ madc_graph_impact(&out, handle, id); return out; }
```

- [ ] **Step 5: `include/madc/ns_madc` declaration-only decls (after line 202)**

```cpp
    // Code-graph MCP L2 (design 2026-09-12): derived CALLS/REFERENCES edges.
    //   graph_callees(handle, func_id): functions func_id calls (CALLS out).
    //   graph_callers(handle, func_id): functions that call func_id (CALLS in).
    //   graph_references(handle, def_id): every use-site of def_id (REFERENCES,
    //     site granularity + enclosing) — def_id a FUNCTION type-id OR a GLOBAL
    //     decl-id (from graph_search). graph_search(handle, kind, name~): decl
    //     nodes by kind + name substring (v1: Function, Global). graph_impact(
    //     handle, id): batched callers + references + enclosing in one call.
    //   Each returns { nodes, edges?, truncated? }.
    value &graph_callees(value &out, int64_t handle, int64_t func_id);
    value &graph_callers(value &out, int64_t handle, int64_t func_id);
    value &graph_references(value &out, int64_t handle, int64_t def_id);
    value &graph_search(value &out, int64_t handle, const char *kind, const char *name_sub);
    value &graph_impact(value &out, int64_t handle, int64_t id);
```

---

### Task 8: MCP seat — 5 descriptors + 5 dispatch arms

**Files:** Modify `tools/madcide/madcide_mcp.inc` — `graph_tool_descriptors`
(after t8, ~line 118) + `graph_call` (before `else known = false`, ~241).

- [ ] **Step 1: Descriptors (after `tools[] = t8;`)**

```cpp
    // Code-graph MCP L2 (design 2026-09-12): the derived-edge verbs.
    var kindprop = { "type": "string", "description": "a node kind filter (optional; v1: Function, Global)" };
    var subprop = { "type": "string", "description": "a name substring to match" };
    var searchprops = { "kind": kindprop, "name": subprop };
    var searchargs = { "type": "object", "properties": searchprops };
    var funcargprops = { "func": funcprop };
    var funcargs = { "type": "object", "properties": funcargprops };
    var defprop = { "type": "integer", "description": "a function type-id or a global decl-id" };
    var defprops = { "func": defprop };
    var defargs = { "type": "object", "properties": defprops };
    var t9 = { "name": "graph.callees",
	"description": "the functions a function calls (CALLS out-edges)",
	"inputSchema": funcargs };
    tools[] = t9;
    var t10 = { "name": "graph.callers",
	"description": "the functions that call a function (CALLS in-edges)",
	"inputSchema": funcargs };
    tools[] = t10;
    var t11 = { "name": "graph.references",
	"description": "every use-site of a function or global (REFERENCES + enclosing)",
	"inputSchema": defargs };
    tools[] = t11;
    var t12 = { "name": "graph.search",
	"description": "find decl nodes by kind + name substring (v1: functions, globals)",
	"inputSchema": searchargs };
    tools[] = t12;
    var t13 = { "name": "graph.impact",
	"description": "batched callers + references + enclosing for a function or global",
	"inputSchema": idargs };
    tools[] = t13;
```

> `references` accepts `func` (a def id — function or global); `impact` reuses
> `idargs` (`id`), matching §6.4's `impact(id)`. Match the dispatch below exactly.

- [ ] **Step 2: Dispatch arms (before `else known = false;`)**

```cpp
    else if ( strcmp(verb, "graph.callees") == 0 )
	madc::graph_callees(snap, h, args["func"].as_integer());
    else if ( strcmp(verb, "graph.callers") == 0 )
	madc::graph_callers(snap, h, args["func"].as_integer());
    else if ( strcmp(verb, "graph.references") == 0 )
	madc::graph_references(snap, h, args["func"].as_integer());
    else if ( strcmp(verb, "graph.search") == 0 )
	madc::graph_search(snap, h, args["kind"].c_str(), args["name"].c_str());
    else if ( strcmp(verb, "graph.impact") == 0 )
	madc::graph_impact(snap, h, args["id"].as_integer());
```

> `args["kind"]`/`args["name"]` on an absent key return a null `var`; `.c_str()`
> on a null `var` yields `""` (the L1 `graph.definition` arm relies on the same).
> CB-3 confirms.

- [ ] **Step 3: Single build**

```bash
make -C src ../bin/madc
```
Expected: exit 0, zero warnings.

---

### Task 9: Tests

**Files:**
- Create: `tests/testgraphedges.mad`, `.expect`, `.expect_quiet`.
- Modify: `tests/testmadcide_serve_graph.mad`, `.expect`.

- [ ] **Step 1: `tests/testgraphedges.mad` (value-first; functions + a global)**

```c
// Code-graph MCP L2: the DERIVED-EDGE verbs over the live parse-handle forest.
// callees/callers/references/impact walk every call site (resolved via the ONE
// overload-aware resolver) and every name-use (by Variable identity), reporting
// CALLS/REFERENCES edges — all compiler ground truth. Covers a global variable
// referenced across two functions. Compile-NEVER-execute (.expect_quiet proves
// it — an executed buffer would print and break .expect).
int main()
{
    var src = "long g_total = 0;\n"
	      "long helper(long x) { return x * 2; }\n"
	      "long twice(long x) { g_total = g_total + 1; return helper(x); }\n"
	      "long run(long n) {\n"
	      "    long t = 0;\n"
	      "    for (long i = 0; i < n; i = i + 1)\n"
	      "        t = t + twice(i) + helper(i) + g_total;\n"
	      "    return t;\n"
	      "}\n";
    long h = madc::parse_open(src.c_str(), "graphedges.mad");
    println("opened: {}", h > 0 ? 1 : 0);

    // Function ids from graph.symbols.
    var sy;
    madc::graph_symbols(sy, h);
    long id_helper = 0, id_twice = 0, id_run = 0;
    for ( var n : sy["nodes"] )
    {
	if ( n["name"] == "helper" ) id_helper = n["id"].as_integer();
	if ( n["name"] == "twice" )  id_twice  = n["id"].as_integer();
	if ( n["name"] == "run" )    id_run    = n["id"].as_integer();
    }
    println("found funcs: {}", (id_helper && id_twice && id_run) ? 1 : 0);

    // The global id from graph.search(kind=Global).
    var gs;
    madc::graph_search(gs, h, "Global", "g_total");
    long id_global = 0;
    var gkind;
    for ( var n : gs["nodes"] )
	if ( n["name"] == "g_total" ) { id_global = n["id"].as_integer(); gkind = n["kind"]; }
    println("found global: {}", id_global != 0 ? 1 : 0);
    println("global kind is Global: {}", (gkind == "Global") ? 1 : 0);

    // callees(run) -> {twice, helper}; all edges CALLS.
    var ce;
    madc::graph_callees(ce, h, id_run);
    var seen_callee;
    for ( var n : ce["nodes"] )
	seen_callee[n["name"].c_str()] = true;
    println("run calls twice: {}", seen_callee["twice"].is_null() ? 0 : 1);
    println("run calls helper: {}", seen_callee["helper"].is_null() ? 0 : 1);
    bool ce_all_calls = true;
    for ( var e : ce["edges"] )
	if ( !(e["kind"] == "CALLS") ) ce_all_calls = false;
    println("callees edges all CALLS: {}", ce_all_calls ? 1 : 0);

    // callers(helper) -> {twice, run}; callers(twice) -> {run}.
    var cr;
    madc::graph_callers(cr, h, id_helper);
    var seen_caller;
    for ( var n : cr["nodes"] )
	seen_caller[n["name"].c_str()] = true;
    println("helper called by twice: {}", seen_caller["twice"].is_null() ? 0 : 1);
    println("helper called by run: {}", seen_caller["run"].is_null() ? 0 : 1);
    var cr2;
    madc::graph_callers(cr2, h, id_twice);
    var seen_caller2;
    for ( var n : cr2["nodes"] )
	seen_caller2[n["name"].c_str()] = true;
    println("twice called by run: {}", seen_caller2["run"].is_null() ? 0 : 1);

    // references(helper): >= 2 call-sites; all REFERENCES.
    var rf;
    madc::graph_references(rf, h, id_helper);
    long refedges = 0;
    bool rf_all_refs = true;
    for ( var e : rf["edges"] )
    { refedges = refedges + 1; if ( !(e["kind"] == "REFERENCES") ) rf_all_refs = false; }
    println("helper reference edges >= 2: {}", refedges >= 2 ? 1 : 0);
    println("references edges all REFERENCES: {}", rf_all_refs ? 1 : 0);

    // references(g_total): the global is used in twice (write+read) and run
    // (read) -> >= 2 REFERENCES sites; all REFERENCES.
    var gr;
    madc::graph_references(gr, h, id_global);
    long grefs = 0;
    bool gr_all_refs = true;
    for ( var e : gr["edges"] )
    { grefs = grefs + 1; if ( !(e["kind"] == "REFERENCES") ) gr_all_refs = false; }
    println("global reference edges >= 2: {}", grefs >= 2 ? 1 : 0);
    println("global references all REFERENCES: {}", gr_all_refs ? 1 : 0);

    // search kind=Function name~"help" finds helper.
    var se;
    madc::graph_search(se, h, "Function", "help");
    bool found_helper = false;
    for ( var n : se["nodes"] )
	if ( n["name"] == "helper" ) found_helper = true;
    println("search help finds helper: {}", found_helper ? 1 : 0);

    // impact(helper): CALLS and REFERENCES present.
    var im;
    madc::graph_impact(im, h, id_helper);
    bool im_calls = false, im_refs = false;
    for ( var e : im["edges"] )
    { if ( e["kind"] == "CALLS" ) im_calls = true; if ( e["kind"] == "REFERENCES" ) im_refs = true; }
    println("impact has CALLS: {}", im_calls ? 1 : 0);
    println("impact has REFERENCES: {}", im_refs ? 1 : 0);

    // impact(g_total): REFERENCES present, no CALLS (globals aren't called).
    var gim;
    madc::graph_impact(gim, h, id_global);
    bool gim_calls = false, gim_refs = false;
    for ( var e : gim["edges"] )
    { if ( e["kind"] == "CALLS" ) gim_calls = true; if ( e["kind"] == "REFERENCES" ) gim_refs = true; }
    println("global impact has REFERENCES: {}", gim_refs ? 1 : 0);
    println("global impact has no CALLS: {}", gim_calls ? 0 : 1);

    // Ground-truth guards: unknown id -> empty; callees(global) -> empty
    // (a global has no callees).
    var none;
    madc::graph_callees(none, h, 999999);
    println("callees unknown func nodes: {}", php::count(none["nodes"]));
    var gce;
    madc::graph_callees(gce, h, id_global);
    println("callees of a global nodes: {}", php::count(gce["nodes"]));

    // Re-query stability (recon Q1 residual): a second callees(run) is unchanged.
    var ce2;
    madc::graph_callees(ce2, h, id_run);
    var seen2;
    for ( var n : ce2["nodes"] )
	seen2[n["name"].c_str()] = true;
    println("requery stable: {}",
	    (!seen2["twice"].is_null() && !seen2["helper"].is_null()) ? 1 : 0);

    madc::parse_close(h);
    return 0;
}
```

- [ ] **Step 2: `tests/testgraphedges.expect`**

```
opened: 1
found funcs: 1
found global: 1
global kind is Global: 1
run calls twice: 1
run calls helper: 1
callees edges all CALLS: 1
helper called by twice: 1
helper called by run: 1
twice called by run: 1
helper reference edges >= 2: 1
references edges all REFERENCES: 1
global reference edges >= 2: 1
global references all REFERENCES: 1
search help finds helper: 1
impact has CALLS: 1
impact has REFERENCES: 1
global impact has REFERENCES: 1
global impact has no CALLS: 1
callees unknown func nodes: 0
callees of a global nodes: 0
requery stable: 1
```

- [ ] **Step 3: `tests/testgraphedges.expect_quiet`** — create the file (content
  ignored; presence enables the empty-stderr JIT check).

- [ ] **Step 4: Extend `tests/testmadcide_serve_graph.mad`** — after the L1b
  seat block, drive the 5 L2 tools over the seat against a served fixture with
  two functions calling each other and a shared global. Assert the 5 tool names
  appear in `tools/list`, and that a CALLS edge (from callees/callers) and a
  REFERENCES edge (from references on the global) appear in the structured
  results. Mirror the existing L1b seat-drive block exactly (same
  `ensure_phandle`, same structured-content parse). Add the matching lines to
  `tests/testmadcide_serve_graph.expect`.

- [ ] **Step 5: Run the targeted tests**

```bash
( ulimit -t 120; timeout 180 bin/madc tests/testgraphedges.mad )
```
Expected: exit 0, empty stderr, every `.expect` line present. Then:
```bash
( ulimit -t 120; timeout 180 bin/madc tests/testmadcide_serve_graph.mad )
```
Expected: exit 0, empty stderr, every `.expect` line present.

- [ ] **Step 6: Commit** (scoped like L1b)

Commit A — engine + plumbing (`src/` + `include/`, rule trailers):
`src/madc_program.cpp`, `src/parser.cpp`, `include/ns_common.h`,
`src/ns_madc.cpp`, `include/madc/ns_madc`.

Commit B — MCP seat + tests (`tools/` + `tests/`, no trailers):
`tools/madcide/madcide_mcp.inc`, `tests/testgraphedges.*`,
`tests/testmadcide_serve_graph.*`.

Commit C (if the design doc needs a clarifying note) — `docs/`.

---

## Confirm-before-build points

Pre-edit confirmations the implementer resolves against real source BEFORE the
single build — not conflicts:

- **CB-1 (`<set>` availability).** The dedup uses `std::set<uint32_t>`. Confirm
  `<set>`/`<unordered_set>` is included in `src/madc_program.cpp`; add if
  missing.
- **CB-2 (search type-coverage).** v1 `graph.search` covers Function + Global.
  Struct/typedef/enum search needs the decl-map iterators — leave deferred with
  a code comment; do NOT invent a `datatype_map` iteration API.
- **CB-3 (null-var `.c_str()`).** Confirm a null `var`'s `.c_str()` returns `""`
  (the L1 `graph.definition` arm relies on it); else guard with `is_null()`.
- **CB-4 (Variable identity across use-sites).** Confirm `&TokenVar::var` at a
  use-site equals `top_decls[i].var` for the same global — i.e. a use-site is
  `new TokenVar(*findVariable(name))` and `findVariable` returns the one global
  `Variable*` (traced: `parser.cpp:13730/15892`, `datatokens.h:277`,
  `madc.h:747/761`). If identity somehow does NOT hold for some global path,
  fall back to (name + global-scope) match and note it — but pointer identity is
  the verified expectation.
- **CB-5 (qualified `TopDecl` spelling).** Confirm `::Program::TopDecl`,
  `::Program::DeclKind::dkGlobalVar`, and `child.top_decls` compile from the
  file-scope static helpers in `src/madc_program.cpp` (they are `class Program`
  members, `madc.h:4742-4757`); adjust the qualification if the file already
  aliases `Program`.

---

## Self-review (controller, against the spec)

- **Spec coverage (§6.4 L2):** `graph.references` ✓ (Task 4, functions + globals),
  `graph.callers` ✓ (Task 3), `graph.callees` ✓ (Task 2), `graph.search` ✓
  (Task 5, functions + globals), `graph.impact` ✓ (Task 6, batched, routed).
  The `graph.query` cypher/gql escape hatch is a LATER layer, not L2.
- **Invariants:** live structures only (TokenBase AST + live `top_decls`) ✓;
  ground-truth edges (resolver + Variable identity, no inference) ✓; enums for
  edge kinds ✓ (Task 1); terse + capped results ✓; entity handles/ids never
  offsets ✓ (a partitioned decl-id, sibling to the body-id) ; no-parallel-
  implementations ✓ (two collectors share the ONE descent; reuse resolver + id
  round-trip + `top_decls`); MCP-as-adapter ✓.
- **Placeholder scan:** every code step carries real code; the deferrals (type-
  as-target; typedef/enum search; params/locals/members; member-access-of-global)
  are documented layer boundaries with stated reasons, not silent gaps.
- **Type consistency:** verb arg order `(out, handle, id)` uniform;
  `func`/`id`/`kind`/`name` property names matched between descriptors (Task 8
  Step 1) and dispatch (Step 2); `internal_program_graph_*` signatures match
  across engine (2-6), forward decls + bridges (7.1-7.2), prototypes (7.3), and
  wrappers (7.4-7.5). `graph_id_space`/`GRAPH_DECL_ID_BASE`/`graph_global_*`
  defined in Task 1 before every consumer.
- **Global widening is coherent:** the decl-id partition is truncation-safe
  (Task 1 Step 6 guards the L1/L1b verbs); a global is discoverable
  (`graph.search`), addressable (`graph_global_node_value`), and its references
  derive by the same forest walk (collector #2). callees/callers stay
  function-only (a global has no call edges — asserted in Task 9).
- **The open decision (§10):** edges are **on-demand** (no cache) in v1,
  isolated behind the two collectors so a per-handle incremental cache is a
  drop-in memoization later — decided default, flagged for owner veto.

## SDD execution note

Tasks 1-9 are tightly coupled (all in `src/madc_program.cpp` + the shared
five-layer plumbing + one seat file + tests), the same shape as L1/L1b.
Recommended execution: ONE implementer does Tasks 1-8 with NO intermediate
builds, then the single `make -C src ../bin/madc`, then the Task 9 targeted
tests, then commits A/B(/C). Controller reviews the full diff line-by-line;
then an independent READ-ONLY reviewer (the L1b go-forward gate — it found F1
with zero stall friction); then a fix round if needed (inline, per owner
subagent fatigue). The controller may instead implement inline (verified recon
+ verbatim templates make most of this mechanical) and keep only the
independent read-only review — owner's execution choice. The FULL battery is
NOT this slice's job; it rides the V6 arc RELEASE seam (owner law). Ledger:
`tmp/sdd-ast-graph-mcp-L2/progress.md`.
