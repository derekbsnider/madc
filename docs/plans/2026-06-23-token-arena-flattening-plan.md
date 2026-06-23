# Token arena flattening — from `vector<TokenBase*>` to a flat POD arena

**Date:** 2026-06-23
**Branch:** `feature/front-end-performance-claude`
**Status:** PLAN (execution not started). This is the real P1 + P3 + P4 of
`docs/plans/2026-06-22-front-end-performance-plan.md`, made concrete.

## ARCHITECTURE — SETTLED (design owner, 2026-06-23). Do not re-scope.

The flat arena is **ONLY for lexing — the tokens.** It is NOT a de-polymorphization
of the whole parser (an earlier draft of this plan wrongly estimated "~480 .str +
thousands of ->id()/->type()" by conflating the lexer-token representation with the
parser's AST — that estimate is WRONG and is struck).

- **Lexer →** a flat POD token arena: `std::vector<TokenRec>`, each token addressed
  by a `uint32_t` **slot-id**. Flat, reserve()'d, serializable to disk.
- **Parser →** still builds the **`cir_node` AST tree** (the MC11-IR), exactly as
  today in nature. It reads token data from the arena by slot-id.
- **Each `cir_node` carries a `uint32_t[]` of arena slot-ids** for its originating
  tokens. THAT is the MC11-IR "carries its originating tokens + source positions" —
  realized as slot-ids into the flat arena, NOT `TokenBase *` pointers. Provenance
  (file/line/col) lives in the arena `TokenRec`, looked up by slot-id.

So the scope is: (a) lexer emits `TokenRec`s into the arena; (b) the cir_node ↔
token linkage becomes `uint32_t[]` slot-ids instead of `TokenBase *`. The parser's
internal working set is an implementation detail — it reads `TokenRec` by slot-id;
it does NOT require rewriting every `->id()` call as a precondition.

### The AST is ALSO a contiguous serializable block (2026-06-23, design owner)

The `cir_node` AST tree must itself live in a **contiguous arena** so the whole IR
serializes to disk (the embedded-header forest / on-disk PCH —
`[[project_embedded_header_forest]]`, `docs/plans/2026-06-22-embedded-header-forest-execution-plan.md`).
So there are **TWO serializable blocks**: the token arena AND the cir_node arena.
For that, every pointer in a `cir_node` becomes an index/id:
- **children / operands →** `uint32_t` index into the cir_node arena (not pointers).
- **originating tokens →** `uint32_t[]` slot-ids into the token arena.
- **types →** **type-id** into the segmented type table
  (`[[project_type_table_value_abi]]`, `docs/plans/2026-06-12-type-table-value-abi-design.md`),
  not `DataDef *`.
All three pointer classes become indices ⇒ the IR is `memcpy`-to-disk /
`mmap`-from-disk.

**Tension to resolve (not a blocker):** `cir_node` *derives from* c2mir's `node_t`,
whose operands are a **pointer-linked DLIST** (`NL_HEAD`/`NL_NEXT`). The contiguous
index form is the *serialized* representation; feeding c2mir relocates indices →
`node_t` pointers on load (pointer fix-up, or `node_t` views over the arena).
Design this with the forest plan, since it is that plan's keystone.

## The problem (why the current "arena" is not done)

P1 as landed made the token stream a `TokenStream` wrapping
`std::vector<TokenBase *>` — a contiguous vector **of pointers** to individually
heap-`new`'d **polymorphic** token objects. That is NOT the P1 target:
- still a per-token `new` (the ~47% malloc the profile blamed),
- not POD (vtable + `std::string leading_trivia` + `DataDef *` per token),
- **not serializable to disk** (pointers + vtables don't serialize) — and we want
  to persist the arena (token forest / PCH-on-disk).

## Why it is not a trivial swap — `TokenBase` is triple-duty

`include/tokens.h:76` — each `TokenBase` is simultaneously:
1. a **lexer token** (`_token`, subclass payload: `cnt` / `str` / int/real value),
2. a **parse-tree node** (`parent`, and `TokenOperator::left/right` build the
   expression tree the compiler walks),
3. an **IR annotation carrier** (`_datatype` resolved during parse; the MC11-IR
   `cir_node` attaches the originating `TokenBase` + subtree; `read_count`,
   `leading_trivia`, file/line/col/pos).

A flat POD arena cleanly serves (1). (2) and (3) are what keep objects alive. So
the refactor is **staged**: flatten the lexer stream first; let the parser
materialize the (fewer, longer-lived) objects it needs from arena entries.

## Phase 0 finding (2026-06-23) — two populations; flatten only one

The 115 `Token*` subclasses split into two populations, and only the first needs
the arena:
1. **Lexer tokens** — `Space`/`Tab`/`EOL` (`int cnt`), `Ident` (`std::string str`),
   `Int` (int64 value + `source_text`), `Real` (`double _val` + `source_text`),
   `String` (`str` + `wide`), operators + punctuation (no payload). These are the
   324K-token bulk and the per-token-`new` cost → flatten to POD `TokenRec`.
2. **Parser-built AST nodes** — `Operator` (`left`/`right`), `Ternary`
   (`condition`/`true_expr`/`false_expr`), `Case` (`value`/`range_high`), `Try`
   (`try_body`), `Throw` (`throw_expr`), switch `init_stmt`, `Goto`/`Label`
   (`target`/`labeled`/`indirect_target`), plus annotation fields
   (`resolved_type`, `target_type`, `query_type`). These carry child `TokenBase*`
   and **are the parse tree** — constructed during parse, not lexed. **They stay
   objects.**

So the arena targets population (1); the AST (2) remains pointer-linked nodes
materialized during parse. `TokenRec` only needs to encode population-(1) payloads
(cnt / spelling-id / int64-or-double value / source_text-id). This bounds Phase 1
to the lexer stream and the macro/template substitution loops (which clone
population-(1) tokens) — NOT the AST.

## Phase 0 finding (3) — measured scope + a detour to NOT repeat

**Detour (2026-06-23, reverted):** a class-specific pool allocator for `TokenBase`
(`operator new/delete` over bump chunks) was built and measured. It is NOT the
arena — tokens stay polymorphic heap objects, the stream stays `vector<TokenBase*>`
(pointers), nothing becomes flat/serializable. A/B at -O0: **no measurable win**
(the 47% malloc is an -O2 figure). It was reverted. Lesson: an allocator optimizes
the *current* model; the flat arena *replaces* it — don't conflate them.

**CORRECTED (2026-06-23):** the estimate below was WRONG — it assumed the whole
parser must de-polymorphize (conflating lexer tokens with the parser's AST). Per
the SETTLED architecture at the top, the AST is `cir_node` (carrying `uint32_t[]`
slot-ids), and only the LEXER token representation flattens. The `~480 .str` and
`thousands of ->id()/->type()` figures do NOT gate this work and are struck.
~~Measured refactor surface … "serializable to disk" forces de-polymorphization …~~
What remains true and useful from the trace: there are **53 `delete tb`** sites
(token ownership) to reconcile when tokens become arena slots rather than
individually-owned heap objects.

## Phase 0 finding (2) — reuse what exists (3R credo)

- **Materializer already exists:** `src/pch.cpp` has a `TokenID → new TokenX`
  factory switch (~line 150+, used by PCH deserialize). That IS the
  "materialize a `TokenBase` from a `TokenRec.kind`" the arena needs — reuse it,
  do not write a parallel one.
- **Flat encoding already exists:** `serialize_tokens` / `write_madh` (pch.cpp)
  already encode the token stream to a flat byte form for `.madh` PCH. The
  `TokenRec` layout should converge with (or become) that encoding, so the arena
  and the on-disk forest (Phase 4) share ONE format.
- **Construction surface:** 129 `new Token*` sites in `src/lexer.cpp` (33
  `TokenDataType`, then `Ident`/`Int`/`Real`/`Str`/`Char` payload-bearing; the
  rest payload-free operators/punctuation). The two-step seam for Phase 1:
  route construction through a single factory (payload-free ones via the existing
  `token_from_id(kind)`), validate behavior-identical, THEN make the factory
  append a `TokenRec` + return a materialized/lazy object.

## Target representation

- **Arena** = `std::vector<TokenRec>` of POD records, bump-appended, never freed
  per-token. Serializable. Roughly:
  ```
  struct TokenRec {        // POD, trivially copyable, no pointers
      uint16_t kind;       // TokenID
      uint16_t flags;      // tokflag_t
      uint32_t spelling;   // interned id (0 = none) — P3
      uint32_t file_id;    // interned filename id (not a char*)
      uint32_t line, col;  // occurrence/provenance — PER SLOT, not re-stamped
      int64_t  value;      // _token / ival; reinterpreted for real/char
  };
  ```
  (Exact field set TBD in Phase 1; `value` may need a side-table for doubles /
  long strings / wide payloads — design against the real subclass payloads.)
- **Stream + instantiations** = `std::vector<uint32_t>` of arena indices (ids).
  Backtrack = index rewind (already true). Macro/template substitution builds a
  new id-vector with substituted ids — **no `clone()` of objects** (109 sites today).
- **Identity vs occurrence**, separated (today `clone()` conflates them, and that
  conflation is already a bug — `parser.cpp:3829` provenance misfiling):
  - identity (kind + spelling) → interned, shared, in the arena/intern pool;
  - occurrence (file/line/col) → per **slot** (its own arena record, or
    tinycc-style interleaved `TOK_LINENUM` markers in the id-stream).

## Phases (each: fulltest green + torture failset == baseline + --emit=c11
byte-identical + perf re-measure via `scripts/perf_vs_gcc.sh`)

- **Phase 0 — recon + baseline.** Enumerate every `TokenBase` subclass and its
  payload (what a `TokenRec` must hold); map the 109 `clone()` sites and the
  ~95 injection sites; record `perf_vs_gcc.sh` numbers for testsubscript/testmap
  + memcpy-a1 as the before-picture. Decide the `value`/payload side-table shape.
- **Phase 1 — POD lexer arena behind the current API.** Lexer appends `TokenRec`s
  to the arena; the stream becomes `vector<uint32_t>`. Keep `TokenBase` objects by
  **materializing on demand** from a record when the parser actually needs an
  object (parse-tree node / `_datatype` annotation). Two-step seam method (as P1
  step 1): introduce the record+id path while behavior is identical, validate,
  then flip the lexer to fill records. Kills the per-token `new` on the lex path.
- **Phase 2 — interning (P3).** Spellings/filenames → `uint32_t` ids; identifier
  compare and `define_find`/typedef/template lookups become id/array-index, not
  `std::map<string>`. Folds the macro `define_map`/`macro_map` keys to ids.
- **Phase 3 — no-clone instantiation/macro (P4 + P5).** Template instantiation and
  macro expansion build substituted **id-vectors** instead of cloning objects;
  provenance is per-slot. Retire the 109 `clone()` sites on these paths. This is
  where the ~43% deque-copy/instantiate cost dies.
- **Phase 4 — serialization.** Dump/load the arena (+ intern pool) to disk — the
  token-forest / on-disk PCH the embedded-header-forest track wants
  (`docs/plans/2026-06-22-embedded-header-forest-execution-plan.md`).

## Hard constraints (do not violate)

- **MC11-IR invariant** (`.claude/rules/mc11-ir.md`): `cir_node` must still reach
  its originating tokens + parse subtree + file/line/col. The arena records (by
  id) + materialized parse-tree objects satisfy this; provenance moves to the slot.
- **No parallel implementations** (`.claude/rules/no-parallel-implementations.md`):
  the seam is two-step (old path replaced, not kept alongside); no `MADC_*_OLD`
  left as dead code.
- **Coexistence, not big-bang:** the *stream* becomes ids while the *parse tree*
  stays objects materialized from the arena — they are not in tension.

## Open questions to settle in Phase 0 (verify, don't assume)

- Payload for wide values (double, long double, long string literals, `__int128`):
  inline a union in `TokenRec`, or an index into a side-table? (tinycc inlines
  CValue words after the tok id in its int-stream — consider the analogue.)
- Where exactly the parser needs a live object vs. can read a record (audit the
  `->id()/->type()/->str/->datadef()` hot accessors; many can read the record).
- Lifetime of materialized parse-tree objects vs. the arena (arena outlives them;
  nodes reference arena ids for provenance).
