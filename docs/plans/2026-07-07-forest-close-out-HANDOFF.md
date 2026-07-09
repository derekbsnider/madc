# FOREST CLOSE-OUT — the ordered worklist to CLOSE the embedded-header-forest plan

**Date:** 2026-07-07 · **Owner directive:** bring
`2026-06-22-embedded-header-forest-execution-plan.md` to a CLOSE. It has blocked
every other madc track for five weeks. This document is the execution order; the
`FOREST-SUBSTRATE-READ-FIRST.md` RESUME banner stays the mechanism reference.
**Read both IN FULL before touching anything. Do not re-derive either.**

---

## ⏯ RESUME HERE (ninth sitting cont., 2026-07-09 — ITEM 5 IN FLIGHT @69ea8c98: 4 drivers GREEN, 5 packed regressions to burn down)

**READ THIS FIRST AFTER COMPACTION. The item-5 implementation IS COMMITTED AND
PUSHED (@69ea8c98, fulltest 680/0/0/16 exit 0 + bind gate 18/18 byte-identity +
oracles — develop == origin/develop, tree clean except untracked
mir-debug-support.md, NEVER stage it).**

- **WHAT LANDED (@69ea8c98):** forest_restore_decls moved from mid-tokenize
  first-bind (both lexer.cpp bind sites ~3054/~3174) to the TOP of
  flush_forest_pending_globals (parser.cpp) — forest_chain_set is complete
  there. Every family filters through the B4a decl index:
  `forest_declared_bound` map (name → any declaring unit ∈ closure) +
  `forest_name_permitted(name, ns)` (qualified→bare→unindexed-passes) +
  `forest_product_permitted(dd)` (instantiation products judged by their
  canonical-spelling HEAD before '<'). Dropped owners skip their
  param-default re-derivations. EMPTY closure = whole-container (unit
  tests). PLUS restore-parity: STRUCT/UNION tags now stage the flat
  datatype pair (tag-as-type, struct≡class) — fixes the same-name tag
  typedef family (`typedef union pthread_attr_t pthread_attr_t;` has NO
  typedef record live; reducer tmp/myu.h + tmp/myu_c.cpp `7 16`).
- **PROVEN:** all 4 item-5 drivers GREEN bound to tmp/pack.msnap, byte-exact
  vs .expect (gnuattributemode `1 2 4 8`; memclralignwide `ok`;
  staticconstsibling `0 1 8 15`; servent full-diff rc=0);
  stoi/stod/multiret/subscript unregressed bound.
- **THE OPEN BURN-DOWN (packed suite 676 → 673/680): the filter OVER-DROPS
  for 5 NEW failures — testflock, teststat, teststatret, teststructinterop,
  testsmaug_requests (the struct-stat/glibc-struct family).** First move:
  reducer from teststat (`#include <sys/stat.h>` + `struct stat st;
  stat(path,&st)`) against tmp/pack.msnap (CURRENT at 69ea8c98, re-frozen).
  Hypotheses to TEST, not assume: (a) a name those TUs need is judged
  unbound (sys/stat.h in corpus? its unit's decl index entries?);
  (b) the NEW struct-tag datatype staging collides with the tests' own
  `struct stat` usage (tag staged as bare type may shadow/conflict with
  a live-parsed declaration — check the apply-order last-wins semantics);
  (c) an unindexed-name family that should have been dropped now stages
  MORE datatypes than before (the struct branch staging is NEW state).
  MADC debug: -v prints per-registration `forest_restore_decls:` lines;
  `--dump-forest=tmp/pack.msnap | grep declindex` for name→unit.
- **AFTER the burn-down:** re-pack (cp bin/madc tmp/madc_packed2 + bash
  scripts/forest_pack.sh tmp/madc_packed2), packed suite target 680/680,
  then item 6 measure (--no-forest-bind A/B on SMAUG 51-TU --project,
  --show-stats) + stamp the 06-22 plan CLOSED. Then follow-ons (__stoa
  _Ret collapse first — see prior block).
- **GOTCHAS for the resume:** re-freeze tmp/pack.msnap after ANY save-side
  change AND after rebuilds before bind tests; suites only after real code
  changes; ONE heavy job at a time; commit -F heredoc with the two
  trailers; NEVER git add -A.

---

## ⏯ RESUME HERE (ninth sitting, 2026-07-09 — stoi @c2777b4d + minmax @1e96a128 CLOSED; packed 676/680)

- **Git:** develop == origin/develop **@1e96a128** — TWO pushed fix batches this
  sitting, each gated fulltest 680/0/0/16 exit 0 (incl. bind gate 18/18
  whole-TU byte-identity + oracles) + packed suite **673 → 675 → 676/680**.
  BOTH families were latent LIVE bugs the forest bind exposed — fixed at the
  deepest layer, not in the forest.
- **MINMAX FAMILY @1e96a128 (testmultiret):** `using namespace std;` BEFORE a
  global fn definition left the name bound to the single-Variable IMPORT alias
  (storage_alias_name = __ns_std_minmax; global name index is first-wins), so
  calls resolved to a std placeholder that never materializes — ORDER-dependent
  ([namespace.udir]: directive members join unqualified lookup but do not HIDE
  a real global declaration). parseFunction's reuse branch now RECLAIMS the
  name for a plain global definition (clears storage_alias_name + data; the ns
  member stays reachable via using_namespace_call_fallback + overload ranking).
  Reducers tmp/mm3/mm4/mm5.cpp (mm5 = order-independence proof); testmultiret
  3 2 7 42 live AND bound. The corpus's <algorithm> surface made this visible
  in EVERY bound TU — that leak itself is item 5, unchanged.


- **stoi FAMILY @c2777b4d — NOT A FOREST BUG, a latent LIVE bug the bind
  EXPOSED.** Root cause: explicit-specialization instantiation KEYS used the
  RAW source spelling (`template<> struct __is_integer<int>` → `..._int`)
  while use sites spell args through the canonical DataDef the lexer emits
  (`int` → ddINT32 → `..._int32_t`). Every libstdc++ spec keyed on a
  builtin-INTEGER spelling was invisible → PRIMARY silently instantiated →
  `__is_integer<int>::__value=0` → `__is_integer_nonstrict<int>::__width=0`
  → `__numeric_traits_integer<int>{__digits=-1, __max=-1, __min=0}` → __stoa
  range check rejects EVERYTHING. Bind exposed it because the bound TU
  resolves `__stoa<long,int>`'s `_Ret=int` FAITHFULLY (true_type check
  active); live collapses `_Ret`→long (false_type → check skipped → "works").
  FIX @c2777b4d: builtin spelling→canonical-dd table extracted from
  resolve_named_datadef into `resolve_builtin_type_spelling()` (ONE owner);
  `canonical_arg_key_fragment` routes type spellings through it (wchar_t/
  char16_t/char32_t carve-out preserved); PLUS first-spec-wins skip when two
  C++ spellings collapse to one madc type (`long`/`long long`, `double`/
  `long double` — a later sibling spec = benign re-spelling, was "Class
  already defined"). Reducers: tmp/nti_red.cpp (30-line forest-free mirror,
  folds g++-exact), tmp/stoi1/3/4.cpp + tmp/stod1.cpp all correct live AND
  bound. teststdstringconv flipped too (same family).
- **RECORDED FOLLOW-ON (no failing test): live drops `__stoa<long, int>`'s
  explicit SECOND template arg** — `_Ret` collapses to the `_Ret = _TRet`
  default (emitted `__stoa__o2` returns int64_t, `is_same<_Ret,int>` =
  false_type). Live infidelity vs g++ AND a live≠bind divergence (bind's o10
  gets int32_t + true_type — MORE faithful). Both now produce correct output
  (values fixed), but the divergence violates LOADED==PARSED in spirit; fix
  LIVE's explicit-template-arg handling for function templates, then the
  emitted-C shapes converge.
- **PACKED-SUITE remaining 4 — ALL the item-5 eager-restore drivers**
  (testgnuattributemode + testmemclralignwide repeated int16_t;
  teststaticconstsibling own `struct ios` vs restored corpus class;
  testservent pthread_attr_t token promotion). ONE structural fix.
- **NEXT:** **item 5 lazy defrost** (demand-key the ONE restore path; 4
  drivers above) → item 6 measure (--no-forest-bind A/B, SMAUG 51-TU
  --project, --show-stats) + stamp the 06-22 plan CLOSED. Then the recorded
  follow-ons (\_Ret collapse first).
- **ITEM-5 DESIGN (recon done 2026-07-09, bank it — implement from here):**
  - THE LEAK (all 4 drivers, reproduced): the tests include ONLY C headers
    (stdio.h/netdb.h) whose real glibc paths ARE corpus units → the include
    BINDS → first bind's `forest_restore_decls` registers the WHOLE
    240-unit surface → stdint.h's int16_t/…, iostream's `ios` flat alias,
    pthread types leak into TUs that never included them (repeated
    declaration / already defined / token promotion).
  - THE DEMAND KEY EXISTS: `forest_chain_set` (Program, lexer.cpp
    forest_bind_include ~1843) = the exact transitive closure of BOUND
    units (frozen include edges, DFS, cycle-safe). Complete at END of
    tokenize (phase-split!) — so the correct filter point is the
    POST-TOKENIZE FLUSH, where flat datatype writes are ALREADY staged
    (seventh sitting, forest_pending_datatypes) and globals/funcs already
    flush (flush_forest_pending_globals).
  - THE NAME→UNIT MAP EXISTS: the B4a DECL INDEX (per-unit, per-kind
    name entries — `--dump-forest` `declindex` lines), and
    `forest_index_oracle` (in fulltest) ASSERTS it covers every registered
    lookup (5121 names / 4081 lookups / 41 allowlisted) — it was built as
    exactly this demand key. `defrec` has NO file/unit field (do NOT add
    one — no format bump needed; the index is the provenance).
  - IMPLEMENTATION SHAPE (one path, demand-keyed): (1) at flush time build
    the permitted-name set = union of decl-index entries of units in
    forest_chain_set (+ the oracle's allowlisted names unconditionally);
    (2) MOVE the currently-eager registrations (ns/struct/template maps,
    written mid-tokenize at first bind in forest_restore_decls) to the
    same flush — they are lexer-invisible (seventh-sitting finding), parse
    reads them only post-tokenize; (3) gate every family's registration
    (types, typedefs, enums, templates ×8 pattern maps, funcdefs, globals,
    overload sets, nslinks) on the permitted set; (4) `forest_restore_decls`
    callers must also flush (memory gotcha) — 2 call sites lexer.cpp
    ~3056/~3176.
  - GATES: the 4 drivers flip; bind gate 18/18 byte-identity MUST hold
    (single-header freezes bind everything they froze — closure == whole
    container there, so filtering is an identity on those cases); fulltest;
    packed suite expect 680/680.
- **GOTCHAS:** tmp/pack.msnap + tmp/madc_packed2 are CURRENT at 1e96a128
  (re-frozen/re-packed after each fix — the spec-key change renames container
  records); tmp/madc_packed (old) is STALE — delete or ignore. Other latent
  gaps found while probing (backlog, no failing tests): global-scope fn
  templates don't register (namespace-wrapped do); qualified template-static
  access (`NTI<int>::__min`) fails in NON-template bodies; cv-qualified
  spec-arg key fragments still raw.

---

## Prior sitting (eighth, 2026-07-08 — ITEM 4 LANDED: default-on embedded forest)

- **Git:** develop == origin/develop **@a1da0687** — TWO pushed eighth-sitting
  batches, each gated fulltest 680/0/0/16 exit 0 + bind gate 18/18
  (strbind/strops whole-TU byte-identity UNREGRESSED) + selfexe + oracles.
- **ITEM 4 @561cce34 (format v27): THE PRODUCT MOMENT — a packed madc binary
  compiles with ZERO flags by binding system #includes from its own appended
  forest.** Pack = `scripts/forest_pack.sh` (19-header list v2, +algorithm —
  the B4a blocker is GONE; cstdint/cmath/iomanip still live-parse blockers,
  parser track) → 240 units / 45,085 records appended. PROOFS: zero-flag
  `<iostream>`/`<string>` consumer binds (units 71/48) == live == g++;
  owner's-bar testsubscript.mad packed == live == .expect, no flags; C-mode
  (`--std=gnu17`) correctly REFUSES the C++ corpus and live-parses; `-dM`
  parity except ONE glibc-internal tombstone
  (__GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION — PP-export undef-replay
  nit); **2.2× end-to-end** (622ms vs 1358ms, eager restore still in place).
  The wiring: default-on bind from /proc/self/exe when no forest flag
  (SILENT fall-through when blob-less/pin-mismatch — open() quiet_missing);
  freeze modes excluded from the default (pure producers; re-pack stays
  clean); `--no-forest-bind` = the item-6 A/B lever; `--project` TUs bind
  (madc_project_execute params — compiles BIND, only the build-time pack
  freezes; NO multi-TU freeze writer needed, the item-4 design question
  DISSOLVED). v27 = producer-config gate: dir header records
  LanguageStd|gnu_dialect<<16 + -D fold (madc_forest_config_word/
  _defines_hash, ONE derivation both sides); mismatch = silent live parse —
  REQUIRED: a C++ corpus carries real glibc paths a C compile's resolved
  #include would match. PLUS the first corpus-shape bug: the v12-era load
  skip dropped symbol-less body-less ctors/operators — a NEVER-ODR-USED
  producer (the corpus!) leaves EVERY inline special member in that state
  (bodies ride DEFBODY) → cdd->ctors empty → NO-MATCH on first construction
  (probe: nctors=0). Live registers every parsed method declaration; the
  restore now does too. Reducer: tmp/nouse_prod.cpp + tmp/nouse_cons.cpp.
- **BATCH 2 @a1da0687 — the stod chain (reducer tmp/stod1.cpp → d=3.14 ==
  live == g++ against the corpus), three stacked never-used-producer gaps:**
  (1) a DEFBODY-backed restored fn is NOT declaration_only — the flush's
  Itanium alias stamp sent bound calls MANGLED-DIRECT to header-inline fns
  with no .so export (stod undefined import); (2) DF_BODY_IN_INSTANTIATION
  (bit 26): an instantiated __oN body (__stoa__o2) parses at
  fn_template_instantiation_depth>0 live (the local-class reuse allowance,
  `struct _Save_errno`) — capture stamps the context,
  parse_deferred_function_body brackets the depth on the re-run; (3)
  IN-CLASS inline bodies parse at CLASS CLOSE (parse_deferred_function_body)
  — piece (a)'s parseFunction capture never saw them → a local class's
  ctor/dtor bodies were LOST at freeze (undefined ___Save_errno___dtor__2);
  capture now rides the class-close path, ctor mem-init list = DK_DEFBODY
  run slot 3, parseFunction gate widened to methods.
- **PACKED-SUITE SCORE (MADC_BIN=tmp/madc_packed, runner override landed):
  673/680.** The 7, classified: **4 = the item-5 class** (eager
  whole-container restore leaks never-bound units' state: testgnuattributemode
  + testmemclralignwide repeated int16_t; teststaticconstsibling own
  `struct ios` vs restored corpus class; testservent pthread_attr_t token
  promotion) — lazy defrost IS the structural fix, these are its driver
  tests. **3 = materialization residuals:** teststod + teststdstringconv now
  compile but stoi("42") THROWS out_of_range at runtime (static-member-const
  VALUE fidelity — __numeric_traits<int>::__min/__max — a NEW family, next
  target); testmultiret __ns_std_minmax placeholder never materializes.
- **NEXT SITTING:** the stoi value-fidelity family + minmax → **item 5 lazy
  defrost** (demand-keyed restore on the ONE path; 4 driver tests above; the
  user asked "is it lazy?" — unit binding + node records + bodies YES,
  declaration surface NO, that's item 5) → item 6 measure (--no-forest-bind
  = the A/B lever; SMAUG 51-TU --project) + stamp the 06-22 plan CLOSED.
- **GOTCHAS:** re-freeze tmp containers after ANY save-side change
  (stale-container tripwire — burned once today via or_probe on a pre-fix
  container); re-PACK tmp/madc_packed after every rebuild (binary+blob both
  stale); `grep -c error` matches `madc_error.o` — grep `error:`.

---

## Prior sitting (seventh, 2026-07-08 — soak-first + burn-down)

- **Git:** develop == origin/develop **@110148a4** — TWO pushed seventh-sitting
  batches (below). Soak-first CONFIRMED the sixth-sitting state at **662/674**:
  piece (a) flipped testincludenext but REGRESSED testmacroargsspace
  (header-DECLARED root-DEFINED fn → double definition) — fixed in batch 4.
- **Batch 4 @5fe7b093 (PUSHED, gates 18/18 + units + fulltest 680/0/0/16 exit 0):**
  1. **TOKEN-SHAPE PARITY (the REAL fd_set family — the "TokenSTRUCT fallback
     map" hypothesis was DISPROVEN by the reducer):** the lexer PROMOTES flat
     datatype_map names to TokenDataType at TOKENIZE time (getToken ~4978);
     live's tokenize fully precedes parse, so user-header types NEVER shape
     the root's tokens — but the eager bind-restore wrote datatype_map
     MID-tokenize, so `struct fd_set` died at the TAG position and
     teststringparam's `char *string` param at 22:34 (the v24 class-token-
     demotion mechanism on legitimately-restored include-origin names).
     FIX: restored flat datatype_map writes STAGE (forest_pending_datatypes)
     and apply at the post-tokenize flush; ns/struct/template maps stay eager
     (lexer-invisible). FLIPS testfdsetfromsystime + teststringparam.
  2. **DF_TYPEDEF_TAG_ALIAS (bit 25):** struct/class-KEYWORD typedefs also
     register the alias as a TAG live (struct_map writes at 24055/25002/28146;
     the plain TokenTYPEDEF path never does) — save stamps from the producer's
     own map state (CArray walks to its element for `typedef struct tag X[N]`),
     restore reproduces the struct_map write. Needed WITH (1) for fd_set.
  3. **6b DEFBODY BODY-ORIGIN FENCE:** piece (a) fenced only on the DK_FUNC's
     TU_ROOT (decl provenance = the header for a header-declared root-defined
     fn); now ALSO fences on the BODY tokens' origin (the v25 1b rule).
     FIXES the testmacroargsspace regression.
- **Batch 5 @110148a4 (PUSHED, same gates):** the smaug pair = TWO v14 gaps in
  cir_forest_fill_globals' scalar tail (reducer tmp/sg_main.c): `char *last_log
  = NULL;` (RHS = TokenCast(void*,0) — now unwraps to its literal) and `int
  REQ;` (UNINITIALIZED tentative definition — new CIR_GLOBALF_SCALAR_UNINIT
  form, bit 7; flush leaves initialize NULL → live's bss item; covers plain
  struct/array globals too). FLIPS testsmaug_requests + smaug_requests_source.
- **RECLASSIFIED OUT of the forest worklist: teststructinit** — its BIND_DIFF
  is a PRE-EXISTING LIVE bug (string member in a struct initializer list
  prints garbage live too: `m = (, <garbage>, <garbage>)`; the test has no
  .expect so fulltest masks it; the soak compares two garbage runs). Fix
  belongs to the gcc-parity track, not forest.
- **BATCH 6 @7af7eee7 (PUSHED, gates 18/18 + units + fulltest 680/0/0/16 exit 0):
  family (c) CLOSED — testforeachheaderbody binds == live. SOAK CONFIRMED
  668/674 (99.1%): EVERY BIND-STATE FAMILY IS NOW CLOSED.** The remaining 6
  non-OK are NOT bind-state: testproject×5 (item-4 class) + teststructinit
  (live bug). Two roots, both measured (probes tmp/ctor_probe.cpp +
  tmp/or_probe.cpp proved the ARENA faithful — ranks/params/defaults all
  correct; the gaps were LOAD/EMIT-side):
  1. **Restored CTOR local_emit_name (cir_freeze.cpp ~1871):** the
     disambiguation restore gated on a non-empty DISPLAY name — ctors have
     none — so every __oN-ranked ctor lost the symbol live stamps on
     local_emit_name; FuncDef-only emitters (ctor_call_symbol: shim/global/
     default-construct paths) degraded to the canonical Class__Class symbol —
     naming whichever ctor HOLDS the canonical rank (basic_string's
     __sv_wrapper delegating ctor) → its DEFBODY materialized + char* passed
     to a struct param. The ctor arm now mirrors the method arm (compare
     against Class__Class).
  2. **Late-referenced funcdef-sourced callees (cir_builder.cpp, m&l callee
     guard):** Pass 0.75 is a ONE-shot referenced-only proto sweep; a callee
     first referenced by a LATE-materialized loaded body (second m&l round —
     _M_construct__mti's _ZNK..._M_dataEv) arrives after it, and the
     forest_funcdef_syms guard still blocked the producer-extern load → the
     call implicit-int'd ("subscripted value is neither array nor pointer").
     The guard now keys on what 0.75 ACTUALLY emitted (typed_proto_syms,
     hoisted above the m&l lambda; pass075_done flag). Pre-0.75 behavior
     byte-identical; bind-only reach (forest_lazy empty on a live compile).
- **NEXT SITTING = ITEM 4 (corpus pack + append-to-binary default-on).**
  Design the multi-TU freeze shape THERE (the --project driver never calls
  madc_cir_freeze — only the extern decl at madc_project.cpp:40); then item 5
  lazy defrost → item 6 measure + stamp the 06-22 plan CLOSED.

### Sitting-7 detail (superseded interim states kept below)

- **RE-SOAK after batch 5: 667/674.** REMAINING then (7 non-OK):
  testforeachheaderbody (m&l over-materialization, reducer tmp/fe_red2.cpp +
  tmp/fe_hdr.h) · teststructinit (live bug, above) · testproject×5 +
  testfreezerun (item-4 class: the --project driver NEVER calls
  madc_cir_freeze — only the extern decl at madc_project.cpp:40; design the
  multi-TU freeze shape AT item 4, don't guess early).
- **Sub-bar note:** user-header bodied-fn reducers (sg_main) show proto/label
  ORDER + declaration-source divergence vs live in MADC_DUMP_MIR — the known
  post-flip residual class; rc+stdout is the family bar (whole-TU byte-identity
  stays gated on strbind/strops).

---

## Prior sitting log (fifth sitting, 2026-07-08 — v25 family burn-down)

- **Git:** develop == origin/develop **@c10e7557** — THREE pushed v25 batches:
  @ddd13b01 (families a+b+c), @ad8e3697 (family d + quoted-include binding),
  @c10e7557 (anon-gensym fixup: restore bumps the `__anon_N` counter past the
  container's max — the tagless-record change had made a consumer's fresh anon
  collide with a restored tag, 4 soak regressions, all reflipped GREEN).
  Untracked `mir-debug-support.md` is NOT ours — never stage, never `git add -A`.
- **DONE this sitting (soak 623 → 648 OK at @ddd13b01 → 655 OK at @c10e7557 =
  97.2% of the 674 runnable; 13 BIND_RC + 5 FREEZE_FAIL + 1 BIND_DIFF left):**
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
- **SIXTH-SITTING SCOREBOARD (2026-07-08): FOUR pushed code batches —
  @66ff0927 (v26 batch 2: flat-alias + capture-over-replay, below),
  @ac59411e (v26 batch 3: plain/anon-enum ENUMERATOR constants,
  CIR_GLOBALF_CONST_SCALAR — testdirent + testsockaddr flipped),
  @82e44acd (piece (a): unreferenced bodied FREE-fn bodies as ownerless
  DK_DEFBODY runs — testincludenext flipped; item 2 below records the
  implementation). Soak after batch 2: 655→660; after batch 3 CONFIRMED
  662/674 (98.2%); after piece (a) expect 663 (NOT yet re-soaked —
  seventh sitting: soak FIRST). Every batch gated: bind gate 18/18,
  unit suites, fulltest 680/0/0/16 exit 0.
  **REMAINING non-OK (~11) in burn-down order:**
  a. testfdsetfromsystime: `struct fd_set readfds;` — fd_set is a
     TAGLESS-typedef struct (`typedef struct {...} fd_set;`); live's
     TokenSTRUCT `struct`-keyword path falls back to the TYPEDEF when
     no tag exists; the restored state misses whatever map that
     fallback reads (trace live's TokenSTRUCT::parse fallback first).
  b. teststringparam (22:34 "Expecting identifier after type") — parse
     desync, untraced.
  c. testforeachheaderbody: bind's m&l fixpoint OVER-materializes
     basic_string ctor chains live never references (__sv_wrapper
     delegating ctor → C1EPKcmRKS3_ extern; c2mir struct-param check,
     left=__sv_wrapper right=nil). Reducer tmp/fe_red2.cpp (quoted hdr
     + fn-ptr `void (*fn)(std::string)` param + call through it).
     Related sub-bar: testfstream/testloop stderr noise `Unknown class
     scope basic_istream_char___gnu_cxx__char_traits_char_` (a _Traits
     identity gap one layer past the batch-2 flat-alias fix).
  d. teststructinit (BIND_DIFF); testsmaug_requests (last_log);
     smaug_requests_source (14 c2mir checks).
  e. testproject×5 FREEZE_FAIL + testfreezerun freeze-under-bind —
     the item-4 class (--project × --freeze writes no container).**
- **v26 BATCH 2 (sixth sitting, 2026-07-08 — the DEFBODY residual's REAL
  roots, both landed, PUSHED @66ff0927):** the "__n undeclared"
  residual was NOT a DEFBODY/owner gap. The prior "owner class has no
  arena record" probe had hit a STALE container (the reducer-loop
  last-freeze-wins tripwire — it is IN the tripwire list); at HEAD
  `__gnu_cxx__char_traits_char` records fine (DK_CLASS, 14 methodrecs,
  DEFBODY owners resolve). The real chain, traced live-vs-bind:
  1. **EXPLICIT-SPEC FLAT-ALIAS KEYS (DF_TYPEDEF_FLAT_ALIAS, landed).**
     Live's spec registration (TokenTEMPLATE::parse alias_key block
     ~38118) writes the QUALIFIED key (std__char_traits_char) into the
     FLAT datatype_map + struct_map + ns map — all to the ONE product
     under the legacy bare key (char_traits_char). The arena already
     carried the namespaced DK_TYPEDEF alias; the restore wrote only the
     ns map ([iobind] clobber fix) — but instantiate_template_use's
     CACHE CHECK reads the FLAT map (parser.cpp:3955), so a bound
     consumer missed the cached specialization and re-instantiated the
     PRIMARY (`std__char_traits_char : public __gnu_cxx::char_traits`),
     whose inherited compare materialized a __gnu_cxx deferred body live
     never touches → the misattributed `__n`. SAVE stamps the flag when
     the producer's flat map holds same-key→same-definition (pmr-style
     ns typedefs have no flat twin — the clobber class stays fixed);
     LOAD reproduces the three writes. FLIPS testheaderstringops +
     teststrplusbody_realhdr.
  2. **DEFAULT-ARG CAPTURE OVER INSTANTIATION REPLAY (DefCapState,
     landed).** param_default_capture_begin required an EMPTY pushback —
     but instantiation replays the substituted class body THROUGH the
     pushback, so EVERY instantiated method's default silently failed to
     capture (basic_ofstream::open: param_defaults live, NO def-run in
     the arena → bound `open(fname)` failed the concrete overloads'
     arity gate → instantiated the C++17 _Path member template → "too
     few arguments"). Capture now snapshots {cursor, pushback} at begin
     and reconstructs the consumed run = popped snapshot entries +
     consumed buffer range (stop-token lands back on its own snapshot
     slot → structurally excluded; rewrite/interleave → skip, cleanly
     lacks). FLIPS testdefer.
  **REMAINING (measured at this state):**
  - testfstream/testloop: ADVANCED (hard parse fail → recovered noise +
    output): residual `Unknown class scope
    'basic_istream_char___gnu_cxx__char_traits_char_'` ×3 on stderr — a
    _Traits identity gap still building a __gnu_cxx-spelled product for
    an out-of-line istream member scope. Same identity family, one
    layer deeper. Reducer: testfstream with its .flags.
  - testforeachheaderbody: bind's m&l fixpoint materializes basic_string
    ctor chains live never references (the __sv_wrapper delegating ctor
    → C1EPKcmRKS3_) → c2mir struct-param check error on a call in that
    over-materialized graph. Fix = the loaded-body callee-closure
    overshoot, NOT type identity. Reducer: tmp/fe_red2.cpp (quoted
    header, fn-ptr param `void (*fn)(std::string)`, call through it).
  2. **Piece (a) — IMPLEMENTED IN-TREE, UNBUILT/UNGATED (sixth sitting;
     if resuming after compaction: `git status` shows the 6-file diff —
     BUILD IT, run the testincludenext reducer, then gates):** the six
     edits: (1) FuncDef::forest_body_tokens + capture around
     parseCompound (parser.cpp ~40195, DefCapState, gated
     forest_arena_enabled && !owner_class && !tsubst_body_skipped;
     clone_funcdef_with_return copies the field); (2) cir_arena.h
     DF_FUNC_DEF_TOKENS = 1u<<24; (3) madc_cir.cpp arena_complete phase
     6b: ownerless DK_DEFBODY (ref0=0, non-full, run[0]=body) per
     funcdef_map entry with forest_body_tokens whose DK_FUNC record is
     !HAS_FOREST_BODY/!TU_ROOT, + stamps DF_FUNC_DEF_TOKENS on the
     DK_FUNC; (4) cir_freeze.cpp: the was_bodied&&!has_body declaration
     drop lifts when the flag is set; CirRestoredFunc grows mparams
     (aliasrec names — free-fn records already carry them, probed
     __ns_std_abs aliases=1 __i); (5) parser.cpp flush: PendingForestFunc
     carries mparams → the new Method's parameters (live param-loop
     parity); the EXISTING v26 DEFBODY flush plants the ownerless entry
     (findVariable(key) = the just-registered free-fn Variable) and the
     EXISTING m&l fixpoint (referenced_funcs ∋ key) materializes it;
     (6) parse_deferred_function_body: ownerless body_namespace =
     fd->namespace_name. ORIGINAL DESIGN NOTES (superseded by the
     implementation, kept for the trace):**
     UNREFERENCED bodied FREE fns (std::abs — records flags 0x30000 =
     IS_FREE_FUNC|WAS_BODIED, no body stamp; the `was_bodied && !has_body`
     exclusion at cir_freeze.cpp:2137 drops even the DECLARATION → "'abs'
     is not a member of namespace 'std'"). The traced shape:
     (i) CAPTURE: parseFunction's ENTRY is at `(` — exactly the token
     shape parse_deferred_lazy_body's full_definition arm re-parses
     (it pushes definition_tokens then calls parseFunction, 25692-25714).
     So begin a DefCapState at parseFunction entry (owner_class==NULL &&
     forest_arena_enabled), end it where the DEFINITION path completes
     (the body-brace branch), into a new FuncDef field (forest_def_tokens
     = `(params) { body }`). Prototype paths never call end — dropped.
     (ii) RECORD: ride the DK_FUNC record per-kind-reuse — body_unit/
     body_idx + carray_count_lo/hi are FREE on a !has_body DK_FUNC =
     4 words for (off/bytes/count/file_id); flag DF_FUNC_DEF_TOKENS.
     (iii) LOAD: at the 2137 exclusion, a WAS_BODIED&&!has_body record
     WITH the flag restores its DECLARATION (existing path, declaration_
     only) + stages the run; the flush ALSO plants deferred_lazy_bodies
     [key] (full_definition, owner NULL, var = the registered Variable).
     (iv) MATERIALIZE: parse_deferred_lazy_body's full arm needs an
     OWNERLESS variant: owner==NULL allowed when the var's FuncDef has
     namespace_name (restored ns_id) — NamespaceScope(fd->namespace_name)
     + parseFunction(mfd->return_value_type(), fd->function_display_name
     [e.g. "abs"], NULL). parseFunction's repeat-declaration path then
     binds the definition to the ALREADY-REGISTERED overload rank (the
     restored __oN — param-type matching picks the slot, live's proto-
     then-definition shape). VERIFY: which trigger materializes deferred
     entries for a FREE fn on ODR-use (methods key on emit_symbol; check
     the v26 DEFBODY m&l trigger covers funcdef-keyed free fns).
     ⚠ force-emit-at-freeze stays DEAD. Fixes testincludenext (std::abs).
  2. **ANONYMOUS ENUMs — ✅ CLOSED (v26 batch 3, family h):** a plain enum's
     enumerators are parse-time constants (addVariable+set+makeconstant, NO
     TopDecl) that glibc's extern "C" wrapping stamped vfEXTERN — so the
     fill's EXTERN_REF branch restored them as extern DATA imports
     (SOCK_STREAM/DT_REG undefined). Now: TokenENUM's global branch stamps
     forest_enum_const_origin[name]=file (the funcdef_files precedent); the
     fill records CIR_GLOBALF_CONST_SCALAR (checked BEFORE the vfEXTERN
     branch, flags verbatim, TU-root fence from the origin file); the flush
     rebuilds the live registration. testdirent + testsockaddr FLIPPED.
  3. Remaining classified singles: make_preferred trio (testdefer/testfstream/
     testloop — path-ish alias overload selection on basic_string);
     testfdsetfromsystime + teststringparam (parse desyncs); teststructinit
     (BIND_DIFF); testsmaug_requests (last_log); smaug_requests_source (14) +
     testmemclralignwide (2) c2mir checks; testproject×freeze FREEZE_FAIL 5 +
     testfreezerun freeze-under-bind (item-4 class).
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
