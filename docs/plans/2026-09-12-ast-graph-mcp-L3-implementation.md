# Code-graph MCP L3 — node editing (`graph.insert` / `graph.replace` / `graph.delete`) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an agent EDIT the live forest by node: `graph.replace(id, src)`,
`graph.delete(id)`, `graph.insert(id, where, src)` — a statement, a function
definition or a global declaration is the edit unit; the edit is validated by
the same parser before it commits, applied through madcide's ONE text-mutation
owner (undo, change log, anchors, multi-client fan-out for free), logged as a
node-op beside the byte splices, and answered with the edited node's NEW id.
Plus the three read primitives the edit stands on and every agent needs anyway:
`graph.span(id)` (a node's exact source extent), `graph.at(line, column)` (the
statement/definition starting at a position), and generation-stamped ids that
make a stale id a LOUD refusal instead of a silently wrong answer.

**Architecture:** Four layers, each doing one thing. (1) **Parser** stamps the
source extent of every statement-level construct as it finishes parsing it
(`TokenBase::head_tok` / `end_line` / `end_column` — the deepest layer: the
parser is the one thing that knows where a statement ends). (2) **Engine**
(`src/madc_program.cpp`) reads those stamps into `graph_span`/`graph_at`, stamps
the handle's parse GENERATION into every id it hands out (refusing stale ones),
and offers `parse_refresh_checked` — parse the candidate whole-TU text into a
fresh child and SWAP it in only if it introduces no new error (atomic
validate-then-commit; a rejected edit leaves the live tree and the buffer
untouched). (3) **madcide seat** (`tools/madcide/madcide_mcp.inc`): the
`graph.*` family gains an enum-dispatched verb table with a tier gate
(mutations need `tierEDITOR`, the same ladder the registry commands use), and
`graph_edit_apply` = span → splice offsets → candidate text → checked refresh →
`edit_checkpoint` + node-op record + `ed_text_erase`/`ed_text_insert` +
`refresh_spans` + `broadcast_events` → the new node via `graph.at`.
(4) **Change log**: a `kind:"nodeop"` record precedes its splices; replay
applies `splice` records only.

**Tech Stack:** C++11 engine (`src/parser.cpp`, `include/tokens.h`,
`include/madc.h`, `src/madc_program.cpp`), the C bridge (`src/parser.cpp` +
`include/ns_common.h`), the dialect wrapper (`src/ns_madc.cpp` +
`include/madc/ns_madc`), the texteditor core (`tools/texteditor/editor_events.inc`),
the madcide registry + MCP seat (`tools/madcide/madcide_enums.inc`,
`tools/madcide/madcide_mcp.inc`), value-first `.mad` integration tests.

**Spec:** `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` (§5 invariants 3/5/6/7/8,
§6.3 identity, §6.6 editing, §7 L3, §9 cruxes, §10 open decisions) +
`docs/plans/2026-09-09-nexus-client-server-design.md` §2.4 (change event log),
§2.5 (tiers). The plan argues from those specs; executors read both. Owner
directive (L1b, standing): "proceed with care and ensure this is done correctly
as it forms the basis for the most critical part of the Nexus."

## Decisions this pass settles (spec §10 "node-spec form" + §6.3 "identity")

1. **Node spec = a SOURCE FRAGMENT, validated by the real parser.** Not a
   builder DSL: a DSL is a second grammar for the same language (a parallel
   implementation of the surface, invariant §5.8) and an LLM writes code far
   better than it writes a DSL. The fragment is spliced into the TU text at the
   target node's parser-stamped extent and the WHOLE TU is re-parsed into a
   fresh child — "validated by the same parser/sema" (§6.6) literally — and the
   child is swapped in only on acceptance ("atomic to tree-well-formedness").
2. **Reverse-render at statement granularity = the retained text.** madc has a
   retained-token echo (`--emit=c++`, `cir_emit_cxx`) but no TokenBase→source
   renderer; §9 rules L3's render unit is the whole statement/definition. For an
   UNCHANGED statement its source text IS its render; the NEW statement's render
   IS the fragment. So the buffer after the edit is exactly "the tree rendered":
   the splice boundaries come from the tree (never from the agent), the text
   between them from the fragment. Never hand-patched: the agent supplies ids
   and code, never offsets (invariant §5.3).
3. **Extents are PARSER stamps, not stream heuristics.** The end of `if (c) x;
   else y;` or `do x; while (c);` is grammar knowledge; re-deriving it in the
   graph layer by bracket-walking the token stream would be a shim one layer
   up (Top-10 #2). The parser stamps `head_tok` (the first token it was handed)
   and `end_line/end_column` (the static parse position — the END of the last
   consumed token) on every node `parseStatement` returns, and the head of the
   one free function a definition statement registers.
4. **Identity across edits (§6.3 crux, v1 = HONEST, not GumTree):** every id
   carries the handle's parse generation (bits 40..60); a refresh bumps it; a
   stale id is refused with `{error, stale:true}` by every verb — never resolved
   against the new tree. The edit result carries the edited node's NEW id
   (located by `graph.at` at the splice position) so the agent continues
   without a full re-query. Content-derived ids that survive edits elsewhere
   (the GumTree refinement) are a documented increment over the same
   `graph_id_stamp` seam, not v1.
5. **The verb IS madcide's registry convention.** `graph.*` stays the distinct
   MCP tool family (L1 precedent) but is now dispatched through an ENUM
   (`graph_verb`, converted once from the wire name — the enum law), gated by a
   `graph_min_tier` switch that mirrors `cmd_min_tier` (mutations =
   `tierEDITOR`; an MCP client is born `tierOBSERVER` and promoted by
   `clienttier`), and its mutations flow through `ed_text_insert`/`ed_text_erase`
   — the ONE text-mutation owner — so undo (one `edit_checkpoint`), the change
   log (`journal_splice`), anchor shifting and the hub fan-out
   (`broadcast_events`) come for free, exactly as §6.6 promises.
6. **Node-op record = a second record KIND in the one JSONL stream** (client-
   server §2.4 names two kinds; this adds `nodeop` beside `splice`/`checkpoint`):
   `{kind:"nodeop", es, doc, verb, target:{...span}, src}` appended BEFORE the
   splices it caused (causal order). `clog_replay` applies `splice` records only.
7. **Acceptance policy:** an edit is rejected iff it INCREASES the error count
   (error-severity diagnostics + `error_nodes`) over the live tree — a clean
   tree stays clean; a tree the user left broken can still be repaired through
   the verb. The candidate's diagnostics come back either way.
8. **Scope v1:** targets = statement-level body nodes (elements of a
   `TokenCpnd::statements` list inside a TU-own function, nested blocks
   included), TU-own free function definitions, global declarations with a
   recorded `TokenDecl`. `where` ∈ {`before`, `after`}. NOT v1 (refused loudly,
   documented): expression nodes, class members/methods, functions inside a
   statement that registers several (class bodies), macro-headed statements
   (synthetic positions), inserting into an EMPTY block (replace the block).

## Global Constraints

Every task's requirements implicitly include this section.

- **Substrate = the live structures only** (L1b/L2 constraint, unchanged): the
  live `TokenBase` AST via `graph_function_token` + `graph_body_children`, the
  live `child.top_decls`, the live `child.tokens` stream. NEVER the transient
  c2mir arena, NEVER a frozen forest, NEVER a second parser or renderer.
- **The parser stamps extents; nothing else computes them.** `graph_span` READS
  `head_tok`/`end_line`/`end_column`; it never scans the stream for a `;`.
- **Ids are generation-stamped everywhere.** Every id EMITTED by any graph verb
  (node ids, edge endpoints `from`/`to`/`at`/`in`) passes through
  `graph_id_stamp`; every id ACCEPTED is checked with `graph_id_fresh` at the
  verb's entry and compared/indexed via `graph_id_raw`. A gen-0 id is
  bit-identical to the pre-L3 id (L1/L1b/L2 tests stay green unchanged).
- **Atomic validate-then-commit.** The live child is replaced ONLY after the
  candidate parsed acceptably; on reject the candidate is deleted and neither
  the handle nor the buffer changes. The buffer is mutated ONLY via
  `ed_text_erase`/`ed_text_insert` (never `ui::text_*` directly), once per
  edit, after ONE `edit_checkpoint` (one undo step per node-op).
- **Tier gate before anything else.** `eff_tier(self_id) < graph_min_tier(gv)`
  refuses with the registry's prose shape; the refusal is `isError` content.
- **Enums, not strings** (invariant §5.4): the verb name and `where` convert
  ONCE at the wire boundary (`graph_verb_of`, `graph_where_of`); dispatch is a
  `switch`; a misspelling is a refusal. The record `kind` is text only in the
  persisted line (the existing enum-boundary rule of the change log).
- **Terse results** (§6.5): `graph.span`/`graph.at` return ONE node object
  `{id, kind, name?, line, column, end_line?, span:{line, column, end_line,
  end_column}}`; an edit returns `{ok, gen, node, span, seq}` or `{ok:false,
  error, diagnostics}`. `span.column`/`span.end_column` are 0-based byte offsets
  within their (1-based) lines, start inclusive / end exclusive — the
  highlight-row convention (`highlight_token_rows`).
- **Rule trailers** on every `src/`+`include/` commit; `Oracle: n/a — IDE
  machinery, the .mad integration tests are the oracle` is honest here.
- **Targeted tests per slice; the FULL battery rides the V6 arc RELEASE seam,
  never this slice** (owner law, seventh repeat). Zero warnings (`-Wall
  -Werror`). No `&&` chains. Scratch in `tmp/`. Value-first `.mad` (`var`, bare
  `println`, out-param `madc::graph_*`). Push only to owner remotes.
- **Thread-safety contract:** unchanged from L1/L2 — a parse handle is used
  only from the program that opened it; the seat is cooperative single-thread.
  The one new window: `parse_refresh_checked` may YIELD (Stage-2 slices); the
  seat re-reads the buffer after it returns and aborts the commit if the text
  changed underneath (then re-syncs the handle with `reparse_buffer`).

---

## Verified recon appendix (facts checked against the tree at HEAD `18c29b29`, 2026-09-12)

**The change event log exists (texteditor layer, `tools/texteditor/editor_events.inc`).**
- `changelog_of(w, doc)` (239) — a per-doc "changelog" entity whose piece-table
  text is JSONL; `seq` lives on it. `clog_append(w, doc, rec)` (252) stamps
  `rec["seq"]` and appends one line. `journal_splice` (265) writes THE splice
  record `{kind:"splice", es, doc, at, del, ins}`; `clog_checkpoint` (275)
  writes `{kind:"checkpoint", doc, text}`. `clog_replay` (298-341) replays from
  the newest checkpoint; its `else` arm (332-338) applies ANY non-checkpoint kind
  as a splice — Task 6 narrows it to `kind == "splice"`. `clog_head` (346)
  = current seq without creating the log; `events_since` (359) = the raw records
  past a seq (the broadcast feed).
- **The ONE text-mutation owner:** `ed_text_insert(w, es, doc, off, s)` (415) =
  `ui::text_insert` + `shift_anchors` + `journal_splice`; `ed_text_erase(w, es,
  doc, off, n)` (422) likewise. Precedent for a replace = erase then insert:
  `madcide_core.inc:2777-2781` (block move).
- **Undo:** `edit_checkpoint(w, es, doc, caret)` (566) = ONE undo step before a
  mutation; `undo_edit(w, es, doc)` (576) / `redo_edit` (590); `clamped_caret(w,
  es, doc)` (539) is the caret the checkpoint records.
- **Offsets:** `line_of(w, doc, off)` (60) and `line_start_of(w, doc, line)` (67)
  — 1-based lines, byte offsets — are the seat's own coordinate converters
  (`session_state` uses them, `madcide_api.inc:210-211`).

**The seat (`tools/madcide/madcide_mcp.inc`, `madcide_api.inc`).**
- `graph_call(out, S, doc, nm, params)` (229-301) is a `strcmp` ladder over verb
  names (L1/L2) with no `self_id`; `mcp_handle` (309) HAS `self_id` and calls
  it at 375. Task 7 threads `self_id` through and replaces the ladder with an
  enum switch.
- `mcp_tools` (156-179) appends `graph_tool_descriptors` (55-150) rows verbatim
  to tools/list; Task 7 gives each row a `code` and projects only
  `name/description/inputSchema` to the wire.
- `api_run` (`madcide_api.inc:150-190`): the tier gate `eff_tier(self_id) <
  cmd_min_tier(code)` → `{ok:false, error: "'<name>' requires <tier> (you are
  <tier>)"}`; `clog_head` before/after + `broadcast_events(w, adoc, head_before,
  self_id)` (55) after a command that advanced the log. `eff_tier(id)` (93): an
  untracked id (−1, the local/MCP operator) is `tierOWNER`.
- Tiers: `enum ide_tier { tierOBSERVER = 0, tierEDITOR, tierOWNER }`
  (`madcide_enums.inc:536`); `cmd_min_tier` (569-623) is ONE switch listing the
  elevated codes — the shape `graph_min_tier` mirrors.
- `ensure_phandle(w, es, doc)` (`madcide_core.inc:1696`) — one handle per doc;
  `reparse_buffer` (1742) = `parse_refresh` + `refresh_spans`; `refresh_spans(w,
  es, doc, h)` (1669); `diag_error_count(diags)` (1766) counts rows whose
  `severity_code == madc::diag_severity::error`.

**Parse handles (`src/madc_program.cpp`).**
- `struct parse_tu_state` (4950-4963): `child`, `display_name`, L1b `body_nodes`
  / `body_ids`. `parse_handle_child_init` (4972): `keep_trivia = true` (the
  retained stream keeps trivia). `internal_program_parse_refresh` (5262-5284):
  whole-TU re-parse = `delete st->child; st->child = new ::Program(...)`;
  `compile_source_child_frontend` (4626: mute + parse + `compile()`); clears the
  body registry. Task 4's checked refresh is this function with the swap made
  conditional; Task 2 adds the generation bump to both.
- `diagnostic_rows_from_child` (4644); `::Program::DiagnosticSeverity` is
  `::madc::diag_severity` (`include/madc.h:2553`); `Program::error_nodes`
  (`madc.h:2651`, public `size_t`, "the NODE count … reset by
  clear_diagnostics") — the acceptance count = error-severity rows + error_nodes.
- `enclosing_func_at(child, display_name, line, column)` (4683) and
  `tu_own_function` (4667) — the TU-own filter + innermost function at a
  position; `graph_function_token(child, dd)` (5573).
- Id partition (5633-5670): `GRAPH_DECL_ID_BASE = 0x2000…`, `GRAPH_BODY_ID_BASE
  = 0x4000…` (bits 61/62); `graph_id_space`; `graph_type_from_id_guarded(child,
  id)`; `graph_global_resolve(child, id)`; `graph_global_node_value(td, id)`;
  `graph_func_node_value(child, tf)` (5589, `id = child.type_id_for(tf->var.type)`);
  `graph_node_value(child, dd)` (5609). Body registry: `graph_body_intern` (6121),
  `graph_body_resolve` (6134), `graph_body_node_value` (6286), `graph_body_walk`
  (6319), `graph_body_children` (6151), `GRAPH_BODY_NODE_CAP` (6462),
  `GRAPH_COLLECT_VISIT_CAP` (L2). Bits 40..60 of every id are free — the
  generation field fits without touching space or index bits.
- Id makers/consumers to retrofit (exact lines at HEAD): `graph_func_node_value(`
  callers 5615, 5711, 5936, 6552, 6604, 6623, 6669, 6700, 6752, 6797, 6829;
  `graph_node_value(` callers 5756, 5796, 5830, 5834, 5841, 5880, 5907, 6563;
  `graph_global_node_value(` 5741, 6671, 6764 (`GRAPH_DECL_ID_BASE + i`), 6799;
  `type_id_for(` inside verbs: 5592, 5618 (makers), 6561 `cid`, 6615 `caller_id`,
  6618 / 6690 / 6816 (comparisons against an INPUT id), 6698 / 6824 `encl_id`.
  Verb entry points (`bool internal_program_graph_*`): symbols 5697, node 5727,
  type_of 5773, definition 5817, members 5857, bases 5892, enclosing 5920, body
  6464, children 6496, callees 6536, callers 6588, references 6649, search 6731,
  impact 6778. The forward declarations live in `src/parser.cpp:336-376`; the
  bridges at `1092` (`madc_parse_refresh`: `require_runtime_eval_program(owned)`
  gives `*active`) and `1184-1228` (L1b/L2 thunks); `include/ns_common.h:245-252`;
  `src/ns_madc.cpp:271-290`; `include/madc/ns_madc:189-217`.

**Tokens and positions.**
- `TokenBase` (`include/tokens.h:244-283`): `file`, `parent`, `line`, `column`,
  `std::streampos pos` (DEAD: no reader, no writer anywhere in `src/`+`include/`
  — grep 2026-09-12 finds only the constructors' `pos = 0`), `TokenRec rec`,
  `read_count`, `leading_trivia`; static `_parse_file/_parse_line/_parse_column`
  = "the position of the most recently consumed source token", updated by
  `Program::nextToken()` (`include/madc.h:5772-5789`) and restored by the
  speculative-scan RAII/rollback sites (`parser.cpp:7889-7905, 9839-9846,
  10739-10741, 13858-13866, 16838-16846, 17156-17164`); `peekToken()`
  (`madc.h:5700`) and `pushToken()` (`5709`) do NOT touch it.
- **Columns are END-anchored:** `highlight_token_rows` (`madc_program.cpp:5493-5503`)
  — "Token stamps are END-anchored (the repo's diagnostic convention); a span's
  contract is START + length": `start = t->column - spelling.size()`. A string
  literal's source geometry is `TokenStr::src_pieces` (`{line, col, len}`,
  `tokens.h:1458`; `col` is the START). `madc_token_spelling(TokenBase *)`
  (`madc.h:2124`) is the ONE spelling owner; `is_synthetic_position()`
  (`tokens.h:321`) marks macro-expansion tokens whose spelling occupies no bytes.
- `TokenCpnd` (`madc.h:741-770`): `statements` (`std::vector<TokenStmt *>`),
  `int end_line` ("line of closing } (set by parseCompound)", 766, ctor 768).
  `TokenFunc : public TokenVar, public TokenCpnd` (792); `TokenVar : public
  virtual TokenBase` (`datatokens.h:274`), `TokenCpnd : public virtual TokenBase`
  (741) — ONE TokenBase subobject, so a hoisted field is unambiguous. `TokenStmt
  : TokenBase` (705); `TokenDecl : TokenVar` (814). `as_cpnd_tok()` /
  `as_func_tok()` / `as_str_tok()` (`tokens.h:386/387/360`).
- `parseStatement(TokenBase *tb)` (`src/parser.cpp:70098-71013`, 93 returns, 54
  callers) is THE statement entry — `parseCompound` (64249-64302) calls it per
  block statement; `Program::parse` (71693; loop 71726-71812) calls it per
  top-level construct with `tb = nextToken()` (the real stream head token) and
  pushes a non-compound result into `tp->statements` (71805) or adopts a
  script-mode statement. `parseCompound` stamps `code->end_line = tb->line` at
  the `}` (64273). A function definition parses its body EAGERLY inside the
  statement (`parseFunction` 64901: `parseCompound()` at 66650; the deferred
  sink at 66499 is the class-body path) and registers it (`ast.push_back(tf);
  pending_funcs.push_back(tf)` 66720-66721) — `parseFunction` is `void`, the
  statement returns no node; `tf->end_line = tc->end_line` at 66684 (and the
  deferred-body twin 45894). `tf->method` (66679) is non-NULL for methods.
- Live globals record their `TokenDecl`: `parser.cpp:68436-68447` (`gtd.decl =
  td; … return td;`) and `69147-69157` (index captured, "the TokenDecl built
  later … linked back"); `gtd.origin = tb` (the head type token) both times.
- Namespaces: `parse_namespace_block` (41967-42120) consumes only the header;
  members are parsed by the enclosing loop's `parseStatement` calls — so a
  namespaced free function's head is stamped by ITS statement (the outer
  statement, seeing `head_tok` already set, leaves it).

**Dialect facts (confirm-before-build already settled in L1/L1b/L2 ledgers):**
no value-return — carriers via out-param; `js::stringify` returns a ring
`const char *` (copy into a `var` before reuse); no nested brace literal as a
literal's value (pre-build named vars); `php::substr` does not exist —
`perl::substr(var, off, len)` does (`madcide_core.inc:8076`); `strncmp`/`strcmp`
over `c_str()`; `switch` over a `long` code is fine (`cmd_min_tier`);
`raw.c_str()[i]` indexes bytes (8074).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `include/tokens.h` | modify (TokenBase) | `head_tok`/`end_line`/`end_column` replace the dead `pos` |
| `include/madc.h` | modify | drop `TokenCpnd::end_line` (hoisted); declare `parseStatementBody` |
| `src/parser.cpp` | modify | the stamping `parseStatement` wrapper; `end_column` at `}` and on functions; L3 forward decls + 3 bridges |
| `src/madc_program.cpp` | modify | generation-stamped ids; `graph_span`; `graph_at`; `parse_refresh_checked` |
| `include/ns_common.h`, `src/ns_madc.cpp`, `include/madc/ns_madc` | modify | five-layer plumbing for the 3 new verbs |
| `tools/texteditor/editor_events.inc` | modify | `clog_replay` applies `splice` records only |
| `tools/madcide/madcide_enums.inc` | modify | `graph_verb` / `graph_where` enums, `graph_where_of`, `graph_min_tier` |
| `tools/madcide/madcide_mcp.inc` | modify | descriptors carry codes; `graph_verb_of`; `graph_call` switch + tier gate + `self_id`; `graph_edit_apply` |
| `tests/testgraphedit.mad` `.expect` `.expect_quiet` | create | engine: spans, at, checked refresh, staleness |
| `tests/testmadcide_serve_edit.mad` `.expect` | create | seat: the 5 tools end to end + log + undo + tier |
| `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` | modify | §6.3/§6.6/§10 settled |

Commits: **A** = Tasks 1-5 (`src/`+`include/`, rule trailers, ONE build) ·
**B** = Tasks 6-8 (`tools/`+`tests/`) · **C** = Task 9 (`docs/`).

---

### Task 1: Parser extent stamps — `head_tok` / `end_line` / `end_column`

**Files:**
- Modify: `include/tokens.h:251-283` (TokenBase fields + both constructors)
- Modify: `include/madc.h:766-768` (TokenCpnd), `include/madc.h:6072` (Program decl)
- Modify: `src/parser.cpp:64273` (parseCompound), `45894` and `66684` (parseFunction end copies), `70098` (rename + wrapper)

**Interfaces:**
- Produces: `TokenBase::head_tok` (`TokenBase *`, the construct's first source
  token or NULL), `TokenBase::end_line` / `end_column` (the END of the last
  consumed token; 0 = unstamped). `TokenCpnd::end_line` and `TokenFunc`'s reads
  of it now resolve to the base field (same values as before, plus a column).
- Consumes: `TokenBase::_parse_line/_parse_column` (nextToken's static
  position), `pending_funcs`, `TokenFunc::method`.

- [ ] **Step 1: TokenBase — replace the dead `pos` with the extent fields**

In `include/tokens.h` replace line 255 (`std::streampos pos;`) with:

```cpp
    // Source EXTENT of a parsed construct (code-graph MCP L3, design §6.6/§9:
    // the statement is the edit unit). Stamped by the PARSER, the one owner
    // (Program::parseStatement's wrapper, parseCompound's '}', parseFunction):
    //   head_tok   — the FIRST source token of the construct (the token
    //                parseStatement was handed); its START is the extent start
    //   end_line / end_column — the END of the LAST consumed token: the static
    //                parse position when the construct finished (a simple
    //                statement's ';', a compound's '}'). Columns are END-
    //                anchored (the byte after the token's last char).
    // NULL / 0 = no extent (a leaf, an expression node, a synthesized token).
    // TokenCpnd's former end_line (the closing-brace line) lives here now.
    // Replaced the never-read, never-written `std::streampos pos`.
    TokenBase *head_tok;
    int end_line;
    int end_column;
```

In both constructors (282-283) replace `pos = 0;` with
`head_tok = NULL; end_line = 0; end_column = 0;`.

- [ ] **Step 2: TokenCpnd — hoist `end_line`**

In `include/madc.h` delete line 766 (`int end_line; // line of closing } …`) and
change the constructor at 768 to
`TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; }`
(the base constructor zeroes `end_line`). Every existing `->end_line` read
(`madc_program.cpp` ×9, `parser.cpp` ×3) compiles unchanged against the base
field. After line 6072 (`TokenBase *parseStatement(TokenBase *);`) add:

```cpp
    TokenBase *parseStatementBody(TokenBase *);	// the grammar; parseStatement stamps its extent
```

- [ ] **Step 3: parseCompound — stamp the closing column too**

`src/parser.cpp:64273`: after `code->end_line = tb->line;` add
`code->end_column = tb->column;`.

- [ ] **Step 4: parseFunction — copy the column with the line**

At `66684` (and the deferred-body twin `45894`) after `tf->end_line = tc->end_line;`
add `tf->end_column = tc->end_column;`.

- [ ] **Step 5: the stamping wrapper**

At `70098` rename the existing definition to
`TokenBase *Program::parseStatementBody(TokenBase *tb)` and insert directly
above it:

```cpp
// The ONE extent stamp (code-graph MCP L3, design §6.6/§9). Every construct
// parseStatement returns records its source extent as it finishes: head_tok =
// the first token this statement was handed (the stream head — Program::parse
// and parseCompound hand the real token), end = the static parse position, i.e.
// the END of the last token nextToken() consumed (a simple statement's ';', a
// compound's '}' — peek/pushToken never move it). A definition statement that
// returns no node but registered exactly ONE free function (parseDeclaration ->
// parseFunction, whose body parses eagerly inside this statement) stamps that
// function's head; parseCompound's '}' + parseFunction's end copy already gave
// it its end. A statement that registered several functions (a class body) or
// a method stamps nothing — no extent beats a wrong one; an inner statement's
// stamp (a namespace member parsed by the enclosing loop) is never overwritten.
TokenBase *Program::parseStatement(TokenBase *tb)
{
    size_t funcs_before = pending_funcs.size();
    TokenBase *r = parseStatementBody(tb);
    if ( r )
    {
	if ( !r->head_tok )
	    r->head_tok = tb;
	r->end_line = TokenBase::_parse_line;
	r->end_column = TokenBase::_parse_column;
    }
    else if ( pending_funcs.size() == funcs_before + 1 )
    {
	TokenFunc *tf = pending_funcs.back()
	    ? pending_funcs.back()->as_func_tok() : (TokenFunc *)0;
	if ( tf && !tf->method && !tf->head_tok )
	    tf->head_tok = tb;
    }
    return r;
}
```

- [ ] **Step 6: no build yet** (Tasks 1-5 build once in Task 5).

---

### Task 2: Generation-stamped ids — stale ids refuse loudly

**Files:**
- Modify: `src/madc_program.cpp:4950-4963` (parse_tu_state), `5633-5670` (partition helpers), `5589-5620` (node value makers), `6121-6140` (body registry), every verb entry point and id maker listed in the recon appendix, `5262-5284` (refresh)

**Interfaces:**
- Produces: `parse_tu_state::generation` (`uint32_t`, 0 at open, +1 per refresh);
  `graph_id_stamp(st, raw)`, `graph_id_raw(id)`, `graph_id_gen(id)`,
  `graph_id_fresh(st, id)`, `graph_stale_result(st, id)`;
  `graph_func_node_value(parse_tu_state *st, TokenFunc *tf)` and
  `graph_node_value(parse_tu_state *st, DataDef *dd)` (signature change: st
  replaces child).
- Consumes: the L1/L1b/L2 verbs as they stand.

- [ ] **Step 1: the generation and the stamp helpers**

In `parse_tu_state` after `body_ids` add `uint32_t generation;` and initialise
it in the constructor (`… : child((::Program *)0), generation(0) {}`). Directly
after `GRAPH_BODY_ID_BASE` (5634) add:

```cpp
// ---- L3: parse-generation-stamped ids (design §6.3 — identity across edits)
// Bits 40..60 of every id carry the handle's parse GENERATION; the space bits
// (61/62) and the index / type-id bits (0..39) are untouched, so a generation-0
// id is bit-identical to the pre-L3 id. A refresh bumps the generation; an id
// minted under an older one is STALE and every verb refuses it with
// {error, stale:true} — never resolved against the wrong tree (a silent wrong
// answer is the one failure a graph an agent edits through must not have).
static const int     GRAPH_GEN_SHIFT = 40;
static const int64_t GRAPH_GEN_MASK  = 0x1FFFFFLL;			// 21 bits
static const int64_t GRAPH_GEN_FIELD = GRAPH_GEN_MASK << GRAPH_GEN_SHIFT;

static int64_t graph_id_stamp(const parse_tu_state *st, int64_t raw)
{
    int64_t gen = (int64_t)st->generation & GRAPH_GEN_MASK;
    return (raw & ~GRAPH_GEN_FIELD) | (gen << GRAPH_GEN_SHIFT);
}
static int64_t  graph_id_raw(int64_t id) { return id & ~GRAPH_GEN_FIELD; }
static uint32_t graph_id_gen(int64_t id)
{
    return (uint32_t)((id >> GRAPH_GEN_SHIFT) & GRAPH_GEN_MASK);
}
static bool graph_id_fresh(const parse_tu_state *st, int64_t id)
{
    return graph_id_gen(id) == (uint32_t)((int64_t)st->generation & GRAPH_GEN_MASK);
}
static madc::value graph_stale_result(const parse_tu_state *st, int64_t id)
{
    std::map<std::string, madc::value> f;
    f["error"] = value(std::string("stale node id: minted at parse generation ")
	+ std::to_string(graph_id_gen(id)) + ", the handle is at generation "
	+ std::to_string(st->generation) + " (a refresh or an edit re-parsed the"
	" TU) - re-query");
    f["stale"] = value(true);
    f["nodes"] = value::make_array(std::vector<madc::value>());
    f["edges"] = value::make_array(std::vector<madc::value>());
    return value::make_object(f);
}
```

- [ ] **Step 2: resolvers read the RAW id**

`graph_type_from_id_guarded` (5652): `return child.type_from_id((uint32_t)graph_id_raw(id));`.
`graph_global_resolve` (5664): `size_t idx = (size_t)(graph_id_raw(id) - GRAPH_DECL_ID_BASE);`.
`graph_body_resolve` (6136-6138): `if ( !st || graph_id_space(id) != GraphIdSpace::Body ) return 0;
size_t idx = (size_t)(graph_id_raw(id) - GRAPH_BODY_ID_BASE);`.
(`graph_id_space` needs no change — the generation bits lie below bit 61.)

- [ ] **Step 3: makers STAMP**

`graph_body_intern` (6128): `int64_t id = graph_id_stamp(st, GRAPH_BODY_ID_BASE + (int64_t)st->body_nodes.size());`.
`graph_func_node_value` becomes:

```cpp
static madc::value graph_func_node_value(parse_tu_state *st, TokenFunc *tf)
{
    std::map<std::string, madc::value> f;
    f["id"] = value(graph_id_stamp(st, (int64_t)st->child->type_id_for(tf->var.type)));
    f["kind"] = value(std::string("Function"));
    f["name"] = value(tf->var.name);
    f["line"] = value((int64_t)tf->line);
    f["column"] = value((int64_t)tf->column);
    f["end_line"] = value((int64_t)tf->end_line);
    return value::make_object(f);
}
```

`graph_node_value(child, dd)` becomes `graph_node_value(parse_tu_state *st,
DataDef *dd)`: its internal `graph_function_token(child, dd)` reads
`*st->child`, its delegation passes `st`, and its id line is
`f["id"] = value(graph_id_stamp(st, (int64_t)st->child->type_id_for(dd)));`.
Update every caller listed in the appendix (`child` → `st`; inside the L2 verbs
`st` is already in scope). `graph_global_node_value` keeps its signature; its
one raw-constructing caller (6764) becomes
`graph_global_node_value(td, graph_id_stamp(st, GRAPH_DECL_ID_BASE + (int64_t)i))`.

Inside the L2 verbs: every `type_id_for(...)` result that is EMITTED (`cid` →
`e["to"]`, `caller_id` → `e["from"]`, `encl_id` → `e["in"]`) is stamped at
emission — e.g. `e["to"] = value(graph_id_stamp(st, (int64_t)cid));` — while the
dedup sets keep raw `uint32_t` keys; every comparison against an INPUT id
compares raw: `child.type_id_for(...) != (uint32_t)graph_id_raw(func_id)` (6618,
6690, 6816) and the seed inserts `(uint32_t)graph_id_raw(func_id)` /
`graph_id_raw(def_id)`. `e["from"] = value(func_id)` (the input, already
stamped) stays.

- [ ] **Step 4: freshness at every id-taking entry**

Right after `st` resolves in `graph_node`, `graph_type_of`, `graph_members`,
`graph_bases`, `graph_body`, `graph_children`, `graph_callees`, `graph_callers`,
`graph_references`, `graph_impact` insert (with the verb's id parameter name):

```cpp
    if ( !graph_id_fresh(st, node_id) )
    {
	out = graph_stale_result(st, node_id);
	return true;
    }
```

- [ ] **Step 5: refresh bumps the generation**

`internal_program_parse_refresh` (5281-5282): after the two `clear()` calls add
`++st->generation;`. (Task 4's checked refresh does the same on acceptance.)

---

### Task 3: `graph.span(id)` and `graph.at(line, column)` — extents from the stamps

**Files:**
- Modify: `src/madc_program.cpp` — new helpers + two verbs after `internal_program_graph_impact` (6778…)

**Interfaces:**
- Produces: `internal_program_graph_span(int64_t handle, int64_t id, value &out)`,
  `internal_program_graph_at(int64_t handle, int64_t line, int64_t column, value &out)`;
  helpers `graph_token_start`, `graph_extent_of`, `graph_span_value`,
  `graph_is_statement_level`, `graph_owner_function`, `struct GraphExtent`.
- Consumes: Task 1 stamps; Task 2 stamp/fresh helpers; `enclosing_func_at`,
  `tu_own_function`, `graph_function_token`, `graph_body_children`,
  `graph_body_node_value`, `graph_func_node_value`, `graph_global_node_value`,
  `graph_global_resolve`, `graph_body_resolve`, `madc_token_spelling`.

- [ ] **Step 1: the extent readers**

```cpp
// ---- L3: source extents (design §6.6/§9 — the statement is the edit unit) ----
// An EDITABLE node's extent = [START of its head token, END of its last consumed
// token], both stamped by the parser (TokenBase::head_tok / end_line /
// end_column — Task 1). Editable = a statement-level node (an element of some
// TokenCpnd::statements list reachable from a TU-own function body, nested
// blocks included), a TU-own free function definition, or a global declaration
// with a recorded TokenDecl. Expression nodes have no extent of their own — the
// enclosing statement is the reverse-render unit (§9). This layer READS the
// stamps; it never scans the token stream for a terminator.
struct GraphExtent { int line, column, end_line, end_column; };

// START of a token: stamps are END-anchored (column = the byte after the last
// char — highlight_token_rows' convention), so start = column - spelling; a
// string literal reads its lex-recorded first piece. False = a synthetic-
// position head (a macro expansion: the spelling occupies no source bytes) —
// no extent rather than a wrong one.
static bool graph_token_start(TokenBase *t, int &line, int &column)
{
    if ( !t || t->is_synthetic_position() )
	return false;
    if ( TokenStr *ts = t->as_str_tok() )
	if ( !ts->src_pieces.empty() )
	{
	    line = ts->src_pieces.front().line;
	    column = ts->src_pieces.front().col;
	    return true;
	}
    std::string sp = madc_token_spelling(t);
    line = t->line;
    column = t->column - (int)sp.size();
    if ( column < 0 )
	column = 0;
    return true;
}

static bool graph_extent_of(const TokenBase *n, GraphExtent &x)
{
    if ( !n || !n->head_tok || n->end_line <= 0 )
	return false;
    if ( !graph_token_start(n->head_tok, x.line, x.column) )
	return false;
    x.end_line = n->end_line;
    x.end_column = n->end_column;
    // A stamp skewed by a pushback (the parser consumed past the terminator and
    // handed the token back) could read before its own start: refuse, never a
    // negative span.
    if ( x.end_line < x.line || (x.end_line == x.line && x.end_column <= x.column) )
	return false;
    return true;
}

static madc::value graph_span_value(const madc::value &node, const GraphExtent &x)
{
    std::map<std::string, madc::value> f = node.as_object();
    std::map<std::string, madc::value> s;
    s["line"] = value((int64_t)x.line);
    s["column"] = value((int64_t)x.column);
    s["end_line"] = value((int64_t)x.end_line);
    s["end_column"] = value((int64_t)x.end_column);
    f["span"] = value::make_object(s);
    return value::make_object(f);
}

static madc::value graph_error_result(const char *why)
{
    std::map<std::string, madc::value> f;
    f["error"] = value(std::string(why));
    return value::make_object(f);
}

// Is `t` a statement-level node under `root` (a TokenFunc IS-A TokenCpnd)? BFS
// the subtree with the L1b descent; at every compound, its `statements` are the
// statement-level nodes. Bounded by the L2 visit cap (a cycle net, never a
// truncation of a legitimate body).
static bool graph_is_statement_level(const TokenBase *root, const TokenBase *t)
{
    std::vector<const TokenBase *> q;
    q.push_back(root);
    for ( size_t qi = 0; qi < q.size() && qi < GRAPH_COLLECT_VISIT_CAP; ++qi )
    {
	const TokenBase *n = q[qi];
	if ( TokenCpnd *c = const_cast<TokenBase *>(n)->as_cpnd_tok() )
	    for ( size_t i = 0; i < c->statements.size(); ++i )
		if ( (const TokenBase *)c->statements[i] == t )
		    return true;
	std::vector<const TokenBase *> kids;
	graph_body_children(n, kids);
	q.insert(q.end(), kids.begin(), kids.end());
    }
    return false;
}

// The TU-own function whose body holds `t` as a statement-level node; NULL if
// `t` is an expression node (or foreign). The position-innermost function is
// tried first (enclosing_func_at), then every TU-own function — a node's
// minting token can sit on a line the enclosing test does not cover.
static TokenFunc *graph_owner_function(parse_tu_state *st, const TokenBase *t)
{
    ::Program &child = *st->child;
    TokenFunc *guess = enclosing_func_at(child, st->display_name, t->line, t->column);
    if ( guess && graph_is_statement_level(guess, t) )
	return guess;
    for ( size_t i = 0; i < child.pending_funcs.size(); ++i )
    {
	TokenFunc *tf = tu_own_function(child.pending_funcs[i], st->display_name);
	if ( tf && tf != guess && graph_is_statement_level(tf, t) )
	    return tf;
    }
    return (TokenFunc *)0;
}
```

- [ ] **Step 2: `graph.span`**

```cpp
// graph.span(handle, id): the node + its exact source extent {line, column,
// end_line, end_column} (0-based byte columns, start inclusive / end exclusive;
// 1-based lines) — the edit verbs' one coordinate source, and the read an agent
// uses to quote a node's text. Routes on the id space: a TU-own function's
// definition, a global's declaration (its recorded TokenDecl), or a statement-
// level body node. Anything else answers {error} — an expression node, a
// method, a macro-headed statement, a node with no stamp — never a guess.
bool internal_program_graph_span(int64_t handle, int64_t id, madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    if ( !graph_id_fresh(st, id) )
    {
	out = graph_stale_result(st, id);
	return true;
    }
    ::Program &child = *st->child;
    GraphExtent x;
    switch ( graph_id_space(id) )
    {
    case GraphIdSpace::Type:
    {
	DataDef *dd = graph_type_from_id_guarded(child, id);
	TokenFunc *tf = dd ? graph_function_token(child, dd) : (TokenFunc *)0;
	if ( !tf || !tu_own_function(tf, st->display_name) )
	{
	    out = graph_error_result("not an editable node: only a TU-own function definition, a global declaration or a statement-level body node has a span");
	    return true;
	}
	if ( !graph_extent_of(tf, x) )
	{
	    out = graph_error_result("no extent recorded for this function (a method, a definition registered together with others, or a macro-headed definition)");
	    return true;
	}
	out = graph_span_value(graph_func_node_value(st, tf), x);
	return true;
    }
    case GraphIdSpace::Global:
    {
	const ::Program::TopDecl *td = graph_global_resolve(child, id);
	if ( !td )
	{
	    out = graph_error_result("unknown global decl-id");
	    return true;
	}
	if ( !td->decl || !graph_extent_of(td->decl, x) )
	{
	    out = graph_error_result("no extent recorded for this global (no declaration statement was retained for it)");
	    return true;
	}
	out = graph_span_value(graph_global_node_value(*td, id), x);
	return true;
    }
    case GraphIdSpace::Body:
    {
	const TokenBase *n = graph_body_resolve(st, id);
	if ( !n )
	{
	    out = graph_error_result("unknown body node id");
	    return true;
	}
	if ( !graph_owner_function(st, n) )
	{
	    out = graph_error_result("not a statement-level node: edit its enclosing statement (graph.children of the block lists the statements)");
	    return true;
	}
	if ( !graph_extent_of(n, x) )
	{
	    out = graph_error_result("no extent recorded for this statement (a macro-headed statement, or a synthesized node)");
	    return true;
	}
	out = graph_span_value(graph_body_node_value(st, n), x);
	return true;
    }
    }
    return true;
}
```

- [ ] **Step 3: `graph.at`**

```cpp
// graph.at(handle, line, column): the editable node whose extent STARTS exactly
// at (line, 0-based column) — how an edit answers "what is the node I just
// wrote": TU-own functions and globals first (top-level definitions), then the
// statement-level nodes of the function enclosing the position (BFS over the
// L1b descent; the FIRST start match wins — a fragment that produced several
// statements answers with its first). Empty {} = nothing starts there.
bool internal_program_graph_at(int64_t handle, int64_t line, int64_t column,
			       madc::value &out)
{
    out = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    ::Program &child = *st->child;
    GraphExtent x;
    for ( size_t i = 0; i < child.pending_funcs.size(); ++i )
    {
	TokenFunc *tf = tu_own_function(child.pending_funcs[i], st->display_name);
	if ( tf && graph_extent_of(tf, x) && x.line == line && x.column == column )
	{
	    out = graph_span_value(graph_func_node_value(st, tf), x);
	    return true;
	}
    }
    for ( size_t i = 0; i < child.top_decls.size(); ++i )
    {
	const ::Program::TopDecl &td = child.top_decls[i];
	if ( td.kind != ::Program::DeclKind::dkGlobalVar || !td.decl )
	    continue;
	if ( graph_extent_of(td.decl, x) && x.line == line && x.column == column )
	{
	    int64_t gid = graph_id_stamp(st, GRAPH_DECL_ID_BASE + (int64_t)i);
	    out = graph_span_value(graph_global_node_value(td, gid), x);
	    return true;
	}
    }
    TokenFunc *encl = enclosing_func_at(child, st->display_name, line, column);
    if ( !encl )
    {
	std::map<std::string, madc::value> empty;
	out = value::make_object(empty);
	return true;
    }
    std::vector<const TokenBase *> q;
    q.push_back(encl);
    for ( size_t qi = 0; qi < q.size() && qi < GRAPH_COLLECT_VISIT_CAP; ++qi )
    {
	const TokenBase *n = q[qi];
	if ( TokenCpnd *c = const_cast<TokenBase *>(n)->as_cpnd_tok() )
	    for ( size_t i = 0; i < c->statements.size(); ++i )
	    {
		const TokenBase *s = c->statements[i];
		if ( graph_extent_of(s, x) && x.line == line && x.column == column )
		{
		    out = graph_span_value(graph_body_node_value(st, s), x);
		    return true;
		}
	    }
	std::vector<const TokenBase *> kids;
	graph_body_children(n, kids);
	q.insert(q.end(), kids.begin(), kids.end());
    }
    std::map<std::string, madc::value> empty;
    out = value::make_object(empty);
    return true;
}
```

---

### Task 4: `parse_refresh_checked` — validate, then swap atomically

**Files:**
- Modify: `src/madc_program.cpp` — after `internal_program_parse_refresh` (5284)

**Interfaces:**
- Produces: `bool internal_program_parse_refresh_checked(::Program &self, int64_t handle, const std::string &source_text, madc::value &out_diags)`; helper `child_error_count(::Program &)`.
- Consumes: `parse_handle_child_init`, `compile_source_child_frontend`,
  `diagnostic_rows_from_child`, `Program::error_nodes`, `Program::diagnostics`.

- [ ] **Step 1: the checked refresh**

```cpp
// Error-severity diagnostics + synthesized error nodes: the count an edit must
// not raise (warnings never gate — the diag_error_count rule).
static size_t child_error_count(::Program &child)
{
    size_t n = child.error_nodes;
    for ( size_t i = 0; i < child.diagnostics.size(); ++i )
	if ( child.diagnostics[i].severity == ::Program::DiagnosticSeverity::error )
	    ++n;
    return n;
}

// The VALIDATED whole-TU refresh (code-graph MCP L3, design §6.6: "validated by
// the same parser/sema before commit … atomic to tree-well-formedness"). Parse +
// compile the candidate text into a FRESH child; if it carries MORE errors than
// the live child, delete it and answer false — the live tree, its ids and the
// caller's buffer are untouched. Otherwise swap it in (the L1b registry clears,
// the generation advances — every prior id is now loudly stale) and answer
// true. out_diags = the candidate's diagnostics rows either way, so a rejected
// edit says why. One parse per attempt; a rejected attempt costs one parse and
// changes nothing.
bool internal_program_parse_refresh_checked(::Program &self, int64_t handle,
					    const std::string &source_text,
					    madc::value &out_diags)
{
    out_diags = value();
    parse_tu_state *st = parse_tu_get(handle);
    if ( !st )
	return false;
    self.clear_diagnostics();
    self.clear_error();
    ::Program *cand = new ::Program(self.engine);
    parse_handle_child_init(*cand);
    compile_source_child_frontend(self, *cand, source_text, st->display_name);
    diagnostic_rows_from_child(*cand, out_diags);
    if ( child_error_count(*cand) > child_error_count(*st->child) )
    {
	delete cand;
	return false;
    }
    delete st->child;
    st->child = cand;
    st->body_nodes.clear();
    st->body_ids.clear();
    ++st->generation;
    return true;
}
```

---

### Task 5: Five-layer plumbing for `graph_span` / `graph_at` / `parse_refresh_checked` + ONE build

**Files:**
- Modify: `src/parser.cpp:376` (forward decls), `1228` (bridges), `include/ns_common.h:252`, `src/ns_madc.cpp:290`, `include/madc/ns_madc:217`

**Interfaces:**
- Produces (dialect): `value &graph_span(value &out, int64_t handle, int64_t id)`,
  `value &graph_at(value &out, int64_t handle, int64_t line, int64_t column)`,
  `bool parse_refresh_checked(value &out_diags, int64_t handle, const char *source)`.

- [ ] **Step 1: parser.cpp forward declarations** (after line 376):

```cpp
// Code-graph MCP L3 (design 2026-09-12): node extents, position lookup and the
// VALIDATED refresh the edit verbs commit through — see madc_program.cpp.
bool internal_program_graph_span(int64_t handle, int64_t id, value &out);
bool internal_program_graph_at(int64_t handle, int64_t line, int64_t column,
			       value &out);
bool internal_program_parse_refresh_checked(::Program &self, int64_t handle,
					    const std::string &source_text,
					    value &out_diags);
```

- [ ] **Step 2: parser.cpp bridges** (after `madc_graph_impact`, line 1228):

```cpp
// Code-graph MCP L3 bridges (design 2026-09-12): same thin-thunk shape.
void *madc_graph_span(void *result, int64_t handle, int64_t id)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_span(handle, id, out);
    return result;
}
void *madc_graph_at(void *result, int64_t handle, int64_t line, int64_t column)
{
    madc::value &out = *(madc::value *)result;
    madc::internal_program_graph_at(handle, line, column, out);
    return result;
}
// The validated refresh: result = diagnostics rows (the candidate's), true =
// the candidate was swapped in. Same active-program discipline as
// madc_parse_refresh.
bool madc_parse_refresh_checked(void *result, int64_t handle, void *source)
{
    std::unique_ptr<Program> owned;
    Program *active = require_runtime_eval_program(owned);
    if ( !active )
	return false;
    madc::value &out = *(madc::value *)result;
    return madc::internal_program_parse_refresh_checked(
	*active, handle, *(const std::string *)source, out);
}
```

- [ ] **Step 3: ns_common.h** (after line 252):

```cpp
// Code-graph MCP L3 (design 2026-09-12): extents + position lookup + the
// validated refresh (result = the candidate's diagnostics rows; true = swapped).
void *madc_graph_span(void *result, int64_t handle, int64_t id);
void *madc_graph_at(void *result, int64_t handle, int64_t line, int64_t column);
bool  madc_parse_refresh_checked(void *result, int64_t handle, void *source);
```

- [ ] **Step 4: ns_madc.cpp** (after line 290):

```cpp
// Code-graph MCP L3 (design 2026-09-12): a node's exact source extent, the node
// starting at a position, and the validated whole-TU refresh (true = the
// candidate replaced the live tree; false = rejected, nothing changed;
// out_diags = the candidate's diagnostics either way).
value &graph_span(value &out, int64_t handle, int64_t id)
	{ madc_graph_span(&out, handle, id); return out; }
value &graph_at(value &out, int64_t handle, int64_t line, int64_t column)
	{ madc_graph_at(&out, handle, line, column); return out; }
bool parse_refresh_checked(value &out_diags, int64_t handle, const char *source)
	{ std::string s = source ? source : "";
	  return madc_parse_refresh_checked(&out_diags, handle, &s); }
```

- [ ] **Step 5: include/madc/ns_madc** (after line 217):

```cpp
    // Code-graph MCP L3 (design 2026-09-12): node editing primitives.
    //   graph_span(handle, id): the node + span {line, column, end_line,
    //     end_column} — 1-based lines, 0-based byte columns, start inclusive /
    //     end exclusive — for a TU-own function definition, a global
    //     declaration or a statement-level body node; {error} otherwise (an
    //     expression node has no extent of its own: edit its statement).
    //   graph_at(handle, line, column): the editable node whose span STARTS
    //     there (a function, a global, or a statement of the enclosing
    //     function); {} = none.
    //   parse_refresh_checked(out_diags, handle, source): parse `source` into
    //     a fresh tree and SWAP it in only if it adds no error over the live
    //     one (true); false = rejected, the live tree and every id untouched.
    //     out_diags = the candidate's diagnostics rows either way. A swap
    //     advances the handle's generation: every earlier id is refused as
    //     stale by every graph verb — re-query (or read the edit result's node).
    value &graph_span(value &out, int64_t handle, int64_t id);
    value &graph_at(value &out, int64_t handle, int64_t line, int64_t column);
    bool parse_refresh_checked(value &out_diags, int64_t handle, const char *source);
```

- [ ] **Step 6: ONE build, then the L1/L1b/L2 regression tests**

Run (foreground, never backgrounded-and-yielded): `make -C src ../bin/madc`
Expected: zero warnings, exit 0. Then, each capped (`ulimit -t 120; timeout
180`): `bin/madc tests/testgraphaccessors.mad`, `tests/testgraphbody.mad`,
`tests/testgraphedges.mad`, `tests/testmadcide_serve_graph.mad <
tests/testmadcide_serve_graph.input`, `tests/testparsehandle.mad` — every
`.expect` line present, stderr empty where `.expect_quiet` exists. (Generation
0 ids are bit-identical to before; the stamps are additive.)

- [ ] **Step 7: Commit A**

`git add include/tokens.h include/madc.h src/parser.cpp src/madc_program.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc`

```
feat(graph-mcp): L3 engine — parser extent stamps, generation-stamped ids, graph.span/graph.at, validated refresh

Hypothesis: a node edit needs (a) the exact source extent of a statement/
  definition, (b) ids that cannot silently resolve against a re-parsed tree,
  (c) a refresh that validates before it commits. None existed: TokenCpnd had
  only end_line, ids were snapshot-positional with no staleness signal, and
  parse_refresh swapped unconditionally.
Layer: graph verb (madc_program.cpp) -> parse handle -> PARSER. Extents are
  stamped in Program::parseStatement/parseCompound/parseFunction — the parser is
  the one layer that knows where a statement ends; a token-stream bracket walk
  in the graph layer would re-derive grammar (a shim). Staleness lives in the
  id itself (the handle's generation) — the deepest place an id can carry it.
Searched: "statement/declaration end position or extent" -> only
  TokenCpnd::end_line (parseCompound) and TokenFunc's copy; "byte position on a
  token" -> TokenBase::pos never written or read (retired here); "validated
  re-parse / try-parse" -> none (parse_refresh is unconditional; source_emit
  parses a throwaway child but never swaps); "id staleness / generation" -> none.
Oracle: n/a — IDE machinery (no codegen); tests/testgraphedit.mad (Task 8) is
  the oracle: hand-computed spans of a fixed fixture, reject-keeps-tree,
  accept-advances-generation.
```

---

### Task 6: Change log — replay applies `splice` records only

**Files:**
- Modify: `tools/texteditor/editor_events.inc:330-338` (`clog_replay`)

- [ ] **Step 1:** change the `else` arm to an explicit kind test so a `nodeop`
record (Task 7) — or any future non-splice kind — is a replay no-op rather than
a `{at:0, del:0, ins:""}` pseudo-splice:

```cpp
	if ( rec["kind"] == "checkpoint" )
	    ui::text_load(w, scr, rec["text"].c_str());
	else if ( rec["kind"] == "splice" )
	{
	    long at = rec["at"].as_integer();
	    long del = rec["del"].as_integer();
	    var ins = rec["ins"];
	    ui::text_replace(w, scr, at, del, ins.c_str());
	}
	// any other kind (a node-op annotation, L3) changes no text: skipped
```

Update the header comment (lines 234-236: "replay converts it once and ignores
an unknown kind") — it now does what it says.

---

### Task 7: The seat — enum-dispatched `graph.*` family, tier gate, `graph_edit_apply`

**Files:**
- Modify: `tools/madcide/madcide_enums.inc` (after `cmd_min_tier`, 623)
- Modify: `tools/madcide/madcide_mcp.inc:55-150` (descriptors), `156-179` (mcp_tools), `229-301` (graph_call), `375` (call site)

**Interfaces:**
- Produces: `enum graph_verb`, `enum graph_where`, `graph_where_of(name)`,
  `graph_min_tier(code)`, `graph_verb_of(name)`, `graph_call(out, S, doc,
  self_id, nm, params)`, `graph_edit_apply(out, S, doc, self_id, gv, args)`.
- Consumes: `eff_tier`, `tier_name`, `tierEDITOR/tierOBSERVER`, `ensure_phandle`,
  `line_of`, `line_start_of`, `replaybuf_of`, `edit_checkpoint`, `clamped_caret`,
  `clog_append`, `clog_head`, `ed_text_erase`, `ed_text_insert`, `refresh_spans`,
  `reparse_buffer`, `broadcast_events`, `madc::graph_span/graph_at/parse_refresh_checked`.

- [ ] **Step 1: the enums + policy (madcide_enums.inc, after line 623)**

```cpp
// ---- the code-graph MCP tool family (design 2026-09-12) --------------------
// The graph.* verbs as CODES (enums-not-strings): the wire name converts ONCE
// (graph_verb_of, over the descriptor table — one list of names) and every
// dispatch / policy is a switch on the code. gvNONE = unknown verb (a refusal).
enum graph_verb : unsigned char {
    gvNONE = 0,
    gvSYMBOLS, gvDEFINITION, gvNODE, gvTYPE_OF, gvMEMBERS, gvBASES, gvENCLOSING,
    gvBODY, gvCHILDREN,
    gvCALLEES, gvCALLERS, gvREFERENCES, gvSEARCH, gvIMPACT,
    gvSPAN, gvAT, gvINSERT, gvREPLACE, gvDELETE
};

// graph.insert's placement relative to its target; text only on the wire.
enum graph_where : unsigned char { gwNONE = 0, gwBEFORE, gwAFTER };

long graph_where_of(const char *name)
{
    if ( strcmp(name, "before") == 0 ) return gwBEFORE;
    if ( strcmp(name, "after") == 0 )  return gwAFTER;
    return gwNONE;
}

// The minimum tier a graph verb requires — the SAME ladder cmd_min_tier is
// (V6a tiers): reads and navigation observe; the three mutations are text
// mutation, so tierEDITOR (an MCP client is born observer and promoted by
// clienttier — design §2.5).
long graph_min_tier(long code)
{
    switch ( code )
    {
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:
	return tierEDITOR;
    }
    return tierOBSERVER;
}
```

- [ ] **Step 2: descriptors carry their code (madcide_mcp.inc)**

Give every existing `tN` row a `"code": gvX` field (t0 `gvSYMBOLS` … t13
`gvIMPACT`) and append, after `t13`:

```cpp
    // Code-graph MCP L3 (design 2026-09-12): extents, position lookup and the
    // three mutations. `src` is a SOURCE FRAGMENT (the parser validates it —
    // no builder DSL); `where` is before|after the target.
    var srcprop = { "type": "string", "description": "source text: the statement(s) or definition to place (validated by the parser before commit)" };
    var whereprop = { "type": "string", "description": "before | after (relative to the target node)" };
    var spanargs = idargs;
    var atargs = posargs;
    var insprops = { "id": idprop, "where": whereprop, "src": srcprop };
    var insargs = { "type": "object", "properties": insprops };
    var repprops = { "id": idprop, "src": srcprop };
    var repargs = { "type": "object", "properties": repprops };
    var t14 = { "name": "graph.span", "code": gvSPAN,
	"description": "a node's exact source extent {span:{line,column,end_line,end_column}} (statement / function / global)",
	"inputSchema": spanargs };
    tools[] = t14;
    var t15 = { "name": "graph.at", "code": gvAT,
	"description": "the editable node whose span starts at a position (1-based line, 0-based column)",
	"inputSchema": atargs };
    tools[] = t15;
    var t16 = { "name": "graph.insert", "code": gvINSERT,
	"description": "insert src before|after a node (editor tier; validated, logged, undoable)",
	"inputSchema": insargs };
    tools[] = t16;
    var t17 = { "name": "graph.replace", "code": gvREPLACE,
	"description": "replace a node's source with src (editor tier; validated, logged, undoable)",
	"inputSchema": repargs };
    tools[] = t17;
    var t18 = { "name": "graph.delete", "code": gvDELETE,
	"description": "delete a node (editor tier; validated, logged, undoable)",
	"inputSchema": spanargs };
    tools[] = t18;
```

Then, after `graph_tool_descriptors`, the ONE name→code converter:

```cpp
// The wire name -> verb code, over the descriptor table (ONE list of names:
// what tools/list advertises is exactly what dispatch knows). gvNONE = unknown.
long graph_verb_of(const char *name)
{
    var nm = name;			// own the text (ring discipline)
    var tools;
    graph_tool_descriptors(tools);
    for ( var t : tools )
	if ( t["name"] == nm )
	    return t["code"].as_integer();
    return gvNONE;
}
```

In `mcp_tools` (174-177) project the wire fields only:

```cpp
    var gtools;
    graph_tool_descriptors(gtools);
    for ( var gt : gtools )
    {
	var tool = { "name": gt["name"], "description": gt["description"],
		     "inputSchema": gt["inputSchema"] };
	tools[] = tool;
    }
```

- [ ] **Step 3: `graph_edit_apply` (madcide_mcp.inc, before `graph_call`)**

```cpp
// The leading whitespace of the buffer line holding byte `off` — the
// indentation a fragment placed on a new line inherits.
void line_indent_at(var &out, var &t, long w, long doc, long line)
{
    long ls = line_start_of(w, doc, line);
    long n = strlen(t.c_str());
    long i = ls;
    while ( i < n && (t.c_str()[i] == ' ' || t.c_str()[i] == '\t') )
	i = i + 1;
    out = perl::substr(t, ls, i - ls);
}

// Does [at, end) own its line(s): only whitespace before it on its first line
// and after it on its last? Then a delete takes the whole line(s).
bool region_owns_lines(var &t, long at, long end)
{
    long n = strlen(t.c_str());
    long i = at - 1;
    while ( i >= 0 && t.c_str()[i] != '\n' )
    {
	if ( t.c_str()[i] != ' ' && t.c_str()[i] != '\t' )
	    return false;
	i = i - 1;
    }
    long j = end;
    while ( j < n && t.c_str()[j] != '\n' )
    {
	if ( t.c_str()[j] != ' ' && t.c_str()[j] != '\t' )
	    return false;
	j = j + 1;
    }
    return true;
}

// ONE node-op (code-graph MCP L3, design §6.6): span -> splice -> candidate ->
// VALIDATED swap -> commit through the ONE text-mutation owner. Order matters:
//   1. the target's extent from the tree (graph.span) — never an agent offset
//   2. the splice {at, del, ins} for the op (replace / delete / insert)
//   3. the candidate = the live buffer with that splice, built in the doc's
//      scratch buffer through the engine's text_replace (no string surgery)
//   4. parse_refresh_checked(candidate): rejected -> {ok:false, diagnostics},
//      nothing changed (not the tree, not the buffer, not the log)
//   5. accepted -> one undo checkpoint; the node-op RECORD; the splice(s) via
//      ed_text_erase / ed_text_insert (anchors shift, journal_splice logs);
//      spans refresh from the already-swapped handle; the hub fan-out
//   6. the answer: the node now at the edit position (graph.at) + gen + seq
// The checked refresh may yield (Stage-2 slices): if the buffer changed under
// it, abort — re-sync the handle to the live text and refuse (the agent
// retries against fresh ids).
void graph_edit_apply(var &out, IdeSession &S, long doc, long self_id, long gv, var &args)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    long id = args["id"].as_integer();
    var srcv = args["src"];
    var src = srcv.is_null() ? "" : srcv;
    long where = gwNONE;
    if ( gv == gvINSERT )
    {
	var wv = args["where"];
	where = wv.is_null() ? gwNONE : graph_where_of(wv.c_str());
	if ( where == gwNONE )
	{
	    out = { "ok": false, "error": "graph.insert needs where = before | after" };
	    return;
	}
    }
    if ( gv != gvDELETE && strlen(src.c_str()) == 0 )
    {
	out = { "ok": false, "error": "src is empty (use graph.delete to remove a node)" };
	return;
    }
    // 1. the extent
    var sp;
    madc::graph_span(sp, h, id);
    if ( !sp["error"].is_null() )
    {
	out = { "ok": false, "error": sp["error"] };
	return;
    }
    var span = sp["span"];
    long l0 = span["line"].as_integer();
    long c0 = span["column"].as_integer();
    long l1 = span["end_line"].as_integer();
    long c1 = span["end_column"].as_integer();
    var t;
    ui::text(t, w, adoc);
    long size = strlen(t.c_str());
    long at = line_start_of(w, adoc, l0) + c0;
    long end = line_start_of(w, adoc, l1) + c1;
    // 2. the splice
    long del = 0;
    var ins = "";
    long new_line = l0;
    long new_col = c0;
    var indent;
    line_indent_at(indent, t, w, adoc, l0);
    long indent_len = strlen(indent.c_str());
    switch ( gv )
    {
    case gvREPLACE:
	del = end - at;
	ins = src;
	break;
    case gvDELETE:
	if ( region_owns_lines(t, at, end) )
	{
	    at = line_start_of(w, adoc, l0);
	    long nl = end;
	    while ( nl < size && t.c_str()[nl] != '\n' )
		nl = nl + 1;
	    end = nl < size ? nl + 1 : size;
	}
	del = end - at;
	break;
    case gvINSERT:
	if ( where == gwBEFORE )
	    at = line_start_of(w, adoc, l0);
	else
	{
	    long nl = end;
	    while ( nl < size && t.c_str()[nl] != '\n' )
		nl = nl + 1;
	    at = nl < size ? nl + 1 : size;
	    new_line = l1 + 1;
	}
	ins = format("{}{}\n", indent, src);
	new_col = indent_len;
	break;
    }
    // 3. the candidate, through the engine's text_replace on the scratch buffer
    long scr = replaybuf_of(w, adoc);
    ui::text_load(w, scr, t.c_str());
    ui::text_replace(w, scr, at, del, ins.c_str());
    var cand;
    ui::text(cand, w, scr);
    // 4. validate + swap (atomic)
    var diags;
    bool accepted = madc::parse_refresh_checked(diags, h, cand.c_str());
    if ( !accepted )
    {
	out = { "ok": false, "error": "edit rejected: the result introduces a parse/semantic error (nothing changed)",
		"diagnostics": diags };
	return;
    }
    var t2;
    ui::text(t2, w, adoc);
    if ( !(t2 == t) )
    {
	reparse_buffer(w, es, adoc);	// re-sync the handle to the live text
	out = { "ok": false, "error": "buffer changed while validating: nothing applied, re-query" };
	return;
    }
    // 5. commit
    long head_before = clog_head(w, adoc);
    edit_checkpoint(w, es, adoc, clamped_caret(w, es, adoc));
    var verb = gv == gvREPLACE ? "graph.replace" : (gv == gvDELETE ? "graph.delete" : "graph.insert");
    var rec = { "kind": "nodeop", "es": es, "doc": adoc, "verb": verb,
		"target": sp, "src": src };
    clog_append(w, adoc, rec);
    if ( del > 0 )
	ed_text_erase(w, es, adoc, at, del);
    if ( strlen(ins.c_str()) > 0 )
	ed_text_insert(w, es, adoc, at, ins.c_str());
    refresh_spans(w, es, adoc, h);
    broadcast_events(w, adoc, head_before, self_id);
    // 6. the answer
    var node = {};
    if ( gv != gvDELETE )
	madc::graph_at(node, h, new_line, new_col);
    long seq = clog_head(w, adoc);
    out = { "ok": true, "node": node, "seq": seq, "splice_at": at, "splice_del": del };
}
```

(The `?:` in `var verb = …` nests two ternaries — if the dialect rejects the
nested form, use an `if`/`else if` chain assigning `verb`. `var src = … ? "" :
srcv` likewise: split into `var src; if (srcv.is_null()) src = ""; else src =
srcv;` when needed — confirm-before-build CB-4.)

- [ ] **Step 4: `graph_call` — enum dispatch, tier gate, `self_id`**

Replace the `strcmp` ladder (238-286) so the function reads:

```cpp
void graph_call(var &out, IdeSession &S, long doc, long self_id, var &nm, var &params)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    var args = params["arguments"];
    if ( args.is_null() )
	args = {};
    long gv = graph_verb_of(nm.c_str());
    var content = {};
    if ( gv == gvNONE )
    {
	var why = format("unknown graph verb '{}'", nm);
	var item = { "type": "text", "text": why };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    // Permission gate (V6a tiers): the SAME refusal shape api_run gives a
    // registry command above the caller's tier — prose, never a silent run.
    long my_tier = eff_tier(self_id);
    long need = graph_min_tier(gv);
    if ( my_tier < need )
    {
	var why = format("'{}' requires {} (you are {})", nm, tier_name(need), tier_name(my_tier));
	var item = { "type": "text", "text": why };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    var snap;
    switch ( gv )
    {
    case gvSYMBOLS:    madc::graph_symbols(snap, h); break;
    case gvNODE:       madc::graph_node(snap, h, args["id"].as_integer()); break;
    case gvTYPE_OF:    madc::graph_type_of(snap, h, args["id"].as_integer()); break;
    case gvDEFINITION: madc::graph_definition(snap, h, args["name"].c_str()); break;
    case gvMEMBERS:    madc::graph_members(snap, h, args["id"].as_integer()); break;
    case gvBASES:      madc::graph_bases(snap, h, args["id"].as_integer()); break;
    case gvENCLOSING:  madc::graph_enclosing(snap, h, args["line"].as_integer(), args["column"].as_integer()); break;
    case gvBODY:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? -1 : dv.as_integer();
	madc::graph_body(snap, h, args["func"].as_integer(), d);
	break;
    }
    case gvCHILDREN:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? 1 : dv.as_integer();
	madc::graph_children(snap, h, args["id"].as_integer(), d);
	break;
    }
    case gvCALLEES:    madc::graph_callees(snap, h, args["func"].as_integer()); break;
    case gvCALLERS:    madc::graph_callers(snap, h, args["func"].as_integer()); break;
    case gvREFERENCES: madc::graph_references(snap, h, args["func"].as_integer()); break;
    case gvSEARCH:     madc::graph_search(snap, h, args["kind"].c_str(), args["name"].c_str()); break;
    case gvIMPACT:     madc::graph_impact(snap, h, args["id"].as_integer()); break;
    case gvSPAN:       madc::graph_span(snap, h, args["id"].as_integer()); break;
    case gvAT:         madc::graph_at(snap, h, args["line"].as_integer(), args["column"].as_integer()); break;
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:     graph_edit_apply(snap, S, doc, self_id, gv, args); break;
    }
    // A result that carries an error (a stale id, a non-editable target, a
    // rejected edit) is isError content — an agent must notice, never read
    // an empty node list as "nothing there".
    bool iserr = !snap["error"].is_null();
    var js = js::stringify(snap);		// copy the ring const char*
    var txt = js;
    var item = { "type": "text", "text": txt };
    content[] = item;
    out = { "content": content, "isError": iserr };
}
```

Update the call site (375): `graph_call(result, S, doc, self_id, nm, params);`.
Update the L1 header comment above `graph_call` to describe the enum dispatch
and the tier gate.

---

### Task 8: Tests

**Files:**
- Create: `tests/testgraphedit.mad`, `tests/testgraphedit.expect`, `tests/testgraphedit.expect_quiet`
- Create: `tests/testmadcide_serve_edit.mad`, `tests/testmadcide_serve_edit.expect`

- [ ] **Step 1: the engine test — hand-computed spans over a fixed fixture**

Fixture (line → text; every span below is computed from these exact bytes):

```
1  long g_total = 0;                          span {1,0,1,17}
2  long helper(long x) { return x * 2; }      helper {2,0,2,37}; its return statement {2,22,2,35}
3  long run(long n) {                         run {3,0,8,1}
4      long t = 0;                            {4,4,4,15}
5      for (long i = 0; i < n; i = i + 1)     the for statement {5,4,6,36}
6          t = t + helper(i) + g_total;
7      return t;                              {7,4,7,13}
8  }
```

`tests/testgraphedit.mad`:

```c
// Code-graph MCP L3: extents (graph_span / graph_at) from the parser's stamps,
// generation-stamped ids (a stale id is a LOUD refusal), and the validated
// refresh (reject leaves the tree; accept swaps it and advances the generation).
// Every expected span is hand-computed from the fixture bytes below (1-based
// lines, 0-based byte columns, end exclusive). Compile-NEVER-execute
// (.expect_quiet proves it).
bool span_is(var &r, long l0, long c0, long l1, long c1)
{
    var s = r["span"];
    if ( s.is_null() )
	return false;
    return s["line"].as_integer() == l0 && s["column"].as_integer() == c0
	&& s["end_line"].as_integer() == l1 && s["end_column"].as_integer() == c1;
}

int main()
{
    var src = "long g_total = 0;\n"
	      "long helper(long x) { return x * 2; }\n"
	      "long run(long n) {\n"
	      "    long t = 0;\n"
	      "    for (long i = 0; i < n; i = i + 1)\n"
	      "        t = t + helper(i) + g_total;\n"
	      "    return t;\n"
	      "}\n";
    long h = madc::parse_open(src.c_str(), "graphedit.mad");
    println("opened: {}", h > 0 ? 1 : 0);

    var sy;
    madc::graph_symbols(sy, h);
    long id_helper = 0, id_run = 0;
    for ( var n : sy["nodes"] )
    {
	if ( n["name"] == "helper" ) id_helper = n["id"].as_integer();
	if ( n["name"] == "run" )    id_run    = n["id"].as_integer();
    }
    var sh;
    madc::graph_span(sh, h, id_helper);
    println("helper span: {}", span_is(sh, 2, 0, 2, 37) ? 1 : 0);
    var sr;
    madc::graph_span(sr, h, id_run);
    println("run span: {}", span_is(sr, 3, 0, 8, 1) ? 1 : 0);

    var gs;
    madc::graph_search(gs, h, "Global", "g_total");
    long id_global = 0;
    for ( var n : gs["nodes"] )
	if ( n["name"] == "g_total" ) id_global = n["id"].as_integer();
    var sg;
    madc::graph_span(sg, h, id_global);
    println("global span: {}", span_is(sg, 1, 0, 1, 17) ? 1 : 0);

    // Statement-level nodes of run, found by their minting line via graph_body.
    var body;
    madc::graph_body(body, h, id_run, -1);
    long id_decl = 0, id_for = 0, id_ret = 0, id_leaf = 0;
    for ( var n : body["nodes"] )
    {
	long ln = n["line"].as_integer();
	var k = n["kind"];
	if ( ln == 4 && id_decl == 0 && !(k == "NameRef") && !(k == "Literal") ) id_decl = n["id"].as_integer();
	if ( ln == 5 && id_for == 0 && !(k == "NameRef") && !(k == "Literal") && !(k == "Expr") ) id_for = n["id"].as_integer();
	if ( ln == 7 && id_ret == 0 && !(k == "NameRef") ) id_ret = n["id"].as_integer();
	if ( ln == 7 && k == "NameRef" ) id_leaf = n["id"].as_integer();
    }
    var sd;
    madc::graph_span(sd, h, id_decl);
    println("decl stmt span: {}", span_is(sd, 4, 4, 4, 15) ? 1 : 0);
    var sf;
    madc::graph_span(sf, h, id_for);
    println("for stmt span: {}", span_is(sf, 5, 4, 6, 36) ? 1 : 0);
    var st;
    madc::graph_span(st, h, id_ret);
    println("return stmt span: {}", span_is(st, 7, 4, 7, 13) ? 1 : 0);
    var sl;
    madc::graph_span(sl, h, id_leaf);
    println("leaf has no span: {}", sl["error"].is_null() ? 0 : 1);

    // graph_at round-trips a statement and a function by their span START.
    var at1;
    madc::graph_at(at1, h, 7, 4);
    println("at round-trip stmt: {}", at1["id"].as_integer() == id_ret ? 1 : 0);
    var at2;
    madc::graph_at(at2, h, 2, 0);
    println("at round-trip func: {}", at2["id"].as_integer() == id_helper ? 1 : 0);

    // REJECT: a broken candidate answers false, carries diagnostics, and the
    // live tree (and its ids) are untouched.
    var bad = "long g_total = 0;\n"
	      "long helper(long x) { return x * 2; }\n"
	      "long run(long n) {\n"
	      "    long t = 0;\n"
	      "    for (long i = 0; i < n; i = i + 1)\n"
	      "        t = t + helper(i) + g_total;\n"
	      "    return t +;\n"
	      "}\n";
    var d1;
    bool ok1 = madc::parse_refresh_checked(d1, h, bad.c_str());
    println("reject returns false: {}", ok1 ? 0 : 1);
    println("reject has diagnostics: {}", php::count(d1) >= 1 ? 1 : 0);
    var sh2;
    madc::graph_span(sh2, h, id_helper);
    println("reject keeps handle: {}", span_is(sh2, 2, 0, 2, 37) ? 1 : 0);

    // ACCEPT: the return statement becomes `return t + 1;` — the swap advances
    // the generation, every old id is refused as stale, fresh ids work.
    var good = "long g_total = 0;\n"
	       "long helper(long x) { return x * 2; }\n"
	       "long run(long n) {\n"
	       "    long t = 0;\n"
	       "    for (long i = 0; i < n; i = i + 1)\n"
	       "        t = t + helper(i) + g_total;\n"
	       "    return t + 1;\n"
	       "}\n";
    var d2;
    bool ok2 = madc::parse_refresh_checked(d2, h, good.c_str());
    println("accept returns true: {}", ok2 ? 1 : 0);
    var stale;
    madc::graph_span(stale, h, id_helper);
    println("old id stale: {}", (!stale["stale"].is_null() && stale["stale"].as_boolean()) ? 1 : 0);
    var sy2;
    madc::graph_symbols(sy2, h);
    long id_helper2 = 0;
    for ( var n : sy2["nodes"] )
	if ( n["name"] == "helper" ) id_helper2 = n["id"].as_integer();
    println("ids re-minted: {}", (id_helper2 != 0 && id_helper2 != id_helper) ? 1 : 0);
    var at3;
    madc::graph_at(at3, h, 7, 4);
    println("new return span: {}", span_is(at3, 7, 4, 7, 17) ? 1 : 0);
    var sh3;
    madc::graph_span(sh3, h, id_helper2);
    println("fresh id resolves: {}", span_is(sh3, 2, 0, 2, 37) ? 1 : 0);

    madc::parse_close(h);
    return 0;
}
```

`tests/testgraphedit.expect`:

```
opened: 1
helper span: 1
run span: 1
global span: 1
decl stmt span: 1
for stmt span: 1
return stmt span: 1
leaf has no span: 1
at round-trip stmt: 1
at round-trip func: 1
reject returns false: 1
reject has diagnostics: 1
reject keeps handle: 1
accept returns true: 1
old id stale: 1
ids re-minted: 1
new return span: 1
fresh id resolves: 1
```

`tests/testgraphedit.expect_quiet`: one line, `stderr must stay empty (the fixture is compiled, never executed)`.

- [ ] **Step 2: run it** — `( ulimit -t 120; timeout 180 bin/madc tests/testgraphedit.mad )`:
rc 0, empty stderr, every `.expect` line. A span mismatch is diagnosed by
printing the actual span next to the expected in a `tmp/` copy of the test —
never by loosening the expectation: the fixture bytes are the oracle (CB-1/CB-2).

- [ ] **Step 3: the seat test — the five tools end to end**

`tests/testmadcide_serve_edit.mad` (harness = `testmadcide_serve_graph.mad`'s
in-process `mcp_handle` drive; `self_id` −1 = the operator, owner tier):

```c
// Code-graph MCP L3 over the SEAT: graph.span / graph.at / graph.replace /
// graph.insert / graph.delete through mcp_handle (the deployed --mcp path). A
// mutation is validated first, applied through the ONE text-mutation owner,
// logged as a nodeop record ahead of its splices, undoable in one step, and
// answers with the NEW node; a stale id and a rejected edit are isError; the
// tier policy is data (graph_min_tier).
#include "../tools/texteditor/lined_core.inc"
#include "../tools/texteditor/editor_events.inc"
#include "../tools/madcide/madcide_core.inc"
#include "../tools/madcide/madcide_client.inc"
#include "../tools/madcide/madcide_api.inc"
#include "../tools/madcide/madcide_mcp.inc"

// One tools/call through the seat; `snap` = the parsed structured content,
// `iserr` = the envelope's isError.
bool call_tool(var &snap, bool &iserr, IdeSession &S, long doc, long id, const char *name, var &args)
{
    var params = { "name": name, "arguments": args };
    var req = { "jsonrpc": "2.0", "id": id, "method": "tools/call", "params": params };
    var resp;
    mcp_handle(S, doc, -1, req, resp);
    var result = resp["result"];
    iserr = result["isError"].as_boolean();
    var content = result["content"];
    var item0 = content[0];
    var txt = item0["text"];
    return js::parse(snap, txt.c_str());
}

void buffer_line(var &out, long w, long doc, long line)
{
    var t;
    ui::text(t, w, doc);
    var lines;
    php::explode(lines, "\n", t.c_str());
    out = lines[line - 1];
}

int main()
{
    const char *path = "madc_ide_graph_edit.mad";
    php::file_put_contents(path,
	"long add(long a, long b) {\n"
	"    return a + b;\n"
	"}\n"
	"long run(long n) {\n"
	"    long total = 0;\n"
	"    total = add(total, n);\n"
	"    return total;\n"
	"}\n");
    IdeSession S;
    long doc = S.open(path, false);
    if ( doc <= 0 )
    {
	println(stderr, "madcide: cannot read {}", path);
	php::unlink(path);
	return 1;
    }
    S.terminal(false);
    long w = S.world();
    long es = S.es;

    // tools/list advertises the five L3 tools (and no `code` leaks to the wire).
    var lreq = { "jsonrpc": "2.0", "id": 1, "method": "tools/list" };
    var lresp;
    mcp_handle(S, doc, -1, lreq, lresp);
    var ljs = js::stringify(lresp);
    var ltxt = ljs;
    println("{}", ltxt);
    println("code leaks: {}", ltxt.index("\"code\"") >= 0 ? 1 : 0);

    // The return statement of add: graph.at(2, 4).
    var noargs = {};
    var atargs = { "line": 2, "column": 4 };
    var ret;
    bool err;
    call_tool(ret, err, S, doc, 2, "graph.at", atargs);
    long rid = ret["id"].as_integer();
    println("at found statement: {}", rid != 0 ? 1 : 0);

    // replace: validated, applied, logged (nodeop + erase + insert = 3 records).
    long head0 = clog_head(w, doc);
    var repargs = { "id": rid, "src": "return a + b + 1;" };
    var rep;
    call_tool(rep, err, S, doc, 3, "graph.replace", repargs);
    println("replace ok: {}", (!err && rep["ok"].as_boolean()) ? 1 : 0);
    var l2;
    buffer_line(l2, w, doc, 2);
    println("replace applied: {}", l2 == "    return a + b + 1;" ? 1 : 0);
    println("log records: {}", clog_head(w, doc) - head0);
    var evs;
    events_since(evs, w, doc, head0);
    var ev0 = evs[0];
    println("nodeop logged: {}", ev0["kind"] == "nodeop" ? 1 : 0);
    println("nodeop before splice: {}", evs[1]["kind"] == "splice" ? 1 : 0);
    var rnode = rep["node"];
    println("replace answers node: {}", rnode["id"].as_integer() != 0 ? 1 : 0);

    // The OLD id is stale — and stale is an isError answer.
    var spargs = { "id": rid };
    var stale;
    call_tool(stale, err, S, doc, 4, "graph.span", spargs);
    println("stale is error: {}", (err && !stale["stale"].is_null()) ? 1 : 0);

    // insert after the (new) return statement: a new line, same indentation.
    long rid2 = rnode["id"].as_integer();
    var insargs = { "id": rid2, "where": "after", "src": "long extra = 0;" };
    var ins;
    call_tool(ins, err, S, doc, 5, "graph.insert", insargs);
    var l3;
    buffer_line(l3, w, doc, 3);
    println("insert after applied: {}", (!err && l3 == "    long extra = 0;") ? 1 : 0);

    // delete it again: the whole line goes.
    var inode = ins["node"];
    var delargs = { "id": inode["id"].as_integer() };
    var del;
    call_tool(del, err, S, doc, 6, "graph.delete", delargs);
    var l3b;
    buffer_line(l3b, w, doc, 3);
    println("delete applied: {}", (!err && l3b == "}") ? 1 : 0);

    // A rejected edit: isError, the buffer untouched, the handle still fresh.
    var at2args = { "line": 2, "column": 4 };
    var ret2;
    call_tool(ret2, err, S, doc, 7, "graph.at", at2args);
    var badargs = { "id": ret2["id"].as_integer(), "src": "return a + ;" };
    var bad;
    call_tool(bad, err, S, doc, 8, "graph.replace", badargs);
    var l2b;
    buffer_line(l2b, w, doc, 2);
    println("reject is error: {}", (err && !bad["ok"].as_boolean()) ? 1 : 0);
    println("reject leaves buffer: {}", l2b == "    return a + b + 1;" ? 1 : 0);
    println("reject has diagnostics: {}", php::count(bad["diagnostics"]) >= 1 ? 1 : 0);

    // One undo step per node-op: the delete comes back whole.
    undo_edit(w, es, doc);
    var l3c;
    buffer_line(l3c, w, doc, 3);
    println("undo restores: {}", l3c == "    long extra = 0;" ? 1 : 0);

    // The tier policy is data.
    println("replace needs editor: {}", graph_min_tier(gvREPLACE) == tierEDITOR ? 1 : 0);
    println("span is observer: {}", graph_min_tier(gvSPAN) == tierOBSERVER ? 1 : 0);
    println("unknown where refused: {}", graph_where_of("sideways") == gwNONE ? 1 : 0);

    php::unlink(path);
    return 0;
}
```

`tests/testmadcide_serve_edit.expect`:

```
"name":"graph.span"
"name":"graph.at"
"name":"graph.insert"
"name":"graph.replace"
"name":"graph.delete"
code leaks: 0
at found statement: 1
replace ok: 1
replace applied: 1
log records: 3
nodeop logged: 1
nodeop before splice: 1
replace answers node: 1
stale is error: 1
insert after applied: 1
delete applied: 1
reject is error: 1
reject leaves buffer: 1
reject has diagnostics: 1
undo restores: 1
replace needs editor: 1
span is observer: 1
unknown where refused: 1
```

- [ ] **Step 4: run the seat test + the neighbours**

`( ulimit -t 120; timeout 180 bin/madc tests/testmadcide_serve_edit.mad )` — rc 0,
every `.expect` line. Then re-run `testmadcide_serve_graph` (with its `.input`),
`testmadcide_serve_mcp`, `testmadcide_changelog` (Task 6's replay change),
`testmadcide_serve_tiers` — unregressed.

- [ ] **Step 5: Commit B**

`git add tools/texteditor/editor_events.inc tools/madcide/madcide_enums.inc tools/madcide/madcide_mcp.inc tests/testgraphedit.* tests/testmadcide_serve_edit.*`

```
feat(graph-mcp): L3 seat — enum-dispatched graph.* family, tier gate, graph.insert/replace/delete + graph.span/graph.at, nodeop log record, tests
```

---

### Task 9: Design doc — settle §6.3 / §6.6 / §10

**Files:**
- Modify: `docs/plans/2026-09-12-ast-graph-mcp-for-agents.md` §6.3 (194-204), §6.6 (233-242), §10 (288-295)

- [ ] **Step 1:** In §6.6 replace the last sentence ("Node specs are supplied in
a structural form … settled in the L3 writing-plans pass") with: "**Settled
(L3 plan, 2026-09-12):** node specs are SOURCE FRAGMENTS validated by the same
parser through a whole-TU checked refresh (`parse_refresh_checked` — parse into
a fresh child, swap only if no error is introduced: atomic); the splice
boundaries are the target's PARSER-STAMPED extent (`TokenBase::head_tok` /
`end_line` / `end_column`, `graph.span`); at statement granularity the buffer
text IS the tree's render (§9), so nothing is hand-patched. The mutation flows
through madcide's one text-mutation owner (`ed_text_insert`/`ed_text_erase`)
after one undo checkpoint; the change log gets a `nodeop` record ahead of the
splices (the second record kind beside `splice`/`checkpoint`, client-server
§2.4); the tier gate is `graph_min_tier` (mutations = editor)."

- [ ] **Step 2:** In §6.3 after "Prefer a persistent/content-derived id over a
positional one" add: "**L3 v1:** every id carries the handle's parse generation
(bits 40..60); a refresh advances it and every verb refuses a stale id with
`{error, stale:true}`; the edit result names the edited node's new id
(`graph.at` at the splice position). Content-derived ids that survive edits
elsewhere remain the GumTree increment over the same `graph_id_stamp` seam."

- [ ] **Step 3:** In §10 mark "Node-spec form for L3 (builder DSL vs.
parse-a-fragment)" as **SETTLED: parse-a-fragment (L3 plan §Decisions 1)**.

- [ ] **Step 4: Commit C** — `docs(plan): code-graph MCP §6.3/§6.6/§10 settled by the L3 plan`.

---

## Confirm-before-build points (pre-edit confirmations the implementer resolves against the real tree — NOT rulings)

- **CB-1 (Task 1 / Task 8 spans):** after `parseStatement` returns for `long t =
  0;`, `for (...) stmt;` and `return t;`, `TokenBase::_parse_column` is the END
  of the `;` (the fixture asserts `{4,4,4,15}`, `{5,4,6,36}`, `{7,4,7,13}`). If a
  statement kind consumes past its terminator and pushes back (a skew), the
  fix is at THAT statement parser (restore the position, or stamp before the
  pushback) — never a heuristic in `graph_span`.
- **CB-2 (Task 3 globals):** a live `long g_total = 0;` reaches `parseStatement`'s
  return as the SAME `TokenDecl` the `top_decls` entry records (`gtd.decl = td`,
  `parser.cpp:68444` / the 69144 link-back). If the `=`-flow's link-back sets
  `decl` to a different object than the returned node, link the returned one
  (the deepest layer: the registration site).
- **CB-3 (Task 2):** `TopDecl` indices are stable within a generation (append-only
  during parse — L2's finding) and `type_id_for` ids fit in 32 bits (they are
  `uint32_t`), so the stamp never collides with the index bits.
- **CB-4 (Task 7 dialect):** nested `?:` in a `var` initializer and `var x = a ?
  "" : b` — if the dialect refuses either, spell them as `if`/`else` chains (the
  L1/L1b ledgers record similar adaptations: no nested brace literal, `php::substr`
  absent → `perl::substr`).
- **CB-5 (Task 7):** `perl::substr(var, off, len)` and `var.index("...")` exist
  (`madcide_core.inc:8076`, `madcide_enums.inc:188`); `ui::text_load` on the
  replay buffer accepts a `const char *` (`editor_events.inc:307`).
- **CB-6 (Task 8 seat):** `S.es`, `S.world()`, `S.active(doc)`, `undo_edit`,
  `events_since`, `clog_head` are reachable from the test's include set (they
  are for `testmadcide_serve_graph` / `testmadcide_changelog`).

## Self-review (controller, against the spec)

1. **Spec coverage.** §6.6 verb triple → Task 7 (+ Tasks 2-5 engine); "validated
   by the same parser/sema before commit" → Task 4; "logged as a node-op (a new
   ChangeEvent payload kind beside {at,del,ins})" → Task 7 step 3 + Task 6;
   "reverse-rendered, never hand-patched" → Decisions 2 (boundaries from the tree,
   text from the fragment; no agent offsets); "composes with undo, the event log,
   multi-client propagation" → `edit_checkpoint` + `journal_splice` via the one
   owner + `broadcast_events`; §6.3 "L3 confronts identity" → Task 2 + the
   `graph.at` answer; §5.3 handles-not-offsets → the agent passes ids only; §5.4
   enums → `graph_verb`/`graph_where`; §5.7 terse → one node + span object;
   client-server §2.5 tiers → `graph_min_tier`; §9 statement granularity → the
   statement-level check in `graph_span`. §6.4's `graph.insert(target, spec)`
   signature: v1 spells `spec` as `src` (a fragment) + `where`.
2. **Placeholder scan.** No TBD/TODO; every code step is complete; the two
   dialect-syntax uncertainties are named CB points with the exact fallback
   spelling.
3. **Type consistency.** `graph_func_node_value(st, tf)` / `graph_node_value(st,
   dd)` are used with `st` everywhere in Tasks 2-3; `graph_id_stamp(st, raw)`
   takes `int64_t raw`; `graph_extent_of(const TokenBase *, GraphExtent &)`;
   `graph_span_value(node, x)`; `internal_program_graph_span(handle, id, out)` ↔
   bridge ↔ wrapper ↔ decl arg order uniform; `parse_refresh_checked(out_diags,
   handle, source)` mirrors `parse_build(out_diags, handle, …)`. Seat:
   `graph_call(out, S, doc, self_id, nm, params)` matches the updated call site;
   `graph_edit_apply(out, S, doc, self_id, gv, args)`.
4. **Invariant check.** No second parser (the fragment is validated by THE
   parser via the TU); no second renderer (retained text at statement
   granularity); no second mutation path (`ed_text_*` only); no second log
   (one JSONL stream, one more record kind); extents computed exactly once (the
   parser) and read by one accessor; ids stamped by one helper and checked by
   one predicate.

## SDD execution note

Same shape as L2 (owner-approved: subagent fatigue): Tasks 1-9 are tightly
coupled and are executed INLINE by the controller with NO intermediate builds —
ONE foreground `make -C src ../bin/madc` after Task 5, the targeted tests, then
Commit A (src/include, rule trailers) · Commit B (tools/tests) · Commit C
(docs). Then an independent opus READ-ONLY review (the go-forward gate; a
package diff of A+B+C) and an inline fix round. Ledger:
`tmp/sdd-ast-graph-mcp-L3/progress.md`. Extra scrutiny (owner's L1b
directive): CB-1 stamps on every fixture statement kind; the id-stamp retrofit
covering EVERY emitted id (grep `type_id_for(` and `GRAPH_DECL_ID_BASE +` after
the edit — zero raw emissions); the atomic order in `graph_edit_apply`
(validate → checkpoint → nodeop → splices → spans → broadcast). The FULL battery
rides the V6 arc RELEASE seam, never this slice.
