# FOREST / DATA-SUBSTRATE — READ THIS FULL DOCUMENT BEFORE DOING ANYTHING ELSE

**⚠️ STOP. If you are about to touch anything under the forest / freeze / bind /
type-table / substrate surface — or you just resumed after a compaction — read
this ENTIRE document first. It is self-contained on purpose. Do NOT start from
the scattered plan docs; they are background, linked at the very bottom. This one
file is the source of truth. Skimming this file is how every serious mistake in
this campaign happened.**

**Date:** 2026-07-05 · **Status:** AUTHORITATIVE, single-source. Supersedes the
"read-order" section of `2026-07-05-forest-phase6-HANDOFF.md`.

---

## RESUME HERE — current state & open threads (2026-07-06)

**🏛️ DIRECTION CHANGED 2026-07-06 — B3 ARENA REARCHITECTURE (owner GO). READ FIRST:
`docs/plans/2026-07-06-forest-arena-native-scoping.md` (decision + measured sizes) and
`docs/plans/2026-07-06-forest-b3-record-layout-DESIGN.md` (step-1 record schema).** The
per-category swizzle tail (v6→v16, ~1,540 LOC across `cir_freeze.{h,cpp}`+`madc_cir.cpp`+
`parser.cpp`) is being REPLACED, not extended. B3 = make `DataDef` STORAGE arena-native (flat
POD records in one contiguous arena; cross-refs = INDICES not pointers; strings = intern
offsets; `std::vector`/`std::map` fields = `(begin,count)` slices), while PRESERVING the
virtual READ interface (`DataDef` stays polymorphic to callers) → SAVE = dump the arena,
LOAD = `mmap` + wrap thin handles, a new field serializes FREE. Measured: B1 (arena-native
LIVE objects, zero-swizzle) = ~1,200 reasoned conversions across the whole front end — REJECTED.
B3 = ~548 mutation sites (~75% in 6 named fns: `TokenSTRUCT::parse`, `TokenCLASS::parse`,
`parseFunction`, `TokenTEMPLATE::parse`, `register_skipped_class_template_function`,
`cir_builder` clone/symbol-binding) + ~13 builder-method bodies; the ~1,130 READ sites are
UNCHANGED (why B3 is ~half of B1). KEY: the serialization format ALREADY IS a POD-record arena
(`madcdis/pod_record.h` + `cir_forest_type_record/_member/_base/_method` via `pod_append`) —
B3 PROMOTES it to canonical live storage, NOT a new arena; `materialize_types`(313) +
`cir_forest_fill_type_records`(275) are the two field-copy directions it deletes. **#23
(whole-`<string>`-TU byte-identity) closes BY CONSTRUCTION**: its RC1 (emission ORDER — freeze
enumerates classes from `struct_map`/alpha + dependency-fixpoint, losing parse decl order) and
RC2 (**FREE-FUNCTION-DECL gap** — a bound `<cstdio>` restores types/globals but NO free
functions, so `printf`→dlsym implicit-variadic fallback; NOT the earlier "i_5/I_5 size/cast
nuance" framing, which probing DISPROVED) both dissolve into the arena (built in decl order;
free functions = just another record kind). SEQUENCE: (1) record schema [DESIGN done] →
(2) arena+thin-handle layer behind `FEATURE_FOREST_ARENA` guard (route 13 builders + 137 allocs
+ a NEW `FuncDefBuilder`) → (3) convert ~411 scattered writes → (4) switch freeze/restore to
dump/`mmap`, DELETE `materialize_types`+`fill_type_records`+per-category restore + the 3
delete-segments, build `SEG_DEFS` in decl order → (5) gate (fulltest + forest_bind_gate +
test_cir_freeze + torture). `develop` @ `4a237ff4` (v16 `21dc0c81` + doc fix, NOT pushed).
**FOUNDATION COMMITTED `168bbf9c` (2026-07-06, additive, develop green): `include/cir_arena.h` (DefArena +
FrozenDefArena + defrec/memberrec/baserec/methodrec/vbaserec/vgrouprec/paramrec) + `tests/unit/test_cir_arena.cpp`
(7/120, incl. all 3 hard-tier flattenings + a REAL snapshot byte round-trip). Schema-validation phase DONE; the
load-bearing B3 risk (lossless round-trip of a DataDef's complete state, zero live pointers) is retired.**

**CORRECTION (owner, end of session): the static-init "wall" I raised is NOT real — pre-defined DataDef
singletons (ddINT/ddCHAR/ddCHARptr) are pinned-id CONSTANTS (madc_stamp_primitive_type_ids: VOID=1, INT=5, …;
project types ≥ MADC_TYPEID_PROJECT_BASE), never saved / never handle-backed; only DYNAMIC types (born during
parse, when a Program+arena exist) get arena records. So the "Option C" pivot is WITHDRAWN and B3 handle-backing
STANDS. Model clarified: the arena is keyed by type_id (the spine) — a cross-ref stores the referent's type_id;
pinned → resolve to the process global via madc_type_from_id (not in arena), project → an arena record.
**SLICE 1a DONE (2026-07-06, additive, test-only — `include/cir_arena.h` + `tests/unit/test_cir_arena.cpp`):**
the schema is refined to the real type-id model. `defs[]` is now id-addressed by PROJECT-ID SLOT (slot k ⟺
project id PROJECT_BASE+k, mirroring the live id_table<DataDef> the freeze already walks by tid=base+i,
cir_freeze.cpp:439). Cross-ref fields are renamed `*_id` and hold the referent's SERIALIZED type_id (the
forest_serialize_type_id policy). A PINNED primitive is NEVER recorded — referenced by its pinned id, resolved
on load via madc_type_from_id to the process global; `int` is a pinned-id ref, not a DK_PRIM record (the
correction, now enforced by an `assert_no_primitive_records` gate). test_cir_arena.cpp models the policy through
the PUBLIC chokepoints (madc_type_id_for/madc_type_from_id + a local project id_table bound as the active table,
as parser.cpp:9260 anticipates for tests); 6 cases / 157 assertions GREEN, full unit suite GREEN. Zero production
code touched. **SLICE 1b/1c DONE (2026-07-06):** `Program` now carries a `DefArena forest_arena` + a runtime
`forest_arena_enabled` flag (default OFF → bin/madc unchanged), and `Program::getPointerType` — the single
memoized funnel where a NEW project `DataDefPTR` is born (parser.cpp:13431; the well-known void*/char*/int*
globals return early as pinned, never recorded) — write-throughs a `DK_PTR` record when the flag is on. The
write reuses the ONE cross-ref policy (`forest_serialize_type_id`, exposed via a `Program::forest_arena_record_ptr`
method in madc_cir.cpp so there is NO parallel encoder); the pointer keeps `base_type` as its read-cache so its
~97 reads are UNCHANGED. **The guard is a RUNTIME flag, NOT the doc's #ifdef** — parser.o is shared between
bin/madc and the units, so a compile guard could not be on-for-test/off-for-ship in one build (owner-approved
deviation). Gate: test_cir_arena 7 cases/170 assertions (new write-through case asserts reads-unchanged +
record-populated at the ptr's project-id slot, ref0=pointee id); fulltest 680/0/0/16 + all forest gates
byte-identical to live (flag off = zero change, byte-identical by construction).
**SLICE 1d DONE (2026-07-06):** the other two unary derived-type funnels now write-through too —
`getReferenceType` → `DK_REF` and `getConstType` → `DK_CONST`. `forest_arena_record_ptr` was generalized to
`forest_arena_record_unary(DataDef*)`, which dispatches on the actual type (REF checked before PTR, since REF
is-a PTR) for the record kind + reads the operand from `base_type` — one method, one policy, still reusing
`forest_serialize_type_id`. The collapse/idempotency early-returns in getReferenceType/getConstType never reach
the write, so an existing ref/const is not re-recorded. Gate: test_cir_arena 8 cases/185 assertions (new
REF/CONST case: reads-unchanged [is_reference/is_const/base_type] + DK_REF/DK_CONST records at distinct
project-id slots, ref0 = operand id); fulltest 680/0/0/16 + all forest gates byte-identical to live; no new
warnings. **1a–1d PUSHED (origin/develop @ `5baf2c0f`, 2026-07-06):** the whole unary derived-type tier
(PTR/REF/CONST) is banked off-machine. **NEXT (SLICE 1e+, the high-touch stretch) — PLAN READY:** see
`docs/plans/2026-07-06-forest-b3-slice-1e-aggregate-writethrough-PLAN.md`. 1e = aggregate write-through
(TokenSTRUCT::parse after finalize @parser.cpp:23982; TokenCLASS::parse after the layout trio @27134–27136 →
`DK_STRUCT`/`DK_CLASS` via a new `Program::forest_arena_record_aggregate`, PER-AGGREGATE + NON-recursive, cross-refs
by type-id, mirroring test_cir_arena's `arena_ensure` + reusing `forest_serialize_type_id`). 1f = FuncDef → `DK_FUNC`
via a new FuncDefBuilder (the one class with no builder funnel). Then a later slice flips READS onto the record +
deletes the per-category freeze (`materialize_types` + `cir_forest_fill_type_records` + the 3 delete-segments) —
`#23` whole-TU byte-identity closes by construction. develop @ `5baf2c0f` == origin.**

**Everything BELOW this banner is pre-B3 history — accurate, but superseded in DIRECTION.**

**Landed + pushed on `develop`:** v8 inline method bodies (`0599f3ec`), v9
pointer/reference/const members (`968f4a29`), v10 namespace-qualified types
(`4a656bbd`). Each is a "one mechanism widened" extension of the type-table
serialization; all gated byte-identical (`MADC_DUMP_MIR` == live, torture 50-name).

**#14 helper-sharing refactor + libmadc export — DONE (banked, pushed):**
- **Step 1 (`dc06a92b`)** — the forest save/load POD boilerplate is now the PUBLIC
  `madc::dis::pod_record` codec (`include/madcdis/pod_record.h`:
  `pod_words`/`pod_append`/`pod_read`), and the load-side typeid→`DataDef*` swizzle
  is one `forest_swizzle_type` helper. Byte-identical (9/9 `forest_bind_gate`, torture
  by construction).
- **Step 2a (`2e690830`)** — `include/libmadc/dis.h`, the curated public **C++** surface
  a host includes to reach the substrate (pod_record / intern_table / id_table / value_pool
  / snapshot); round-trip test `tests/unit/test_libmadc_dis.cpp`. "Reusable via libmadc" ✓.
- **Follow-ons (NOT started, recorded in the export-surface plan):** 2b C-host `extern "C"`
  shim (speculative — no C consumer yet; keep it thin+late per cpp-first-api). 2c script-facing
  export is **BLOCKED on a pre-existing parser bug** — a `.mad` can't `#include` any substrate
  header until madc parses `<cstdint>`'s `using ::int_fast8_t;` (a separate PARSER track,
  parked; do NOT fold it into forest work). Owner decision (2026-07-06): bank 2a, return to #13.

**ACTIVE THREAD — #13, reframed by the owner into the SYSTEMATIC COMPLETE-FIELD PASS.**
The corpus (`std::string`/`std::vector`) kept surfacing "we dropped field X" bugs
because the freeze serialized a **hand-picked SUBSET** of a `DataDefSTRUCT`/`CLASS`'s
state. Owner's call (2026-07-06): stop the reactive per-field slices (a smell) and
**serialize a DataDef's COMPLETE state** — every semantic field, via the two swizzle
kinds we already have (`DataDef*`→typeid, scalar→verbatim, `TokenBase*`→node-ref). The
pointer-swizzle model is RIGHT and already covers this; the gap was never the swizzle,
it was incomplete field coverage. (Considered + rejected: ids-exclusively / flat-POD
DataDefs — wouldn't fix field coverage, is a core rearchitecture, and the type-table
design already chose "keep the objects, table the ids" because DataDefs are polymorphic.
That flat-POD substrate is the long-term endgame, NOT this slice.)

- **A0 LANDED (`c43d4556`, format v11):** anonymous-aggregate grouping (each nameless
  sub-aggregate = its own `CIR_TYPEK_STRUCT/UNION` record + a `cir_forest_type_anon`
  group slice via `pod_append`; load rebuilds `anonymous_aggregates`, sub-agg excluded
  from `_restored` so it is never emitted standalone) + the layout scalars
  (`pack`/`tag_explicit_align`/`is_anonymous`/`reverse_scalar_storage`/`has_anon_aggregate`).
  The `has_anon_aggregate` skip is GONE. Without this a struct with an anon union bound
  with the overlap LOST (a silent miscompile — `sizeof` right, `i`/`buf` no longer shared).
  Gated: `forest_bind_gate` **10/10** (hardened `anon` case WRITES via `i`, READS via `buf`),
  `forest_selfexe_gate` v11, `test_cir_freeze` 325, MADC_DUMP_MIR byte-identical.
- **Freeze-completeness diagnostic LANDED (`e1beda7d`):** `madc -v --freeze=…` lists every
  complete non-poly class the fixpoint could NOT record + the blocking base/member (with a
  per-member type classification). The "what is the freeze dropping and why" probe — USE IT.
- **A2 LANDED (`3e2499ed`, save-side only):** a member / operand / method-param / return /
  typedef-underlying whose type is a btSimple SCALAR-primitive typedef alias materialized
  fresh by tsubst (the real `std::string`'s `size_type` == `unsigned long`, given a distinct
  PROJECT id) used to make `cir_forest_record_derived` bail → the whole `basic_string<char>`
  product dropped. **Design Q settled by GROUND TRUTH** (not guessed): `bin/madc --emit=c11`
  emits `unsigned long _M_string_length;` with NO `size_type` typedef in the C at all —
  `append_type_specs` renders a scalar SOLELY from `rawtype()` — so a scalar alias is
  byte-identical to the pinned primitive of its rawtype. FIX = resolve-to-underlying (minimal
  machinery): `forest_pinned_primitive_id` maps such an alias to its pinned slot,
  `forest_serialize_type_id` serializes members/operands/params/returns/typedef-underlyings
  through it, load resolves it via `madc_type_from_id` UNCHANGED (no record). Pointers/refs
  (DataDefPTR inherits btSimple + is_integer over the POINTEE rawtype — a real bug caught
  mid-impl), const, enum, SIMD, template-param, _Complex are excluded structurally (each its
  own concept/slice). Normal compile path UNTOUCHED (freeze-only reach) → torture byte-identical
  by construction. Gated: `basic_string<char>` now RECORDS (UNRECORDED 34→30); fulltest
  680/0/0/16; `forest_bind_gate` 10/10; `test_cir_freeze` **21/339** (new A2 case freezes real
  `<string>`, asserts basic_string<char> materializes, every member resolved, size_type→8-byte int).
- **A1 LANDED (`14642e78`, save-side only):** the v10 namespace walk in
  `cir_forest_fill_type_records` SKIPPED an alias whose ns key ≠ the record name — `std::string`
  is exactly that (`namespace_datatype_map["std"]["string"]` → the `basic_string<char,…>` product;
  key "string" ≠ the product's mangled record name). A1 turns the skip into an EMIT: for a
  namespaced alias to a RECORDED aggregate, emit a namespaced `CIR_TYPEK_TYPEDEF` record
  (name=alias, namespace_id=ns, ref0=product id). materialize_types Pass 3 (already ns-aware) →
  a typedef CirRestoredType (ns set, underlying = product); `forest_restore_decls` (already
  ns-aware) registers `namespace_datatype_map[ns][alias]`. GENERIC (every namespaced alias to a
  recorded aggregate: string/wstring/u16string/u32string/string_view/…, never keyed on a name).
  **Ground truth:** live emits NO `typedef … string;` — the variable is declared with the PRODUCT
  name and `std::string` is a parse-time NAME alias only — so this restores name resolution, not
  a C typedef. (`sizeof(std::string)` failing in live = a separate pre-existing expression-path
  limitation, NOT forest.) Save-side only (freeze reach) → torture byte-identical by construction.
  Gated: fulltest 680/0/0/16; `forest_bind_gate` 10/10; `test_cir_freeze` **22/347** (new A1 case:
  `std::string` materializes as a namespaced typedef whose underlying is the basic_string<char> product).
- **v12 LANDED (2026-07-06) — the FIRST corpus class BINDS end-to-end: `std::string`.** The freeze
  SKIPPED a class's ctors (`disp==cdd->name`/empty-disp), dtor (`disp[0]=='~'`), and operators, so a
  restored `basic_string<char>` had an EMPTY `cdd->ctors` → binding `std::string s;` failed
  `no matching constructor`. v12 classifies each method STRUCTURALLY (ctor = member of `cdd->ctors`;
  dtor = the `class_own_dtor` var — a `~` `method_map` key whose Variable is in `methods`; operator =
  display begins `operator`), and serializes the LIBRARY ones (concrete `emit_symbol` like
  `…C1Ev`/`…D1Ev`/`…aSEPKc`) with `CIR_METHF_CTOR`/`CIR_METHF_DTOR`. LOAD (`materialize_types`)
  attaches a `CIR_METHF_CTOR` method to `cdd->ctors` (allows empty display), the dtor's `~`-key to
  `method_map` (its own `method_display_name` is empty — the key rides `display_id`), operators to
  `method_map[display]` — so `select_ctor_overload`, `class_own_dtor`, and assignment resolution all
  see parse-equivalent state. One more load-fidelity gap fixed: the dtor is referenced ONLY via the
  scope-exit `cleanup` attribute (never a direct call), so its extern proto comes solely from the
  Pass-0.75 sweep over `funcdef_map`; a parsed dtor is in `funcdef_map`, a restored one was not, so
  the cleanup referenced an UNDECLARED symbol → c2mir defaulted it to `int()` (spurious return slot).
  `forest_restore_decls` now registers the restored dtor in `funcdef_map` keyed by its `emit_symbol`
  → the dtor is declared `void`, the call has no return slot, `main` matches live (13 locals).
  **RESULT:** freeze `<string>`, `--forest-bind` a `std::string s; s="hello"; s.size()` consumer →
  constructs + assigns + sizes + destroys, output `len=5` == live == g++, cross-process, genuinely
  bound (`#include <string> bound to grove unit 46`), NO re-parse. Gated: `forest_bind_gate`
  **11/11** (new `strbind` RUNTIME-CORRECTNESS case); `test_cir_freeze` **23/359** (new v12 case:
  restored `basic_string<char>` has a default ctor in `ctors`, a `class_own_dtor`-discoverable dtor
  with a D1 symbol, and `operator=`); fulltest green; single-class byte-identity gates unregressed;
  freeze/bind-only reach → torture byte-identical by construction.
- **v13 LANDED (2026-07-06) — restore a bound header's file-scope CLASS-typed global variables +
  `__madc_global_init`.** The forest serialized TYPES only; a header's inline globals are a separate
  category, so binding `<string>` omitted `in_place`/`piecewise_construct`/`allocator_arg`/`ignore` and
  the `__madc_global_init` that runs their ctors (`main` never called it). v13 serializes each
  file-scope CLASS-typed global (`cir_forest_global_record` = name + type-id + flags; same predicate as
  `collect_global_ctors`) whose class is RECORDED; NO initializer is stored — the default ctor is
  synthesized from the class's v12 ctor set. Load (`materialize_types`) swizzles the type-id back and
  fills `restored_globals()`. RESTORE is DEFERRED: `forest_restore_decls` runs during lexer `#include`
  handling BEFORE `tkProgram` exists, so it STAGES the globals in `forest_pending_globals`, and
  `flush_forest_pending_globals()` (called at the end of `tokenize()`, after `tkProgram` is created)
  rebuilds each into `tkProgram->variables` + a `dkGlobalVar` TopDecl — then the EXISTING emission
  passes (dkGlobalVar storage + `collect_global_ctors` + `__madc_global_init` synthesis) emit them as a
  live parse does. A class that can't be default-constructed at emit (`has_user_ctor && ctors.empty()`
  — a bodyless DEFAULTED ctor v12 skipped, e.g. `in_place_t`) cleanly LACKS its global (never a
  no-matching-ctor error). RESULT: the bound `<string>` consumer now emits `__madc_global_init` +
  `piecewise_construct`/`allocator_arg`/`ignore` (+ their ctor exports), matching live; `main` calls
  `__madc_global_init`. Gated: `forest_bind_gate` 11/11 (strbind runtime-correct; single-class
  byte-identical unregressed); `test_cir_freeze` 24/375 (new v13 case: `restored_globals()` lists the
  tag globals, `piecewise_construct` : `piecewise_construct_t`); the flush is a no-op on a live compile
  (`forest_pending_globals` empty) → normal path untouched, torture byte-identical by construction.
- **v14 LANDED (2026-07-06) — restore a bound header's file-scope SCALAR-const global init VALUES.**
  v13 covered only CLASS-typed globals (`decl=NULL`, default-ctor synthesized). A scalar-const global
  (`hardware_constructive/destructive_interference_size = 64` in `<new>`) has NO ctor — its init is a
  compile-time constant baked into the data segment (live emits `hardware_*: u64 64` + `export`), so v14
  serializes the VALUE. `cir_forest_global_record` gains `gflags` (`CIR_GLOBALF_SCALAR_INIT`) + `int64_t
  init_value`; the freeze collects a non-class, non-function file-scope global that has a `dkGlobalVar`
  TopDecl whose initializer folds to an integer literal (a function / non-integer / non-foldable init /
  unresolvable type cleanly LACKS). GROUND TRUTH probe: the ONLY such scalar candidates in `<string>`'s
  closure are the 2 `hardware_*` — matches live's emitted set exactly, so NO "only in BIND" extras. Load
  swizzles the type-id (a pinned scalar via `madc_type_from_id`) + carries `init_value` into
  `restored_globals()`; `flush_forest_pending_globals()` builds a `dkGlobalVar` decl whose `initialize`
  is a bare `TokenInt(init_value)` — the same shape `var_decl` consumes for a live `T name = value;` — so
  the existing pass emits `hardware_*: u64 64` **byte-identically to live** (a constant data item, no
  `__madc_global_init` entry). Gated: `forest_bind_gate` 11/11 (strbind now ALSO asserts the bound
  `<string>` emits `hardware_destructive_interference_size: u64 64` cross-process — a whole-TU
  byte-identity SLICE, not the whole TU); `test_cir_freeze` 25/390 (new v14 case: `restored_globals()`
  lists the scalar global with `CIR_GLOBALF_SCALAR_INIT` + `init_value == 64`); fulltest exit-0 / all
  ratchets+oracles GREEN; flush is a no-op on a live compile → normal path untouched, torture
  byte-identical by construction. RESULT: the bound-`<string>` "only in LIVE" data set dropped 3 → 1.
- **v15 LANDED (2026-07-06, task #20) — serialize a class's OWN dtor even when it has neither an
  external `emit_symbol` NOR a producer-emitted body; kills the synth-dtor overshoot.** GROUND TRUTH
  (Pass-1.6 probe): the "only in BIND" set was 11 spurious trivial `X___dtor` func-defs
  (`_Save_errno`, `__new_allocator_*`, `allocator_*`, wide-char `basic_string_char16/32_t`) — and the
  discriminator was NOT struct_map membership (bind's set is a SUBSET of live's) but the `user_dtor`
  predicate: LIVE has `class_own_dtor` non-NULL (a `~` method_map key) → Pass 1.6 synthesizes NO
  `Cls___dtor`; BIND had it NULL → Pass 1.6 synthesized a spurious trivial dtor. Root cause: v12's
  is_special skip (`emit_symbol.empty() && !has_body`) dropped a class's inline dtor the *producer*
  never emitted (unreferenced) — so the restored class looked dtor-less. Fix (freeze-side, one line):
  do NOT skip a **dtor** in that state — serialize it DECLARATION-ONLY (`CIR_METHF_DTOR`, no body, no
  symbol); load re-keys `method_map` with the `~` tag → `class_own_dtor` finds it → no synth, matching
  live; it is emitted by no pass (declaration_only + unreferenced), also matching live. Ctors/operators
  in the same state still skip (a ctor's `{}`/trivial construction is the separate #22 concern; 145
  skipped ctors/ops verified untouched). Format bumped **v14 → v15** (no layout change — the freeze
  CONTENT changed, so a stale v14 container lacking these dtor records would re-introduce the overshoot).
  RESULT: the bound-`<string>` func + export sets now MATCH live EXACTLY (29 == 29, zero either
  direction); the only remaining "only in LIVE" item is the `in_place` data item (#22). Gated:
  `forest_bind_gate` 11/11 (strbind now ALSO asserts NO spurious `_Save_errno___dtor`); `test_cir_freeze`
  25/390; fulltest exit-0 / all ratchets+oracles+self-exe GREEN; freeze-only reach (cir_forest_append_methods
  runs only under `--freeze`) → normal path untouched, torture byte-identical by construction.
- **v16 LANDED (2026-07-06, task #22) — serialize file-scope CLASS-global INITIALIZER FORMS + `nvsize`, and
  stop emitting restored classes via the wrong struct-def path.** GROUND TRUTH (layered probes overturned
  the stale "just restore in_place" framing — the gap was FOUR sub-bugs, all fixed):
  1. **Init form** (`cir_forest_global_record.gflags` gains `CIR_GLOBALF_CLASS_VALUE_INIT` / `CLASS_COPY_TEMP`):
     the freeze inspects each class-global's parsed `TokenDecl` and records whether its RHS is a `TokenObjTemp`
     (`T x = T()` → COPY_TEMP, stack-temp+ctor+copy) or a self-`TokenVar` (`T x{}` value-init → VALUE_INIT,
     trivially-copyable self-copy via `try_implicit_copy_construct`, needs NO ctor). Flush rebuilds the matching
     `TokenAssign` RHS so `global_ctor_call` emits byte-identically to live. v13's `decl=NULL` had made the
     built-in path default-construct DIRECTLY on the global; live builds a temp/self-copy. **`in_place` binds
     for free** (VALUE_INIT bypasses the flush `ctors.empty()` guard — a self-copy needs no serialized ctor).
  2. **`nvsize`** (added to `cir_forest_type_record`, restored verbatim): a functional-construction temp's
     alloca + the struct-copy are sized by `DataDefCLASS::nvsize`; it was never serialized (restored 0), so an
     empty tag class's global init emitted alloca 0 + no copy.
  3. **THE BIG ONE — struct-def emission path:** `forest_restore_decls` pushed restored CLASSES as `dkStruct`
     TopDecls, so bind emitted them EARLY via Pass 0's plain `struct_def` (no empty-class `char __pad0[1]`,
     c2mir sizes the struct 0) and marked `emitted_structs`, SHADOWING Pass 0.5's `class_member_list`. A live
     parse keeps classes in `struct_map` ONLY (never `top_decls`). Fix: don't push the `dkStruct` TopDecl for
     a class → Pass 0.5 emits it via `class_member_list` (with `__pad0`, vptr, flattened members) EXACTLY like
     live. (`class_member_list` ran 0× in bind before this, 423× in live.) This was pre-existing (every bound
     empty system class was affected); v16's temp+copy just exposed it.
  RESULT: `in_place` restored (`bss` + `export`), the whole `__madc_global_init` body + all struct defs now
  **byte-identical to live**; the whole-`<string>`-TU `MADC_DUMP_MIR` diff dropped **147 → 103** lines.
  Gated: `forest_bind_gate` 11/11 (strbind now ALSO asserts `in_place: bss` restored); `test_cir_freeze`
  **26/403** (new v16 case: `in_place`→VALUE_INIT, `piecewise_construct`→COPY_TEMP, restored class `nvsize>0`);
  fulltest exit-0 / all ratchets+oracles+self-exe GREEN; freeze/bind-only reach + the flush is a no-op on a
  live compile → normal path untouched, torture byte-identical by construction. Format bumped **v15 → v16**
  (the record grew a `uint32_t nvsize` + the gflags carry the new form bits — a stale v15 container would
  re-introduce the direct-construct shape + drop in_place).
- **NEXT — the residual whole-`<string>`-TU gap is now PRE-EXISTING emission ORDER + a local-name nuance
  (task #23), NOT globals.** The remaining 103 diff lines are dominated by TWO pre-existing divergences
  (VERIFIED at the v15 baseline — my v16 did NOT introduce them; the v15 diff was 147 lines and already
  contained both, just masked because the strbind gate asserted func/export SETS, which are order-independent):
  1. **Function-emission ORDER**: ~6 trivial tag ctors + allocator dtors
     (`allocator_arg_t__allocator_arg_t`, `piecewise_construct_t__piecewise_construct_t`,
     `__new_allocator_char16/32_t___dtor`, `allocator_char16/32_t___dtor`) emit BEFORE `main` in bind, AFTER
     `main` in live → cascades into `protoN` renumbering + every `call protoN` reference. This is a bind vs
     live emission-pass ORDER divergence (which deferred/late list these land in), independent of #22's global
     work — investigate the late ctor/dtor emission passes (Pass 1.5/1.6/1.9x) and where `main` is appended.
  2. **`i_5` vs `I_5`**: main's `(int)s.size()` result local names differ (a signedness/type nuance in how the
     bound `size()` return / the cast resolves) — a method-return-type restore nuance in the consumer's own code.
  Closing #23 (both) would make the whole `<string>` TU byte-identical. Reducers staged: `tmp/cs_producer.cpp`
  / `tmp/cs_consumer.cpp`. The `strbind` gate stays runtime-correctness + the v14 scalar-slice + v15
  no-overshoot + v16 in_place-restored checks until #23 closes, then it can assert whole-TU byte-identity.
- **Polymorphic classes stay a SEPARATE, explicit boundary** (not folded in): the `_pmr_`
  `basic_string` variant is blocked by the polymorphic `std::pmr::memory_resource` (vtable).
  Serializing vtable/typeinfo is its own slice — a coherent subsystem, not a reactive drop.

**COURSE-RETURN (anti-drift):** the SAVE/LOAD state model governs everything (§0, §1, §7).
Do the systematic complete-field pass (serialize the whole object, not a subset), one field
at a time down the diagnostic's list, each gated byte-identical. No re-parse, no separate
module, no re-derivation, no parallel format. **A2 (`3e2499ed`), A1 (`14642e78`), v12
(`e30acb5b`, ctor/dtor/operator serialization + dtor funcdef_map registration), v13
(`47409b35`, class-typed file-scope globals + `__madc_global_init`), v14 (`115db29d`,
scalar-const global init values), and v15 (`cb651109`, dtor-completeness / synth-dtor
overshoot fix) are COMMITTED AND PUSHED (everything through v15 is on `origin/develop`).
**v16 (`21dc0c81`, class-global init-forms + nvsize + Pass-0.5 struct-def-path fix, task
#22) is COMMITTED LOCAL — NOT yet pushed** (verified 2026-07-06). To see the unpushed
commit(s): `git log --oneline origin/develop..develop`. **Next session: read THIS file +
the memory `feedback_forest_load_never_reparse` IN FULL first, as always.**

---

## 0. The five things that, if you get them wrong, you will cause damage

1. **madc NEVER LOWERS.** There is no lowering pass. The `cir_node` tree is the
   complete, high-level thing.
2. **`cir_node` *contains* c2mir's `node_t` as its base.** c2mir only reads the
   `node_t` fields — a keyhole. Everything else on the `cir_node` (the DataDef
   graph, tokens, provenance, the whole high-level structure) is **invisible to
   c2mir, NOT absent.** Tree-2 is handed to c2mir **unlowered**; c2mir just can't
   see the rest.
3. **Tree-1 (the forest ROM) has EVERYTHING.** If the freeze drops something,
   the freeze is BROKEN — fix the freeze, do not "reconstruct" or "re-derive" the
   dropped thing on load.
4. **There is ONE serialization machinery** (`madc::dis` snapshot container over
   `intern_table` / `arena` / `id_table` / `value_pool`). Never hand-roll a
   second serializer or a parallel format beside it.
5. **Serialize pointers AS IDS; swizzle back to pointers on LOAD.** Load never
   re-parses, never re-computes layout, never re-derives. It reads stored values
   verbatim and relinks ids → pointers.

If any change you are about to make violates one of these, it is wrong no matter
how small.

---

## 1. The mental model (the two trees)

- **Tree-1 — immutable ROM.** Parsed ONCE: every template pattern **and the whole
  header corpus**. Never mutated, never handed to c2mir. It is (a) the COPY
  SOURCE for template instantiation and (b) **the serialized form = the
  header-forest.** g++ analogue: `DECL_SAVED_TREE`, generalized to the corpus.
- **Tree-2 — mutable, per-TU.** Built by `tsubst` = **copy a Tree-1 subtree into
  FRESH `node_t`s + substitute template params.** Handed to c2mir, which writes
  its own sema (`node->attr`) onto the `node_t` base in place. NEVER re-parsed.

Parse-once is DONE and RELEASED (v0.33.0): templates instantiate by copy+subst,
never re-parse. That is what makes freezing Tree-1 *sufficient* — a loaded forest
can instantiate with no token text.

**c2mir hard constraint (why Tree-2 is fresh):** c2mir writes `attr` on every
`node_t` it traverses, so each instantiation needs its OWN `node_t` structure
(copy, never splice a live Tree-1 node_t; two instantiations never share one).
The back-reference to Tree-1 rides in the `cir_node` EXTENSION (invisible to
c2mir). "Some ROM, some RAM": the heavy madc info stays ROM in Tree-1 and is
*referenced by id*; only the `node_t` base + substituted fields are fresh RAM.

---

## 2. The one substrate (already built — USE IT, do not reinvent)

All of these are index-based specifically so they serialize / mmap **with zero
fixup** (dump the block, map it back, ids still valid):

- `include/madcdis/intern_table.h` — index-linked (NOT pointer-chained) string/
  identifier table: three contiguous blocks (byte arena / entry array / bucket
  array); the `id` IS the entry index; collisions chain by index. A
  `frozen_intern_table` gives a read-only view over the three serialized blocks.
- `include/madcdis/arena.h` — bump allocator; pointer-stable; drop-all on
  destruction. `TokenArena` HAS-A `arena`.
- `include/madcdis/id_table.h` — segmented stable `uint32`-id ⇄ object* registry.
  This IS the compiler's **type table** (`Program::type_id_for` / `type_from_id`,
  `project_types`). Interface: `add(T*)→id`, `get(id)→T*`, `base()`, `size()`.
- `include/madcdis/value_pool` — deduping `uint32` handles over wide-literal
  limbs (128-bit literals etc.). Same three-block zero-fixup discipline.
- `include/madcdis/snapshot.h` (`src/madcdis_snapshot.cpp`) — **THE container:**
  header / 16-aligned segment frames (zstd/zlib per-segment) / directory / footer
  (magic + directory offset). Two placements: a standalone file, or **appended to
  the `madc` binary** (footer read from EOF via `/proc/self/exe`). This is the
  ONE format family for the forest blob, the `.madh` PCH, and `snapshot://`.

`cir_freeze.{h,cpp}` sits ON TOP of this: it flattens the live `cir_node` DLIST
tree into POD records + a child-index pool and stages them as snapshot segments.

---

## 3. The type table (the DataDef identity + serialization spine)

From `docs/plans/2026-06-12-type-table-value-abi-design.md` §2 (the doc I skipped
that caused the confusion):

```
id 0                  invalid / "no type"
[1, 0x100)            primitives — pinned ABI constants, process-invariant,
                      resolve in ANY process with no table
[0x100, 0x01000000)   SYSTEM segment — "types from the embedded system forest,
                      frozen at forest build time, identical across runs."
                      THIS SEGMENT IS THE FOREST'S.
[0x01000000, 2^32)    project segment — per-Program user typedefs/structs/
                      classes/enums; deterministic registration order
```

- `uint32 typeid` is the canonical type identity. Every `DataDef` carries
  `type_id` (`datadef.h`), stamped at registration.
- §6.4 (a *later campaign*, i.e. THIS work): **"P4 forest type-refs serialize as
  ids + table segments."** §2 AOT constraint: **"serialize the table so ids
  survive a save/load round trip."**
- **So the DataDef graph is serialized by serializing the TABLE**, with every
  pointer field written as an id (member/base type → typeid; names → intern
  handle) and swizzled back on load. NOT by a bespoke format, NOT by re-deriving
  from the lowered view (there is no lowered view — see §0.1/§0.2).

---

## 4. What is already built and gated (do not redo)

- **Track A (substrate spine), DONE:** `frozen_intern_table` + the snapshot
  container (`62c1b91b`); `value_pool` + 128-bit literals (`7d7c0e5d`); wide-fold
  spine (`956e7030`).
- **B1 (`0b1e618a`):** every `cir_node` EXTENSION pointer is now position-
  independent — `datadef` → `datadef_id` (typeid), `char*` → intern handle,
  `tree1_origin` → `cir_ref{seg,idx}` behind the one `madc_cir_node_for(ref)`
  resolver. Raw pointers remain ONLY in the c2mir-visible `node_t` base (op links,
  `u.s`, `attr`), which map to handles at freeze time. **This is the "cir_node
  half" of the swizzle. It proves the pattern.**
- **B2 (`b62089ad`):** single-segment freeze/thaw — flatten the tree to POD
  records + child pool → snapshot segment → load → resolve-on-touch →
  materialize real pointer-linked `cir_node`s at the c2mir edge.
- **B3 (`75830eec`):** multi-segment forest, append-to-binary, `/proc/self/exe`
  loader, context-hash pin, **cross-process closure** (container carries its own
  intern pool + positions + libs + the typeid→**name** closure). A FRESH process
  runs a frozen program with no parse. `--run-frozen` MEASURED 20× vs live.
- **B4a (`54aff2ce`):** grove payload v2 (per-unit token slices, a decl index,
  PP-export events, include edges) + pack-time recording + oracles. **NOTE: the
  token-slice + decl-index-as-token-ranges parts are the RE-PARSE approach and
  are being retired — see §6.**

**The gap B3 explicitly left:** the typeid→**name** closure serializes each
DataDef's id and NAME and **nothing else** (`cir_freeze.cpp`, the `type_names`
loop). So cross-process `madc_type_from_id` returns NULL for forest types — the
DataDefs' members/offsets/bases/methods/vtable were **left behind.** That is the
bug this campaign fixes (§5).

---

## 5. THE CURRENT TASK — make the freeze complete (serialize the whole type table)

**Bug:** the freeze drops DataDef content (§4). **Fix:** serialize each project/
system DataDef's COMPLETE content as a type-table segment, pointer fields as ids,
**verbatim**; swizzle on load into real DataDef objects so `madc_type_from_id`
resolves; bind points the parser's symbol tables at them.

**LANDED `1b2d7171` (2026-07-05): the mechanism + typedef/struct/union coverage.**
Format v6, freeze serializes the complete type table (full content, ids), load
swizzles to complete DataDef objects VERBATIM, bind points symbol tables at them;
the parallel `decl_records`/`struct_members` format + `finalize`-regeneration are
retired. Verbatim load means unnamed-bitfield-gap/packed structs now bind
correctly (the old rebuild refused them). Gated green (fulltest 679/0/0/16 +
forest_bind_gate cross-process + torture 50-name byte-identical).

**LANDED (2026-07-05): `CIR_TYPEK_CLASS` — class DATA LAYOUT.** Freeze serializes a
non-polymorphic class's members (verbatim, inheritance-flattened) + direct bases
(`cir_forest_type_base`: subobject offset / access / is_primary) + flags
(`from_system_header`/`has_user_ctor`/`has_user_dtor`) + size/align; load allocates
a `DataDefCLASS` and swizzles base ids → loaded `DataDefCLASS*` verbatim; restore
registers it into `struct_map` + `datatype_map` + a `dkStruct` TopDecl. A class
carrying state this slice does not serialize (a vtable / polymorphic, a union
layout, or a virtual base) is skipped WHOLE → loud error at use, never mis-link.
Two freeze subtleties handled: (a) `TokenCLASS::parse` registers classes in
`struct_map` only (NOT `top_decls`, unlike structs), and (b) lazy id-stamping is
not definition order — so classes are enumerated from `struct_map` via a
dependency-driven **fixpoint** (bases/member-types recorded before their users).
The shared `cir_forest_serialize_members`/`_bases`/`_record_aggregate` helpers back
both the struct and class paths. Gated green (fulltest 679/0/0/16 +
`forest_bind_gate` `class` case: MI hierarchy binds cross-process, `sum=6 bb=2
sz=12` == live == g++, incl. nonzero base offset + upcast; `test_cir_freeze` 17/17).
**LANDED (2026-07-05): class METHOD DECLARATIONS (slice 3d, format v7).** Freeze
serializes each non-virtual method's decl — mangled call name (`Counter__get`),
display name (`get`), return typeid, explicit-param typeids, `emit_symbol`, and
flags (const/varargs/void-params/static) — into the type record (`method_begin`/
`method_count` + `cir_forest_type_method`). Load rebuilds a `FuncDef`+`Variable`,
reconstructing the hidden `__this` (param 0 of a non-static method) as a pointer to
the owning class, and attaches it to `method_map`/`methods`; `forest_restore_decls`
needs no change (methods ride the class object). Ctors/dtors/operators/templates and
methods with an unserializable return/param are skipped individually (not bailing
the class). A member call on a bound class now **RESOLVES**. Gated: `test_cir_freeze`
slice-3d unit test (get/add reconstruct with correct signatures + `__this`);
`forest_bind_gate` still green; fulltest green.

**⚠️ THE MENTAL MODEL — SAVE STATE / LOAD STATE (owner, emphatic, 2026-07-05):**
The forest is a video-game-emulator **save state / load state**, nothing more.
SAVE = parse headers once into the memory arenas, write the arenas to disk.
LOAD = next time, do NOT parse — read the arenas back; you are now in the **EXACT
SAME in-memory state** parsing would have produced. So downstream there is **NO
"bind path" vs "parse path"** — ONE state reached two ways; translate/emit/compile/
link all run UNCHANGED. **The task is a BUG HUNT, not a design:** *what is the
LOADED state missing vs a freshly-PARSED state?* Fix save or load so the loaded
arenas equal the parsed arenas. **The machinery already exists — invent nothing.**

**🚩 DRIFT (STOP if you catch yourself doing ANY of these):** a second/separate
module, an import-set filter, a synthesized `N_MODULE`, a "grove emission" step,
cross-module linking, re-lowering, re-parsing, re-deriving, or bind producing a
different tree/module structure than parse. If bind ≠ parse downstream, it is
WRONG regardless of output.

**RETRACTED (was itself drift):** a prior version of this section proposed
building a *separate grove `N_MODULE` + `MIR_load_module` + cross-module link* to
resolve the inline-method call. That is a DIFFERENT tree-2 than parse produces —
dead. Do not follow it.

**THE TEST is state-equivalence, not output:** `MADC_DUMP_MIR` under
`--forest-bind` must be **byte-identical** to the live (parsed) dump. Live puts a
class's inline methods as **func-defs, `export`ed, in the ONE consumer module next
to `main`**; current bind emits them as `import`s (bodies missing) → link fails
`import of undefined item Counter__get`. `output == 15` is NOT sufficient.

**The gap, per method kind (both = "does the loaded state match the parsed state?"):**
- **External / system methods** (the corpus, e.g. `std::string::size`): even in
  PARSE mode these carry `emit_symbol` (a real libstdc++ Itanium symbol) and NO
  body — the call links to the `.so`. So a loaded decl-only FuncDef + `emit_symbol`
  ALREADY matches parse. Correct as landed. **Pointer-member serialization LANDED
  (format v9, 2026-07-06, task #10)** — see the block below — so a corpus class's
  pointer members (`std::string`/`std::vector` internals) now serialize; what remains
  for the corpus is exercising their ctors / dtors / template instantiations end to
  end (a follow-on that builds ON this primitive, not a blocker for it).
- **Inline user methods** (e.g. `Counter::get`): in PARSE mode these produce a
  full func-def-WITH-BODY in the consumer's one module. **✅ LANDED (v8, 2026-07-05):**
  the save now records each method's Tree-1 body location and the load reconnects it,
  so `translate_module` emits the body into the ONE consumer module — the bind's
  `MADC_DUMP_MIR` is **byte-identical** to a live compile. See the LANDED block below.

**INLINE vs LIBRARY — the dividing line (owner, 2026-07-05):** a LIBRARY method
(`std::string::size`) has its body in a `.so`; the header only had a declaration,
so a saved declaration + `emit_symbol` that links to the `.so` is correct (3d). An
INLINE method (`Counter::get`) has NO `.so` — its body exists only in the header,
so the **body IS Tree-1 content**. When a TU USES the class, the inline body is
**COPIED out of Tree-1 into that TU's Tree-2** (fresh `node_t`) and compiled into
that TU's own module — exactly like a **template instantiation** and like a normal
`#include` (each TU emits its own copy of an inline body). That copy-from-Tree-1
machinery ALREADY EXISTS: the parse-once / template-instantiation copy path
(`copy_cir_subtree` / tsubst). So `declaration_only` (3d, for ALL methods) is wrong
for INLINE methods — an inline method must carry its **body as a Tree-1 recipe**.

**✅ LANDED — INLINE method body save/load (format v8, 2026-07-05).** The mechanism,
end to end, with NO re-parse / NO separate module / NO new machinery beyond the load:
- **SAVE** (`cir_freeze_partitioned` + `cir_forest_append_methods`): the AST freeze
  already assigns every node a `(unit, idx)`; it now also indexes every `N_FUNC_DEF`'s
  symbol → `(unit, idx)`. A method whose mangled symbol has a func-def in that index is
  INLINE → its record gets `CIR_METHF_HAS_BODY` + `body_unit`/`body_idx`. A symbol with
  no func-def is a LIBRARY method → declaration-only + `emit_symbol` (3d, unchanged).
  This structural presence/absence IS the inline-vs-library discriminator (no names).
- **LOAD** (`materialize_types`): a `HAS_BODY` method's FuncDef carries `has_forest_body`
  + the body location and is **not** `declaration_only`.
- **EMIT** (`CirBuilder::translate_module`, guarded by `prog->forest_chain` non-empty so
  the normal compile path is byte-identical): a **reachability fixpoint** seeds with the
  bound methods in `referenced_funcs`, then follows each emitted body's calls to sibling
  bound methods (`cir_collect_call_callees`) — so a forward / mutual reference emits the
  callee too (not just a direct one). Each reachable method is materialized via
  `bind_forest->node_for(unit, idx)` (deserialize the saved lowered node = the copy into
  Tree-2; `set_c2m` gives the parse-time forest the session c2m) and emitted into the ONE
  consumer module in declaration order, each preceded by a **forward prototype** built by
  copying the loaded def's own return-spec + declarator (real param names) — matching the
  live proto pass so a forward reference sees a real declaration, not an implicit-int
  default (a silent miscompile). Gates: `forest_bind_gate` `method` (`get=15`) + `fwd`
  (`chain=41`, forward/mutual ref) both **MIR byte-identical to live**; `test_cir_freeze`
  18/296; fulltest green.

**✅ LANDED — POINTER-member serialization (format v9, 2026-07-06, task #10).** A
member / method-param / method-return / typedef-underlying type that is a pointer,
reference, or const-qualified type is a `DataDefPTR` / `DataDefREF` / `DataDefCONST`
over an operand. Before v9 the member pass BAILED on any such type that was not a
pinned pointer slot (`void*`/`char*`/`int*`), dropping the WHOLE aggregate — a bound
member access then failed `Unidentified member`. v9 serializes each as its OWN table
entry — a derived-type record `CIR_TYPEK_POINTER` / `REFERENCE` / `CONST` with `ref0`
= the operand's typeid and no member payload — the SAME "table entry, pointer field
as an id, swizzle on load" shape as a typedef record (one mechanism widened, NOT a
parallel format).
- **SAVE** (`cir_forest_record_derived`, madc_cir.cpp): on a member/param/return/
  underlying type that is neither a pinned primitive nor an already-`recorded`
  aggregate, record the derived type transitively (recurse to its operand first; a
  self-referential `Node *next` is allowed because the enclosing aggregate counts as
  recordable), deduped globally by `recorded`. An unrecorded aggregate still bails →
  the outer fixpoint retries (unchanged). This replaces the old inline
  primitive-or-recorded check in `cir_forest_serialize_members` and the method
  param/return `serializable` lambda.
- **LOAD** (`materialize_types` pass 1b): a fixpoint reconstructs each derived record
  via `new DataDefPTR/REF/CONST(operand)`, operand-before-derived — so chains
  (`T**`, `const T*`) and self-references resolve (the self-pointer's `base_type` IS
  the aggregate allocated in pass 1). Members/params/typedef-underlying then swizzle
  through `by_id` exactly as before.
- The normal compile path is UNTOUCHED — every changed function is reachable only via
  `--freeze` / `--forest-bind` — so the gcc-torture failset is byte-identical by
  construction (verified: 50 names, 0 timeouts).
- Gates: `forest_bind_gate` `ptr` case (self-ref + sibling-aggregate + scalar
  `double*`; `v=10 dv=2.5 nv=20 px=3 sz=32` == live == g++, and `MADC_DUMP_MIR`
  byte-identical to live); `test_cir_freeze` 19/310 (a self-reference reconstructs to
  the SAME object: `selfp->base_type == node`); fulltest green.

**✅ LANDED — NAMESPACE-qualified type restoration (format v10, 2026-07-06, task #12).**
A type defined inside a namespace (`namespace N { struct P {...}; }`, and ultimately
`std::…`) used to bind only into the FLAT `struct_map`/`datatype_map` — so a bound
`N::P` failed `Unknown namespace 'N'`. A type's defining namespace lives ONLY in
`namespace_datatype_map`'s KEY (no DataDef field carries it), so:
- **SAVE** (`cir_forest_fill_type_records`, madc_cir.cpp): after building all records,
  reverse-walk `prog->namespace_datatype_map` (the verbatim source of truth) and stamp
  each record's new `namespace_id` — guarded to stamp only when the ns key interns to
  the SAME id as the record name (the non-template guarantee); a template-instantiation
  product (key/name carries `<`) is skipped, a follow-on. First defining namespace wins.
- **LOAD** (`materialize_types`): `CirRestoredType.ns` = the record's namespace (or NULL).
- **RESTORE** (`forest_restore_decls`, parser.cpp): a namespaced type ALSO registers into
  `namespace_map[ns]` (so the namespace resolves as a scope) + `namespace_datatype_map[ns]
  [name]` (reusing the branch's TokenDataType for typedef/class, matching the live path
  where the SAME object goes into both maps), in ADDITION to the flat registration.
- Normal compile path UNTOUCHED (freeze/bind-only) → torture 50-name failset byte-identical
  (verified, 0 timeouts). Gates: `forest_bind_gate` `ns` case (`namespace N { struct P }`,
  `s=16` == live == g++, `MADC_DUMP_MIR` byte-identical to live); `test_cir_freeze` 20/325
  (rt.ns == "N"; restore populates namespace_map + namespace_datatype_map); fulltest green.
- **The corpus follow-on:** `std::string` is a typedef in `std` for `basic_string<char,…>`
  — a TEMPLATE-instantiation product (key/name carries `<`), which this slice deliberately
  skips. Binding the `std::string`/`std::vector` corpus end to end now needs the
  template-instantiation naming (serialize the instantiation product + its `std::` typedef)
  + ctor/dtor emission — the next slice, building ON namespace + pointer-member serialization.

NOTE (freeze fidelity, orthogonal): the `struct`-keyword-with-base `sizeof` bug was
FIXED (`6f008d0c`) — see the memory. Residual: exact vbase-diamond member offsets vs
g++ (tail-padding reuse, task #8), no failing test.

NOTE (freeze fidelity, orthogonal): the `struct`-keyword-with-base `sizeof` bug was
FIXED (`6f008d0c`) — see the memory. Residual: exact vbase-diamond member offsets vs
g++ (tail-padding reuse, task #8), no failing test.

Concretely (the shape, for the class widening and any further types):
- `cir_freeze.h`: `cir_forest_type_record` (id, kind, name_id, spelling_id, size,
  align, flags, ref0, member/base slices) + `cir_forest_type_member` (verbatim
  offset/count/access/origin/bitfield) + `cir_forest_type_base`. Replaces the
  name-only closure.
- `cir_freeze.cpp` **serializer:** for each type, dispatch on `basetype()` /
  subclass and write full content — names → intern ids, member/base types →
  typeids.
- `cir_freeze.cpp` **loader:** two passes — allocate all DataDef objects (so ids
  are known), then swizzle (relink member/base typeids → the loaded DataDef*s,
  primitives via `madc_type_from_id`), filling member vectors **verbatim**. A
  forest-local `map<typeid, DataDef*>` is the swizzle table.
- `parser.cpp forest_restore_decls`: register the loaded DataDefs by name into
  `struct_map` / `datatype_map` / `namespace_datatype_map` / `user_typedef_names`
  (and, for emission, `struct_map` — Pass 0.5 iterates it; structs also push a
  `dkStruct` TopDecl for Pass 0). **No rebuild, no `finalize`, no re-parse.**
- **RETIRE:** `cir_forest_decl_record`, `struct_members`, `cir_forest_collect_decls`
  (madc_cir.cpp), `cir_forest_plain_struct_faithful`, and the layout-regeneration
  in the old `forest_restore_decls` struct branch — the parallel format and the
  re-derivation.

**Coverage order (one mechanism, widened — NOT a parallel format each time):**
typedef → struct/union → class (data layout: members verbatim, bases, `has_vtable`
/`has_vptr_slot`/`from_system_header` flags, size/align). Class METHOD-CALL
dispatch needs external libstdc++ symbols (the class's methods bind to real
Itanium symbols; madc emits no bodies for `from_system_header`/`is_externally_defined`
classes) — that is a follow-on after the type/layout is complete, not a blocker
for the type serialization.

---

## 6. Retirement (the re-parse turn that was wrong)

`2026-07-04-forest-default-mode-design.md` chose "frozen token slices + re-parse"
and is **SUPERSEDED** — re-parse contradicts parse-once and §0.5. Retire, once the
type serialization covers the corpus:
- token slices (`SNAP_KIND_CIR_UNIT_TOKENS`), decl-index-as-token-ranges
  (`SNAP_KIND_CIR_DECL_INDEX`), any `parse_forest_slice` / cascade hooks.
- The B4b branch `feature/forest-b4b-bind-claude` (unmerged) — reference only.

KEEP: PP-export restore + include edges + branch-macros (macro restore + DAG walk
= "load, not re-derive"), the container/pools/closure, `CirFrozenForest::find_unit`,
the `--forest-bind` flag, forest-open-at-parse-time.

---

## 7. INVARIANTS (never violate — these are the whole point)

1. **Never re-parse at bind/load.** No `parseStatement`, no token re-lex. A lookup
   miss after load is a load-fidelity bug to FIX, never a re-parse to add.
2. **Never re-derive.** No `finalize()`/`compute_layout()` re-run on load — offsets
   and layout are serialized and loaded verbatim.
3. **Never a parallel format.** One serializer, the substrate. If you find yourself
   adding a `struct cir_forest_*_record` beside the type table, stop — extend the
   table serialization.
4. **The freeze must be COMPLETE.** If it drops something, fix the freeze.
5. **Fix at the deepest layer** (gcc/clang methodology); no name-keyed special cases.

---

## 8. GATE (every commit — correctness only, never perf-gate)

- `make -C src` clean, **no new warnings**.
- `make -C src fulltest` green at baseline **679/0/0/16** + all unit suites +
  ratchets + `forest_selfexe_gate` + the forest oracles.
- gcc.c-torture failset **byte-identical to `docs/parity/torture-failset-current.txt`**
  (currently 50 names after the `__int128_t` fix), 0 timeouts.
- `--emit=c11` byte-identical on representative TUs.
- **Cross-process:** freeze in one process, load+compile+run in a FRESH process,
  output == g++ (`scripts/forest_bind_gate.sh`, wired into fulltest).

---

## 9. Working discipline (repo-specific, easy to forget after a compaction)

- Branch `develop`; commit via `git commit -F -` heredoc; **stage files
  explicitly — never `git add -A`**; the untracked `mir-debug-support.md` is NOT
  ours, never stage it.
- No chained `&&` shell commands; no sleep/poll loops (use `run_in_background`);
  scratch/temp files under `tmp/` (gitignored) or `$CLAUDE_JOB_DIR/tmp`.
- Do NOT `/promote` develop→master.
- Commit trailers: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  the `Claude-Session:` line.

---

## 10. Background source docs (NOT required reading — this file subsumes them)

Only open these if you need a detail this file omits:
- `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` — §2 two trees, §6 phases,
  the g++ model. Phases 0–5 COMPLETE (v0.33.0); Phase 6 = this work.
- `2026-06-12-type-table-value-abi-design.md` — §2 segments, §6.4 "table segments",
  §2 AOT "serialize the table". **The doc whose §2/§6.4 are the swizzle mechanism.**
- `2026-06-13-embedded-ast-frontend-design.md` — "arena + u32 index handles, no
  internal pointers"; segmented mmap / zero-copy.
- `2026-07-04-data-substrate-first-customer-PLAN.md` — the governing track order
  (A → B → C); §0 "one serialization machinery"; landing history B1–B4a.
- `feedback_forest_load_never_reparse` (memory) — the one-line invariant.

**If this file and a background doc disagree, this file wins — then reconcile.**
