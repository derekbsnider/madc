# Instantiate-bucket plan — rung 4, third phase (Slices A, B, Option C)

**Date:** 2026-07-13 · **Branch policy:** each slice on its own
`feature/…-claude` branch off `develop`, full gate matrix before merge.
**Prereq reading:** `docs/plans/2026-07-09-forest-full-evaluation-lazy-materialize-PLAN.md`
("Rung-4 third phase" section — the measurements this plan executes on),
`.claude/rules/parse-once.md`, the topic memory RUNG-4 blocks.

## SETTLED (do not re-derive)

- The bound testsubscript -O2 wall sits at the **0.56 plateau**
  (0.572/0.561/0.570 = one noise band). Mechanical/container levers are
  EXHAUSTED — verdict stamped 2026-07-12.
- Bucket map (tmp/cg_refset.out, 3.465B bound window, inclusive Ir):
  class-template instantiation `instantiate_template_use` 1,316 calls /
  2.36B = 68%; inside it the class-body re-parse (`parseKeyword`) 251
  calls / **1.81B = 52% of the whole compile**. Fn-template failed
  attempts ≈ 30M (DEAD as a lever — do not build attempt filtering).
  `parse_deferred_lazy_body` 163 / 256M ≈ 7%. `node_for` 25 / 120M ≈ 3%.
- Only **7 top-level specializations** instantiate on the bound run — the
  `vector<int32_t>` consumer chain; its base/member/traits closure drives
  the ~251 class-body parses (parser consumes 296× the lexer's tokens).
- The TU-root fence is CORRECT: consumer specializations are TU state and
  must not silently enter the header grove. Any pack-side coverage of them
  goes through Option C (an explicit prelude TU), owner-approved, never
  through relaxing the fence.
- Attribution hygiene: name-substring caller attribution pollutes buckets
  (map<…less<string>…> in SIGNATURES). Verify per-callee, and re-profile
  the mechanism before benching (lookup-churn lesson).

## Slice A — drain-failure burn-down (mechanical, ~7% + pack quality)

### MEASURED 2026-07-13 (steps 1–2 executed; supersedes the expectations below)

- **Step-1 cross-reference REFUTED the priority hypothesis.** Bound
  testsubscript's 187 lazy-parses (`MADC_MTI_PROBE=_` capture) intersect the
  pack-drop set in exactly ONE symbol: `basic_string<char>` ctor `__o9`
  (73 `_Rb_tree*` + 26 `vector` + 14 `_Vector_base` + 10 `_Node_handle` +
  15 `allocator` derives are bodies of CONSUMER-instantiated specializations
  — `map<string,int32_t>` internals — which the TU-root fence forbids
  packing; that cost belongs to Slice B / Option C). Same pattern across
  teststringref/testmap/testvector/testforeach2 (1 hit each — the same
  ctor `__o9`); testfstream adds `std::stoi` (body-span carried, 1 derive).
  **Slice A's bound-wall value is ~0; its real value is pack completeness
  + the correctness bugs the census exposed.**
- **The 175 trap stubs UNDERCOUNT drops** — a stub only appears for a
  referenced-but-undefined import. The authoritative list is the freeze
  stderr (`pack drop:` lines): **515 drops**, classified:
  - ~165 local-class web: `_M_construct`'s function-local `_Guard` (GCC 13)
    methods are categorically un-carriable (66 direct "local-class method"
    drops; `__patN__` husk names are process-specific — pack minted
    `__pat129__`, a consumer mints `__pat19__`), cascading through
    `_M_construct__mti` to ctor `__o9` and ~85 string-flavor bodies.
    BY DESIGN (consumer re-derives with its own local classes) — reversing
    it means carrying function-local classes in the arena (new record
    family): owner-decision territory, NOT a parser gap.
  - 33 varargs-mangling victims: `_ZSt24__throw_out_of_range_fmtPKc` lacks
    the Itanium `z` — the whole `at`/`_M_check`/`__sv_check` throw-path web.
    **FIXED in this slice (family 3a): drops 515 → 482, stubs 175 → 174.**
  - ~35 typedef-leak mangles: `St9streamoff`/`St10streamsize` where the real
    exports have `l` (Itanium mangles desugared types only);
    `R14__ostream_type`/`R14__istream_type` in member-template return types
    where the real exports have `RSo`/`RSi` (`_M_insert`/`_M_extract`/
    `__basic_file`/`__ostream_insert`/`__num_base` clusters). Family 3b —
    **FIXED** (DataDef::mangle_scalar_spelling + FuncDef::mangle_param_spelling
    + desugar_member_type_spelling; 0 leak survivors on the fstream reducer
    freeze). Landed WITH the enum-typedef minting fix the first cut flushed
    out: `ios_base::openmode` wrapped its enum in a plain DataDef (enum-ness
    lost; the desugar spelled it `i`, `basic_filebuf::open` stopped binding
    external, and its madc-compiled body hit the mbstate identity split —
    testfstream/testdefer/testloop). Enum typedefs now keep the enum dd,
    like class typedefs. Net live effect: wifstream string-open derive
    4 check errors → 1 (residual = the wchar drain cluster, reducer
    tmp/reducer_wifstream_open_string.mad); ~4 already-rotten wchar open
    bodies moved from carried-with-int-flattened-enums to DEFBODY.
  - ~9 dropped-param mangles: `_S_create_c_locale` lost its reference param,
    `_List_node_base::swap` its second param, `_S_format_float` its first.
  - ~26 harvest misclassification: calls through fn-pointer PARAMS
    (`__pf`, `__convf`) and `alloca` recorded as named callees by
    `cir_collect_call_callees` → un-homed → caller drops. Fix = filter
    callee names declared within the def (careful: block-scope function
    declarations must still count).
  - ~91 pack-time drain parse failures (madc parser messages, not c2mir):
    `__cerb` undeclared ×86 (istream/ostream sentry bodies), mixed-identity
    `istreambuf_iterator<int32_t,char_traits<wchar_t>>` ×44 (wchar_t→int32_t
    canonicalization split), `iostate`/`openmode`/`result` member-typedef
    lookups ×16, chained arrow ×8, `Expected type in catch parameter` ×5
    (also fails LIVE: `catch (const out_of_range &e)` — reproducible with
    tmp/_oor.mad), typedef-typename ×4.
  - ~16 stoa/to_xstring instantiation husks (hash-suffixed pack-context
    names) — consumers re-derive via body-span carry (stoi = 1 derive).
- Cross-runtime EH note (pre-existing, unchanged by 3a): a NATIVE-thrown
  `std::out_of_range` terminates instead of entering madc's setjmp/longjmp
  catch dispatch — live and packed behave identically; separate track.
- **3b full-corpus honesty (measured on the release pack):** 0 net drop
  recoveries — the seekoff/xsgetn caller family's reason CHANGED from
  "calls unresolvable <typedef-leaked symbol>" to "c2mir check errors":
  correct mangles let those bodies reach the check gate for the first
  time, where the NEXT pre-existing gap layer (the mbstate/fpos struct
  family) drops them. Drops 482 → 498 (+16: bodies with enum-typed
  `openmode` DEFAULT-ARG expressions — fstream `open__o2`, stringstream
  ctors — newly visible to the same residual). Pack completeness for
  these families is gated on family D (drain parse gaps), not mangling.
  The mangle fixes stand on live correctness: real symbols resolve
  (at()/seekoff/_M_insert callers), enum typedefs keep identity.
- Pre-existing live gap found while probing (NOT from this slice; fails
  identically on the 3a-era binary): `istringstream >> int` mis-resolves
  as SHIFT ("shift operands should be of an integer type") — the suite
  has NO istringstream-extraction coverage. Reducer banked:
  tmp/reducer_istringstream_extract.mad. Candidate early family-D item.
  **FIXED 2026-07-13 (family-D prelude commit): the root cause was
  GENERAL — CIR-time operator overload selection never walked base
  classes, so every member operator inherited from a base was invisible
  (all derived streams' scalar `<<`/`>>`, plus user-class operators
  across single/multiple inheritance; only direct cin/cout objects
  worked). Both selectors now do the [class.member.lookup] base walk
  (name-hiding preserved; method_map is flattened and NOT a hiding
  signal) and all three call-lowering branches bind __this to the
  owner's subobject (base_offset_of, virtual bases via static vbase
  offset — `!stream` on basic_ios works). New test
  tests/testopinherit.mad, byte-identical to g++. HONEST FINDING: pack
  census unchanged (472 drops, identical set — the stream operator
  bodies were already recovered by 3c; the remaining drops fail on
  __cerb/wchar/mbstate). Live-correctness value only. Follow-up banked:
  `while (s >> a)` needs operator-bool-in-boolean-context (explicit
  operator bool on basic_ios) — DIFFERENT machinery (conversion
  operator, not overload selection), reducer tmp/red_iss_9.mad.**
- **FAMILY D RUNGS 1–2 (2026-07-13, same sitting): the `__cerb` root cause
  is a TEMPLATE-CLASSIFICATION bug** — `template<...> class
  Owner<T>::Nested {...};` (basic_istream/basic_ostream's `sentry`) was
  captured as a SPECIALIZATION of Owner (TokenTEMPLATE::parse's class path
  never checked for `::` after the template-id); the bogus spec could
  replace the primary (reducer tmp/red_cerb_2.mad: "Unknown class scope")
  and sentry never existed as a type. Fixed: qualified-head detection +
  OutOfLineNestedClassDef capture + eager shell parse per owner
  monomorphization. Rung 2: sentry bodies' first statement
  (`ios_base::iostate __err = ios_base::goodbit;`) exposed that qualified
  member-typedef DECLARATIONS at statement position inside bodies always
  routed to the expression parser — `string::size_type n` failed in plain
  user code (!); the registered-datatype arm now runs the same
  qualified-expr discriminator + member-type-chain probe as the
  template-id arm. Test tests/testoolnested.mad. **LADDER FINDING (the
  slice's central measurement): the drain bodies fail through a gap
  LADDER — fixing a rung advances bodies to the next. fstream-reducer
  corpus (tmp/_bf3.cpp): baseline 429 drops [wchar-'equal' ×149, __cerb
  ×86, __n ×45]; rung 1 → 534 [wchar family GONE, iostate ×96 surfaced];
  rungs 1+2 → 538 [iostate GONE; catch-param ×43 (`__catch(__forced_
  unwind&)`), chained arrow ×36, `_M_num_put` member ×30, __cerb ×108 as
  more flavors start]. Remaining rungs to net-positive: catch-parameter
  types, chained arrow in drain bodies, protected-member access through
  the basic_ios vbase (`this->_M_num_put`), the `__n` ×45 identifier-scope
  family. Full release corpus: drops 472 → 580, trap stubs 178 → 211.
  Every drop is a DEFBODY revert (correctness-neutral); live gates stay
  the arbiter per commit.**

The pack reverts ~175 library bodies to DEFBODY (trap stubs in the packed
binary; census from `bin/madc-release --run-frozen -v`, 2026-07-13):
basic_string 45 · reverse_iterator 18 · locale machinery
(__ctype_abstract_base/num_get/codecvt) ~20 · basic_string_view 10 ·
basic_filebuf 7 · __alloc_traits 6 · basic_istream 6 · _Hash_impl 4 · rest
long tail. A consumer that references one re-parses it at bind time
(`parse_deferred_lazy_body`, 163 calls / 256M on bound testsubscript).

1. **Cross-reference first.** Instrument nothing: run bound testsubscript
   with `-v` and capture which deferred bodies actually lazy-parse
   (the `parse_deferred_lazy_body` entry DBG or add a one-line env-gated
   log if none exists). Prioritize the intersection with the 175 — those
   families pay on every consumer compile. Expect basic_string members
   and reverse_iterator (vector's rbegin/rend surface) on top.
2. **Get the pack-time drop REASONS** (not just names): re-freeze with the
   drain log visible (`scripts/forest_pack.sh` on a dev-binary copy with
   verbose/env drain logging — pack_record_drop logs "why"). Classify
   into parser-gap families.
3. **Fix the biggest family at the deepest layer** (parser gap, not drain
   shim), one family per commit. After each: re-pack, confirm the
   unresolved count drops, full gate matrix.
4. **Acceptance:** unresolved count meaningfully down (target: the
   consumer-hit families to zero), `parse_deferred_lazy_body` calls on
   bound testsubscript down accordingly, fulltest 681 + packed suite 681
   + bind gate 18/18 + tsubst ratchet green, bench row recorded.
   ~0.04s of bound wall is the realistic ceiling here — the strategic
   value is pack completeness (fewer trap stubs) as much as the wall.

## Slice B — class-template instantiation on the parse-once spine (~52% ceiling)

**The design track.** Today a class specialization instantiates by
re-running the PARSER (`instantiate_template_use` → substituted saved
tokens → `parseKeyword` → TokenSTRUCT/TokenCLASS::parse) for the class
shell + member declarations — method BODIES are already lazy. Extend the
parse-once model (g++ tsubst; `.claude/rules/parse-once.md` names the
construction/member-type KINDs) to the class KIND: parse the class
pattern ONCE into a reusable declaration tree; instantiate by
substituting over that tree — types, layout, member/method registration —
without the parser.

This is a DESIGN SITTING first, not code. Charter:

1. **Enumerate what TokenSTRUCT/TokenCLASS::parse produces per
   instantiation** (read, don't guess): DataDefCLASS + members +
   method_map Variables + vtable groups + layout + type_aliases +
   nested types + out-of-line member registration + forest taps. The
   substitution pass must produce EXACTLY this set. List every side
   effect (struct_map/datatype_map writes, template registrations,
   pending_funcs, MTI hooks).
2. **Define the pattern memo:** what is saved per class template primary /
   partial spec (the parsed member-declaration tree with placeholder
   types), where it lives (alongside `m_tsubst_body_patterns` /
   TemplateDef), and the eligibility predicate (start NARROW: container
   shapes — data members, base specs, member fn declarations, typedefs;
   anything else falls back to today's token parse, tallied like the
   tsubst `[why:]` fallbacks so the ratchet model applies).
3. **Define the substitution pass:** placeholder→concrete DataDef mapping
   (reuse tsubst's binding machinery), layout computation via the
   existing compute_layout, member Variable minting without TokenIdent
   re-parse. Nested dependent types resolve through the existing
   dependent-member-type machinery.
4. **Gates for the first landing:** the instantiated result must be
   INDISTINGUISHABLE from the parsed one — same struct_map/datatype_map
   entries, same layouts (sizeof/offset oracle vs g++), same mangled
   symbols, whole-suite green + bind byte-identity gates. Add a
   `--show-stats` counter: class instantiations via pattern vs via parse
   (the new burndown metric, mirroring the tsubst HIT/FALLBACK model).
5. **Sizing checkpoints:** vector<int32_t> chain first (the measured 52%);
   then string/map chains. Expected shape of win: bound AND live both
   drop (this is not forest-specific — it is the front end).

Do NOT start B's implementation in the same sitting as its design doc;
the design lands as a plan appendix or its own doc, then implementation
slices follow the ratchet model (eligibility widens per-KIND, fallback
tally must go down or stay flat — never up).

## Option C — explicit-instantiation prelude TU (OWNER DECISION, not scheduled)

C++'s extern-template model applied to the pack: add a prelude TU to the
corpus that explicitly instantiates the common set (`vector<int32_t>`,
`vector<int64_t>`, `string` iterators, …). Its instantiations freeze as
that TU's OWN parsed state — LOADED==parsed holds, the TU-root fence is
respected (nothing consumer-specific leaks into header groves; the
prelude is just another producer). Consumers whose specializations match
restore instead of instantiating; mismatches still take path B.
Cost: corpus policy change + a guessed specialization list to maintain.
**Do not implement without the owner's explicit go.** If B lands well,
C may be unnecessary.

## Ordering

1. Slice A (mechanical, independent, fresh sitting, full gate cycle).
2. Slice B design sitting (produces the design doc + eligibility spec).
3. Slice B implementation slices (ratcheted, one shape family at a time).
4. Option C only on owner sign-off, and only if B leaves a gap worth it.

## Bench discipline (unchanged)

`scripts/forest_phase_bench.sh` rows at load<2 (one self-exiting
background command), TSV `docs/perf/forest-timings.tsv`, callgrind site
counts as the mechanism proof (bound runs complete within the 120s cap;
live runs truncate — compare sites, never totals), re-profile mechanism
before benching, mirrors at milestone cadence.
