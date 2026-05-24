# Cross-Cutting Insights from 2026-05-24 Analysis

How research and analysis from this session affects other plans.

## 1. Typed-Register IR Plan ← Code Cleanup Synergy

**Finding:** The code cleanup analysis found 66 `emit_ir_value()` calls and
90 `safemov()` calls — exactly the "spread of coercion logic" the IR plan
aims to centralize.

**Impact:** The builtin dispatch table (cleanup Priority 1) should be done
BEFORE Stage 3 of the IR migration (calls). Reason: extracting 45 builtins
into a table makes the subsequent IR port of TokenCallFunc::compile() much
cleaner — each builtin handler becomes an independent function that can be
ported to IR independently, rather than all being tangled in a 1,329-line
if/else chain.

**Recommended sequence:**
1. Cleanup Phase A (dispatch table, file splitting) — ~16-24 hours
2. IR Stage 0-1 (scaffolding, leaves) — 1-2 weeks
3. IR Stage 2 (arithmetic) — 1 week
4. IR Stage 3 (calls, now with dispatch table) — 1-2 weeks

**Also:** The AST visitor pattern (cleanup Priority 6) directly enables IR
Stage 4 (control flow). `contains_label()` is a prototype of the traversal
pattern needed for IR's Branch/Label/Jump nodes. Generalize it first.

**Test count update:** The IR plan references "170-test baseline" — now 452.

## 2. libmadc Phase 4 ← PCH + alloca

**Finding:** Two new capabilities affect the embedding API:

- **PCH:** `madc --emit-pch` could be exposed as `madc::program::emit_pch()`
  for hosts that want to pre-compile headers programmatically. Also, the PCH
  lookup chain (text-embedded → pre-compiled → filesystem) is a policy
  decision that `compile_options` should control.

- **Stack bump-pool alloca:** The 64KB pool per function is a fixed default.
  For embedded use (eval_expression, sandboxed exec), hosts may want to
  configure this — `compile_options::alloca_pool_size` or similar. The
  current `MADC_ALLOCA_POOL_SIZE` env var is a placeholder.

- **zlib dependency:** libmadc.so now links `-lz`. Node.js hosts and
  `pkg-config` must reflect this.

## 3. libmadc Phase 4 ← asmjit Constraints

**Finding:** asmjit Compiler mode uses rsp-relative addressing for locals.
`setPreservedFP()` adds rbp prologue/epilogue but does NOT change locals to
rbp-relative. This means:

- `sub rsp` for alloca is impossible without the bump-pool workaround
- `__builtin_frame_address(0)` returns rsp (not rbp) unless PreservedFP set
- Fork-based isolation (libmadc Phase 4) uses separate processes — the
  JIT frame layout per-process is independent, no conflict

**For future:** If madc ever needs in-process multi-threading (JIT'ing on
multiple threads), the asmjit Compiler's rsp-relative model means each
thread needs its own stack frame. The current design (one Program per
compilation) is safe.

## 4. Pre-Compiled Headers ← Code Cleanup

**Finding:** The PCH deserialization currently recreates tokens via
`new TokenInt(val)`, `new TokenIdent(s)`, etc. The token class explosion
(137 classes) makes this fragile — if a new token class is added, the
serializer must be updated.

**Impact:** If the token hierarchy is flattened (cleanup Priority 5), the
PCH serializer becomes simpler — fewer classes to handle, more table-driven.
Do cleanup first, then stabilize the PCH format around the cleaned-up token
set.

**Also:** The parser can't yet handle full system header declarations (the
PCH blocker). This overlaps with the code cleanup — the 932 if-branches in
parseExpression are partly WHY new constructs fail. A cleaner parser would
be more resilient to unexpected input from pre-processed system headers.

## 5. Revival Plan ← Status Update

**Finding:** The revival plan says "Phase 3 Complete" but the current state
is well past that:

- GCC parity: 1649/1685 (97.9%) — was ~90% at Phase 3
- Integration tests: 452 — was 170 at IR plan creation
- libmadc: extensive C++ API with eval, security policy, invoke limits
- PCH: Phase 1 infrastructure in place
- Stack alloca: real bump-pool implementation

The revival plan should be marked "Phase 3.5+ Complete" or updated to
reflect the current feature state.

## 6. Namespace System ← Cleanup Opportunity

**Finding:** 128 namespace functions across 8 namespaces, all using the
same three-layer pattern (C++ wrapper → addFunction → namespace_map).
The `ns_common` shared library is well-factored.

**Impact for cleanup:** The namespace registration is already clean — it
follows the data-driven pattern we want for builtins. The builtin dispatch
table should mirror this architecture: a registry of handlers keyed by
name, with shared infrastructure.

## Summary: Recommended Execution Order

1. **Code cleanup Phase A** (dispatch table, AST visitor, file split)
2. **IR Stage 0-1** (scaffolding, leaves) — builds on cleaner codebase
3. **PCH Phase 1 completion** (parser improvements for system headers)
4. **Code cleanup Phase B** (parser dereference/subscript unification)
5. **IR Stage 2-3** (arithmetic, calls) — benefits from parser cleanup
6. **libmadc updates** (PCH API, alloca config, zlib dep)
7. **Code cleanup Phase C** (macro system, token hierarchy)
8. **PCH Phase 2** (AST serialization) — needs stable token hierarchy
