# Code-Graph MCP — L1b (body walk: `graph.children` + `graph.body`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the two body-walk read verbs — `graph.children(id, depth?)` and `graph.body(func, depth?)` — that expose a function's statement/expression tree as terse `{id, kind, name, span}` nodes plus `CONTAINS` edges, over the live parse-handle AST.

**Architecture:** L1 exposed the decl/type graph (types + functions as nodes, id = type-id). L1b adds the *body* graph: the persistent `TokenBase` AST reachable from each `TokenFunc` (a `TokenFunc` *is-a* `TokenCpnd`, so its body is `tf->statements`). A one-boundary `switch(tok->type())` classifier converts each token to a `GraphBodyKind` enum → wire name once (enum-over-strings). Body nodes get opaque, snapshot-stable ids from a per-handle registry, partitioned from type-ids so `graph.children` can route either id space. Same five-layer plumbing as L1 (`internal_program_graph_*` → `madc_graph_*` C bridge → declaration-only `graph_*` dialect wrapper, mangled-direct) and the same `graph.*` MCP seat family.

**Tech Stack:** C++11 (engine), the madc dialect (`--std=madc`, tests + MCP seat `.inc`), the MCP seat in `tools/madcide/madcide_mcp.inc`.

**Spec:** `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` (§6.2 node/edge schema, §6.4 verb set, §6.5 result shape, §7 L1 layering). Supersedes nothing in L1; extends the `graph.*` family shipped in `a42683fa..82edd912`.

## Substrate finding (settled 2026-09-12, owner-ratified) — READ FIRST

The design doc §6.2/§6.3/§8 name **cir_node** as the body source with `cir_ref{seg,idx}` ids. That is imprecise for a live parse handle, and L1b corrects it (a doc clarification is Task 7):

- The **c2mir `node_t` / cir_node arena is transient** — `madc_cir_emit_native` builds it via `cir_translate_guarded(...)` and ends with `delete builder; // owns the node arena — outlives every tree read` (`src/madc_cir.cpp:6613`). `CirBuilder` is **not** a `Program` member; the arena (and every `cir_ref` segment) is freed after each compile. `internal_program_parse_build` (`src/madc_program.cpp:5854`) restores parse state after emit — *"the handle keeps only its parse state."*
- The **persistent structure on a parse handle is the `TokenBase` AST** on `parse_tu_state::child` (`TokenProgram → TokenFunc (is-a TokenCpnd) → statements`). It survives every recompile and is what the editor renders from. This is the MC11-IR "high-level face."
- **Therefore L1b walks the live `TokenBase` AST**, reachable from the same `TokenFunc*` L1 already resolves (`graph_function_token`, `src/madc_program.cpp:5559`). This is *not* a departure from "cir_node = bodies" — it is that, in the persistent-AST sense.
- **Body-node ids** are snapshot-stable per-handle handles (§6.3: "opaque stable handle… stable for the handle's current parse; after a mutating command the agent re-queries"), **not** the transient `cir_ref`.

## Global Constraints

Every task's requirements implicitly include these. Copied from the L1 slice's constraints (still binding), the spec, and the owner laws.

- **Substrate:** read ONLY the live parse-handle child's `TokenBase` AST (via `parse_tu_get(handle)->child` and the `TokenFunc*` from `graph_function_token`). NEVER the cir_node arena, NEVER `DefArena`/`get_def_at`, NEVER re-parse or re-compile inside a verb.
- **Enum-over-strings (owner law):** the body-node kind is a C++ `enum class GraphBodyKind`, converted to a wire string EXACTLY ONCE at the boundary (`graph_body_kind_name`). No char/string discriminators; the classifier is a `switch`, never a string-compare ladder. Same for the statement subkind (keyed on `TokenID`).
- **Terse result (spec §6.5):** default node projection is `{id, kind, name?, span?}`. `name` only where the node has an identifier/spelling; `span` = `line`,`column` (+ `end_line` where the token records it). `depth` bounds descent; deep results are **capped** with `truncated: true` (cursor pagination is deferred to L2, a documented ruling). Edges are `{kind:"CONTAINS", from, to}`.
- **Ids:** body-node ids come from a per-handle registry, partitioned by `GRAPH_BODY_ID_BASE` from L1's type-ids so a single integer id is self-identifying. The registry is cleared on handle refresh (a re-parse invalidates the pointers). Re-query after any mutation.
- **Plumbing (mirror `parse_*`/L1 `graph_*`):** engine walker `internal_program_graph_X(...)` in `src/madc_program.cpp` → C bridge `void *madc_graph_X(void *result, ...)` in `src/parser.cpp` (prototype in `include/ns_common.h`) → declaration-only dialect wrapper `value &graph_X(value &out, ...)` body in `src/ns_madc.cpp`, declaration in `include/madc/ns_madc`. Resolved mangled-direct; NO `extern "C"` on the dialect side.
- **MCP seat:** extend the existing `graph.*` family in `tools/madcide/madcide_mcp.inc` (`graph_tool_descriptors` + `graph_call`), dispatched by the existing `strncmp(nm.c_str(),"graph.",6)` prefix BEFORE the registry path. Resolve the handle via `ensure_phandle` (as `session_state`/L1 do). `js::stringify` returns a ring `const char*` — copy into a `var` before reuse.
- **Rule trailers** on every `src/`/`include/` commit (`Hypothesis`/`Layer`/`Searched`/`Oracle`; `Oracle: n/a — new read surface`). `tools/`+`tests/` commits carry none.
- **Testing:** targeted tests per slice (unit-shaped `.mad` fixture + the MCP-seat integration test). NO full battery (that rides the V6 arc release seam). Value-first `.mad` (zero includes/`using`/`std::`, `var` carrier, bare `print`/`println`/`format`; `.expect` + `.expect_quiet`). ZERO WARNINGS. No `&&` chains. Scratch in `tmp/`.

## Verified recon appendix (use these exact members — do not re-discover)

Token classes and the members L1b reads (verified against the headers 2026-09-12):

- `TokenBase` (`include/tokens.h:243`): `int line, column; const char *file; TokenBase *parent;` · `virtual TokenType type() const` · `virtual TokenID id() const` · O(1) downcasts `as_X_tok()` (return `this` on the concrete class, else NULL — null-check the receiver). `spelling()` lives on `TokenIdent` (below), not `TokenBase`.
- `TokenCpnd` (`include/madc.h:741`): `std::vector<TokenStmt *> statements; int end_line;` · `type()==ttCompound` · `as_cpnd_tok()`.
- `TokenFunc` (`include/madc.h:792`): *is-a* `TokenCpnd` AND `TokenVar`; body = `tf->statements`; identifier = `tf->var.name`; span = `tf->line/column/end_line`. `as_func_tok()`.
- `TokenProgram` (`include/madc.h:1290`): *is-a* `TokenCpnd`; `type()==ttProgram`.
- `TokenOperator` (`include/tokens.h:437`): `TokenBase *left, *right;` `type()==ttOperator`; `as_operator_tok()`. Base of `TokenMultiOp` (`str`), `TokenAssign` (`include/tokens.h:715`), `TokenTerQ` (ternary, `include/tokens.h:985`) — all descend via `left`/`right`.
- `TokenCallFunc` (`include/madc.h:937`): `std::vector<TokenBase *> parameters;` callee name = `var.name` (is-a `TokenVar`); `type()==ttCallFunc`; `as_callfunc_tok()`.
- `TokenMember` (`include/madc.h:1011`): *is-a* `TokenCallFunc`; `Variable &object; TokenBase *parent_expr;` member name = `var.name`; `type()==ttMember`; `as_member_tok()`.
- `TokenCallMethod` (`include/madc.h:1189`): *is-a* `TokenMember`; `type()==ttCallMethod`; `as_callmethod_tok()`.
- `TokenSubscript` (`include/madc.h:1205`): `TokenBase *index; std::vector<TokenBase *> extra_indices; Variable &object;` `type()==ttSubscript`; `as_subscript_tok()`.
- `TokenSubscriptExpr` (`include/madc.h:1266`): `TokenBase *base_expr, *index;` `type()==ttSubscript`; `as_subscript_expr_tok()`.
- `TokenCast` (`include/madc.h:1173`): `TokenBase *expr;` `type()==ttBase`(!); `as_cast_tok()`.
- `TokenDerefExpr` (`include/madc.h:1117`): `TokenBase *expr;` `type()==ttMember`(!); `as_deref_expr_tok()`.
- `TokenDeref` (`include/madc.h:1105`): `Variable &var;` (no token child); `type()==ttMember`(!); `as_deref_tok()`.
- `TokenDerefStep` (`include/madc.h:1154`): `Variable &var;` (no token child); `type()==ttBase`; `as_deref_step_tok()`.
- `TokenAddrOf` (`include/madc.h:1058`): `Variable &var;` (no token child); `type()==ttBase`; `as_addr_of_tok()`.
- Control flow (all *is-a* `TokenKeyword` → `type()==ttKeyword`, `include/tokens.h:1490`; disambiguate by `id()`):
  - `TokenIF` (`include/tokens.h:2037`): `init_stmt, condition_decl, condition, statement, elsestmt` (all `TokenBase *`); `id()==tkIF`; `as_if_tok()`.
  - `TokenRETURN` (`include/tokens.h:2055`): `TokenBase *returns; std::vector<TokenBase *> return_exprs;` `id()==tkRETURN`; `as_return_tok()`.
  - `TokenDO` (`include/tokens.h:2067`): `statement, condition`; `id()==tkDO`; `as_do_tok()`.
  - `TokenWHILE` (`include/tokens.h:2079`): `condition, statement`; `id()==tkWHILE`. **NO `as_while_tok()` downcast exists** — see Task 2 (add one, mirroring the family, after confirming `while` is a `TokenWHILE` and not lowered to `TokenFOR`).
  - `TokenFOR` (`include/tokens.h:2090`): `initialize, condition, increment, statement; std::vector<TokenBase*> init_extras, incr_extras;` `id()==tkFOR`; `as_for_tok()`.
  - `TokenFOREACH` (`include/tokens.h:2111`): `container, statement`; `id()==tkFOR`; `as_foreach_tok()`.
  - `TokenSWITCH` (`include/tokens.h:1715`), `TokenTRY` (`include/tokens.h:1689`), `TokenTHROW` (`include/tokens.h:1705`), `TokenCASE` (`include/tokens.h:1671`), `TokenBREAK`/`TokenCONT` (`include/tokens.h:2007/2017`), `TokenGOTO` (`include/tokens.h:1552`): **confirm sub-part members before enumerating** (Task 2). `as_switch_tok`/`as_try_tok`/`as_throw_tok` exist; CASE/BREAK/CONT/GOTO have none — classify as `Statement`, enumerate no children (leaf-safe) unless a member is confirmed.
- Leaves: `TokenVar` (`include/datatokens.h:274`, name `var.name`, `type()==ttVariable`, `as_var_tok()`), `TokenIdent` (`include/tokens.h:1401`, `spelling()`, `type()==ttIdentifier`), `TokenStr` (`include/tokens.h:1434`, `spelling()`==`str`, `type()==ttString`), `TokenInt`/`TokenReal`/`TokenChar` (`include/tokens.h:1256/1373/1241`, `type()==ttInteger/ttReal/ttChar`; value via `ival()`/`dval()`, spelling via `spelling()` where present — **confirm `TokenInt`/`TokenReal`/`TokenChar` spelling accessor in Task 3**).

L1 helpers to reuse (do not duplicate): `graph_function_token(::Program&, DataDef*)` (`5559`), `graph_func_node_value` (`5575`), `type_from_id` binding (id→node walkers bind the child's table before building — M-5), `tu_own_function`, `child.type_id_for(dd)`.

---

## File Structure

- `src/madc_program.cpp` — the enum, classifier, id registry, child enumerator, node builder, subtree walker, and the two `internal_program_graph_*` verbs. Also: clear the registry in `internal_program_parse_refresh` (`5254`). (If `as_while_tok` is added, it goes in `include/tokens.h`.)
- `include/tokens.h` — (only if needed) `as_while_tok()` downcast on `TokenBase` + `TokenWHILE`.
- `src/parser.cpp` — two `madc_graph_*` C bridges + forward decls of the two `internal_program_graph_*`.
- `include/ns_common.h` — two `madc_graph_*` prototypes.
- `src/ns_madc.cpp` — two declaration-only `graph_*` dialect wrapper bodies.
- `include/madc/ns_madc` — two `graph_*` dialect declarations.
- `tools/madcide/madcide_mcp.inc` — `graph.body` + `graph.children` descriptors + `graph_call` dispatch.
- `tests/testgraphbody.mad` (+ `.expect`, `.expect_quiet`) — engine-verb fixture.
- `tests/testmadcide_serve_graph.mad` (+ `.input`, `.expect`) — EXTEND to drive `graph.body`/`graph.children` over the seat.
- `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` — §6.2/§6.3/§8 substrate clarification (Task 7).

---

## Task 1: Body-node kind enum, classifier, and the per-handle id registry

**Files:**
- Modify: `src/madc_program.cpp` (add near the L1 helpers, ~`5528`); `parse_tu_state` (~`4949`); `internal_program_parse_refresh` (~`5254`)
- Test: covered by the integration fixture in Task 7 (these are file-static helpers; the gate is the end-to-end `.mad`).

**Interfaces:**
- Produces: `enum class GraphBodyKind`; `const char *graph_body_kind_name(GraphBodyKind)`; `GraphBodyKind graph_body_kind_of(const TokenBase *)`; `const char *graph_stmt_subkind_name(TokenID)`; `int64_t graph_body_intern(parse_tu_state *, const TokenBase *)`; `const TokenBase *graph_body_resolve(parse_tu_state *, int64_t id)`; constant `GRAPH_BODY_ID_BASE`.
- Consumes: `TokenType`, `TokenID`, the `as_X_tok()` downcasts.

- [ ] **Step 1: Add the registry fields to `parse_tu_state`** (`~4949`)

```cpp
struct parse_tu_state
{
    ::Program *child;
    std::string display_name;
    // L1b body-node id registry (snapshot-stable handles; spec §6.3). Body
    // nodes are TokenBase* (not DataDefs), so they need their own id space,
    // partitioned from L1's type-ids by GRAPH_BODY_ID_BASE. Populated lazily
    // as body verbs surface nodes; CLEARED on refresh (a re-parse frees the
    // arena these pointers came from). Same-snapshot dedup via body_ids.
    std::vector<const TokenBase *> body_nodes;                    // id = BASE + index
    std::unordered_map<const TokenBase *, int64_t> body_ids;      // node -> id
    parse_tu_state() : child((::Program *)0) {}
    ~parse_tu_state() { delete child; }
};
```

- [ ] **Step 2: The kind enum + wire-name converter** (near `graph_kind_name`, ~`5528`)

```cpp
// L1b body-node kinds (spec §6.2 "cir_node bodies") over the LIVE TokenBase
// AST. Enum-over-strings: convert to a wire name EXACTLY ONCE here. Error is
// the contained-parse-error citizen (TokenError); Unknown is the exhaustive
// fallback so the classifier switch stays total (no -Wswitch).
enum class GraphBodyKind {
    TranslationUnit, Block, Statement, Call, NameRef, Expr, Literal, Error, Unknown
};

static const char *graph_body_kind_name(GraphBodyKind k)
{
    switch ( k )
    {
    case GraphBodyKind::TranslationUnit: return "TranslationUnit";
    case GraphBodyKind::Block:           return "Block";
    case GraphBodyKind::Statement:       return "Statement";
    case GraphBodyKind::Call:            return "Call";
    case GraphBodyKind::NameRef:         return "NameRef";
    case GraphBodyKind::Expr:            return "Expr";
    case GraphBodyKind::Literal:         return "Literal";
    case GraphBodyKind::Error:           return "Error";
    case GraphBodyKind::Unknown:         return "Unknown";
    }
    return "Unknown";
}
```

- [ ] **Step 3: The classifier** — `TokenType`-primary, `as_X_tok()` tiebreakers for the `ttMember`/`ttBase`-overloaded cases (verified: `TokenDeref`/`TokenDerefExpr` report `ttMember` but are dereference EXPRESSIONS, not member access; `TokenCast`/`TokenDerefStep`/`TokenAddrOf` report `ttBase`).

```cpp
static GraphBodyKind graph_body_kind_of(const TokenBase *t)
{
    if ( !t )
	return GraphBodyKind::Unknown;
    TokenBase *nt = const_cast<TokenBase *>(t);   // as_X_tok() are non-const
    switch ( t->type() )
    {
    case TokenType::ttProgram:   return GraphBodyKind::TranslationUnit;
    case TokenType::ttCompound:  return GraphBodyKind::Block;
    case TokenType::ttKeyword:   return GraphBodyKind::Statement;  // subkind = id()
    case TokenType::ttStatement: return GraphBodyKind::Statement;
    case TokenType::ttDeclare:
    case TokenType::ttTypedefDecl:
    case TokenType::ttStructDef: return GraphBodyKind::Statement;  // decl-statement
    case TokenType::ttCallFunc:
    case TokenType::ttCallMethod: return GraphBodyKind::Call;
    case TokenType::ttVariable:
    case TokenType::ttIdentifier: return GraphBodyKind::NameRef;
    case TokenType::ttMember:
	// ttMember is overloaded: real member access (NameRef) vs. deref exprs.
	if ( nt->as_member_tok() )       return GraphBodyKind::NameRef;
	return GraphBodyKind::Expr;      // TokenDeref/TokenDerefExpr/TokenComplexPart
    case TokenType::ttOperator:
    case TokenType::ttMultiOp:
    case TokenType::ttSubscript:  return GraphBodyKind::Expr;
    case TokenType::ttString:
    case TokenType::ttChar:
    case TokenType::ttInteger:
    case TokenType::ttReal:
    case TokenType::ttStructLit:  return GraphBodyKind::Literal;
    case TokenType::ttError:      return GraphBodyKind::Error;
    case TokenType::ttBase:
	// ttBase-typed expression tokens (cast, addr-of, deref-step).
	if ( nt->as_cast_tok() || nt->as_addr_of_tok()
	  || nt->as_addr_expr_tok() || nt->as_deref_step_tok() )
	    return GraphBodyKind::Expr;
	return GraphBodyKind::Unknown;
    default:                      return GraphBodyKind::Unknown;
    }
}
```

- [ ] **Step 4: The statement subkind name** (verbose/`fields` only — keeps terse default lean; enum-over-strings keyed on the real `TokenID`)

```cpp
// Statement subkind (only surfaced when fields includes "subkind"): the
// control-flow keyword. Keyed on TokenID (an enum), not a string compare.
static const char *graph_stmt_subkind_name(TokenID id)
{
    switch ( id )
    {
    case TokenID::tkIF:     return "if";
    case TokenID::tkFOR:    return "for";     // TokenFOR and TokenFOREACH both
    case TokenID::tkWHILE:  return "while";
    case TokenID::tkDO:     return "do";
    case TokenID::tkRETURN: return "return";
    case TokenID::tkSWITCH: return "switch";
    case TokenID::tkCASE:   return "case";
    case TokenID::tkBREAK:  return "break";
    case TokenID::tkCONT:   return "continue";
    case TokenID::tkGOTO:   return "goto";
    case TokenID::tkTRY:    return "try";
    case TokenID::tkTHROW:  return "throw";
    default:                return "";
    }
}
```

- [ ] **Step 5: The id registry helpers** (partitioned from type-ids)

```cpp
// Body-node ids live above every plausible type-id so a single integer id is
// self-identifying (graph.children routes on it). Type-ids are small table
// indices; 2^62 is unreachable by them.
static const int64_t GRAPH_BODY_ID_BASE = 0x4000000000000000LL;

static int64_t graph_body_intern(parse_tu_state *st, const TokenBase *t)
{
    if ( !st || !t )
	return 0;
    std::unordered_map<const TokenBase *, int64_t>::iterator it = st->body_ids.find(t);
    if ( it != st->body_ids.end() )
	return it->second;
    int64_t id = GRAPH_BODY_ID_BASE + (int64_t)st->body_nodes.size();
    st->body_nodes.push_back(t);
    st->body_ids[t] = id;
    return id;
}

static const TokenBase *graph_body_resolve(parse_tu_state *st, int64_t id)
{
    if ( !st || id < GRAPH_BODY_ID_BASE )
	return (const TokenBase *)0;
    size_t idx = (size_t)(id - GRAPH_BODY_ID_BASE);
    return idx < st->body_nodes.size() ? st->body_nodes[idx] : (const TokenBase *)0;
}
```

- [ ] **Step 6: Clear the registry on refresh** — find `internal_program_parse_refresh` (`~5254`); after it rebuilds the child (re-parse), clear both containers (the old `TokenBase*` are dangling).

```cpp
    // (inside internal_program_parse_refresh, after the child is re-parsed)
    st->body_nodes.clear();
    st->body_ids.clear();
```

- [ ] **Step 7: Build** — `make -C src` (batched with later tasks per the SDD ruling); confirm zero warnings, exhaustive switches (no `-Wswitch`).

**Commit** (with the child enumerator + builder + verbs — see the SDD batching note at the end): rule trailers required.

---

## Task 2: The child enumerator `graph_body_children`

**Files:**
- Modify: `src/madc_program.cpp` (after Task 1 helpers)
- Possibly modify: `include/tokens.h` (add `as_while_tok()` if `while` is a live `TokenWHILE`)

**Interfaces:**
- Produces: `void graph_body_children(const TokenBase *t, std::vector<const TokenBase *> &out)` — appends this node's direct children in source order (nulls skipped).
- Consumes: the `as_X_tok()` downcasts and the members in the recon appendix.

- [ ] **Step 1: CONFIRM before writing (do not assume — owner directive):**
  1. `while`: write `while (x) { }` in a fixture and check whether the AST node is a `TokenWHILE` (`id()==tkWHILE`) or a lowered `TokenFOR`. If `TokenWHILE` is real, add `virtual TokenWHILE *as_while_tok() { return NULL; }` to `TokenBase` (`include/tokens.h` beside `as_for_tok`) and `virtual TokenWHILE *as_while_tok() override { return this; }` to `TokenWHILE`, then enumerate `condition, statement`. If `while` lowers to `TokenFOR`, skip the addition.
  2. `TokenSWITCH` (`include/tokens.h:1715`), `TokenTRY` (`include/tokens.h:1689`), `TokenTHROW` (`include/tokens.h:1705`): read the classes and record their sub-part members (e.g. switch selector + body; try block + catch clauses). Enumerate those in source order.
  Record what you found in the commit body (the same "confirm-before-build" discipline L1 used).

- [ ] **Step 2: Write the enumerator** (the verified cases; unmatched → no children = safe leaf)

```cpp
// Direct children of a body node, in source order. Reaches each type via its
// O(1) as_X_tok() downcast (null-checked receiver) and reads the members from
// the recon appendix. A node with no recognized child-bearing shape yields no
// children (safe leaf) — the exotic long tail (NEW/DELETE/OBJTEMP/MATCH/
// PACK/COMPLEXPART) is additive follow-up, never a crash.
static void graph_body_children(const TokenBase *t, std::vector<const TokenBase *> &out)
{
    if ( !t )
	return;
    TokenBase *nt = const_cast<TokenBase *>(t);
    std::vector<const TokenBase *> raw;

    if ( TokenCpnd *c = nt->as_cpnd_tok() )          // Block / TranslationUnit / Func body
	for ( size_t i = 0; i < c->statements.size(); ++i )
	    raw.push_back(c->statements[i]);
    else if ( TokenIF *s = nt->as_if_tok() )
    { raw.push_back(s->init_stmt); raw.push_back(s->condition_decl);
      raw.push_back(s->condition); raw.push_back(s->statement); raw.push_back(s->elsestmt); }
    else if ( TokenFOR *s = nt->as_for_tok() )
    { raw.push_back(s->initialize);
      for ( size_t i = 0; i < s->init_extras.size(); ++i ) raw.push_back(s->init_extras[i]);
      raw.push_back(s->condition); raw.push_back(s->increment);
      for ( size_t i = 0; i < s->incr_extras.size(); ++i ) raw.push_back(s->incr_extras[i]);
      raw.push_back(s->statement); }
    else if ( TokenFOREACH *s = nt->as_foreach_tok() )
    { raw.push_back(s->container); raw.push_back(s->statement); }
    else if ( TokenDO *s = nt->as_do_tok() )
    { raw.push_back(s->statement); raw.push_back(s->condition); }
    else if ( TokenRETURN *s = nt->as_return_tok() )
    { raw.push_back(s->returns);
      for ( size_t i = 0; i < s->return_exprs.size(); ++i ) raw.push_back(s->return_exprs[i]); }
    // Step 1 additions: as_while_tok (condition, statement); switch/try sub-parts.
    else if ( TokenMember *m = nt->as_member_tok() )   // also covers TokenCallMethod
    { raw.push_back(m->parent_expr);
      for ( size_t i = 0; i < m->parameters.size(); ++i ) raw.push_back(m->parameters[i]); }
    else if ( TokenCallFunc *cf = nt->as_callfunc_tok() )
      for ( size_t i = 0; i < cf->parameters.size(); ++i ) raw.push_back(cf->parameters[i]);
    else if ( TokenSubscript *s = nt->as_subscript_tok() )
    { raw.push_back(s->index);
      for ( size_t i = 0; i < s->extra_indices.size(); ++i ) raw.push_back(s->extra_indices[i]); }
    else if ( TokenSubscriptExpr *s = nt->as_subscript_expr_tok() )
    { raw.push_back(s->base_expr); raw.push_back(s->index); }
    else if ( TokenCast *s = nt->as_cast_tok() )       raw.push_back(s->expr);
    else if ( TokenDerefExpr *s = nt->as_deref_expr_tok() ) raw.push_back(s->expr);
    else if ( TokenOperator *op = nt->as_operator_tok() ) // assign/ternary/multiop all here
    { raw.push_back(op->left); raw.push_back(op->right); }
    // else: leaf (literals, TokenVar, TokenIdent, addr-of/deref-of-var, exotic).

    for ( size_t i = 0; i < raw.size(); ++i )
	if ( raw[i] )
	    out.push_back(raw[i]);
}
```

Note the probe order: `as_member_tok()` BEFORE `as_callfunc_tok()` (member *is-a* callfunc), and both BEFORE `as_operator_tok()`. `as_cpnd_tok()` first (function bodies and blocks).

- [ ] **Step 3: Build** (batched). Zero warnings.

---

## Task 3: The node builder + subtree walker

**Files:**
- Modify: `src/madc_program.cpp` (after Task 2)

**Interfaces:**
- Produces: `madc::value graph_body_node_value(parse_tu_state *, const TokenBase *)`; `void graph_body_walk(parse_tu_state *, const TokenBase *root, int64_t depth, size_t cap, std::vector<madc::value> &nodes, std::vector<madc::value> &edges, bool &truncated)`.
- Consumes: Task 1 (intern, classifier, kind name), Task 2 (children).

- [ ] **Step 1: CONFIRM the leaf name/value accessors** — read `TokenInt` (`include/tokens.h:1256`), `TokenReal` (`1373`), `TokenChar` (`1241`): record their `spelling()`/`ival()`/`dval()` availability. For `name` on a Literal, prefer `spelling()` when present; otherwise omit `name`. For `TokenVar`/`TokenMember`/`TokenCallFunc` the name is `var.name`; for `TokenIdent`/`TokenStr` it is `spelling()`.

- [ ] **Step 2: The node builder** — terse `{id, kind, name?, span}` (spec §6.5)

```cpp
// A body node's terse projection: { id, kind, name?, line, column }, plus
// end_line for a compound (it records the closing brace). name only where the
// node carries an identifier/spelling. id is the per-handle body handle.
static madc::value graph_body_node_value(parse_tu_state *st, const TokenBase *t)
{
    std::map<std::string, madc::value> f;
    f["id"]   = value(graph_body_intern(st, t));
    f["kind"] = value(std::string(graph_body_kind_name(graph_body_kind_of(t))));

    TokenBase *nt = const_cast<TokenBase *>(t);
    if ( TokenVar *v = nt->as_var_tok() )                 // covers callfunc/member (is-a TokenVar)
	{ if ( !v->var.name.empty() ) f["name"] = value(v->var.name); }
    else if ( TokenIdent *id = nt->as_ident_tok() )       // ident/str leaves
	{ const char *s = id->spelling(); if ( s && *s ) f["name"] = value(std::string(s)); }

    f["line"]   = value((int64_t)t->line);
    f["column"] = value((int64_t)t->column);
    if ( TokenCpnd *c = nt->as_cpnd_tok() )
	f["end_line"] = value((int64_t)c->end_line);
    return value::make_object(f);
}
```

- [ ] **Step 3: The subtree walker** — flat node list + `CONTAINS` edges, depth-bounded, capped

```cpp
// Walk `root`'s subtree to `depth` (root is depth 0; depth<0 = unbounded but
// still capped). Appends each visited node's projection to `nodes` and a
// {kind:"CONTAINS", from, to} edge for each parent->child. Stops at `cap`
// nodes, setting `truncated` (spec §6.5: never dump an unbounded subtree;
// cursor pagination is deferred to L2).
static void graph_body_walk(parse_tu_state *st, const TokenBase *root,
			    int64_t depth, size_t cap,
			    std::vector<madc::value> &nodes,
			    std::vector<madc::value> &edges, bool &truncated)
{
    if ( !st || !root )
	return;
    // BFS so a shallow `depth` returns the top of the tree, not one deep spine.
    std::vector<std::pair<const TokenBase *, int64_t> > q;   // (node, level)
    q.push_back(std::make_pair(root, (int64_t)0));
    for ( size_t qi = 0; qi < q.size(); ++qi )
    {
	if ( nodes.size() >= cap ) { truncated = true; break; }
	const TokenBase *n = q[qi].first;
	int64_t lvl = q[qi].second;
	int64_t nid = graph_body_intern(st, n);
	nodes.push_back(graph_body_node_value(st, n));
	if ( depth >= 0 && lvl >= depth )
	    continue;
	std::vector<const TokenBase *> kids;
	graph_body_children(n, kids);
	for ( size_t i = 0; i < kids.size(); ++i )
	{
	    std::map<std::string, madc::value> e;
	    e["kind"] = value(std::string("CONTAINS"));
	    e["from"] = value(nid);
	    e["to"]   = value(graph_body_intern(st, kids[i]));
	    edges.push_back(value::make_object(e));
	    q.push_back(std::make_pair(kids[i], lvl + 1));
	}
    }
}
```

- [ ] **Step 4: Build** (batched). Zero warnings.

---

## Task 4: `graph.body(func, depth?)` engine verb + plumbing

**Files:**
- Modify: `src/madc_program.cpp`, `src/parser.cpp`, `include/ns_common.h`, `src/ns_madc.cpp`, `include/madc/ns_madc`

**Interfaces:**
- Produces: `bool internal_program_graph_body(int64_t handle, int64_t func_id, int64_t depth, madc::value &out)`; C bridge `madc_graph_body`; dialect `graph_body`.

- [ ] **Step 1: The engine verb** — resolve the function id (a type-id from L1) to its `TokenFunc`, walk its body

```cpp
// graph.body(handle, func_id, depth): the function's body subtree. func_id is
// a type-id (from graph.symbols/definition/enclosing); resolve it to the live
// TokenFunc (graph_function_token), then walk the function node itself as the
// root (a TokenFunc is-a TokenCpnd, so its children are the body statements).
// depth<0 = full (capped). Result: { nodes:[...], edges:[CONTAINS...],
// truncated? }. Empty nodes + true = valid handle, no such function.
static const size_t GRAPH_BODY_NODE_CAP = 2000;

bool internal_program_graph_body(int64_t handle, int64_t func_id,
				 int64_t depth, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    DataDef *dd = child.type_from_id(func_id);          // binds child's table (M-5)
    TokenFunc *tf = dd ? graph_function_token(child, dd) : (TokenFunc *)0;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;
    if ( tf )
	graph_body_walk(st, tf, depth, GRAPH_BODY_NODE_CAP, nodes, edges, truncated);
    std::map<std::string, madc::value> r;
    r["nodes"] = value::make_array(nodes);
    r["edges"] = value::make_array(edges);
    if ( truncated )
	r["truncated"] = value(true);
    out = value::make_object(r);
    return true;
}
```

- [ ] **Step 2: Forward-decl + C bridge** in `src/parser.cpp` (mirror the L1 `madc_graph_*` bridges exactly)

```cpp
// forward decl (with the other internal_program_graph_* decls, ~343)
namespace madc { bool internal_program_graph_body(int64_t, int64_t, int64_t, value &); }

// bridge (with the other madc_graph_* bridges, ~1100)
void *madc_graph_body(void *result, int64_t handle, int64_t func_id, int64_t depth)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_body(handle, func_id, depth, out);
    return result;
}
```

- [ ] **Step 3: Prototype** in `include/ns_common.h` (beside the L1 `madc_graph_*`)

```c
void *madc_graph_body(void *result, int64_t handle, int64_t func_id, int64_t depth);
```

- [ ] **Step 4: Dialect wrapper body** in `src/ns_madc.cpp` (mirror L1's `graph_*` wrappers)

```cpp
value &graph_body(value &out, int64_t handle, int64_t func_id, int64_t depth)
	{ madc_graph_body(&out, handle, func_id, depth); return out; }
```

- [ ] **Step 5: Dialect declaration** in `include/madc/ns_madc` (declaration-only, beside L1's)

```cpp
value &graph_body(value &out, int64_t handle, int64_t func_id, int64_t depth);
```

- [ ] **Step 6: Build** (batched). Zero warnings.

---

## Task 5: `graph.children(id, depth?)` engine verb + plumbing

**Files:** same five layers as Task 4.

**Interfaces:**
- Produces: `bool internal_program_graph_children(int64_t handle, int64_t id, int64_t depth, madc::value &out)`; `madc_graph_children`; `graph_children`.

- [ ] **Step 1: The engine verb** — route the id space (body handle vs. type-id)

```cpp
// graph.children(handle, id, depth): structural descent from any node.
//  - id in the body space (>= GRAPH_BODY_ID_BASE): resolve to the TokenBase and
//    walk its subtree to `depth` (default 1 = immediate children).
//  - id a type-id for a FUNCTION: descend into its body (same as graph.body at
//    the given depth) — a function's structural children are its body nodes.
//  - id a type-id for a non-function (struct/etc.): its structural children are
//    its members; defer to graph.members (return empty here + a note) — L1b
//    owns BODY descent only; the decl-graph children stay L1's graph.members.
// Result shape identical to graph.body.
bool internal_program_graph_children(int64_t handle, int64_t id,
				     int64_t depth, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    std::vector<madc::value> nodes, edges;
    bool truncated = false;

    if ( id >= GRAPH_BODY_ID_BASE )
    {
	const TokenBase *n = graph_body_resolve(st, id);
	if ( n )
	    graph_body_walk(st, n, depth, GRAPH_BODY_NODE_CAP, nodes, edges, truncated);
    }
    else
    {
	DataDef *dd = child.type_from_id(id);            // binds child's table (M-5)
	TokenFunc *tf = dd ? graph_function_token(child, dd) : (TokenFunc *)0;
	if ( tf )
	    graph_body_walk(st, tf, depth, GRAPH_BODY_NODE_CAP, nodes, edges, truncated);
	// non-function type-id: members are graph.members' job (L1); empty here.
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

- [ ] **Step 2-5: Plumbing** — same four layers as Task 4, for `madc_graph_children(void *result, int64_t handle, int64_t id, int64_t depth)` / `graph_children(value &out, int64_t handle, int64_t id, int64_t depth)`.

- [ ] **Step 6: Build** (batched). Zero warnings.

---

## Task 6: MCP seat — `graph.body` + `graph.children` tools + integration test

**Files:**
- Modify: `tools/madcide/madcide_mcp.inc` (`graph_tool_descriptors`, `graph_call`)
- Modify: `tests/testmadcide_serve_graph.mad`, `.input`, `.expect`

**Interfaces:**
- Consumes: `graph_body`, `graph_children` (Tasks 4-5), the existing `ensure_phandle` handle path and the `graph.` prefix dispatch shipped in `0f74b0ed`.

- [ ] **Step 1: Read the existing `graph_call` + `graph_tool_descriptors`** (`tools/madcide/madcide_mcp.inc`) to match the exact shape of the L1 tools (arg extraction, handle resolution, terse-result stringify, the `js::stringify`→ring-copy trap). Follow that shape — do not invent a new one.

- [ ] **Step 2: Add the two descriptors** to `graph_tool_descriptors` (dialect literals; `depth` optional). Names: `graph.body`, `graph.children`. Inputs: `graph.body` = `{ func: int, depth?: int }`, `graph.children` = `{ id: int, depth?: int }`.

- [ ] **Step 3: Add dispatch arms** in `graph_call` — resolve the handle via `ensure_phandle` (same as the L1 arms), read `func`/`id` and optional `depth` (default: `graph.children` → 1; `graph.body` → -1), call `madc::graph_body` / `madc::graph_children` into a `var`, stringify once, return. Unknown-arg / missing-required → the same `isError:true` prose shape the L1 arms use.

- [ ] **Step 4: Extend `tests/testmadcide_serve_graph.*`** to drive the two new tools over the seat on the fixture: call `graph.symbols` to get a function id, `graph.body(func)` and assert kinds (`Block`, `Statement`, `Call`, `Literal`) + `isError:false`; take a body-node id from the result and call `graph.children(id)` asserting a nested node. Follow the existing `.input` JSON-RPC frame shape.

- [ ] **Step 5: Run** `bin/madc tests/testmadcide_serve_graph.mad < tests/testmadcide_serve_graph.input` (capped: `( ulimit -t 120; timeout 180 … )`); assert rc0, empty stderr, every `.expect` line.

**Commit** (B): `tools/`+`tests/` only — NO rule trailers.

---

## Task 7: Engine-verb fixture + design-doc substrate clarification

**Files:**
- Create: `tests/testgraphbody.mad`, `tests/testgraphbody.expect`, `tests/testgraphbody.expect_quiet`
- Modify: `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` (§6.2/§6.3/§8)

- [ ] **Step 1: The fixture** — value-first (zero includes/`using`/`std::`, `var`, `println`/`format`). Open a parse handle on an inline source string containing a function with a `Block`, an `if`, a `for`, a `call`, and a literal; call `madc::graph_symbols` to get the function id; call `madc::graph_body(handle, func_id, -1)`; iterate `nodes`/`edges` and print the kinds and counts; take a child id and call `madc::graph_children(handle, id, 1)`. Mirror `tests/testgraphaccessors.mad`'s structure (the L1 fixture) for the handle-open + assertion idiom.

Fixture source under test (embed as a `var src = "...";`) must exercise each kind:
```c
long add(long a, long b) { return a + b; }
long run(long n) {
    long total = 0;
    for (long i = 0; i < n; i = i + 1) {
        if (i > 0) { total = add(total, i); }
    }
    return total;
}
```

- [ ] **Step 2: `.expect`** — assert the presence of `Block`, `Statement`, `Call`, `NameRef`, `Expr`, `Literal` kind strings in the printed output, a nonzero `CONTAINS` edge count, and that a `graph.children` call on a body id returns a child. `.expect_quiet` — present (empty), asserting clean stderr.

- [ ] **Step 3: Run** the fixture directly (capped); rc0, clean stderr, all `.expect` lines.

- [ ] **Step 4: Design-doc clarification** — in `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md`:
  - §6.2: annotate the "cir_node bodies" row → "the live `TokenBase` AST (the persistent MC11-IR high-level face); the transient c2mir `node_t` arena is not retained on a parse handle."
  - §6.3: replace "a node id is `cir_ref{seg,idx}` (tree nodes)" → "for body/tree nodes, a per-handle opaque body handle (`GRAPH_BODY_ID_BASE`-partitioned), since the `cir_ref` arena is rebuilt per compile; for decl nodes, the type-id."
  - §8: "cir_node = bodies" → "persistent `TokenBase` AST = bodies."
  Keep it to those lines; do not restructure the doc.

**Commit** (C): `tests/`+`docs/` only — NO rule trailers.

---

## SDD execution note (batching, from the L1 slice)

Tasks 1-5 are tightly coupled (all in `src/madc_program.cpp` + the shared five-layer plumbing) and are best executed by ONE generously-briefed implementer doing them in order, with **no intermediate builds** — one foreground `make -C src` after Task 5, then the targeted tests, then TWO commits: (A) the engine verbs + plumbing (`src/`+`include/`, rule trailers), (B) the seat family + tests (Task 6, `tools/`+`tests/`). Task 7's fixture rides commit B; the doc clarification is a small third commit (C) or folds into B. This mirrors the L1 ruling (`make -C src` relinks ~60 unit-test binaries per run; 5 per-task builds cost 5× for no added safety). The controller reviews the whole branch diff against this plan + the madc rules, then an independent fresh-eyes reviewer, then one fix round if needed.

---

## Self-Review

**1. Spec coverage:**
- §6.4 `graph.children(id, depth?)` → Task 5. `graph.body(func, depth?)` → Task 4. ✓
- §6.2 body-node kinds (`TranslationUnit, Block, Statement, Call, NameRef, Expr, Literal`) → Task 1 `GraphBodyKind` (+ `Error`, `Unknown`). ✓
- §6.2 `CONTAINS` edge → Task 3 walker. ✓
- §6.5 terse `{id,kind,name,span}` + `depth` + cap → Tasks 3-5. Cursor pagination explicitly deferred to L2 (documented). ✓
- §6.3 snapshot-stable opaque ids + re-query → Task 1 registry + refresh clear. ✓
- §7 gate (unit-shaped accessor test + dialect MCP-seat integration test) → Tasks 6-7. ✓

**2. Placeholder scan:** every code step carries real code. The three deliberate "confirm-before-build" points (TokenWHILE downcast; SWITCH/TRY/THROW sub-parts; TokenInt/Real/Char spelling accessor) are explicitly flagged as verify-then-write, not silent assumptions — the honest form the L1 slice used, per the owner's don't-assume directive. Not placeholders: they are named, located (file:line), and scoped.

**3. Type consistency:** `graph_body_kind_of`/`graph_body_kind_name` share `GraphBodyKind`. `graph_body_intern`/`graph_body_resolve`/`GRAPH_BODY_ID_BASE` agree across Tasks 1/3/5. `graph_body_walk` signature identical in Tasks 3/4/5. Bridge/wrapper/decl names (`madc_graph_body`/`graph_body`, `madc_graph_children`/`graph_children`) consistent across the five layers. `internal_program_graph_body`/`_children` arg order `(handle, {func|id}, depth, out)` consistent.

**Known-open (additive, not correctness holes):** the exotic expression long tail (`TokenNEW`/`TokenDELETE`/`TokenObjTemp`/`TokenMatch`/`TokenPackExpansion`/`TokenComplexPart`) descends as a leaf (no children) until a follow-up adds its case — same deferral shape as L1's M-2/M-4/M-6. `graph.children` on a non-function *type-id* returns empty (members stay L1's `graph.members`); a future unification could route it. Both recorded here, not silent.
