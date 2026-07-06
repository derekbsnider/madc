# Forest substrate: arena-native `DataDef` — scoping & decision record

**Date:** 2026-07-06
**Status:** SCOPING COMPLETE — decision pending owner go/no-go
**Context:** the forest save-state campaign (see `docs/plans/FOREST-SUBSTRATE-READ-FIRST.md`,
memory `feedback_forest_load_never_reparse`). Task #13 (corpus end-to-end) and #23
(whole-`<string>`-TU byte-identity) surfaced the question this doc answers.

---

## 1. The problem: the tail that grows forever

The forest freeze/restore has grown one *format version per DataDef feature* — v6→v16,
eleven versions, each a **4-file touch** (`cir_freeze.h` struct field + `madc_cir.cpp`
save clause + `cir_freeze.cpp` `materialize_types` clause + `parser.cpp` restore clause):

    v6 complete type table · v7 methods · v8 inline bodies · v9 ptr/ref/const ·
    v10 namespaces · v11 anon aggregates · v12 ctors/dtor/operators · v13 class globals ·
    v14 scalar-const globals · v15 dtor-completeness · v16 global init-forms + nvsize

Two live gaps (task #23, the whole-`<string>`-TU byte-identity residual) are the newest
instances of the same pattern:

- **RC1 (emission order):** a bound header's synthesized functions emit in a different
  order than a live parse → the emitted MIR diverges. Root: the freeze enumerates classes
  from `struct_map` (alphabetical) + a dependency fixpoint, and typedefs from
  `user_typedef_names` (a `std::set`, alphabetical) — both **lose parse declaration order**.
  Restore faithfully replays whatever order the freeze produced.
- **RC2 (free functions):** `printf` from a bound `<cstdio>` loses its real
  `int(const char*,...)` declaration and falls to the dlsym implicit-variadic fallback
  (`long(...)`). Root: **there is no free-function serialization path at all** — the freeze
  never touches `funcdef_map`. (This is a latent *correctness* bug too: a signed-`int` libc
  fn like `strcmp` under the fallback is a miscompile, per `embedded-headers.md`.)

The owner's diagnosis: this is "far too much fiddling, swizzling, and re-establishing from
tree-walking, instead of just SAVE and LOAD." Correct.

## 2. Root cause (one sentence)

The swizzling is **not** save/load coded wrong — it is the **tax for the live representation
being pointer- and vtable-based.** `DataDef` is polymorphic (23 virtual methods → a vptr on
every instance), its cross-references are raw heap pointers, and its symbol tables are
`std::map`s. None of that survives a raw dump, so the ~1,540 lines of freeze/restore
hand-emulate save/load one field-category at a time. The CIR/parse-tree freeze, by contrast,
is **already generic** (one record shape for any node) because `cir_node` *is* a flat
discriminated union — the tail exists *only* where the data structure isn't already flat.

## 3. Evidence (five read-only scoping sweeps, 2026-07-06)

### 3a. `DataDef` anatomy
- **Polymorphic**: base `DataDef` (`include/datadef.h:120`) declares 23 virtual methods
  (`is_pointer`, `rawtype`, `basetype`, `alignment`, …). Every instance carries a vptr.
- **Two tiers.** *Easy* (near-POD, 0–3 fields, no containers): base `DataDef`, `DataDefPTR`/
  `REF`/`CONST`, the 19 primitive scalars, `ENUM`/`SIMD`/`FPTR`/`MemberPtr`/`CArray`/`AUTO`/
  `TemplateParam`. *Hard* (STL-heavy): `DataDefSTRUCT` (~24 fields, 13 containers),
  `DataDefCLASS` (~49 incl. inherited, ~23 containers — incl. a **pointer-keyed** `vbase_offset`
  map and a doubly-nested `vtable_groups`), and `FuncDef` (`include/madc.h:57`, ~45 fields,
  ~18–20 containers up to 3 levels deep).
- `Variable` (`include/datatokens.h:61`) is **vtable-free** and already carries `name_sid`
  (an intern-pool id) — a preview of the target pattern.
- Runtime-only pointers (`DataDefCLASS::vtable`, `extern_ctor`, `Variable::data`) are execution
  state, **not** part of the static type graph — they don't serialize either way.

### 3b. `DataDef*` coupling surface (what B1 would cost)
- **~1,200 individually-reasoned conversions** to make the *live* objects arena-native:
  **535** `dynamic_cast<DataDefX*>` sites + **595** virtual-method call sites (21 methods;
  `is_pointer()` alone = 152) + **~138** allocation sites.
- **~80%** sit in `parser.cpp` + `cir_builder.cpp` (already >55% of the whole source tree).
  Plus `FuncDef::returns` is a C++ *reference* and `DataDef*` is the return type of
  `TokenBase::datadef()` — threaded across the entire AST node zoo.
- The runtime/library layer (`ns_*.cpp`, `cir_emit_c.cpp`, `madc_mir_backend.cpp`,
  exception runtime, value marshalling) has **0** DataDef references. Coupling is confined to
  the two front-end files.

### 3c. Declaration-order spine (what RC1 needs)
Three *partial* ordered spines already exist:
1. `Program::top_decls` (`madc.h:2455`) — true order, but only typedef/struct/union/global-var
   (no class, no function; `dkEnum` is dead).
2. `tkProgram->variables` (`madc.h:374`) — true order, covers globals **and free functions**
   (as `Variable`s wrapping `FuncDef*`), but no types.
3. B4a's `pack_decls` (`madc.h:2259`) — the **only single total order across all kinds**
   (typedef/struct/class/union/enum/function/var/template), already freeze-gated; names match
   the map keys. But its `kind` granularity is coarse (`pdkClass`/`pdkEnum` dead), it carries
   token-ranges from the retired re-parse design, and **restore ignores it today**.

Free functions live in `funcdef_map` + `tkProgram->variables` + `namespace_fn_overload_sets`
(overloads). The freeze touches **none** of this (`funcdef_map`: 0 freeze hits).

### 3d. The tail (what it costs / what B deletes)
- **~1,540 LOC** of per-category swizzle/restore: `cir_freeze.h` ~240, `cir_freeze.cpp` ~400
  (incl. the 313-line `materialize_types`), `madc_cir.cpp` ~685, `parser.cpp` ~218.
- Confined to the **DataDef type-graph + globals** — 3 segments (`CIR_TYPE_RECORDS`,
  `CIR_TYPE_PAYLOAD`, `CIR_GLOBALS`).
- **Kept regardless** (~1,100 LOC infra): the `madc::dis` intern-pool/`pod_record` codec, the
  unit directory + include-DAG edges, the PP-export/macro event stream, and the **generic
  node-freeze** half.
- Tests: ~900–1,200 of the 1,802 LOC in `tests/unit/test_cir_freeze.cpp` (16 of 26 cases)
  exercise the per-category machinery and would be rewritten against a new format.

### 3e. B3 mutation surface (the go/no-go number)
Making DataDef's **storage** arena-backed while preserving the read interface:
- **~548 mutation-touching sites**: **221** field-writes (194 `parser.cpp`, 27 `cir_builder.cpp`)
  + **190** container-mutations + **137** allocations. Plus **~13 builder-method bodies**.
- **>90% in 2 files**; **~75% inside 6 named functions** (`TokenSTRUCT::parse`,
  `TokenCLASS::parse`, `parseFunction`, `TokenTEMPLATE::parse`,
  `register_skipped_class_template_function`, and `cir_builder`'s clone / extern-symbol-binding
  helpers).
- The hardest structures are the **most concentrated**: `vbase_offset` + `vtable_groups`
  written from **5 call sites total**; the layout engine (`compute_layout` /
  `apply_member_layout` / `build_vtable_groups`) fires from **3**.
- The one gap: `FuncDef` has **no** builder methods (every write external, ~64 mutations for
  `parameters`) — but those cluster in the same 6 functions.
- **Verdict: BOUNDED**, an order of magnitude smaller and far more localized than B1.

## 4. The three options

| Option | What changes | Touches | Ends the tail? |
|---|---|---|---|
| **B1** arena-native *live* objects (literal zero-swizzle "mmap and use") | `DataDef*`→index, vtable→tag, everywhere | **~1,200** read-site conversions, whole front end + AST | Yes |
| **B3** arena-*backed* storage, virtual read-interface preserved | DataDef data → flat POD arena records + thin handles; SAVE=`memcpy`, LOAD=`mmap`+rebuild-handles | **~548** mutation sites + 13 methods, ~75% in 6 functions, 2 files | **Yes** — new scalar/index fields serialize for free |
| **B2** complete + declaration-ordered hand-serializer | fix RC1 order + add RC2 free-fn category, keep everything live | RC1 + RC2 patches | No — C++ has no reflection, so per-field serialize/load lines persist |

## 5. Decision & recommendation

**Recommend B3**, as a deliberate feature-guarded track (not more slices):

- It is the achievable form of the owner's "just SAVE and LOAD" — the arena *is* the saved
  form; the freeze becomes a generic dump and the load a generic `mmap` + handle rebuild.
- It **terminates the tail**: after B3, a new DataDef field is added to the POD record and is
  serialized for free — no 4-file touch, no byte-diff surprises, no per-category slices.
- It is **bounded** (~548 localized sites, ~13 methods, 2 files, ~75% in 6 functions) — an
  order of magnitude below B1's whole-front-end rewrite, because the read interface is preserved.
- **RC1 and RC2 dissolve into it**: the arena is built in declaration order during parse
  (RC1 gone); free functions become just another record kind (RC2 gone). Patching them into
  the current hand-serializer first (B2) would be throwaway — B3 deletes that code.

**Not drift.** B3 *replaces* the per-category swizzle with one canonical representation. The
invariant to hold: when B3 lands, the dependency-order type table and the per-category restore
code (§3d, the 3 delete-segments + `materialize_types`) are **deleted**, not kept beside it.

### Proposed sequence (each step feature-guarded, byte-identity as the gate)
1. **Record-layout design** — the POD record schema for `DataDef`/subclasses, `FuncDef`,
   `Variable`: scalars verbatim, `DataDef*`→arena index, `std::string`→intern offset,
   containers→`(begin,count)` slices into side-arenas. Design the 3 hard-tier flattenings
   (`vbase_offset` → parallel `(class-index,size_t)` array; `vtable_groups` → nested slices;
   `FuncDef`'s containers). **This is the crux; get it right before code.**
2. **Arena + thin-handle layer** behind a guard, live objects still working; route the 13
   builder methods + 137 allocations through it.
3. **Convert the ~411 scattered writes** (concentrated in the 6 named functions).
4. **Switch freeze/restore** to dump/`mmap` the arena; delete the ~1,540-line hand-serializer
   and the 3 delete-segments. Build the arena in declaration order (RC1) and include free
   functions (RC2) — both now automatic.
5. **Gate throughout**: `make -C src fulltest` + `scripts/forest_bind_gate.sh` +
   `bin/test_cir_freeze`; the whole-`<string>`-TU `MADC_DUMP_MIR` becomes byte-identical to
   live (closes #23 by construction), and the strbind gate can then assert whole-TU identity.

### Cost & risk
- One-time ~548-site migration + record design + arena infra + test rewrite (~900–1,200 LOC of
  `test_cir_freeze.cpp`). Multi-day-to-multi-week, concentrated in the two most complex files.
- Risk is concentrated where the code already is (parse-once/tsubst in `parser.cpp` /
  `cir_builder.cpp`); mitigated by the feature guard + byte-identity gate at every step.
- `FuncDef` (no builder funnel) is the awkward corner — worth a small builder API of its own
  as part of step 2.

### Alternative if B3 is deferred
Do B2 (complete + declaration-ordered hand-serializer) to close RC1/RC2 and reach #23 byte
identity now — accepting that the tail persists and that this code is deleted if B3 later lands.

---

*Scoping performed via five read-only sweeps; raw findings summarized in §3. No code changed.*
