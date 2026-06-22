# Plan — fix the `vector<T*>` crash (fn-template decl token use-after-free), with clang's model as reference

**Status:** ✅ EXECUTED 2026-06-16 — the crash is FIXED at `e3cc45a`. **Branch:**
`feature/retire-embedded-shims-claude` (pushed to origin). **Companion:**
`docs/parity/feature-drops-audit.md` (crash → Resolved; the residual vector<T*>
*type* errors are now row 6b), `docs/plans/2026-06-13-embedded-ast-frontend-design.md`
(the strategic cure), handoff `docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md`.

---

## OUTCOME (2026-06-16)

The keystone mystery from §0 ("why did cloning at registration leave `decl[0]`
at `0x…d79c00`?") is **resolved**: `TokenCppKeyword::clone()` returned `this` —
the shared `keyword_map` prototype. So (a) cloning a keyword token at
registration was a no-op (it returned the singleton), and (b) every
`constexpr`/`sizeof`/`this`/… occurrence in the whole parse aliased ONE mutable
object that the lexer re-stamped per use (hence the impossible
`memory_resource.h:539`) and that a retained `ft.decl[0]` borrowed — so deleting
any sibling occurrence freed it for ALL borrowers.

Diagnosis path (ASan was unusable — this QNAP kernel refuses ASan's shadow mmap;
used `-g` + gdb instead): broke at `retain_namespace_fn_template_body` and saw
`forward`/`advance`/`crbegin`/`operator==`/… ALL register `decl[0] ==
0x555555d79c00` with identical stale `file=memory_resource.h:539` ⇒ a single
shared object, not per-occurrence tokens. Traced to `clone(){return this}`.

Fix (`e3cc45a`): `TokenCppKeyword::clone()` returns `new TokenCppKeyword(str)`
(leaf class — exact reproduction; the TokenIdent convention). Base
`TokenKeyword::clone()` keeps `return this` (dispatch subtypes TokenDO/TokenIF/…
would be sliced). This was Step-1 option **(c)** — fix the aliasing at its
source — which is deeper than the plan's (a)/(b) and fixes the whole hazard
class, not just the ordering reader.

Validation: `tmp/vp1.mad` (`vector<int*>`) no longer SIGSEGVs the compiler;
fulltest **626/7/0/18**, identical baseline, zero regression (the change touches
every cpp-keyword). `vector<T*>` still does not COMPILE — it now hits a
pointer-element type-threading wall (audit **row 6b**), which is a separate,
deeper track.

---

## 0. The bug (characterized 2026-06-16)

`std::vector<T*>` for ANY pointer element crashes madc **at compile time** the
moment an element is accessed (`v[0]`, `v.at(0)`, `v.front()`). `vector<T>` (non-
pointer) is fine. Minimal repro: `tmp/vp1.mad` (`std::vector<int*> v; int a=1;
v.push_back(&a); *v[0];`). Real test: `tests/testvectorptr.mad` (the pointer-to-
CLASS sibling fails earlier with `Expecting a type argument to rebind<>`).

**Root cause (gdb, `-g` build):** partial-ordering of `std::forward`'s overloads —
`instantiate_namespace_fn_template_for_call` → `po_more_specialized` →
`po_at_least_specialized` → `po_param_spellings` →
`skipped_template_function_declarator_name_index` (parser.cpp:26536) —
dereferences a **freed / clobbered `TokenBase*`** in `ft.decl[0]` of the
`fn_template_map["std::forward"]` candidate: `t->id()` is a virtual call through a
stale vtable → SIGSEGV. Diagnostic tell: `ft.decl[0]` is the SAME address
(`0x…d79c00`) every run while `ft.decl[1..]` vary run-to-run — i.e. token[0] is an
early, deterministically-allocated object whose memory was freed/reused, not a
normal lexed token of this 44-token decl. The `vector<T*>` path is simply what
first EXERCISES `forward`'s overload partial-ordering (a `vector<T>` access binds
`forward` without it).

**Why `forward`:** `std::forward` has two overloads — `forward(remove_reference_t<T>&)`
and `forward(remove_reference_t<T>&&)` — so a use site runs [temp.func.order]
partial-ordering over both. The pointer element makes the element-access /
construct chain forward a pointer argument, taking that ordering path.

**Attempt that did NOT fix it (do not repeat blindly):** cloning the decl tokens at
registration (`retain_namespace_fn_template_body`, parser.cpp:27393 `ft.decl =
tokens` → per-token `clone()`). After the fix, `ft.decl[0]` was STILL `0x…d79c00`
(the pre-clone address). A clone would have a fresh, run-varying address — so the
crashing `ft` was **not produced by that code path**, or token[0] is shared from
elsewhere. Reverted (kept the tree honest).

---

## 1. How clang does it (reference model — agent recon, citations)

- **Representation.** A function template is a `FunctionTemplateDecl`
  (`clang/include/clang/AST/DeclTemplate.h:977-1114`) holding a
  `TemplateParameterList*` and a `NamedDecl* TemplatedDecl` (the pattern = a parsed
  `FunctionDecl` AST). **Never tokens; never re-lexed/re-parsed at instantiation.**
- **Lifetime.** All AST nodes live in `ASTContext`'s `BumpPtrAllocator`
  (`ASTContext.h:609-613`): a linear, **never-freed** arena — "AST objects are
  never destructed … released when the ASTContext is destroyed." So a `Decl*` held
  across passes can never be freed mid-pass and reused. **No use-after-free is
  structurally possible.**
- **Partial ordering** (`SemaTemplateDeduction.cpp:5273-5435 isAtLeastAsSpecializedAs`,
  `5459-5493 getMoreSpecializedTemplate`): synthesize unique types for FT2's
  parameters, then run ordinary template-argument deduction of FT1 against them
  (`DeduceTemplateArguments(..., PartialOrdering=true)`); FT1 ≥ FT2 iff all FT1
  params deduce. Bidirectional → "more specialized" per [temp.deduct.partial]p10.
  **The pattern is read READ-ONLY**; deduction state is local.
- **Instantiation** (`SemaTemplateInstantiateDecl.cpp:2056-2243`): creates a NEW
  `FunctionDecl` via `FunctionDecl::Create` (TreeTransform); the pattern is **never
  mutated**. Specializations live in a side `FoldingSet`, not in the pattern.

**Lesson:** clang's safety is not algorithmic cleverness — it is the *ownership
model*. Immutable, arena-owned pattern + read-only ordering + new-decl
instantiation = the hazard cannot exist.

## 2. Why madc is exposed (the divergence)

madc monomorphizes by **token re-substitution** (Borland model): a retained fn
template is a `std::vector<TokenBase*>` (`FnTemplateDef::decl`) of **raw heap
pointers**, re-parsed per instantiation. Raw `TokenBase*` are individually `new`/
`delete`-managed, so a token can be freed while another structure still points at
it. `po_param_spellings` then reads a dangling `ft.decl[i]`. This is one instance
of a whole hazard CLASS the token model invites (see feature-drops audit; the
S19b "exception-safe scope restore" and "error-corruption" fixes were siblings).

## 3. Cross-reference: the strategic cure already designed

`docs/plans/2026-06-13-embedded-ast-frontend-design.md` §2 specifies exactly the
structural fix, independently motivated:

> **Arena + `u32` index handles, no internal pointers.** This is the single
> decision that makes freeze/thaw, segmentation, and zero-fixup embedding all
> trivial.

That is madc's `BumpPtrAllocator` analogue. With tokens/`cir_node`s in an arena
addressed by `u32` index (never individually freed, never raw pointers), a
retained template decl referencing arena indices **cannot dangle** — the same
property that makes clang safe. The frontend doc also mandates an **append-only
token array** (§1) with a rewind cursor: append-only ⇒ tokens never move or free
⇒ a held index/offset stays valid. So the correct long-term home for this fix is
the frontend-representation refactor, NOT another point-patch on raw pointers.

**Tension to honor:** the frontend refactor is a large later-stage track
(`docs/plans/2026-06-09-frontend-representation-refactor.md`, the header-forest
model). We should NOT block `vector<T*>` on it. So the plan is two-tier: a
contained tactical fix now, landing on the trajectory the refactor will
generalize.

---

## 4. Post-compaction plan

### Step 0 — resolve the keystone mystery (diagnose before fixing)
Determine **why `ft.decl[0]` stayed `0x…d79c00` after cloning at the only
`fn_template_map[...]` writer (parser.cpp:27406).** Hypotheses to test, in order:
1. A SECOND path populates/aliases `fn_template_map["std::forward"]` (grep is
   clean for `fn_template_map[` — so look for: a copy of the map, a `FnTemplateDef`
   copied with its `decl` shared, or `forward` captured via
   `capture_free_function_overload` / a precompiled-header / serialized path).
2. `clone()` for token[0]'s concrete type returns an INTERNED/cached/singleton
   token (fixed address) rather than a fresh `new` — check the `clone()` overrides
   for whatever token[0] is (likely the leading `template`/return-type token).
3. token[0] is owned by the header lexer's token storage that is freed after the
   header parse, and `forward`'s decl is captured BEFORE the clone code (build/
   ordering of registration) — verify with a gated print of `ft.decl[0]` addr at
   registration vs at the crash.
Instrument: `-g` build + gdb `break`/`watch` on the `fn_template_map["std::forward"]`
entry's `decl._M_start`, and a gated `MADC_DIAG_FNTPL_LIFETIME` print of
`(void*)ft.decl[0]` at registration and at `po_param_spellings`. Identify the free
site (gdb `watch` on the freed address, or ASan: rebuild with
`-fsanitize=address` and run `tmp/vp1.mad` — ASan will name the alloc + free
stacks directly; this is the fastest path and should be Step 0's first move).

### Step 1 — tactical fix (deepest layer the diagnosis points to)
Given Step 0's finding, ONE of:
- **(a) Stop the free.** If a header/lexer token buffer is freed while a retained
  decl references it, make the retained decl OWN immutable copies — but verify the
  clone actually lands on the partial-ordered entry (the Step-0 reason the first
  attempt missed). Ensure EVERY `FnTemplateDef` path (operator templates, member
  templates, the manipulator/overload captures) clones, and that `FnTemplateDef`
  copy/move does not re-share.
- **(b) Don't re-order with raw decl reads.** If partial-ordering only needs the
  param-spellings, compute + CACHE them as `std::string`s on `FnTemplateDef` at
  registration (when tokens are known-live), so `po_param_spellings` never touches
  `ft.decl` at ordering time. This sidesteps the lifetime issue for the ordering
  reader specifically and is small + targeted. (Strong candidate: ordering wants
  spellings, not tokens.)
- Prefer (b) if Step 0 shows the free is hard to stop cleanly; prefer (a) if it is
  a single obvious owner. Either way: NEVER A SHIM, fix at the deepest layer.

### Step 2 — validate
3-oracle (g++/clang/madc) on `tmp/vp1.mad` (`vector<int*>`), a `vector<int*>`
+ realloc + readback (extend vecanyX to pointers), and `tests/testvectorptr.mad`
(pointer-to-class — also clears the `rebind<>` symptom, or exposes the NEXT
pointer-element wall to add to the audit). `make -C src fulltest` — this touches
fn-template ordering (hot, shared); gate suite-wide; expect the remaining 7 → 6.
Promote `tmp/vp1` to `tests/testvectorptr_scalar.mad` with fixtures.

### Step 3 — record + connect
Move audit row 6 to Resolved with the commit; note in the handoff how the fix sits
on the frontend-refactor trajectory (arena/index handles will generalize it). If
Step 0 reveals the hazard is broad (many retained decls can dangle), file that as
an explicit driver for prioritizing the frontend-representation refactor and link
it from `docs/plans/2026-06-09-frontend-representation-refactor.md`.

### Out of scope (own rows in the audit, don't fold in)
`delete[]` (parsed as lambda), scalar `new T(v)`, concrete `enable_if<false,T>::type`
acceptance, global-`::`-scope free-fn-templates. The remaining container fails
(map/set/subscript/containerdtor/madc_ns) are separate reductions.
