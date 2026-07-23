# R4 findings — "member-template overload replay" root-cause investigation

**Status: VALIDATED GREEN (2026-07-16, Claude) — R4 COMPLETE.**
Both fences DELETED; full matrix green at the fix stack (default-flags build):
fulltest **697/0/0/16**; bind gate **18/18 incl. subbind** (owner's bar, on an
UNFENCED pack); tsubst ratchet + equivalence oracle + selfexe gate GREEN;
packed suite **697/0/0/16** with MADCSNAP blob; binary **10,381,208** bytes
(−2,096 vs R3's 10,383,304 — fence deletion outweighs fix additions; still
+49,600 vs the 10.33MB reference, R5 sign-off item). Owner's-bar spot check:
unfenced freeze+bind of testsubscript == live output byte-identical.

## RESUME HERE (post-compaction rehydration)

The root cause (full chain below) is: **pattern-lane replay tokens carried C++
SOURCE spellings where the parse lane carries REGISTERED identities
(dd->name)**; frozen decl text with source spellings neither resolves
(datatype lookup) nor deduces (template-id unifier) consumer-side at bind.
The failing shape: `vector<string>` pattern-instantiated at pack →
`__alloc_traits<...>` parse-lane-instantiated as its dependency WITH
source-spelled replay arg tokens → its `construct`/`destroy` member-template
placeholder decl tokens freeze with source spellings → at bind, the derived
`_M_realloc_insert__mti` body's `_Alloc_traits::construct(...)` fails
deduction (ok=0, then STREAM IMBALANCE on a later attempt) → call stays on the
ref-less varargs placeholder → struct passes by value → c2mir "conversion of
non-scalar value requested" ×2 (construct + destroy) → subbind red.

**Three fixes in the working tree (commit them as one rung):**
1. `fn_template` deducer ([temp.deduct.call] derived-class rule): template-id
   param unify failure retries against the argument class's BASES,
   breadth-first, with `binding_pre` copies so failed attempts can't poison
   state (parser.cpp ~39915, after the direct unify).
2. `basic_class_pattern_canonicalize_type_tokens(pgm, out)`: every substituted
   `TokenDataType` in pattern token streams gets its spelling set to
   `definition.name` (applied in `basic_class_pattern_substitute_tokens` —
   which grew a `Program &pgm` first param, all ~13 call sites updated, plus
   `basic_class_pattern_substitute_token_runs` — and in
   `basic_class_pattern_member_template_tokens`).
3. Resolver replay tokens (`instantiate_pattern_template`, parser.cpp ~4886):
   `TokenDataType(memo_arguments[i]->name, *memo_arguments[i])` — ALWAYS the
   registered identity, never `basic_class_pattern_type_spelling`. Key-neutral:
   instantiation keys derive from the attached DataDef (fenced/unfenced packs
   register identical mangled names).

**Next steps (exactly where this left off):**
1. Build in flight: `CXXFLAGS="-std=c++11 -Wall -O0 -DMADC_DEBUG_FNTPL=1"
   make -C src ../bin/madc` (debug define is #if-gated; FINAL validation must
   rebuild parser.o with default CXXFLAGS).
2. Test cycle: `bin/madc --freeze=tmp/r4_fix.msnap tests/testsubscript.mad`
   then `--forest-bind=tmp/r4_fix.msnap` → diff vs `tmp/r4_sub_live.out`
   (13-line expected output; live output captured there). Watch deduce with
   `MADC_DEBUG_FNTPL_DUMP=construct` — param[0] spelling should now be
   `allocator_std__..._&` and ok=1.
3. If green: default-flags rebuild, `make -C src fulltest` (includes bind gate
   18/18 + subbind), `make -C src release`, packed suite
   (`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`), blob check, binary
   size vs 10,383,304. Commit rung (fences deleted + 3 fixes + probes),
   re-freeze everything, push.
4. If still red: the same evidence loop — `MADC_CHECK_ATTRIB=1` names the
   failing def; FNTPL dumps name the failing deduce/parse.
5. Remaining after R4: task #41 (live lazy capture of vector fails —
   dependent-base _M_impl; fix likely benefits from fixes 2+3 since capture
   bodies also parse substituted tokens), then R5 (flip live default + B3 wall
   gate; needs eligibility widening for __iterator_traits/allocator_traits ×14
   + __move_if_noexcept_cond parse-error family).

## What R4 turned out to actually be

The handoff's framing ("replay picks the wrong `_M_data_ptr` overload; fix the
__oN rank model in the nested-recipe replay") is **wrong in every detail but
the symptom**. Evidence chain, each step verified:

1. **The original c2mir error position lied.** `stl_vector.h:2015` is just the
   position stamped on `vector<T>`'s pattern-derived defs. `MADC_CHECK_ATTRIB=1`
   (existing env lever) attributes the 2 bind errors to
   `vector<basic_string>::_M_realloc_insert__mti` — NOT `_M_data_ptr`.
2. **Repro**: fences lifted, `bin/madc --freeze=tmp/x.msnap tests/testsubscript.mad`
   (pack now pattern-instantiates) then `--forest-bind` the snap → 2 ×
   "conversion of non-scalar value requested", empty output. The packed module
   itself is CLEAN (`--run-frozen` no errors; pack check gate clean after 1
   round, and unfenced pack has FEWER drops than fenced: 337 vs 414 on the
   reducer, 125 vs 169 defs).
3. **Class state restores correctly.** Pattern-lane pack + restore give
   byte-correct aliases (`pointer` → `basic_string*`, `_Alloc_traits` →
   `__gnu_cxx::__alloc_traits<...>`) — verified with the new pack-side alias
   probe (`MADC_CLASS_PATTERN_PROBE=<substr>` prints `alias:` lines at pattern
   materialization) and the existing restore probe (`MADC_MTI_PROBE_CLASS`).
   The `__alloc_traits<allocator<string>,string>::construct` member-template
   placeholder is restored in BOTH fenced and unfenced packs.
4. **The real divergence** (normalized diff of the derived
   `_M_realloc_insert__mti` FUNC_DEF subtrees, fenced vs unfenced bind,
   `--dump-cir`): the body's `_Alloc_traits::construct(this->_M_impl, __new_start
   + __elems_before, std::forward<_Args>(__args)...)` resolves to
   - fenced (works): `__alloc_traits_..._construct__mti` (the derived class's
     own member template — correct), first arg lowered `&this->_M_impl`;
   - unfenced (broken): `allocator_traits_..._construct__mti__o2` (the BASE
     class's overload set), args shaped differently; the struct return/param
     mismatch produces the 2 synthesized-cast errors.
5. `FNTPL mcall` (compile `-DMADC_DEBUG_FNTPL=1`, env `MADC_DEBUG_FNTPL_DUMP=`)
   does NOT fire for this call → the qualified STATIC call
   `_Alloc_traits::construct(...)` resolves via a different path than
   parseCallMethod/findMethodOverload — suspect `resolved_call_funcdef` +
   `namespace_fn_overload_sets` ranking or the qualified-static-call path.
   NEXT PROBE: find that resolution path and why the unfenced restored state
   ranks/falls through to the base's `construct__mti__o2` when the derived
   class's own placeholder exists in both packs.

## Secondary discovery (task #41, live-lane blocker)

Live lazy capture (`MADC_CLASS_PATTERN_LIVE=1`) of `std::vector` FAILS:
`capture FAILED: std::vector (poisoned) — no member named '_M_impl'`
(dependent-base `_Vector_base<_Tp,_Alloc>` member lookup during the open-param
capture parse). At PACK the same capture SUCCEEDS. So flagship containers never
pattern-serve live → live-lane wall gains are capped until fixed. This is why
the naive live repro of R4 shows nothing.

## Also learned / kept

- `MADC_DEBUG_FNTPL=1` build + `MADC_DEBUG_FNTPL_DUMP=<substr>` dumps member-
  template deduce/inst/inj-token streams. The injected token bodies are
  IDENTICAL fenced vs unfenced — divergence is in name resolution during the
  derive parse, not the recipe.
- New diagnostics added (keep): pattern-materializer `alias:` probe under
  MADC_CLASS_PATTERN_PROBE; `MADC_CLASS_PATTERN_NO_MEMO` kill-switch in
  BasicClassPatternResolver::find_memoized (used to EXONERATE the R3 memo —
  errors persist with it off).
- The eligibility fence (`UnsupportedMemberTemplateOverloads`) only ever
  fenced member CLASS/alias templates (nested_templates), NOT method
  overloads — `vector` was never eligibility-fenced; only the pack fence
  protected it. Enum value 13 must stay (wire compat) even after the fence
  logic is deleted.
- Include-order sensitivity (pre-existing, unrelated): `<vector>` before
  `<iostream>` fails live parse (`istreambuf_iterator<> expects 2 argument(s)`).
- Corpora for iteration: tmp/r4_sub.msnap (unfenced testsubscript, FAILS bind),
  tmp/r4_sub_fenced.msnap (fenced, binds clean — made with tmp/madc-b2b2cf5e),
  tmp/red_r4_dataptr.mad (scalar reducer: binds OK but shows the same-family
  pointer WARNINGS at stl_vector.h:2015). CIR dumps: tmp/r4_cir_dump.txt
  (unfenced), tmp/r4_cir_dump_fenced.txt; extracted def:
  tmp/r4_mti_string_def.txt; normalized: tmp/r4_mti_*_norm.txt.

## Exit criteria (unchanged from the handoff)

Both fences stay lifted; fulltest + bind gate (incl. subbind) + packed suite
697/0/0/16 + blob all green; the derived `_M_realloc_insert__mti` resolves
`_Alloc_traits::construct` to the derived class's member template at bind.
Fix must be generic by KIND (qualified-static-call resolution on restored
state), never keyed to a name.
