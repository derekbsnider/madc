# FOREST CLOSE-OUT — the ordered worklist to CLOSE the embedded-header-forest plan

**Date:** 2026-07-07 · **Owner directive:** bring
`2026-06-22-embedded-header-forest-execution-plan.md` to a CLOSE. It has blocked
every other madc track for five weeks. This document is the execution order; the
`FOREST-SUBSTRATE-READ-FIRST.md` RESUME banner stays the mechanism reference.
**Read both IN FULL before touching anything. Do not re-derive either.**

---

## ⏯ RESUME HERE (fifth sitting, 2026-07-08 — v25 family burn-down in flight)

- **Git:** develop == origin/develop **@ddd13b01** (v25 batch 1 pushed); a
  SECOND v25 batch (families d + quoted-include binding, ~8 mechanisms) is
  IN THE WORKING TREE gated through bind gate 18/18 + test_cir_freeze 34/646 +
  test_cir_arena 11/316, awaiting its fulltest exit + re-soak + commit.
  Untracked `mir-debug-support.md` is NOT ours — never stage, never `git add -A`.
- **DONE this sitting (soak 623 → 648 OK at @ddd13b01; the uncommitted batch
  adds ≥6 more):**
  1. **@ddd13b01 (v25, format bump, PUSHED) — families a+b+c:** (a) DK_CARRAY
     array-type records (va_list, 9 tests — the typedef's underlying
     `struct tag[1]` had no record kind); (b) ctor-ARG raw-token runs on
     global records (partial_ordering constants, 7 — the v23 token-run
     mechanism; CIR_GLOBALF_CTOR_ARG_TOKENS; flush re-runs the args-list
     parse in the defining ns); (c) 1b body stamp = ROOT-vs-INCLUDE on the
     BODY's origin (funcdef_files) — embedded <ns_*> wrappers + user-header
     helpers restore (10 tests + testincludeonce); plus parseFunction now
     stamps decl_file on the DEFINITION path (bodied fns had NULL → the
     producer's OWN fns leaked as duplicate items), and QUOTED includes now
     BIND to the grove by resolved path (they always re-parsed BESIDE
     restored state → "Repeated item declaration").
  2. **UNCOMMITTED batch (family d + misc, all reducer-green):** inline-ns
     links serialize (DK_NSLINK) + flush re-runs
     mirror_inline_namespace_into_parent (std::__cxx11 members — stod,
     to_string — resolve in std); using-decl fn imports (DK_NSBIND —
     `using ::abort;`); peek_param_list_spelling now REWINDS (savepos) instead
     of consume+pushback — the pushback disabled the v23 default-arg capture
     for EVERY C++ namespace free fn (stod's `size_t* __idx = 0` → arity
     gate); loaded-body CALL-ARG fn references (`__stoa(strtod,...)` — bare
     decayed N_ID in the args N_LIST) load the producer's extern decl;
     object_returning_call_class classifies has_forest_body fns as
     madc-emitted retbuf ABI (to_string's 1-arg no-retbuf call); EXTERN_REF
     globals widened to non-class types (extern FILE *stdout); TAGLESS
     `typedef struct {...} div_t;` records its anon aggregate (the tagged
     hook never fired); embedded-header include FLAGS re-run at the bind
     site (lazy stdin/stdout registration).
- **THE IMMEDIATE NEXT TASK — in order:**
  1. Wait for fulltest exit → commit batch 2 (`git commit -F` with trailers)
     → push → re-soak (`bash scripts/forest_soak.sh`) → update this block
     with the new OK count.
  2. **DEFERRED-LAZY-BODY serialization** — the biggest remaining family
     mechanism: a system-header inline fn the producer never ODR-used has NO
     func-def in the frozen AST; live materializes it on use from
     Program::deferred_lazy_bodies (symbol → DeferredFunctionBody: 4 token
     vectors + var/method + file/line/col, madc.h ~2393). Serialize the map
     (the v20/v23 token-run form, arena tokbytes) + rebind var to the
     restored Variable at flush. Expected to fix: testincludenext (std::abs
     — resolution AND body), testheaderstringops (__gnu_cxx char_traits
     compare undefined import), testforeachheaderbody (basic_string copy-ctor
     import), likely teststringplus/teststrplusbody_realhdr c2mir checks.
  3. Remaining classified singles: make_preferred trio (testdefer/testfstream/
     testloop — path-ish alias overload selection on basic_string);
     DT_REG/SOCK_STREAM (glibc `enum { DT_REG=8 } + #define DT_REG DT_REG` —
     enum-constant macro visibility); testfdsetfromsystime + teststringparam
     (parse desyncs); teststructinit (BIND_DIFF); testsmaug_requests
     (last_log); smaug_requests_source + testmemclralignwide (c2mir checks);
     testfreezerun (freeze-inside-bind unit-count line — item-4 class, like
     testproject×freeze FREEZE_FAIL 5).
- **AFTER the families:** item 4 corpus pack + append-to-binary default-on
  (watch: --project × --freeze writes no container; freeze-under-bind counts
  differ) → item 5 lazy defrost → item 6 measure + stamp the 06-22 plan CLOSED.
- **Discipline:** unchanged (see DISCIPLINE section): batch, reducer-iterate,
  gates ONCE per batch; state INTO the substrate, loaded == parsed, re-run the
  ONE live derivation; no `&&` chains; `ulimit`+`timeout` every run; ONE heavy
  job at a time (never rebuild during fulltest — and never claim fulltest
  green before its EXIT CODE lands); commit `-F` with trailers; push every
  green batch. After compaction: read THIS doc + the READ-FIRST banner IN
  FULL before the first edit.

---

### Sitting-4 detail (mechanisms, superseded framings kept below)

- **v23 DEFAULT ARGUMENTS ([subbind] GREEN).** `bin/madc --freeze=S tests/testsubscript.mad` then
  `--forest-bind=S tests/testsubscript.mad` == live (byte-identical output)
  == `.expect`. Mechanism: parseFunction's `= expr` branches capture the raw
  source token range parseExpression consumes (TokenStream cursor tap, gated
  on forest_arena_enabled) into `FuncDef::param_default_tokens`; record_func
  serializes each run (.madh form) into the NEW arena tokbytes block (segment
  19) with a paramrec default-run reference (def_tok_off/bytes/count/file_id
  — layout change, v22→v23); the restore collects (index, run) pairs per
  FuncDef (`restored_param_defaults()`, owner + defining ns carried); the
  flush's LAST pass re-runs parseExpression over each run inside the
  reproduced live scope — parseFunction's param COMPOUND
  (method->owner_class → `= _S_max_align`), class_scope_stack, and
  NamespaceScope (`= io_errc::stream`). Both scope gaps were found by RUNNING
  the bar reducer (measured burn-down, not guessed).
- **Gates:** forest_bind_gate **18/18** (new **[subbind]** asserts bind ==
  live + every `.expect` line + bound-to-grove; strbind/strops whole-TU
  byte-identity UNREGRESSED), test_cir_freeze 32/569 (new v23 case),
  test_cir_arena 11/316, fulltest exit 0.
- **ITEM 3 SOAK — RUN + CLASSIFIED (2026-07-08, `scripts/forest_soak.sh`,
  ONE capped pass: live oracle → freeze → bind, rc+stdout compared):
  407 OK / 258 BIND_RC / 5 FREEZE_FAIL / 4 BIND_DIFF / 22 skips-by-design.**
  The failures collapse into FIVE state families (results: tmp/soak/results.tsv):
  1. **OWN-TU-ORIGIN LEAK — the dominant family (~250 of 267 non-OK).** The
     per-file harness snapshot carries the PRODUCER'S OWN user-code state
     (its classes/structs/typedefs/enums/globals), and the eager
     whole-container restore installs it into the consumer BEFORE the
     consumer's own code parses. Symptoms unify: "Struct 'X' already defined"
     (79); `class Foo` demoted to identifier (72 — the restored own-class
     made `Foo` lex as a DATATYPE token, so parseStatement's
     class-as-identifier heuristic fired); "Expecting '{' or identifier
     after struct" (43) + typedef/enum grammar errors (24) — same
     token-promotion desync; ~4 BIND_DIFFs (teststringglobal: own global
     restored default-constructed, the consumer's initializing decl skipped
     by the findVariable guard). Reducer: tmp/s6_class.cpp (freeze+bind:
     `#include <iostream>` + `class Foo{}` → "undeclared identifier
     'class'"). **🏛️ OWNER CORRECTION (2026-07-08, BINDING): the freeze's
     FOREST surface holds the #include files' state ONLY — NEVER the
     program's.** The discriminator is NOT system-vs-user (the bind gates
     deliberately freeze+bind USER headers, and testinclude binds its
     included helper): it is **TU-ROOT-origin vs #include-origin** — state
     DEFINED IN THE ROOT FILE (the program) never restores; state from any
     INCLUDED file does. THE FIX (v24): one TU-ROOT origin bit stamped at
     save (write-throughs run at parse completion where _parse_file IS the
     defining file; refresh preserves the stamp; typedef/global/template
     collectors derive from their token provenance) + ONE fence at every
     bind-restore surface (types/enums/typedefs registration, globals,
     free fns, templates, param-defaults). The program's records STAY in
     the arena — --run-frozen's cross-process typeid→name closure reads
     them — they are fenced only from the FOREST/bind view. Lazy defrost
     does NOT structurally fix this (a redefinition check is itself a
     lookup and would defrost the colliding record) — the fence is
     prerequisite hygiene for items 4/5.
  2. **Borrowed-language namespace registration (5):** `Unknown namespace
     'perl'/'python'/'ruby'/'rust'` (+ prefer directive) — binding the
     embedded `<ns_*>` headers skips the live registration side effect;
     re-run the ONE live registration over restored state.
  3. **std member re-exports (3):** `'to_string'/'abort' is not a member of
     namespace 'std'` — `using ::abort;`-style namespace re-export state +
     to_string overload sets not yet in the substrate.
  4. **Enum-constant macro visibility (1):** testdirent's DT_REG (glibc
     `enum { DT_REG=8 } + #define DT_REG DT_REG`).
  5. **--project × --freeze produces no container (5, FREEZE_FAIL rc=0):**
     testproject* — the multi-TU driver never reaches the freeze writer.
     Matters for item 4's default-on path (a --project compile must bind
     the embedded corpus too).
- **FAMILY 1 CLOSED (v24 TU-ROOT FENCE, 2026-07-08): re-soak 407 → 623 OK**
  (45 BIND_RC + 5 FREEZE_FAIL + 1 BIND_DIFF remain = 93% of runnable tests
  green). Reducer tmp/s6_class.cpp + testcommaincrement/teststringglobal/
  testctor all flipped. Gates: bind gate 18/18, test_cir_freeze 32/569,
  test_cir_arena 11/316 (fulltest run recorded in the commit).
- **REMAINING FAMILIES (from tmp/soak/results.tsv, burn down in this order):**
  a. **va_list (9 tests):** `use of undeclared identifier 'va_list'` —
     <stdarg.h> is a bucket-1 compiler header; its typedef registration side
     effect doesn't survive a bind.
  b. **partial_ordering() (7):** "no matching constructor for call to
     'partial_ordering()'" — <compare>'s value-init; the v12 bodyless
     DEFAULTED-ctor serialization gap class.
  c. **Borrowed-language namespaces (10):** Unknown namespace perl/python/
     ruby/rust (+prefer) + their fns (explode/array_push on madc/php tests) —
     binding <ns_*> embedded headers skips the live registration side effect.
  d. **std surface (5):** to_string/stod/abort — `using ::abort;` re-exports
     + <string> conversion-fn overload sets.
  e. **make_preferred / Unidentified member on basic_string (3):**
     testdefer/testfstream/testloop — a namespaced-alias identity nuance
     (path-ish alias resolving to basic_string).
  f. **MIR repeated-item / undefined import (4):** testsmaug_requests ×2
     (bug_calls), testincludeonce (greet_once_count — include-once semantics
     across freeze+bind), testheaderstringops (__gnu_cxx char_traits compare).
  g. **c2mir check errors (3):** testmemclralignwide, teststringplus,
     teststrplusbody_realhdr.
  h. **Enum-constant macros (2):** DT_REG (dirent), SOCK_STREAM (socket) —
     glibc `enum + #define X X` visibility.
  i. **Misc parse (2):** testfdsetfromsystime (struct grammar),
     teststringparam; + teststructinit (the 1 BIND_DIFF).
  j. **--project × --freeze (5, FREEZE_FAIL):** the multi-TU driver never
     reaches the freeze writer — item-4 concern, not a bind-state family.
- **THEN:** burn down families a→i by class → item 4 corpus pack +
  append-to-binary default-on → item 5 lazy defrost → item 6 measure + stamp
  the 06-22 plan CLOSED.
- **Discipline:** batch fixes, reducer-iterate, full gates ONCE per batch;
  state INTO the substrate (no new bespoke record families); loaded state ==
  parsed state (re-run the ONE live derivation over restored state when side
  effects are needed); no `&&` shell chains; `ulimit`+`timeout` every run; ONE
  heavy job at a time; commit `-F` with the standard trailers; push every
  green batch.

---

## DEFINITION OF DONE (the plan's own bars — when these hold, the plan CLOSES)

1. **Phase-3 gate (from the plan, verbatim):** real `<iostream>` / `<string>` /
   `<vector>` compile + run == g++ with the forest mmap'd; `madc -dM` macro
   parity vs `gcc -dM`; SMAUG C89 soak unaffected.
2. **Owner's bar:** real tests run on the forest —
   `bin/madc --freeze=S tests/testsubscript.mad` then
   `bin/madc --forest-bind=S tests/testsubscript.mad` == live == `.expect`.
3. **Phase 4:** build-time pack of the stdlib closure under the pinned config →
   **append to `bin/madc`** (machinery exists: B3 + `/proc/self/exe` +
   `forest_selfexe_gate`) → compiles bind from the embedded forest **by
   default, no flags**.
4. **Lazy defrost (standing plan, 06-22 + 06-13 docs):** only what a compile
   NEEDS materializes. Make the ONE restore path demand-keyed — the eager
   whole-container `forest_restore_decls` defrost is the gap. NO parallel lazy
   path beside the eager one.
5. **The number:** measured compile-time win on the `--project` SMAUG 51-TU
   build + `--show-stats` before/after, recorded in the plan doc, which is then
   stamped CLOSED.

## STATUS AGAINST THE PLAN (evidence, 2026-07-07)

- Phases 0–3 mechanism: **LANDED** (B1/B2/B3; append-to-binary + selfexe gate
  green; context-hash pin in). Phase-2 oracle exceeded: whole bound-`<string>`
  TU **byte-identical** to live (#23). New-specialization instantiation from
  restored tokens **works** (`vector<long>` from a `vector<int>` producer,
  `t=42 n=2` == live == g++, gate `[vecnewspec]`, 14/14).
- **`<map>` — GREEN (v22, 2026-07-07 second sitting): exact-match bind AND a
  new `map<long,long>` specialization run == live == g++ (gates `[mapbind]` +
  `[mapnewspec]`).** The first sitting's 4 fixes (wip @b2903b72: class-scope
  flush re-run; local_emit_name invariant; incomplete-empty `_Tuple_impl_1`;
  funcdef_files origin classification) + the v22 batch. **The "freeze-time
  body completeness" hypothesis was WRONG** — the snapshot already carried
  every needed body (or_probe showed `__o7` with `body=1`); all three
  undefined imports were LOAD-side:
  1. `cir_freeze.cpp` dropped every DF_IS_MEMBER_TEMPLATE methodrec ("the v6
     rule") — so the bodied instantiations (pair/tuple ctor `__oN`) AND the
     decl-only placeholders never restored; now VERBATIM at their saved ranks.
  2. The CIR_TMPLK_MEMBER flush re-ran register_skipped_class_template_function,
     re-MINTING a rank-shifted placeholder family (`__o6/__o7/...` collided
     with the restored ranks). Now it HYDRATES the restored placeholder
     (funcdef_map[record key]) via the extracted stamp_member_template_pattern
     — the ONE derivation shared with the live registration; full re-run stays
     as the missing-placeholder fallback.
  3. `_Auto_node`'s dtor is referenced ONLY via a scope-local's cleanup
     attribute inside a LOADED body — live registers the dtor at the lowering
     site a pre-built body never runs. `cir_collect_cleanup_attr_fns` now
     collects cleanup refs at the two FOREST materialization sites (live
     collector untouched — torture byte-identical by construction).
  4. (newspec) `'piecewise_construct' is not a member of namespace 'std'` —
     the restored global lacked its NAMESPACE binding; v22 records ns_id on
     cir_forest_global_record and the flush reproduces live's
     namespace_map[ns][name] registration. Format v21→v22.
  The 1b "has NO func-def in the partition" diagnostic remains useful but the
  151 listed symbols are mostly never-ODR-used funcdef entries that cleanly
  lack — NOT the map blocker. KNOWN NUANCE (measured, not blocking): a fresh
  map<long,long> instantiation binds pair(...)__o7's reference params to the
  derived node pointer without live's materialized base-ptr conversion temp —
  2 benign c2mir pointer warnings on the newspec bind (stdout == live == g++;
  live is warning-free). Same residual class as vec's proto/label numbering;
  fix = restored-method param-type fidelity at the construction site.
- **`<iostream>` GREEN (third sitting, [iobind] @ebfd30da — supersedes the
  paragraph below, kept for the original framing):** the load selection no
  longer excludes polymorphic classes; see item 2's PROGRESS 1/2/3 notes for
  the five families that closed it (fn-ptr DK_FPTR records, greatest-fixpoint
  closure, extern-ref globals, W2 overload-surface recapture, namespaced-
  typedef flat-map fix).
- ~~`<iostream>` FAILS — polymorphic classes~~ (the load selection still
  excludes DF_HAS_VTABLE/vptr/vbase classes — the v6 rule). Closing it =
  serialize/restore vtable + typeinfo + virtual-dispatch state from the arena
  records (the records already store vgroups/vbase offsets; the LOAD selection
  and the emission-side state are the work). This is the LONG POLE.

## EXECUTION ORDER (strict; one batch each; do not reorder)

1. **MAP:** burn down `pair<>` on `tmp/s4_map_prod.cpp` until exact-match bind
   == live; then a newspec map consumer (`map<long,long>` from an int
   producer); add gate case `[mapbind]` (+ newspec variant) to
   `forest_bind_gate.sh`.
2. **IOSTREAM (polymorphic):** widen the load selection + state for
   vtable-carrying classes; reducer = freeze/bind
   `#include <iostream> int main(){ std::cout << 7 << std::endl; }`; gate case
   `[iobind]`. Success flips `tests/testsubscript.mad` (owner's bar test).
   **MEASURED STARTING POINT (2026-07-07, reducer tmp/s5_io.cpp + tmp/s5_io.msnap):**
   the FREEZE already handles the whole 185-unit `<iostream>` corpus (34,236
   records); the bind fails at parse with `use of undeclared identifier 'cout'`.
   Two known state families behind it: (a) `ostream`/`ios_base` etc. are
   DF_HAS_VTABLE classes — the load selection (cir_freeze.cpp materialize, the
   v6 polymorphic exclusion) fences them; the arena records ALREADY carry
   vgroups/vbase offsets, so the work is load-side selection + whatever
   emission-side vtable/typeinfo state a live parse holds; (b) `std::cout` is a
   vfEXTERN global (a reference to the libstdc++ object, NOT a header-defined
   definition) — `cir_forest_fill_globals` explicitly skips vfEXTERN
   (madc_cir.cpp ~1196), so extern-global references are their own (small)
   restore category. Burn down with the map discipline: reducer-iterate,
   fix = state into the substrate, gates ONCE per batch.
   **IMPLEMENTATION MAP (traced 2026-07-07 — the save side is ALREADY COMPLETE;
   the load is mechanical):** the aggregate encoder (madc_cir.cpp ~929-1041)
   records own_block_off, the vbase run (vbaserec: class_id/offset, sorted),
   and the vgroup run (vgrouprec: owner_id/this_offset/slot-name-id run/
   addr_point — mirrors DataDefCLASS::VtableGroup datadef.h:895 exactly).
   LOAD work: (1) drop the vtable/vptr/vbase exclusions in the closure builder
   (cir_freeze.cpp ~1354-1367; keep DF_UNION_LAYOUT fenced); (2) pass-2 class
   restore adds own_block_off + has_vtable/has_vptr_slot (from flags) +
   cdd->vbase_offset[swizzled class]=offset + rebuilt vtable_groups (slot names
   from the pool) — base-before-derived ordering already covers virtual bases;
   (3) the extern-global category: a vfEXTERN class-typed global records as a
   reference (new CIR_GLOBALF_EXTERN_REF gflag bit, still v22 — v22 shipped
   this session, no container predates it), and the flush rebuilds live's
   extern-cout Variable shape (MEASURE live's cout Variable first: flags +
   storage_alias_name/Itanium binding — do not guess); (4) reducer to `7` ==
   live == g++, gate [iobind], batch gates once. For `cout << 7` the calls are
   non-virtual mangled-direct into libstdc++ (operator<<(int), endl) — madc-
   generated virtual DISPATCH is NOT on the reducer's path; vgroups/vbases are
   restored for state fidelity, not exercised by this reducer.
   **PROGRESS 2 (@77e5d5ba, gated 16/16 + units): cout RESOLVES + COMPILES —
   ONE import left (__ns_std_endl).** Three more families landed: DK_FPTR
   fn-ptr records (forest_arena_record_fptr at the member/param/return resolve
   loops; pass-1b rebuilds the declaration-only target signature);
   GREATEST-fixpoint closure (start all candidates, iteratively REMOVE — the
   additive fixpoint could never admit the basic_ios⇄basic_ostream pointer
   cycle via _M_tie); CIR_GLOBALF_EXTERN_REF globals (cout: vfEXTERN Variable
   verbatim + namespace_cpp_variable_symbol Itanium alias + nsmap binding +
   dkGlobalVar TopDecl → `extern ostream _ZSt4cout`). REMAINING (measured):
   bind emits a call to the PLACEHOLDER symbol __ns_std_endl where live
   instantiates endl<char> and calls _ZSt4endl... mangled-direct. The
   fn_template_map["std::endl"] pattern IS restored (tmpl_probe: kind=4,
   36 body tokens) and the placeholder record carries DECLARATION_ONLY|
   IS_FREE_FUNC; the instantiation entry
   (instantiate_namespace_fn_template_for_call, parser.cpp ~34670, hooked at
   the TokenCallFunc sites 10405/14589) gates on fd->namespace_name +
   function_display_name + the fn_template_map key, then
   try_instantiate_namespace_fn_template DEDUCES from the args — so the
   failure is inside deduction (suspect: deducing basic_ostream<_CharT,
   _Traits>& against the RESTORED basic_ostream_char product — template
   identity state: canonical_cpp_spelling / template_map linkage on the
   restored class) OR the placeholder fd's display/ns fields at flush.
   INSTRUMENT try_instantiate's failure path (MADC_DEBUG_FNTPL exists) and
   compare live-vs-bind -v at the endl call site. Reducer: tmp/s5_io.cpp +
   tmp/s5_io.msnap (re-freeze after save-side changes).
   **PROGRESS 3 — [iobind] GREEN: `std::cout << 7 << std::endl` binds and
   prints 7 == live == g++.** The endl gap was NOT deduction: live lowers
   `os << endl` through the W2 MANIPULATOR path (cir_builder
   try_free_operator_call ~10792), which reads
   Program::free_operator_overloads — string signature tables
   register_skipped_namespace_template_function derives at header parse
   (capture_free_operator_overload / capture_free_manipulator_overload /
   capture_free_function_overload, parser.cpp ~31796-32133) — state the
   restore never reproduced. FIX: recapture_free_overload_surfaces re-runs
   the SAME captures over each restored namespace FN/FN_DECL pattern's
   tokens (NamespaceScope guard; owner-classed patterns skip, as live).
   ALSO: restored NAMESPACED typedef records no longer write the FLAT
   datatype_map (live's record_typedef never does) — once the closure
   admitted polymorphic classes, std::pmr::string's alias record clobbered
   datatype_map["string"] and the bar test built the polymorphic_allocator
   variant.
   **OWNER'S BAR STATUS — ✅ CLOSED 2026-07-08 (v23, gate [subbind] 18/18):
   the default-arguments family below LANDED; see the RESUME block at the top
   for the mechanism + gates. Original framing kept:**
   ~~ONE measured family left — DEFAULT ARGUMENTS on restored method params.~~
   `string greet = "hello"` selects basic_string(const char*, const _Alloc&
   = _Alloc()) — live's FuncDef carries param_defaults, and BOTH the arity
   gate (required_param_count) and the call site read it (parser.cpp
   ~14697/~14947 push fd->param_defaults[i] STRAIGHT into the call's args).
   The arena's paramrec has no default state.
   **DESIGN REFINEMENT (traced): param_defaults[i] is a PARSED EXPRESSION
   TREE** — parseFunction's default branch (parser.cpp ~38977) runs
   `param_default = parseExpression(nextToken(), true)` — NOT lexed tokens,
   so the .madh flat-token codec cannot serialize it directly. The faithful
   shape (mirrors how template typeparam_defaults are token VECTORS):
   (1) parseFunction's `tkAssign` default branch ALSO captures the default's
   RAW SOURCE TOKEN RANGE (clone the tokens parseExpression consumes —
   stream-position tap) into a parallel `param_default_tokens` vector;
   (2) the DefArena grows a TOKEN-BYTES blob (dumped/mapped like its strings
   pool; new arena segment) and paramrec gains a default-run reference
   (off/len/count/file_id, .madh codec);
   (3) record_func serializes param_default_tokens;
   (4) the methodrec restore carries the run descriptors to the pending-func
   FLUSH (parser side — spelling pool + intern_file live there), which
   deserializes the tokens and re-runs parseExpression over them — the ONE
   live derivation, the register_skipped/stamp_member_template precedent.
   The snapshot already holds the full 202-method basic_string surface with
   Itanium ctor symbols (or_probe) — defaults are the only missing
   selection/synthesis state on the bar test's path.
   **PROGRESS 1 (2026-07-07 — steps 1+2 LANDED, gated):** the
   closure fence is dropped (vtable/vptr/vbase classes admitted; union-layout
   stays fenced) and pass 2 restores has_vtable/has_vptr_slot/own_block_off +
   vbase_offset + vtable_groups (get_word for slot-id runs). Gated: bind gate
   16/16 (strbind/strops whole-TU byte-identity UNREGRESSED — the widening
   reaches every bind case), test_cir_freeze 31/546, test_cir_arena 11/316.
   RESULT: polymorphic classes DO restore (ios_base__failure with base +
   methods), BUT the core trio — ios_base / basic_ios<char> /
   basic_ostream<char> — is STILL dropped by the member-chain fixpoint, so
   `cout` stays undeclared. **THE BLOCKER IS MEASURED (the new permanent
   `materialize closure: DROPPED <name> (member <m>)` -v diagnostic, the
   load-side twin of the freeze-completeness probe): ONE member family —
   FUNCTION-POINTER members — blocks the whole hierarchy.** ios_base drops on
   `_M_callbacks` (`_Callback_list*` whose `_M_fn` is a fn-ptr), and
   basic_ios/basic_ostream/basic_istream/basic_iostream all drop on the same
   chain; also `__jmp_buf_tag.__jmpbuf`, pthread cleanup structs (`__routine`),
   `_IO_cookie_io_functions_t.read`. Fn-ptr TYPES have no arena write-through:
   the getPointerType funnel records object pointees, but a pointer whose
   pointee is a FuncDef has no record → forest_serialize_type_id fails → the
   member chain fails. FIX SHAPE (state into the substrate, machinery exists):
   serialize a fn-ptr type as DK_PTR with ref0 = a DK_FUNC record of the
   signature (forest_arena_record_func already encodes FuncDefs); find where
   fn-ptr member DataDefs are born (the DataDefFUNCTIONptr/fn-ptr funnel) and
   write-through there; load-side arena_chain_ok + swizzle then resolve it
   like every derived type. THEN the extern-global category (step 3). freeze+bind every `tests/*.mad` (scripted, capped, ONE
   run); classify remaining failures by state family; burn down by class, not
   per test.
4. **PHASE 4 PACK + DEFAULT-ON:** corpus freeze driver (stdlib closure under
   the pinned config), append to the binary, bind-by-default from
   `/proc/self/exe` when no explicit forest is given; `-dM` parity check;
   fulltest + torture + SMAUG soak green with it ON.
5. **LAZY DEFROST:** make restore demand-keyed (name/unit-lookup-driven
   materialization; decl index + id-addressed arena already support it).
6. **MEASURE + CLOSE:** `--show-stats` and SMAUG 51-TU wall before/after;
   record in the 06-22 plan; stamp it CLOSED; release per cadence.

## DISCIPLINE (the rules that made 2026-07-07 produce six fixes in one day)

- **Batch. Reducer-iterate. Full gates ONCE per batch** (bind gate + the two
  unit suites + one fulltest at the end). NEVER per-tiny-change retests.
- **Fix shape: move the missing state INTO the substrate** (arena records /
  intern pool / the existing template-record segments). NO new bespoke record
  family, NO new phase/slice vocabulary, NO parallel implementations.
- Loaded state must EQUAL parsed state — never re-parse / re-derive /
  hand-order output. When restore needs live registration side effects,
  RE-RUN the one live registration function over restored tokens (the
  register_skipped_class_template_function precedent), never a copy of it.
- Repo hygiene: never `git add -A` (`mir-debug-support.md` is not ours); no
  `&&` chains; `ulimit -t` + `timeout` on every run; ONE heavy job at a time;
  commit `-F` with the standard trailers; push every green batch.
- After any compaction: read THIS doc + the READ-FIRST banner in full before
  the first edit. If tempted to redesign, STOP — it was all in the plan.

## TRIPWIRES (each of these burned a day at least once)

- Diagnostics MISATTRIBUTE the file (consumer path + header line). Trust the
  line/col + `-v` context, not the filename.
- `strings` on a snapshot proves nothing (segments are compressed) — probe
  with the real reader (`tmp/tmpl_probe.cpp`, build line in the banner).
- Unit-test binaries go stale — relink before trusting, never mid-fulltest.
- `"\x01fn-template-placeholder"` parses as `\x01f` + "n-…" — match the
  existing literal byte-for-byte.
- The `.madh` token codec is shared by include-PCH and forest runs; the PCH
  replay's keyword re-promotion can MASK codec infidelity — test via forest.
