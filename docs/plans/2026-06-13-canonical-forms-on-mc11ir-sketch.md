# Sketch — canonical forms on MC11-IR ("SEMA benefits without a SEMA rewrite")

**Date:** 2026-06-13 (session 8). **Status:** **Phase 1 LANDED + gated** (`90e9dcd`, session-8
part 19) — non-type template-arg canonicalization at the one key chokepoint, non-re-entrant
local-cursor evaluator, all six key-build sites routed; `typeparam_types` deferred (evaluator
self-gates). Later increments (type-arg canonical keys, general const-expr canonicalization,
sugared/canonical split) remain unscheduled — build only if the pain recurs. Companion to
`2026-06-13-nontype-template-arg-canonicalization-research.md`.

## The idea in one sentence

Keep madc's single tree (MC11-IR) and its token-resubstitution monomorphization, but stop deriving
**semantic identity from spelling**: compute a **canonical form** for each semantic entity once, at a
**stable** point, attach it to the IR, and use it for all **keying/matching/lookup**. This buys the
parts of a SEMA layer that actually hurt today (canonical identities, no mid-parse re-entrancy)
without a separate Sema pass/tree — which would violate the one-tree invariant (`mc11-ir.md`),
duplicate c2mir's C-level SEMA, and risk the polyglot "keep the sugar" goal.

## Principle (the dividing line)

- **Identity / keying / matching → canonical form** (computed once, value+type based).
- **Body monomorphization → unchanged** (clone tokens + type-subst + re-parse, as today).
- **Reverse-rendering / polyglot → unchanged** (the attached tokens + parse subtree stay; canonical
  forms are *additional*, never a replacement — mirrors clang keeping `Sugared` AND `Canonical`).

So this is additive: a canonical "shadow" on the existing structures, not a new structure.

## Three ingredients

1. **Canonical representation per concept.**
   - *Non-type template arg* → `(int64 value, DataDef *paramType)`, value normalized to the param
     type (bool → 0/1), rendered to ONE canonical key fragment. (Mirrors clang
     `TemplateArgument(Integral, value, paramType)`.)
   - *Type template arg* → the canonical `DataDef *` identity (madc already has most of this; the
     gap is using it uniformly for keys instead of the spelling).
   - *Dependent arg* → stays the spelling/tokens (un-canonicalized), exactly as clang keeps
     value-dependent args as `Expression`.

2. **One key-construction chokepoint.** Today the instantiation key is built by
   `sanitize_template_fragment(arg_spelling)` at MANY sites (use-site `instantiate_template_id`
   ~2752, the `Tmpl<>::member` member-chain path ~3263, alias instantiation, base-clause resolution,
   full-spec registration ~27600). Route ALL of them through a single
   `canonical_arg_key_fragment(slot)` so every site agrees. **This is the lesson of the 202-regression**
   (`tmp/nontype_fold_v2_wip.patch`): canonicalizing at *some* sites makes keys inconsistent and
   shatters the header corpus. It must be all-or-nothing, at the chokepoint.

3. **A NON-re-entrant constant evaluator.** The crux that avoids the regression's second fault.
   - Signature: `bool eval_const_tokens(const std::vector<TokenBase*> &toks, ... , int64 &out)` that
     walks a **local cursor** over an already-collected, closed token vector — it NEVER swaps or reads
     the global `Program::tokens`, never suspends a live parse.
   - Type lookups inside it (for `__has_trivial_destructor(T)`, `sizeof(T)`) use **read-only**
     resolution (`resolve_named_datadef` / `datatype_map`); if an expression would require
     *instantiating* something new, it returns "not foldable" (keep the spelling) rather than recurse.
   - Rejects value-dependent / runtime forms (the `constant_initializer_has_runtime_access`-style
     guard) so only manifest constants canonicalize.
   - Implementation options: (a) refactor the existing `parse_constant_*` family to take a cursor
     instead of reading `Program::tokens` (cleanest, reuses all the logic, bigger diff); (b) a focused
     evaluator for the forms that actually appear in headers (int/bool/char literals, the trait
     builtins, `sizeof`/`alignof`, the C arithmetic/relational/ternary operators). Start with (b)
     scoped to non-type args; graduate to (a) if/when more concepts need it.

## Phase 1 — non-type template arguments (the first concrete increment)

Smallest self-contained slice that proves the pattern and clears w2a's non-type face. Steps:

1. **Record non-type param TYPES in `TemplateDef`** (`std::vector<DataDef*> typeparam_types`,
   filled when the primary's parameter list is parsed). Prerequisite for value normalization + knowing
   which slots are integral/enum (foldable).
2. **Write the non-re-entrant `eval_const_tokens`** (option (b) above).
3. **Add `canonical_arg_key_fragment(paramType, argTokens, argSpelling)`**: if `paramType` is
   integral/enum and `eval_const_tokens(argTokens)` succeeds → render the normalized value (bool →
   `0`/`1`); else → `sanitize_template_fragment(argSpelling)` (today's behavior, byte-identical).
4. **Route every key-build site through it** (audit list in the research doc §3).
5. **Partial/full-spec selection** then unifies for free (the keys match); `non_type_partial_spec_arg_matches`
   can stay as a value-compare backstop.
6. **Gate paranoia.** The 202-regression proves header parsing is exquisitely sensitive. After EACH
   sub-step, run the full integration suite (byte-identical FAIL list vs the 27-baseline) + torture
   failset + SMAUG. Land only when green. Reducers `tmp/nt1.mad` / `tmp/nt2.mad` must pass.

Phase 1 does NOT clear w2a alone — w2a also needs **member-template instantiation** of
`_Destroy_aux<true>::__destroy<int*>` (the separate "B" feature). But Phase 1 is independently correct
and testable, and removes the spelling-keying bug class for non-type args.

## Later increments (only if the pain keeps recurring — don't pre-build)

- **Type-arg canonical keys** — use the canonical `DataDef*` identity for instantiation keys uniformly
  (retire spelling for type args too). Larger blast radius; do only behind the same gate discipline.
- **Constant-expression canonicalization generally** — array dimensions, `enum` values, `static_assert`,
  bit-field widths already fold via `parse_constant_*`; converging them onto `eval_const_tokens`
  (option (a)) removes the global-stream dependence everywhere.
- **Sugared/canonical split** — if reverse-rendering ever needs the *written* form while keying needs
  the canonical (clang's `Sugared`/`Canonical`), store both on the node. Only when a concrete polyglot
  need demands it.

## Why this respects the invariants (so it's not re-litigating the IR)

- **One tree:** canonical forms are fields/keys on existing `DataDef`/instantiation machinery, not a
  second AST. No competing tree, no parallel implementation.
- **c2mir owns C SEMA:** this canonicalizes only madc-front-end identity (template args, dialect
  resolution); it does not type-check C — c2mir still does.
- **Polyglot sugar preserved:** tokens + parse subtree stay attached; canonical forms are additive.
- **gcc/clang canon:** the (value, type) model is exactly clang's; we borrow the *model*, not its
  separate-Sema-object architecture.

## Risk ledger

- **Chokepoint completeness** is do-or-die: miss one key-build site and keys go inconsistent →
  corpus-wide breakage (proven). Mitigation: grep-audit + a single helper + a `scripts/`-style guard
  if feasible.
- **Evaluator scope creep**: option (b) must clearly refuse anything it can't fold (return false →
  spelling fallback), never guess.
- **Re-entrancy**: the local-cursor rule is absolute — if the evaluator ever needs to touch
  `Program::tokens` or instantiate, it has overstepped; redesign rather than swap streams.

## Bottom line

Not a SEMA layer — a **canonicalization discipline on the one IR**: identity by computed value+type,
not by spelling; evaluated once at a stable point, never mid-parse. Phase 1 (non-type args) is the
first, smallest, gateable instance and the prerequisite the w2a wall is currently asking for.
