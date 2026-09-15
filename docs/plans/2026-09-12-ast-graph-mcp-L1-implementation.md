# Code-Graph MCP — L1 (read accessors) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the running madc's **live declaration/type graph** to an LLM agent over the existing MCP seat as a node-addressed, verb-queried surface — `graph.symbols / definition / node / type_of / members / bases / enclosing` — reading the live in-process compiler state (never a re-parse, never a copy).

**Architecture:** Each verb is a new `internal_program_graph_*` walker on the parse-handle's already-compiled child `::Program` (the exact pattern `internal_program_parse_outline/enclosing` use), reached through the proven three-layer surface — engine walker (`madc_program.cpp`) → C bridge (`parser.cpp`) → declaration-only dialect wrapper resolved mangled-direct (`ns_madc.cpp` + `include/madc/ns_madc`). The MCP seat gains a `graph.*` **tool family** dispatched by name before the editor-command path. Every verb returns a **terse structured node projection** (`{id, kind, name, span?}`), extending the `session_state` result shipped at `9620e32b`.

**Tech Stack:** C++11 (engine); the madc dialect (`--std=madc`) for the MCP seat and the tests; MCP JSON-RPC 2.0 over stdio; the `madc::value` carrier for all structured results.

**Spec:** `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` (the approved design; this plan implements its §6.4 **L1** verb family, less `children`/`body` — see Scope below).

## Global Constraints

- **Live, in-process only** (spec §5.1). L1 reads the child `::Program`'s live structures — `pending_funcs`, `project_types`, the name maps. It NEVER touches the freeze-only `DefArena`: `forest_arena_enabled` is set on a parse handle only under `#ifdef _WIN32` (`parse_handle_child_init`, `src/madc_program.cpp:4935`), so `get_def_at` is empty on Linux/macOS. The live `project_types` (`id_table<DataDef>`) is the type graph on every platform; `DefArena` is its frozen mirror, reserved for L4.
- **Stable ids, never byte offsets** (spec §5.3). A node id is a **type-id** — `madc_type_id_for(DataDef*)` (declared `include/datadef.h:2131`) — stable for the handle's current parse; after a mutating command the agent re-queries.
- **Node-kind is an enum at the source, a name only at the wire** (spec §5.4, enum-over-strings law). Map `DataDef::basetype()` (+ `is_pointer()`/`is_reference()`) to a wire name in ONE `switch`; never compare kind strings internally.
- **cpp-first-api / dialect-lean.** The dialect accessors are declaration-only C++ functions in `include/madc/ns_madc` (a dialect fragment: ZERO system includes; `value`/`int64_t`/`const char*`/`bool` signatures only), resolved mangled-direct to real `namespace madc { }` bodies in `src/ns_madc.cpp` — the exact `parse_*` convention. No `extern "C"`.
- **Terse by default** (spec §5.7, §6.5). Result shape `{ nodes: [ {id, kind, name, span?} ] }` for lists; a bare node object for singletons. `span?` = `line/column/end_line`, present only for a source-anchored node (a function). No unfiltered header dump.
- **Every `src/`+`include/` commit carries the four rule trailers** (`Hypothesis:`/`Layer:`/`Searched:`/`Oracle:`; `check-rule-trailers.sh`). For pure-exposure walkers `Oracle:` is `n/a — new read surface; the .mad accessor test is the oracle`.
- **Targeted tests per change; the battery runs ONCE at the track's release seam** (testing-fulltest law) — never per task. Each task below gates on its own `.mad` test plus the api/mcp neighbors (`testmadcide_serve_mcp`, `testparsehandle`), not the full suite.
- **Value-first tests** (value-first law): `.mad` tests carry ZERO includes/`using`/`std::`; `var` (never `value`) as the carrier; `println`/`format` for output; `php::count` for lengths. Every accessor test ships a `.expect` and a `.expect_quiet` (compile-never-execute proof, the `testparsehandle` precedent).
- **Shell hygiene:** one simple command per invocation; no `&&` chains.
- **Scratch/reducers** go in `tmp/` (gitignored), never in `tests/` or the repo root.

## Scope

This plan implements the **read half of L1** — the declaration/type graph (spec §6.4 L1: `symbols, definition, node, type_of, members, bases, enclosing`). These are pure exposure of already-materialized live structures → fast, low-risk, and they lock the addressing + verb + result ergonomics everything else builds on.

`graph.children` and `graph.body` (also listed under §6.4 L1) are **deferred to an L1b slice with its own writing-plans pass**: they require a new body-node enum (`Block/Statement/Call/NameRef/Expr/Literal`, spec §4 greenfield #1) over the `TokenCpnd` parse subtree (`child`/`parent`-linked, not a flat vector) — the one genuinely greenfield schema item in L1. Keeping it separate keeps this slice "expose what exists." (Surfaced to the owner in the handoff; body-walking rides L1b unless the owner wants it folded in.)

**Branch:** continue on the arc branch `feature/client-server-views-claude` as a distinct track (spec §10 lean — it extends V6's MCP seat; the V6 seam battery still gates the develop merge).

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/madc_program.cpp` | The `internal_program_graph_*` **walkers** (the real work) + shared node-projection helpers (`graph_kind_name`, `graph_node_value`). Sits beside `internal_program_parse_*`. | Modify (append after the parse-handle block, ~line 5499) |
| `src/parser.cpp` | The `madc_graph_*` **C bridges** (`void*`/`int64_t` thunks). Sits beside `madc_parse_*`. | Modify (append after `madc_parse_spans`, ~line 1105) |
| `include/ns_common.h` | Declarations of the `madc_graph_*` bridges. Beside `madc_parse_*` (~line 226). | Modify |
| `src/ns_madc.cpp` | The `namespace madc { }` **dialect wrappers** `graph_*` (mangled-direct targets). Beside `parse_*` (~line 246). | Modify |
| `include/madc/ns_madc` | Declaration-only dialect declarations of `graph_*` (a dialect fragment). Beside `parse_*` (~line 159). | Modify |
| `tools/madcide/madcide_mcp.inc` | The MCP `graph.*` **tool family**: `graph_call` (tool name → accessor), the descriptors in `mcp_tools`, the `tools/call` dispatch in `mcp_handle` (graph.* before the registry path). | Modify |
| `tests/testgraphaccessors.mad` (+ `.expect`, `.expect_quiet`) | Dialect integration test driving `madc::graph_*` directly on a fixture — the accessor gate (mirrors `testparsehandle.mad`). | Create |
| `tests/testmadcide_serve_graph.mad` (+ `.input`, `.expect`) | Dialect integration test driving the `graph.*` tools over the MCP seat (mirrors `testmadcide_serve_mcp.mad`). | Create |

### Plumbing pattern (the three layers — established by `parse_*`, repeated per verb)

Every verb adds one line/short body at each layer. For a verb `graph.X` returning a `value`:

1. **Engine walker** — `src/madc_program.cpp`:
   ```cpp
   bool internal_program_graph_X(int64_t handle, /*args,*/ madc::value &out);
   ```
   Gets `parse_tu_state *st = parse_tu_get(handle); if (!st) return false;`, walks `*st->child`, fills `out`. Returns `true` for a valid handle (even with an empty result), `false` for a bad handle.
2. **C bridge** — `src/parser.cpp`:
   ```cpp
   void *madc_graph_X(void *result, int64_t handle /*, args*/)
   { madc::value &out = *(madc::value *)result;
     madc::internal_program_graph_X(handle, /*args,*/ out); return result; }
   ```
3. **Bridge declaration** — `include/ns_common.h`: the `madc_graph_X` prototype.
4. **Dialect wrapper** — `src/ns_madc.cpp`:
   ```cpp
   value &graph_X(value &out, int64_t handle /*, args*/)
   { madc_graph_X(&out, handle /*, args*/); return out; }
   ```
5. **Dialect declaration** — `include/madc/ns_madc`: `value &graph_X(value &out, int64_t handle /*, args*/);`

The engine walker signatures also need forward declarations near the other `internal_program_*` prototypes (search `internal_program_parse_outline` in the `madc` namespace's declaration block — the same header/section that already declares the parse handles).

---

## Task 1: Node projection helpers + `graph.symbols` (foundational)

Establishes the terse node value, the kind enum→name map, the id addressing, the full three-layer plumbing, and the first walker. Every later task reuses these helpers and this pattern.

**Files:**
- Modify: `src/madc_program.cpp` (append after `internal_program_parse_spans`, ~line 5499)
- Modify: `src/parser.cpp` (append after `madc_parse_spans`, ~line 1105)
- Modify: `include/ns_common.h` (after `madc_parse_spans`, ~line 226)
- Modify: `src/ns_madc.cpp` (after `parse_spans`, ~line 247)
- Modify: `include/madc/ns_madc` (after the `parse_*` decls, ~line 159)
- Test: `tests/testgraphaccessors.mad`, `tests/testgraphaccessors.expect`, `tests/testgraphaccessors.expect_quiet`

**Interfaces:**
- Produces (used by every later task):
  - `static const char *graph_kind_name(DataDef *dd)` — kind wire name.
  - `static madc::value graph_node_value(DataDef *dd)` — terse `{id, kind, name}` for a type/function DataDef.
  - `static madc::value graph_func_node_value(TokenFunc *tf)` — terse `{id, kind:"Function", name, line, column, end_line}` for a source-anchored function.
  - `bool internal_program_graph_symbols(int64_t handle, madc::value &out)` → `{ nodes: [...] }`.
  - Dialect: `value &madc::graph_symbols(value &out, int64_t handle)`.

- [ ] **Step 1: Write the failing test** — `tests/testgraphaccessors.mad`

```c
// Code-graph MCP L1: the live declaration/type graph accessors, given a
// LIFETIME by a parse handle. Compile-NEVER-execute (.expect_quiet is the
// proof — an executed buffer would print EXECUTED and break .expect).
int main()
{
    var src = "struct Point { long x; long y; };\n"
	      "long add(long a, long b) { return a + b; }\n"
	      "long twice(long n) { return add(n, n); }\n";
    long h = madc::parse_open(src.c_str(), "graph.mad");
    println("opened: {}", h > 0 ? 1 : 0);

    // graph.symbols — the TU's own functions (file-accurate, spanned).
    var sy;
    madc::graph_symbols(sy, h);
    var nodes = sy["nodes"];
    println("symbols: {}", php::count(nodes));
    for ( var n : nodes )
	println("  {} {} @ {}:{}", n["kind"], n["name"], n["line"],
		n["column"]);
    madc::parse_close(h);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: FAIL — a compile error, `graph_symbols` is not a member of `madc` (the accessor does not exist yet).

- [ ] **Step 3: Add the shared helpers + walker** — `src/madc_program.cpp`, after `internal_program_parse_spans` (~line 5499)

```cpp
// ---- Code-graph MCP L1 (design 2026-09-12): the live declaration/type
// graph as node-addressed accessors, beside the parse-handle surface. A
// node id is a TYPE-ID (madc_type_id_for); the graph is the child's LIVE
// project_types + name maps + pending_funcs — never the freeze-only
// DefArena (empty off-Windows). "Index is not the graph": the name maps
// are the declindex, project_types is the type graph.

// Kind wire name from the live kind discriminator — enum-law: switch on
// basetype(), with the two derived-type predicates first (a pointer /
// reference DataDef reports basetype() btSimple but overrides is_pointer /
// is_reference). Searched "BaseType -> display name": no existing helper
// (grep src/ include/ 2026-09-12) — this is the one home.
static const char *graph_kind_name(DataDef *dd)
{
    if ( dd->is_pointer() )   return "Pointer";
    if ( dd->is_reference() ) return "Reference";
    switch ( dd->basetype() )
    {
    case BaseType::btFunct:         return "Function";
    case BaseType::btStruct:        return "Struct";
    case BaseType::btClass:         return "Class";
    case BaseType::btTemplateParam: return "TemplateParam";
    case BaseType::btSimple:        return "Type";
    }
    return "Type";
}

// The terse node projection (spec §6.5): { id, kind, name }. id is the
// stamped type-id (stable for this parse); a source span is added only for
// a source-anchored node (graph_func_node_value).
static madc::value graph_node_value(DataDef *dd)
{
    std::map<std::string, madc::value> f;
    f["id"] = value((int64_t)madc_type_id_for(dd));
    f["kind"] = value(std::string(graph_kind_name(dd)));
    f["name"] = value(dd->name);
    return value::make_object(f);
}

// A function node from its parse token: the FuncDef's type-id + the source
// span (line/column/end_line — the outline coordinates).
static madc::value graph_func_node_value(TokenFunc *tf)
{
    std::map<std::string, madc::value> f;
    f["id"] = value((int64_t)madc_type_id_for(tf->var.type));
    f["kind"] = value(std::string("Function"));
    f["name"] = value(tf->var.name);
    f["line"] = value((int64_t)tf->line);
    f["column"] = value((int64_t)tf->column);
    f["end_line"] = value((int64_t)tf->end_line);
    return value::make_object(f);
}

// graph.symbols(handle): the TU's OWN top-level functions as graph nodes —
// the same TU-own filter (tu_own_function) outline uses, so it is
// file-accurate and spanned. Types are reached by name/id (definition /
// node / members / bases), not by an unfiltered header dump (spec §6.5).
bool internal_program_graph_symbols(int64_t handle, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes;
    for ( size_t i = 0; i < child.pending_funcs.size(); ++i )
    {
	TokenFunc *tf = tu_own_function(child.pending_funcs[i],
					st->display_name);
	if ( !tf )
	    continue;
	nodes.push_back(graph_func_node_value(tf));
    }
    std::map<std::string, madc::value> res;
    res["nodes"] = value::make_array(nodes);
    out = value::make_object(res);
    return true;
}
```

Also add the forward declaration for `internal_program_graph_symbols` beside the other `internal_program_*` prototypes (search the concept "internal_program_parse_outline declaration" to find the block).

- [ ] **Step 4: Add the C bridge** — `src/parser.cpp`, after `madc_parse_spans` (~line 1105)

```cpp
// Code-graph MCP L1 bridges (design 2026-09-12): result = madc::value*,
// handle = a parse handle. Thin thunks over internal_program_graph_*.
void *madc_graph_symbols(void *result, int64_t handle)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_symbols(handle, out);
    return result;
}
```

- [ ] **Step 5: Declare the bridge** — `include/ns_common.h`, after `madc_parse_spans` (~line 230)

```cpp
void *madc_graph_symbols(void *result, int64_t handle);
```

- [ ] **Step 6: Add the dialect wrapper** — `src/ns_madc.cpp`, after `parse_spans` (~line 247)

```cpp
value &graph_symbols(value &out, int64_t handle)
	{ madc_graph_symbols(&out, handle); return out; }
```

- [ ] **Step 7: Declare the dialect accessor** — `include/madc/ns_madc`, after the `parse_*` decls (~line 159)

```cpp
    // Code-graph MCP L1 (design 2026-09-12): the live declaration/type
    // graph as node-addressed reads over a parse handle. A node id is a
    // type-id; a node is { id, kind, name } (+ line/column/end_line for a
    // function). graph_symbols returns { nodes: [...] } — the TU's own
    // top-level functions.
    value &graph_symbols(value &out, int64_t handle);
```

- [ ] **Step 8: Write the fixtures**

`tests/testgraphaccessors.expect`:
```
opened: 1
symbols: 2
Function add @ 2:
Function twice @ 3:
```

`tests/testgraphaccessors.expect_quiet` (presence enables the empty-stderr / compile-never-execute check; content ignored):
```
compile-never-execute: no buffer with a main runs through the graph accessors
```

- [ ] **Step 9: Build**

Run: `make -C src`
Expected: builds without errors or new warnings.

- [ ] **Step 10: Run test to verify it passes**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: PASS — output contains each `.expect` line; stderr empty.

- [ ] **Step 11: Commit**

```bash
git add src/madc_program.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc tests/testgraphaccessors.mad tests/testgraphaccessors.expect tests/testgraphaccessors.expect_quiet
git commit
```
Commit message body (rule trailers required):
```
feat(graph-mcp): L1 graph.symbols — the live TU function graph as nodes

Hypothesis: an agent needs the TU's symbols as {id,kind,name,span} nodes
  over a parse handle, from the live child Program (not the freeze arena).
Layer:      internal_program_graph_symbols (madc_program.cpp) is the
  deepest — it reads the live pending_funcs, the same structure outline
  walks; the bridge/wrapper are transport only.
Searched:   "BaseType -> display name" (grep src/ include/) — no existing
  helper; "TU-own function filter" -> tu_own_function (reused).
Oracle:     n/a — new read surface; tests/testgraphaccessors.mad is the oracle.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
```

---

## Task 2: `graph.node(id)` + `graph.type_of(id)`

Node-by-id lookup (the addressing round-trip) and the HAS_TYPE edge (a node → its type).

**Files:**
- Modify: `src/madc_program.cpp`, `src/parser.cpp`, `include/ns_common.h`, `src/ns_madc.cpp`, `include/madc/ns_madc`
- Test: extend `tests/testgraphaccessors.mad` / `.expect`

**Interfaces:**
- Consumes: `graph_node_value` (Task 1); `Program::project_types` (`id_table<DataDef>&`, `include/madc.h:2765` — reachable from the child like `pending_funcs`; if exposed only via an accessor, use it) with `get(uint32_t)` → `DataDef*`.
- Produces:
  - `bool internal_program_graph_node(int64_t handle, int64_t node_id, madc::value &out)` → the node object (empty object if no such node).
  - `bool internal_program_graph_type_of(int64_t handle, int64_t node_id, madc::value &out)` → the type node.
  - Dialect: `value &madc::graph_node(value &out, int64_t handle, int64_t node_id)`, `value &madc::graph_type_of(value &out, int64_t handle, int64_t node_id)`.

- [ ] **Step 1: Write the failing test** — append to `tests/testgraphaccessors.mad`, before `parse_close`

```c
    // graph.node(id) round-trips a symbol id back to its node; graph.type_of
    // on a function id yields its return-type node.
    var first = nodes[0];
    long fid = first["id"].as_integer();
    var nd;
    madc::graph_node(nd, h, fid);
    println("node: {} {}", nd["kind"], nd["name"]);
    var ty;
    madc::graph_type_of(ty, h, fid);
    println("type_of: {}", ty["kind"]);
    var none;
    madc::graph_node(none, h, 0);
    println("node@0 empty: {}", none.is_null() || php::count(none) == 0 ? 1 : 0);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: FAIL — `graph_node` / `graph_type_of` not members of `madc`.

- [ ] **Step 3: Add the walkers** — `src/madc_program.cpp`, after `internal_program_graph_symbols`

```cpp
// graph.node(handle, id): the node for a type-id, from the child's LIVE
// type table. Empty object = a valid handle with no such project node
// (a pinned primitive id or an out-of-range id); false = a bad handle.
bool internal_program_graph_node(int64_t handle, int64_t node_id,
				 madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    DataDef *dd = st->child->project_types.get((uint32_t)node_id);
    if ( !dd )
    {
	std::map<std::string, madc::value> empty;
	out = value::make_object(empty);
	return true;
    }
    out = graph_node_value(dd);
    return true;
}

// graph.type_of(handle, id): the HAS_TYPE edge. A function -> its return
// type; a pointer/reference -> its operand (base_type; DataDefREF derives
// from DataDefPTR, so the cast reads both); anything else -> itself with
// top-level const peeled (unqualified()).
bool internal_program_graph_type_of(int64_t handle, int64_t node_id,
				    madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    DataDef *dd = st->child->project_types.get((uint32_t)node_id);
    if ( !dd )
    {
	std::map<std::string, madc::value> empty;
	out = value::make_object(empty);
	return true;
    }
    DataDef *ty;
    if ( dd->basetype() == BaseType::btFunct )
	ty = &((FuncDef *)dd)->returns;
    else if ( dd->is_pointer() || dd->is_reference() )
	ty = ((DataDefPTR *)dd)->base_type;
    else
	ty = dd->unqualified();
    out = graph_node_value(ty ? ty : dd);
    return true;
}
```

Add both forward declarations beside the Task 1 one.

- [ ] **Step 4: Add the bridges** — `src/parser.cpp`, after `madc_graph_symbols`

```cpp
void *madc_graph_node(void *result, int64_t handle, int64_t node_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_node(handle, node_id, out);
    return result;
}
void *madc_graph_type_of(void *result, int64_t handle, int64_t node_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_type_of(handle, node_id, out);
    return result;
}
```

- [ ] **Step 5: Declare the bridges** — `include/ns_common.h`

```cpp
void *madc_graph_node(void *result, int64_t handle, int64_t node_id);
void *madc_graph_type_of(void *result, int64_t handle, int64_t node_id);
```

- [ ] **Step 6: Add the dialect wrappers** — `src/ns_madc.cpp`

```cpp
value &graph_node(value &out, int64_t handle, int64_t node_id)
	{ madc_graph_node(&out, handle, node_id); return out; }
value &graph_type_of(value &out, int64_t handle, int64_t node_id)
	{ madc_graph_type_of(&out, handle, node_id); return out; }
```

- [ ] **Step 7: Declare the dialect accessors** — `include/madc/ns_madc`

```cpp
    value &graph_node(value &out, int64_t handle, int64_t node_id);
    value &graph_type_of(value &out, int64_t handle, int64_t node_id);
```

- [ ] **Step 8: Extend the expect** — add to `tests/testgraphaccessors.expect`

```
node: Function add
type_of: Type
node@0 empty: 1
```

- [ ] **Step 9: Build, run**

Run: `make -C src`
Run: `bin/madc tests/testgraphaccessors.mad`
Expected: PASS.

- [ ] **Step 10: Commit** (rule trailers; `Layer:` = the walkers own id→DataDef resolution; `Searched:` = "id_table reverse lookup" → `project_types.get`; `Oracle: n/a — read surface, the .mad test is the oracle`).

```bash
git add src/madc_program.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc tests/testgraphaccessors.mad tests/testgraphaccessors.expect
git commit
```

---

## Task 3: `graph.definition(name)`

Name → definition, the declindex face (spec §8: declindex = name lookup). Resolves a bare name against the child's function and type maps and returns the node(s).

**Files:** the five layers + `tests/testgraphaccessors.mad`/`.expect`.

**Interfaces:**
- Consumes: `graph_node_value` (Task 1); the child name maps — `funcdef_map` (`funcdef_map_t`, keyed by call name → `FuncDef*`), `struct_map` (`StructRegistry`, by mangled/bare name → `DataDefSTRUCT*`), `datatype_map` (`flat_datatype_map_t`, interned → `TokenDataType*`). Confirm the exact lookup call on each (`struct_map.get(name)` / `find`; `datatype_map` interned probe) via a pre-edit read of their class defs (`include/madc.h:2660`, `2742`; `StructRegistry` ~`1614`).
- Produces:
  - `bool internal_program_graph_definition(int64_t handle, const std::string &name, madc::value &out)` → `{ nodes: [...] }` (0, 1, or more matches).
  - Dialect: `value &madc::graph_definition(value &out, int64_t handle, const char *name)`.

- [ ] **Step 1: Write the failing test** — append to `tests/testgraphaccessors.mad`

```c
    // graph.definition(name) resolves a name to its node(s): a function,
    // then a struct type; an unknown name yields zero nodes.
    var df;
    madc::graph_definition(df, h, "add");
    println("def add: {} {}", php::count(df["nodes"]),
	    df["nodes"][0]["kind"]);
    var ds;
    madc::graph_definition(ds, h, "Point");
    println("def Point: {} {}", php::count(ds["nodes"]),
	    ds["nodes"][0]["kind"]);
    var dn;
    madc::graph_definition(dn, h, "nope");
    println("def nope: {}", php::count(dn["nodes"]));
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: FAIL — `graph_definition` not a member of `madc`.

- [ ] **Step 3: Add the walker** — `src/madc_program.cpp`

```cpp
// graph.definition(handle, name): the declindex face — resolve a bare name
// to its node(s) through the child's LIVE name maps (funcdef_map for
// functions, struct_map for aggregates, datatype_map for other named
// types). Returns { nodes: [...] } (0, 1, or several). This is name lookup,
// NOT the type graph (spec §8: index is not the graph).
bool internal_program_graph_definition(int64_t handle,
				       const std::string &name,
				       madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes;
    // Function definitions (funcdef_map -> FuncDef*, which is a DataDef).
    funcdef_map_iter fi = child.funcdef_map.find(name);
    if ( fi != child.funcdef_map.end() && fi->second )
	nodes.push_back(graph_node_value(fi->second));
    // Aggregate/type definitions (struct_map / datatype_map). Use the
    // confirmed lookup call for each registry (pre-edit read).
    if ( DataDefSTRUCT *sd = child.struct_map.get(name) )
	nodes.push_back(graph_node_value(sd));
    else if ( TokenDataType *td = child.datatype_lookup(name) )
	if ( td->datadef )
	    nodes.push_back(graph_node_value(td->datadef));
    std::map<std::string, madc::value> res;
    res["nodes"] = value::make_array(nodes);
    out = value::make_object(res);
    return true;
}
```

> Note for the implementer: `struct_map.get`, `datatype_lookup`, and `TokenDataType::datadef` are the *expected* spellings — confirm each against `include/madc.h` before building (search "struct_map lookup by name" and "datatype_map name probe"). If the real spelling differs (e.g. `struct_map.find(name)` returning an iterator, or the interned `datatype_map` probe requiring a `type_name_pool` intern first), adapt to it — the walker's shape (probe each map, push the node) is what matters, not the exact call.

- [ ] **Step 4–7: Bridge / decl / wrapper / dialect decl**

`src/parser.cpp`:
```cpp
void *madc_graph_definition(void *result, int64_t handle, void *name)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_definition(handle,
					    *(const std::string *)name, out);
    return result;
}
```
`include/ns_common.h`:
```cpp
void *madc_graph_definition(void *result, int64_t handle, void *name);
```
`src/ns_madc.cpp`:
```cpp
value &graph_definition(value &out, int64_t handle, const char *name)
	{ std::string n = name ? name : ""; madc_graph_definition(&out, handle, &n); return out; }
```
`include/madc/ns_madc`:
```cpp
    value &graph_definition(value &out, int64_t handle, const char *name);
```

- [ ] **Step 8: Extend the expect**

```
def add: 1 Function
def Point: 1 Struct
def nope: 0
```

- [ ] **Step 9: Build, run**

Run: `make -C src`
Run: `bin/madc tests/testgraphaccessors.mad`
Expected: PASS.

- [ ] **Step 10: Commit** (rule trailers; `Searched:` names the name-map lookups the walker uses).

```bash
git add src/madc_program.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc tests/testgraphaccessors.mad tests/testgraphaccessors.expect
git commit
```

---

## Task 4: `graph.members(type)` + `graph.bases(type)`

The aggregate structure edges: MEMBER_OF (a struct/class → its members, each with its type node inline) and INHERITS (a class → its direct bases).

**Files:** the five layers + `tests/testgraphaccessors.mad`/`.expect`.

**Interfaces:**
- Consumes: `graph_node_value` (Task 1); `project_types.get` (Task 2); `DataDefSTRUCT::members` (`std::vector<memberpair_t>`; `.first` = name, `.second` = `DataDef*`), `DataDefSTRUCT::member_offsets`; `DataDefCLASS::bases` (`std::vector<BaseSpec>`; `BaseSpec::base` = `DataDefCLASS*`). Kind test: `basetype() == btStruct || btClass` for members; `btClass` for bases.
- Produces:
  - `bool internal_program_graph_members(int64_t handle, int64_t type_id, madc::value &out)` → `{ nodes: [...] }` where each node is `{name, offset, type: {id,kind,name}}`.
  - `bool internal_program_graph_bases(int64_t handle, int64_t type_id, madc::value &out)` → `{ nodes: [...] }` of base type nodes.
  - Dialect: `value &madc::graph_members(value &out, int64_t handle, int64_t type_id)`, `value &madc::graph_bases(value &out, int64_t handle, int64_t type_id)`.

- [ ] **Step 1: Write the failing test** — append to `tests/testgraphaccessors.mad`

```c
    // graph.members / graph.bases on the Point struct id.
    long pid = ds["nodes"][0]["id"].as_integer();
    var mem;
    madc::graph_members(mem, h, pid);
    println("members: {}", php::count(mem["nodes"]));
    for ( var m : mem["nodes"] )
	println("  {} : {}", m["name"], m["type"]["kind"]);
    var bz;
    madc::graph_bases(bz, h, pid);
    println("bases: {}", php::count(bz["nodes"]));
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: FAIL — `graph_members` / `graph_bases` not members of `madc`.

- [ ] **Step 3: Add the walkers** — `src/madc_program.cpp`

```cpp
// graph.members(handle, type_id): the MEMBER_OF edges — each member with
// its byte offset and its type node inline (the member -> type HAS_TYPE
// edge, so the agent gets structure + types in one call). Empty nodes for
// a non-aggregate.
bool internal_program_graph_members(int64_t handle, int64_t type_id,
				    madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    std::vector<madc::value> nodes;
    DataDef *dd = st->child->project_types.get((uint32_t)type_id);
    if ( dd && (dd->basetype() == BaseType::btStruct
	     || dd->basetype() == BaseType::btClass) )
    {
	DataDefSTRUCT *sd = (DataDefSTRUCT *)dd;
	for ( size_t i = 0; i < sd->members.size(); ++i )
	{
	    if ( sd->members[i].first.empty() )
		continue;			// an unnamed bitfield slot
	    std::map<std::string, madc::value> m;
	    m["name"] = value(sd->members[i].first);
	    if ( i < sd->member_offsets.size() )
		m["offset"] = value((int64_t)sd->member_offsets[i]);
	    if ( sd->members[i].second )
		m["type"] = graph_node_value(sd->members[i].second);
	    nodes.push_back(value::make_object(m));
	}
    }
    std::map<std::string, madc::value> res;
    res["nodes"] = value::make_array(nodes);
    out = value::make_object(res);
    return true;
}

// graph.bases(handle, type_id): the INHERITS edges — a class's direct bases
// as type nodes. Empty for a non-class.
bool internal_program_graph_bases(int64_t handle, int64_t type_id,
				  madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    std::vector<madc::value> nodes;
    DataDef *dd = st->child->project_types.get((uint32_t)type_id);
    if ( dd && dd->basetype() == BaseType::btClass )
    {
	DataDefCLASS *cd = (DataDefCLASS *)dd;
	for ( size_t i = 0; i < cd->bases.size(); ++i )
	    if ( cd->bases[i].base )
		nodes.push_back(graph_node_value(cd->bases[i].base));
    }
    std::map<std::string, madc::value> res;
    res["nodes"] = value::make_array(nodes);
    out = value::make_object(res);
    return true;
}
```

- [ ] **Step 4–7: Bridge / decl / wrapper / dialect decl** (same shape as Task 2's two-arg-plus-id verbs)

`src/parser.cpp`:
```cpp
void *madc_graph_members(void *result, int64_t handle, int64_t type_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_members(handle, type_id, out);
    return result;
}
void *madc_graph_bases(void *result, int64_t handle, int64_t type_id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_bases(handle, type_id, out);
    return result;
}
```
`include/ns_common.h`:
```cpp
void *madc_graph_members(void *result, int64_t handle, int64_t type_id);
void *madc_graph_bases(void *result, int64_t handle, int64_t type_id);
```
`src/ns_madc.cpp`:
```cpp
value &graph_members(value &out, int64_t handle, int64_t type_id)
	{ madc_graph_members(&out, handle, type_id); return out; }
value &graph_bases(value &out, int64_t handle, int64_t type_id)
	{ madc_graph_bases(&out, handle, type_id); return out; }
```
`include/madc/ns_madc`:
```cpp
    value &graph_members(value &out, int64_t handle, int64_t type_id);
    value &graph_bases(value &out, int64_t handle, int64_t type_id);
```

- [ ] **Step 8: Extend the expect**

```
members: 2
  x : Type
  y : Type
bases: 0
```

- [ ] **Step 9: Build, run**

Run: `make -C src`
Run: `bin/madc tests/testgraphaccessors.mad`
Expected: PASS.

- [ ] **Step 10: Commit** (rule trailers; `Searched:` = "DataDefSTRUCT members / DataDefCLASS bases").

```bash
git add src/madc_program.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc tests/testgraphaccessors.mad tests/testgraphaccessors.expect
git commit
```

---

## Task 5: `graph.enclosing(line, column)`

The ENCLOSES edge in graph shape: the innermost enclosing function at a position, as a node with its type-id (so the agent can chain into `type_of`/`node`). Wraps the proven `internal_program_parse_enclosing` logic; it does not re-implement the search.

**Files:** the five layers + `tests/testgraphaccessors.mad`/`.expect`.

**Interfaces:**
- Consumes: the enclosing search already in `internal_program_parse_enclosing` (`src/madc_program.cpp:5275`) — reuse `tu_own_function` + the innermost-by-(line,column) loop; `graph_func_node_value` (Task 1) for the result shape.
- Produces:
  - `bool internal_program_graph_enclosing(int64_t handle, int64_t line, int64_t column, madc::value &out)` → the function node (empty object if none).
  - Dialect: `value &madc::graph_enclosing(value &out, int64_t handle, int64_t line, int64_t column)`.

- [ ] **Step 1: Write the failing test** — append to `tests/testgraphaccessors.mad`

```c
    // graph.enclosing(line, column): the function around a caret, as a node
    // (line 2 is inside add; line 99 is inside nothing).
    var en;
    madc::graph_enclosing(en, h, 2, 20);
    println("encl: {} {}", en["kind"], en["name"]);
    var en2;
    madc::graph_enclosing(en2, h, 99, 1);
    println("encl none: {}", php::count(en2) == 0 ? 1 : 0);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bin/madc tests/testgraphaccessors.mad`
Expected: FAIL — `graph_enclosing` not a member of `madc`.

- [ ] **Step 3: Add the walker** — `src/madc_program.cpp`

```cpp
// graph.enclosing(handle, line, column): the ENCLOSES edge — the innermost
// enclosing function definition at a position, as a graph node (id + span),
// so the agent can chain type_of / node. The innermost-by-(line,column)
// search is the one in internal_program_parse_enclosing; only the result
// shape differs (a node, not the outline row).
bool internal_program_graph_enclosing(int64_t handle, int64_t line,
				      int64_t column, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    TokenFunc *best = (TokenFunc *)0;
    ::Program &child = *st->child;
    for ( size_t i = 0; i < child.pending_funcs.size(); ++i )
    {
	TokenFunc *tf = tu_own_function(child.pending_funcs[i],
					st->display_name);
	if ( !tf )
	    continue;
	if ( (int64_t)tf->line > line
	  || ((int64_t)tf->line == line && (int64_t)tf->column > column) )
	    continue;
	if ( (int64_t)tf->end_line < line )
	    continue;
	if ( !best || tf->line > best->line
	  || (tf->line == best->line && tf->column > best->column) )
	    best = tf;
    }
    if ( !best )
    {
	std::map<std::string, madc::value> empty;
	out = value::make_object(empty);
	return true;
    }
    out = graph_func_node_value(best);
    return true;
}
```

> The innermost-search loop is intentionally the same as `internal_program_parse_enclosing`. If, while implementing, that loop is worth factoring into a shared `static TokenFunc *enclosing_func_at(::Program&, const std::string&, int64_t, int64_t)` used by both, do it in this commit (one owner of the rule — helper-methods law) rather than leaving two copies. Otherwise the duplication is a `/dupaudit` finding at the seam.

- [ ] **Step 4–7: Bridge / decl / wrapper / dialect decl**

`src/parser.cpp`:
```cpp
void *madc_graph_enclosing(void *result, int64_t handle, int64_t line,
			   int64_t column)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_enclosing(handle, line, column, out);
    return result;
}
```
`include/ns_common.h`:
```cpp
void *madc_graph_enclosing(void *result, int64_t handle, int64_t line,
			   int64_t column);
```
`src/ns_madc.cpp`:
```cpp
value &graph_enclosing(value &out, int64_t handle, int64_t line, int64_t column)
	{ madc_graph_enclosing(&out, handle, line, column); return out; }
```
`include/madc/ns_madc`:
```cpp
    value &graph_enclosing(value &out, int64_t handle, int64_t line,
			   int64_t column);
```

- [ ] **Step 8: Extend the expect**

```
encl: Function add
encl none: 1
```

- [ ] **Step 9: Build, run**

Run: `make -C src`
Run: `bin/madc tests/testgraphaccessors.mad`
Expected: PASS.

- [ ] **Step 10: Commit** (rule trailers; if the loop was factored, `Layer:` names the shared helper as the single owner; `Oracle: n/a — refactor+read surface, the .mad test is the oracle`).

```bash
git add src/madc_program.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc tests/testgraphaccessors.mad tests/testgraphaccessors.expect
git commit
```

---

## Task 6: The MCP `graph.*` tool family + seat integration test

Expose all seven L1 verbs over the live MCP seat as a distinct `tools/call` family, dispatched by tool name **before** the editor-registry path, returning the terse node projection as the tool's structured content.

**Files:**
- Modify: `tools/madcide/madcide_mcp.inc`
- Test: `tests/testmadcide_serve_graph.mad`, `tests/testmadcide_serve_graph.input`, `tests/testmadcide_serve_graph.expect`

**Interfaces:**
- Consumes: the dialect accessors `madc::graph_*` (Tasks 1–5); the seat's parse handle for the served doc. **Precondition to confirm first:** how the MCP seat reaches a **parse handle** for the open document. `session_state` (`madcide_api.inc:203`) already calls `ensure_phandle(w, es, adoc)` → a handle usable with `madc::parse_*`. The graph tools need that SAME handle. Read `ensure_phandle` and `session_state` before wiring, and reuse `ensure_phandle` — do not open a second handle.
- Produces: a `graph_call(var &out, IdeSession &S, long doc, var &nm, var &params)` in `madcide_mcp.inc`; graph tool descriptors in `mcp_tools`; a `graph.*` branch in `mcp_handle`'s `tools/call`.

- [ ] **Step 1: Confirm the handle path** — read `ensure_phandle` and `session_state` (`tools/madcide/madcide_api.inc`), and how `IdeSession` exposes `world()`/`es`/`active(doc)`. Note the exact call that yields the parse handle `h` for the served doc. (No code yet — this is the pre-edit trace the dispatch depends on.)

- [ ] **Step 2: Write the failing test** — `tests/testmadcide_serve_graph.mad`

```c
// Code-graph MCP L1 over the SEAT: the graph.* tool family driven by a real
// MCP client (the .input JSON-RPC stream). tools/list advertises the graph
// verbs; tools/call graph.symbols / graph.definition return terse node
// results; an unknown graph verb is isError. run_mcp is the deployed --mcp
// path end to end.
#include "../tools/texteditor/lined_core.inc"
#include "../tools/texteditor/editor_events.inc"
#include "../tools/madcide/madcide_core.inc"
#include "../tools/madcide/madcide_client.inc"
#include "../tools/madcide/madcide_api.inc"
#include "../tools/madcide/madcide_mcp.inc"

int main()
{
    const char *path = "madc_ide_graph.mad";
    php::file_put_contents(path,
	"struct Point { long x; long y; };\nlong add(long a, long b) { return a + b; }\n");
    long rc = run_mcp(path, false);
    php::unlink(path);
    return rc;
}
```

`tests/testmadcide_serve_graph.input` (one JSON-RPC message per line):
```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/list"}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"graph.symbols","arguments":{}}}
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"graph.definition","arguments":{"name":"Point"}}}
{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"graph.bogus","arguments":{}}}
```

`tests/testmadcide_serve_graph.expect` (field names are backslash-escaped on the wire — a JSON string inside the JSON-RPC result — so assert unquoted distinctive fragments, the `testmadcide_serve_mcp` precedent):
```
"name":"graph.symbols"
"name":"graph.definition"
"name":"graph.node"
"name":"graph.members"
Function
Struct
"isError":false
"isError":true
unknown graph verb 'graph.bogus'
```

- [ ] **Step 3: Run test to verify it fails**

Run: `bin/madc tests/testmadcide_serve_graph.mad < tests/testmadcide_serve_graph.input`
Expected: FAIL — no `graph.*` tools advertised; `graph.symbols` falls through to the registry path and errors as an unknown command.

- [ ] **Step 4: Add `graph_call`** — `tools/madcide/madcide_mcp.inc`

```c
// One graph.* tool call: resolve the served doc's parse handle (the SAME
// ensure_phandle session_state uses — one handle, never a second), dispatch
// by verb NAME to the matching madc::graph_* accessor, and return the terse
// node projection as the tool's structured content. An unknown graph verb
// is isError content (never a silent drop). `params.arguments` carries the
// verb args (id / name / line / column).
void graph_call(var &out, IdeSession &S, long doc, var &nm, var &params)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    var args = params["arguments"];
    if ( args.is_null() )
	args = {};
    const char *verb = nm.c_str();
    var snap;
    bool known = true;
    if ( strcmp(verb, "graph.symbols") == 0 )
	madc::graph_symbols(snap, h);
    else if ( strcmp(verb, "graph.node") == 0 )
	madc::graph_node(snap, h, args["id"].as_integer());
    else if ( strcmp(verb, "graph.type_of") == 0 )
	madc::graph_type_of(snap, h, args["id"].as_integer());
    else if ( strcmp(verb, "graph.definition") == 0 )
	madc::graph_definition(snap, h, args["name"].c_str());
    else if ( strcmp(verb, "graph.members") == 0 )
	madc::graph_members(snap, h, args["id"].as_integer());
    else if ( strcmp(verb, "graph.bases") == 0 )
	madc::graph_bases(snap, h, args["id"].as_integer());
    else if ( strcmp(verb, "graph.enclosing") == 0 )
	madc::graph_enclosing(snap, h, args["line"].as_integer(),
			      args["column"].as_integer());
    else
	known = false;
    var content = {};
    if ( !known )
    {
	var why = format("unknown graph verb '{}'", nm);
	var item = { "type": "text", "text": why };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    var js = js::stringify(snap);		// copy the ring const char*
    var txt = js;
    var item = { "type": "text", "text": txt };
    content[] = item;
    out = { "content": content, "isError": false };
}

// The seven L1 graph verbs, with their argument shape, for tools/list. A
// distinct family from the editor registry commands: an agent chooses a
// semantic navigation verb over a keystroke command.
void graph_tool_descriptors(var &out)
{
    var idprop = { "type": "integer", "description": "a node id (type-id)" };
    var nameprop = { "type": "string", "description": "a symbol name" };
    var lineprop = { "type": "integer", "description": "1-based line" };
    var colprop = { "type": "integer", "description": "1-based column" };
    var noargs = { "type": "object", "properties": {} };
    var idargs = { "type": "object", "properties": { "id": idprop } };
    var nameargs = { "type": "object", "properties": { "name": nameprop } };
    var posargs = { "type": "object",
	"properties": { "line": lineprop, "column": colprop } };
    var tools = {};
    var t0 = { "name": "graph.symbols",
	"description": "the TU's own functions as graph nodes {id,kind,name,span}",
	"inputSchema": noargs };
    tools[] = t0;
    var t1 = { "name": "graph.definition",
	"description": "resolve a name to its definition node(s)",
	"inputSchema": nameargs };
    tools[] = t1;
    var t2 = { "name": "graph.node",
	"description": "the node for a node id", "inputSchema": idargs };
    tools[] = t2;
    var t3 = { "name": "graph.type_of",
	"description": "the type node of a node (HAS_TYPE)",
	"inputSchema": idargs };
    tools[] = t3;
    var t4 = { "name": "graph.members",
	"description": "a struct/class's members with their types (MEMBER_OF)",
	"inputSchema": idargs };
    tools[] = t4;
    var t5 = { "name": "graph.bases",
	"description": "a class's direct base types (INHERITS)",
	"inputSchema": idargs };
    tools[] = t5;
    var t6 = { "name": "graph.enclosing",
	"description": "the innermost enclosing function at a position (ENCLOSES)",
	"inputSchema": posargs };
    tools[] = t6;
    out = tools;
}
```

- [ ] **Step 5: Advertise the graph tools in `mcp_tools`** — append to the existing `mcp_tools` (after the registry-command loop, before `out = tools;`):

```c
    var gtools;
    graph_tool_descriptors(gtools);
    for ( var gt : gtools )
	tools[] = gt;
```

- [ ] **Step 6: Dispatch graph verbs in `mcp_handle`** — in the `tools/call` branch, AFTER the tool name `nm` is read and BEFORE `mcp_call` (so a `graph.*` verb never falls through to the editor registry):

```c
	if ( php::substr(nm, 0, 6) == "graph." )
	{
	    var result;
	    graph_call(result, S, doc, nm, params);
	    mcp_reply(out, idv, result);
	    return true;
	}
```

> Confirm `php::substr(nm, 0, 6)` yields `"graph."` for a `graph.*` name in the dialect (the value→prefix test). If the cleaner spelling is `nm.c_str()` + `strncmp(..., "graph.", 6) == 0`, use that — either is fine; the point is a prefix test, dispatched once, not a per-verb ladder in `mcp_handle`.

- [ ] **Step 7: Build**

Run: `make -C src`
Expected: builds clean.

- [ ] **Step 8: Run the seat test to verify it passes**

Run: `bin/madc tests/testmadcide_serve_graph.mad < tests/testmadcide_serve_graph.input`
Expected: PASS — output contains each `.expect` fragment (graph tools advertised; `graph.symbols`→Function, `graph.definition Point`→Struct, `graph.bogus`→isError true with the prose).

- [ ] **Step 9: Run the neighbors (targeted, not the battery)**

Run: `bin/madc tests/testmadcide_serve_mcp.mad < tests/testmadcide_serve_mcp.input`
Run: `bin/madc tests/testparsehandle.mad`
Run: `bin/madc tests/testgraphaccessors.mad`
Expected: all PASS (the graph family did not disturb the existing seat or accessors).

- [ ] **Step 10: Commit** (`tools/` + `tests/` only — no `src/`/`include/`, so no rule trailers required, though the Co-Authored-By line stays).

```bash
git add tools/madcide/madcide_mcp.inc tests/testmadcide_serve_graph.mad tests/testmadcide_serve_graph.input tests/testmadcide_serve_graph.expect
git commit
```

---

## Post-plan: banking & the seam

- After Task 6, update `claude_status.json` `live_handoff` (python, 2-space indent) and the `MEMORY.md` arc line: L1 read-accessors COMPLETE; NEXT = L1b (`graph.children`/`graph.body` + the body-node enum) → L2 (derived CALLS/REFERENCES). Commit as `docs(status)`.
- Do **not** run `make -C src fulltest` per task. The V6 track's release seam (the arc's develop merge) runs the ONE battery + the exe/obj lanes + the lane records — L1, L1b, L2, L3 all bank on the arc branch until then (testing-fulltest law).
- Before that develop merge, run `/dupaudit` scoped to `src/madc_program.cpp` + `tools/madcide/` (the enclosing-loop factor from Task 5 is the likely finding to confirm consolidated).

## Self-Review

**Spec coverage (§6.4 L1):** `symbols` (T1), `definition` (T3), `node` (T2), `type_of` (T2), `members` (T4), `bases` (T4), `enclosing` (T5), MCP tool family (T6). `children`/`body` — explicitly deferred to L1b (Scope), the one greenfield body-schema item; owner-flagged. All other L1 verbs have a task.

**Invariant coverage (§5):** live-in-process (reads the child Program, never `DefArena` — Global Constraints); stable ids (type-id via `madc_type_id_for` — T1/T2); bounded named verbs (seven, no query language — T6); enum kinds→wire-name-once (`graph_kind_name` switch — T1); every edge ground truth (all edges from live compiler structures — T4/T5); terse+batched result (`{nodes:[{id,kind,name,span?}]}`, members carry their type inline — T1/T4); MCP-as-adapter (a tool family in the existing seat — T6); no parallel impl (a projection over the live tree, and the enclosing search is reused/factored, not copied — T5).

**Placeholder scan:** no TBD/TODO; every code step has real code with confirmed member names (`pending_funcs`, `tu_own_function`, `project_types.get`, `madc_type_id_for`, `basetype()`, `is_pointer/is_reference`, `FuncDef::returns`, `DataDefPTR::base_type`, `DataDef::unqualified`, `DataDefSTRUCT::members`/`member_offsets`, `DataDefCLASS::bases`/`BaseSpec::base`). Three spellings are marked "confirm before build" with the expected form and the fallback (`struct_map.get`/`datatype_lookup` in T3; the seat's handle path in T6; the prefix-test spelling in T6) — these are pre-edit confirmations, not unresolved placeholders.

**Type consistency:** the node shape `{id, kind, name}` (+ span for functions) is one builder (`graph_node_value`/`graph_func_node_value`) used everywhere; list results are uniformly `{nodes:[...]}`; every dialect accessor is `value &madc::graph_X(value &out, int64_t handle, ...)`; every bridge is `void *madc_graph_X(void *result, int64_t handle, ...)`; every walker is `bool internal_program_graph_X(int64_t handle, ..., madc::value &out)`. Verb names match between the dialect accessors, `graph_call`'s dispatch, and `graph_tool_descriptors`.

## Execution Handoff

Plan complete and saved to `docs/plans/2026-09-12-ast-graph-mcp-L1-implementation.md`. Two execution options:

1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session (executing-plans), batch with checkpoints.
