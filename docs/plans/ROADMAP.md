# madc Roadmap

**✅ VALUE INTRINSIC V1+V2 LANDED (2026-08-09, v0.75.0):** `madc::value`
as a first-class script intrinsic — `value` / `var` / `madc::value`
spellings of the ONE DataDef, `--std=madc`-gated, typedef-lane resolution
with C++ name-hiding guards (never lexer tokens) — and the value-first
`<ns_madc>`: no `#include <string>`, primary API in value + const char*,
std::string overloads PP-gated on the stdlib's own include guards.
madc::-only scripts hit the millisecond floor (testsort 15ms packed, was
~130ms). Shipped with two discovered-defect fixes: proven no-viable
method-overload diagnosis + method default arguments (34d24c67), and the
tokenize file-lane use-after-free (091c977a). Plan:
`docs/plans/2026-08-09-value-intrinsic-plan.md` — follow-up track: the
general [over.match.viable] diagnostic (scorer conversion modeling).

**✅ TRACK 5C SLICE 1 LANDED (2026-08-08, v0.72.0):** the script-facing
channel surface — `madc::channel` in `<ns_madc>` (mangled-direct to the
host class, the first madc-owned class on that spine), `exec://` as a
first-class channel scheme (space-split argv, inherited stderr,
spawn-PATH resolution in `Process`), flavor marshalling's METHOD half
(task #69 — madc-owned classes with `string&` params work under
`-stdlib=libc++`), and the suite's first script-level tcp/exec legs
(`testexecchannel` / `testtcpchannel` / `testhttpget`, all hermetic).
Plan: `docs/plans/2026-08-08-track5c-script-channels-plan.md` — later
5C slices: script-side `DataSet`/query surface, listener channels.

**✅ FLR RANDOM ACCESS S1+S2 LANDED (2026-08-08, v0.71.0):** seekable
channels with positioned IO (`SeekableDataChannel`, truthful per-fd
`seek` capability), the FLR driver reborn lazy (geometry-only open,
streaming cursor, O(1) `record_index` locator lookup, positional
single-record writes), `ChannelBackedDataDriver` records-over-memory,
and the capability-truth gate (a hollow capability claim is a build
failure). Plan: `docs/plans/2026-08-08-flr-random-access-struct-schema-plan.md`
— next slices: S3 locator-as-column pushdown + VLR offset sidecar,
S4 C-struct-as-schema `bind<T>`, S5 typed catalog.

**✅ TRACK 5A LANDED (2026-08-08, v0.70.0):** the data-channel
streaming & process-flow core — dependency-free typed streaming over
memory/file/FIFO/TCP/UDP/UDS and child processes behind ABI-compatible
extension seams; the madcdis/madcdat boundary is dependency-based
(core/OS in madcdis, external-library providers in madcdat).
Guideline: `docs/plans/2026-08-07-data-channel-streaming-process-flow-plan.md`.
Open Track 5 follow-ups: optional madcdat service providers
(libcurl-backed HTTP/REST etc.) and migrating suitable eager
drivers/adapters to native cursors.

Master plan linking all workstreams. Updated 2026-08-09 (v0.75.0 — the
value intrinsic + value-first `<ns_madc>`; previous: v0.72.0 — the
script-facing channel surface; v0.71.0 — FLR
random access S1+S2; v0.70.0 — the
data-channel core; v0.69.0 — the release-lane restoration +
script-mode completion; v0.68.0 —
the libc++ LANE-ZERO release, @429842b4 on feature/libcxx-parity7-claude):
🏁 **P2.7 IS COMPLETE.** The `-stdlib=libc++` flavored lane reached an
EMPTY failing set — full behavior-parity with the default libstdc++
flavor. At the release HEAD: fulltest **997/0/9skip**, lane
**993/0/13skip**, EXE/OBJ **976/0** (the lane's 13 skips = the 9
baseline `.mir_skip` + 4 documented `.libcxx_skip`).
`docs/parity/libcxx-failset.txt` recorded the zero its charter defined
and is now the lane's regression gate (the `libcxxjit` battery stage
holds it there). The final session (#65, wall-grind mode) took the #110
pack wall: a template-template parameter defaulted to a DIFFERENT named
template binds as a template NAME ([temp.param]p11 — libc++ `tuple()`'s
`_IsDefault = is_default_constructible` idiom; gate `testctorttpdefault`),
plus ctor-template constructibility traits (`testctortemplatetrait`),
[namespace.udecl] overload-set joins (`testusingfnoverload`), and
[dcl.link]p7 extern-block scoping (`testexternblockbody`). Residual
follow-ups carried in `claude_status.json` live_handoff (n1-n5: the
swap<allocator> return mistyping, pack-drain gen-only defects,
dependent-decltype pattern-freeze, the trait-laundering sibling, C-style
cast through explicit operator bool). #114 remains blocked on the owner
decision about mangling overloaded user free functions.

**Backend reality:** `madc parser → cir_node (MC11-IR) → c2mir → MIR → JIT` is
the **sole** backend — asmjit and the Gecko parser/MIR-transpiler are gone.
The old parity-with-asmjit-master goal is HISTORY: the CIR backend met the
re-defined promote gate (all class-(a) torture failures fixed, stamped in
[failset-classification.md](../parity/failset-classification.md)) and has been
promoted to `master` repeatedly — v0.38.0 (2026-07-23, the first CIR master),
v0.48.0 (2026-07-25), **v0.69.0 (2026-08-07, current)**. `master` now tracks
develop's release cadence at owner-called promote points; the standing gate is
`.claude/rules/branching.md` (torture class-(a) burndown, currently satisfied;
the 10 remaining failset entries are class-(b) GNU extensions = roadmap items).
Where a track below was completed on the old backend, it is marked "proven on
old backend, re-establishing on CIR."

## The Intermediate Representation — MC11-IR (SET IN STONE, 2026-05-29)

The primary in-memory representation is the **Mad-enhanced-C11 IR (MC11-IR)** —
the `cir_node` AST tree. `cir_node` **derives from c2mir's `node_t`** (so c2mir
consumes the lowered C11 view directly) AND each node **carries its originating
lexed tokens + parse subtree + file/line/column** (so madc retains the original
high-level structure without reconstruction). It is deliberately **both**:
lowered for c2mir, high-level for madc. The `.mc11` text is the on-disk
serialization of the extra info; render targets (C11/MC11/C++/madc) share the
`--std=` language enum to pick which view to emit. See
[`docs/rules/mc11-ir.md`](../rules/mc11-ir.md). **Do not re-pose "lowered vs
high-level" — the answer is both.**

## Current State

- **Track 6.4 Win64 JIT full-suite burndown is complete on
  `feature/win64-46b-burndown-codex` (2026-08-13, code/test head
  `0bc84193`).** The MinGW+UCRT binary's Wine domain moved from
  **947/30/59skip** at the 46b handoff to
  **981/0/0TO/57skip** after LLP64 ABI/type/layout, c2mir/MIR boundary,
  exception, preprocessor, and platform-fixture fixes. The skip audit
  leaves 25 libc++-flavor exclusions, 20 structural Win64/POSIX
  exclusions, 3 Wine-only exclusions, and 9 known MIR gaps. Wine's
  rotating false failures were captured as a client/server connection
  reset and disappear with one persistent `wineserver`; this is now a
  documented gate precondition. Final-content regression gates:
  fulltest **1029/0/9skip**, libc++ JIT **1025/0/13skip**. Next:
  W3 PE/COFF writers, W4 Windows header groves, W5 artifacts/battery.

- **v0.78.0 (2026-08-12): the torture window closes (task #41).** The
  5 standard-C class-(a) regressions from the 2026-07-23→08-11 window
  are root-caused and fixed — long-double struct alignment 16
  (`DataDefLDOUBLE::alignment()` override), `__builtin_classify_type`
  as a real parser builtin (was a macro → 0), anonymous-aggregate
  inline emission after nested-type class promotion, and nested-brace
  aggregate recursion in designated inits — each with a
  gcc+clang-oracled reducer in `tests/`. Torture restored to
  **1614/1/9/0/61**, byte-identical to the 2026-07-23 baseline; the
  failset is again exactly the 10 class-(b) items and the **promote
  gate is met**. Validation: fulltest 1023/0, libcxxjit 1019/0,
  EXE/OBJ green, release+packed green. The window's fails were
  UNMASKED by its own correctness work (114b13a8 real long double,
  6fec105d nested-type scope membership, 8f8f4009 loud aggregate-shape
  gate) — the #74 unprototyped-call hypothesis was disproven. The
  **Windows release lane plan** (Track 6.4) is drafted:
  [2026-08-12-windows-release-lane.md](2026-08-12-windows-release-lane.md)
  (mingw-w64 + libstdc++ + UCRT, Win64 only, full-suite scope,
  validated natively on the Windows 11 host carrying the build
  container).

- **v0.77.0 (2026-08-11): MIR lives in-tree — `third_party/mir` as a
  full-history Git subtree (ADR 0002).** One clone builds everything:
  `make -C src` builds libmir + c2m into `obj/mir/<variant>` (never
  inside the subtree; `mirclean`); `MIR_COMMIT`/`MIR_VERSION` and the
  fork-lockstep release ceremony are retired — the madc commit IS the
  pin. Import tree-hash-identical to the final pin (fork
  `v1.0-madc.0.76.0`, tagged `madc-pre-subtree-migration`).
  `vnmakarov/mir` = upstream (deliberate subtree pulls);
  `derekbsnider/mir` = frozen historical + upstream-PR transport.
  Validation: fulltest 1019/0 TWICE (main tree + a mir-less fresh
  clone), libcxxjit 1015/0, EXE/OBJ 990/0, release+packed green,
  release-macos + Mach-O gates green, MIR suite 1145 tests green,
  torture byte-identical to pre-migration, stale-checkout negative
  control passed. The known-open rider (task #41: 5 torture fails from
  the 2026-07-23→08-11 window) was RESOLVED in v0.78.0 — all class-(a),
  all fixed. NEXT PLAN (owner): a **Windows release** (Track 6.4);
  GitHub-Actions release automation follows once a Windows build works.
  Plan/record: [mir-into-madc-repo-2026-08-11.md](mir-into-madc-repo-2026-08-11.md).

- **v0.76.0 (2026-08-11): the macOS release lane — madc's first PUBLIC
  darwin release.** Tarballs `madc-0.76.0-macos-{arm64,x86_64}.tar.gz`:
  stripped, ad-hoc-signed hosted binaries carrying the packed C/C++
  stdlib forest, `libmadc_rt.a` for emitted-C consumers, and native AOT
  `-o` covering runtime-free C, C-lane-runtime static (try/catch/VLA via
  the forest-carried AOT ledger), AND C++ programs (fork `mir-macho.c`
  binds flat-namespace when extra dylibs ride `LC_LOAD_DYLIB`;
  `cir_apple_extra_dylibs` adds `libc++.1.dylib` on Itanium-mangled
  imports). Provenance-clean end to end: the darwin C prelude flattens
  from the zig-0.16.0-pinned Apple APSL/BSD header tree
  (`scripts/fetch_darwin_open_headers.sh`), the binary bakes its prelude
  provenance into `.rodata`, `verify_macho_release.sh` refuses
  sdk-private or missing markers, and `THIRD_PARTY_NOTICES/` ships the
  240-file license audit. The deepest fix of the arc: AArch64 places the
  hidden by-value-class-return pointer in `x8` — madc's hand-rolled
  first-argument shape was x86-64-only; the parameter is now marked
  (`__attribute__((ret_addr))` → `MIR_T_RBLK`) and placed by the target,
  with `CirBuilder::retbuf_param` its single producer, plus the
  emit-time struct-return fusion so `--emit=c11` output is
  target-neutral too. Battery: fulltest **1019/0/9skip**, libc++ lane
  **1015/0/13skip**, EXE/OBJ **990/0**, packed **1019/0**, on-Mac
  evidence battery 8/2 of 10 (fails = the two named known-opens).
  Remaining on the axis: full-runtime dynamic AOT (`libmadc.dylib`),
  the in-process Mach-O `.o` loader
  ([2026-08-07-macos-release-lane-plan.md](2026-08-07-macos-release-lane-plan.md)).

- **v0.70.0 (2026-08-08): the data-channel streaming & process-flow
  core (Track 5A).** madcdis gains the dependency-free streaming
  framework: lazy typed `Sink`/`Flow` adapters over ABI-compatible
  extension seams (existing Cursor/DataDriver/SourceAdapter vtables
  untouched; `Streaming*`/`ErrorAwareCursor` extensions capability-
  probed with vector-API fallback), a `DataChannel` framework covering
  memory/file/FIFO/TCP/UDP/UDS behind one scheme registry with
  SIGPIPE-safe single-owner writes and datagram-truncation refusal,
  explicit-argv `Process` endpoints with exec-errno self-pipe +
  bounded concurrent pumping, and native incremental DSV scans. The
  madcdis/madcdat boundary is DEPENDENCY-BASED (owner decision, KG
  `Decision{madcdis_madcdat_dependency_boundary}`): core/OS in
  madcdis, external-library providers in madcdat — the storage core
  moved as a verified faithful move and the DSV/FLR/VLR suites now
  run under `--enable-madcdat=no`. Reviewed function-by-function
  pre-merge; hardening: atomic close-on-exec at fd creation (darwin
  post-hoc fallback kept), one channel-pump owner, documented
  channel/pump thread contracts, two new fulltest gates. Battery:
  fulltest **999/0/9skip**, lane **995/0/13skip**, EXE/OBJ **978/0**,
  packed **999/0/9skip**. Implemented by Codex 5.6-sol (session #70).

- **v0.69.0 (2026-08-07): the release-lane restoration + script-mode
  completion.** `make release` had been red since Aug 1 (the packed
  suite at 982/15; no v0.68.0 release binary ever existed): the packed
  19-header forest split template instantiation identities between the
  freezing parse and bound consumers. Nine root-cause fixes across
  parser / freeze recorder / restore / CIR / lexer (canonical scalar
  dds, recorder flavor-twin pinning, thaw ovset rejoin, using-import
  order parity, the builtin-only typedef-alias guard — narrowed after
  the battery itself caught the first cut collapsing glibc's `jmp_buf`,
  namespace-faithful despaced lookup, best-effort default-arg
  re-derivation, copied-call member-access recovery, auto-include
  inside the restore window) took packed to **999/0**, identical to the
  live lane. A cross-TU freeze-consumer gate joined `fulltest` (prior
  forest gates froze only each test's OWN TU — this class was
  invisible). Script mode completed: top-level `defer` (#16) and
  top-level `:=` multi-return receivers (#18) join the synthesized
  main. Release-HEAD battery: fulltest **999/0/0TO/9skip**, lane
  **995/0/13skip**, EXE/OBJ **978/0**, release rc=0, packed
  **999/0/9skip**. Fork unchanged (1.0-madc.0.68.0 @4573a0f3 carries).

- **v0.68.0 (2026-08-06): the libc++ LANE-ZERO release — P2.7 COMPLETE.**
  The flavored lane finished the burn-down begun at v0.61.0: 891/28 →
  980/0 at @520e77d6 across the parity6/parity7 branches, zero newly
  broken tests at every comm-diffed measurement, and the failing set is
  EMPTY — full behavior-parity between `-stdlib=libc++` and the default
  libstdc++ flavor. Release-HEAD battery (@429842b4): fulltest
  **997/0/0TO/9skip**, lane **993/0/13skip**, EXE/OBJ **976/0**. The
  release also lands the multi-return struct transport (class values,
  Go-style heterogeneous `(T0, T1) f()` signatures, loud rejects;
  @a369cb17), restored zero-ceremony madc mode (auto-include + default
  namespace preference; @1fafe265 @0bc6ce07), the `sizeof...`
  function-pack fold (@b411715c), the enum-constant slot heap-overflow
  fix (@429842b4), and the machine-verified docs overhaul (53/53
  examples green). The parity campaign's concluding session's four
  roots: TTP defaults bind as template NAMES ([temp.param]p11 — the #110
  pack wall, libc++ `tuple()`; gate `testctorttpdefault`), ctor-template
  same-class constructibility (trait refusal laundered to a silent 0;
  gate `testctortemplatetrait`), using-declared functions join the target
  overload set ([namespace.udecl]; gate `testusingfnoverload`), and
  extern linkage-block context confined to directly-contained
  declarations ([dcl.link]p7; gate `testexternblockbody`).
  `docs/parity/libcxx-failset.txt` is now the lane's regression gate,
  enforced by the `libcxxjit` battery stage. Fork release
  **1.0-madc.0.68.0** @4573a0f3 (const `__int128` file-scope
  initializers, empty-struct call-result slots, `_Complex` return
  conversion — the three session-#64 raises v0.67.0 did not ship).

- **Previous parity checkpoint (2026-08-04, forwarding-reference owner
  consolidation, P2.7 in progress).** Scalar and direct-pack template call
  parameters now share `FnTemplateParamShape` and `fn_template_deduce_param`;
  member-template recursion and lookup share one value-category-aware call
  shape before the canonical binding memo; dependent `sizeof`/`alignof` folds
  through the same `query_datadef_measure` owner as eager parsing (@672a0966).
  `testfwdpackvaluecategory` matches GCC and Clang at `1 0`; two executable
  drift gates prevent the weaker deduction/key and type-query paths from
  returning. Retained member-template overload scoring delegates to the shared
  structural deducer; template parameter lists and member-template
  instantiation heads each have one parser/transfer owner (@fce67bf8).
  Class-pattern hydration preserves declaration defaults and constraints,
  trailing member `const` survives, and out-of-line definitions rename their
  parameter tokens positionally without replacing declaration identity. New
  `testmembertemplateconstoverload`, `testoutoflinememberconstraint`, and
  `teststringcompare_libcxx` match GCC and Clang and pass JIT/EXE/OBJ. Class
  construction now selects a zero-argument conversion
  method on the source by semantic target class and object cv, honors hiding
  and ambiguous bases, and shares stack/address destination writeback for
  trivially-copyable native results (@6209e622). New `testconvopclass` matches
  GCC and Clang at `41 42 42` and passes madc JIT/EXE/OBJ. Implicit copies of
  empty classes now preserve the object address without manufacturing a
  one-byte write (@fda0e15d), and `testeboemptycopy` passes all three lanes.
  Direct reference parameters preserve every referent pointer layer, so
  `T *&` lowers to the required `T **` ABI (@9013b492); `testrefptrparam`
  matches GCC and Clang at `42 1 43 1` and passes all three lanes. Copied
  dependent calls, deferred construction, and operator lowering now delegate
  reference-formal adaptation to `ref_param_arg_addr_from_value`, the same
  policy owner used by ordinary calls (@40cb8766). The
  `testcopiedrefptrparam` reducer catches the former `T *`-to-`T **` cast and
  the executable `check-ref-arg-lowering-owner.sh` gate prevents another
  partial owner. Both pass JIT/EXE/OBJ and fulltest. Out-of-line
  class-template member definition attachment now
  distinguishes plain declarations from member templates and finds trailing
  cv qualifiers from the declarator parameter-list boundary even when
  `throw()` follows (@5c3a8510). New
  `testoutoflinemembertemplateoverload` matches GCC and Clang at `1 2` and
  passes madc JIT/EXE/OBJ. Native aggregate-return and hidden-retbuf
  copy-initialization
  now use the existing instantiating constructor selector (@84713d03), so
  retained converting constructor templates materialize the target class
  before c2mir. New `testreturnconvctortemplate` and
  `testreturnconvctortemplateretbuf` match GCC and Clang at `73 1` and `91 1`
  and pass madc JIT/EXE/OBJ. Copied member pack calls rebuild their arguments
  against concrete winner formals, including hidden sret/receiver accounting
  and per-element reference adaptation (@2dd53e47). Reference-returning pack IDs
  already store referent addresses and are no longer addressed twice. New
  `testmemberpackrefcall` and `testmemberpackrefsret` match GCC and Clang at
  `34`, pass madc JIT/EXE/OBJ, and cover receiver-only and non-trivial-return
  shapes. Retained constructor-template lookup continues past failed
  same-arity siblings; omitted non-type defaults substitute earlier values
  before partial-specialization matching; winning specializations preserve
  non-type packs; nested expansions distinguish inner and outer packs; and
  member-template `sizeof...` sees both enclosing and deduced pack arities
  (@0fc1abf8). New gates `testmemberctorsibling`,
  `testpartialdefaultnontype`, `testmemberaliasnestedpack`, and
  `testmemberctorpackconstraint` match GCC and Clang at `7`, `1 1 1`,
  `0 1 0 1`, and `1`, and pass madc JIT/EXE/OBJ. The exact libc++
  `tuple<string&>` reducer prints `Alice`. Nested partial-specialization packs
  also resolve
  namespace-qualified aliases through the namespace type map and serialize
  resolved references with source-level reference spelling across cloned
  template bodies (@e34a06f6). New `testnestedpackref` follows two nested
  specialization hops; GCC 13, Clang 18, and madc agree at `9`, the exact
  libc++ `tuple_element` reducer prints `Alice!`, and focused default
  JIT/EXE/OBJ controls pass 6/6 in each lane. Plain aggregates nested in
  class-template instantiations receive owner-derived store keys, emitted
  names, and canonical spellings from their first declaration (@9debe778);
  isolated class-pattern capture
  remains pattern-owned and the forest lookup oracle filters every canonical
  instantiation product. New real-header gate `testnestedaggregateidentity`
  agrees with GCC and Clang and passes madc JIT/EXE/OBJ. Dependent template-id
  shells retain structural origin and typed argument-slot provenance
  (@c4828adb); definition-context class alias
  lookup overrides the ambient caller owner (@2e70fbbf), which fixes
  `testlateinstproto`; and direct-slot non-trivial return initialization keeps
  its `TokenCallFunc` origin through CIR copying (@518412e2); and concrete
  member-template return tokens resolve under the definition owner
  (@ef168838). GCC 13 and Clang 18 agree with all reducers. Fulltest is
  **946/0/0TO/9skip**. The measured whole libc++ lane is **927/16**, with eight
  old failures fixed and zero additions in its two-way failset diff; eligible
  lanes are **EXE 911/0** and **OBJ 911/0**. `testcontainerdtor` now runs through
  vector, set, and map destruction in production; the experimental
  `MADC_FWDREF_ARM` is deleted. Remaining failures are recorded in
  `docs/parity/libcxx-failset.txt`. NEXT: diagnose `testconstructible` against
  the value-category/trait boundary before changing `DataDefREF`; madc still
  collapses source `T&` and `T&&` into one reference type, but that broader gap
  is not yet attributed to a failing test. Keep #114 parked until its ABI/API
  decision is answered.

- **v0.67.0 (2026-08-01): the flavor-ABI release (tasks
  #69/#92/#93/#98, P2.7 in progress).** The flavored lane went 859/40
  → **880/26+2** — 21 net flips, zero broken at every comm-diffed
  step. The headline: **flavor-ABI marshalling (task #69)** — host
  namespace publics (php::, perl::, madc:: eval) export only libstdc++
  (`NSt7__cxx11`) manglings, so a libc++ script minted `NSt3__1`
  imports that either failed loud or, for extern-C twins taking
  `std::string*`, SILENTLY corrupted (raw libc++ bytes read as a
  libstdc++ string — exit 0, wrong values). The CIR builder now
  generates a thunk per boundary callee at `call_emit_symbol` (the ONE
  callee-symbol owner): host-flavor string temps built via the
  exported `C1(const char*, size_t, const allocator&)` from the script
  string's `c_str()`/`size()`, copy-back via the script flavor's
  `assign(const char*, size_type)`, alias-mapped conditional returns.
  Boundary detection = dladdr `dli_fbase` equality against a this-TU
  anchor (a symbol implemented by libc++.so itself never marshals);
  host twins reminted via `host_flavor_fn_symbol` (carrier mask under
  SCRIPT mangler state, respelled through `std_string_type()` under
  `MangleHostFlavorScope`); the scope-capture lane gets the same
  host-temp arm. Ten tests in one flip (design
  `docs/plans/2026-08-01-flavor-abi-marshalling.md`; `MADC_FLVMAR=0`
  escape hatch). Around it: #92 typed protos for declaration-only
  Itanium callees (the `_Znwm` implicit-int family); #98 a deduction
  guide declares NO name ([temp.deduct.guide] — the phantom
  `array`/`vector` VALUE shadowing types in prototype params); #93
  typedef template-arg identity LANDED ([temp.type] — `cin >> string`
  under libc++, the defless-drop fix greened the forest self-exe
  gate); const-overload selection joins the implicit object parameter
  ([over.match.funcs]/4 — silent wrong both flavors); #94 half (slot
  arm dark behind `MADC_XSLOT_ARM=1`); session #44's two
  [dcl.ambig.res] readings + access control judging the SELECTED
  overload. Suite 902 → 911 (nine new gates). Fork unchanged
  (1.0-madc.0.63.0). NEXT (task #34, 28 remaining): #102 non-type
  template packs (map/set/tuple family — groundwork stashed, REGRESSES
  the default lane as written, rework + vri-arming in task #102) →
  #72 skipped-body tsubst → #66 placeholder-beats-template
  (mathheader/stringrel) → singles. #104 banks the #69 follow-ups
  (testflavormarshal gate, exe-mode `-lstdc++`, testphp warnings).

- **v0.66.0 (2026-08-01): the recon-then-strike release (tasks
  #88/#90/#34, P2.7 in progress).** The flavored lane went 811/77 →
  **859/40** across three windows, zero broken at every comm-diffed
  step. First the 28-test `cout << std::string` bucket fell to ONE
  root in two commits (task #88): a pattern-recipe pop kept the
  dependent body parse's COLLATERAL definitions, and a fn-template
  instantiation now outranks ITS OWN varargs placeholder (pairwise via
  `tsubst_source`; gates `testpatcollateral` + `testcoutstr_libcxx`).
  Under it, two-layer SFINAE viability (@6980ba1a): an unrelated
  pointer argument is NOT viable ([conv.ptr] arm incl. numeric-pointee
  rawtype identity) and a scored overload miss inside an unevaluated
  operand is a SFINAE failure — the `allocator_traits::destroy` wall
  fell (gate `testptrviab`). Then the recon-then-strike method: EIGHT
  parallel recon subagents bucketed ALL 59 remaining failures with
  three-way madc/g++/clang++ reducers, and the strike batch took 19
  out (17 fixed + 2 reasoned skips): [dcl.enum]p11 unscoped-enum
  qualification (`testenumqual`), `<=>` comparison-category payload
  discovered from the flavor's own `<compare>` statics — no
  `_M_value`/sentinel hardcode (`testspaceship_libcxx`), fn-template
  memo keyed per-OVERLOAD (`testosmixed_libcxx`), concrete-pointer
  parameter viability closing the `cout << string` wrong-overload
  identity, task #90 (`teststrret_libcxx`), and [expr.ref]p4
  obj.static-member resolution (`testdotstatic`). Trait-fold silent
  wrong answers fixed both flavors (re-entry shell INCOMPLETE not
  dependent; reference args never classes — 7 inversions vs both
  canons). The release battery caught + fixed its own regression
  (@569de94d): the per-overload memo FNV hashed the already-renamed
  declarator, so freeze producers froze duplicate instantiation
  products under shifted names — [vecbind] went red; the hash now
  skips the declarator name ([over.load]); filed #96 (single-element
  pack key fold, needs content-deterministic naming — the #93 arc).
  Suite 889 → 902 (fulltest 902/0/0/9, `--exe` 886/0, `--obj` 886/0,
  packed arbiter 902/0/0/9). Fork unchanged (1.0-madc.0.63.0). NEXT: land
  parked #93 (typedef template-arg identity — `cin >> string` works
  under it, WIP branch @cf39afef; blocker = the freeze self-exe gate's
  `__oN` journal-rollback interaction, plan in task #93), then
  const-overload selection [over.match.funcs]/4 (the map/set family),
  #94 `conjunction` fold, the #69 flavor-ABI wall (11 tests).

- **v0.65.0 (2026-07-31): the VTT wall fell — libc++ `istringstream`
  RUNS (tasks #83/#84, P2.7 in progress).** madc's answer to Itanium
  construction vtables: every madc-emitted ctor of a vbase-carrying
  class takes hidden `struct V *__madc_vb<i>` parameters carrying the
  TRUE virtual-base addresses — ONE predicate
  (`ctor_hidden_vbase_owner`) keys all four signature surfaces
  (func_def, func_proto, Pass-0.75 externs, call sites) so c2mir
  arity-checks any divergence loudly; base/delegating construction
  forwards the caller's params, complete-object sites bind the receiver
  once; `vbase_dynamic_adjust` gains the construction arm (gate
  `testvttinit`). On top of it, the three-link stream chain: (1) an
  in-class decl-only member SHADOWED its attached out-of-line
  definition in the materialize-and-lower fixpoint (`basic_ios::init`
  emitted weak-EMPTY — `__loc_` frame garbage); (2) external D1 as a
  base dtor destroyed vbases TWICE — new `class_base_dtor_symbol` D2
  resolver adopted at all three base-dtor lanes; (3) external C1 as a
  base ctor built a STANDALONE layout (`basic_ios` at +16 over
  `__sb_`) — `ctor_call_assemble` demotes the vbase-forward lane to
  the madc C2 body (gate `testistream_libcxx`). Also: dtor synthesis
  gates on whether the library PROVIDES the D1, per-symbol
  (@37e7069f — libc++'s export split). Lane 803/80 → **811/77** — 3
  FIXED (`teststreambool` byte-identical under BOTH flavors,
  `testusefacet_realhdr`, `testvbasedyn`), ZERO broken (set
  comm-diffed). `/dupaudit`: `dtor_symbol_resolution` CONSOLIDATED;
  `ctor_call_assembly` open (5 sites → task #86). Suite 887 → 889.
  Fork unchanged (1.0-madc.0.63.0). NEXT: #71 ref-qualified members
  (~7 lane tests), testcin `operator>>`, testmanip/testderefpreinc/
  testcastarrow residuals; banked: #60 (5-line reducer), #85 (JIT vs
  emit-C extern-bind divergence).

- **v0.64.0 (2026-07-31): the four-root string/stream breakthrough (task
  #78, P2.7 in progress).** The flavored lane went 747/108 → **803/80**,
  zero regressions at every set-diffed step — 23 tests flipped in ONE
  commit: the `testclass` SIGSEGV was FOUR compiler defects (a
  REFERENCE-typed argument bound to a VIRTUAL-base reference parameter
  passed unadjusted — stream `width()` read `__precision_`, phantom
  padding + precision clobber in every stream test; cast-to-reference
  ctor arguments scored as `Tag*` so libc++ tag-dispatch ctors never
  matched, + the binding twin; the value-init mem-initializer on a
  plain-struct member emitted NOTHING — every default-constructed libc++
  string rep was frame garbage; an untyped base-to-derived facet
  downcast). The detect-idiom chain closed 12 links
  (`vector<int>::push_back` RUNS, `iterator_traits<CLASS>` resolves,
  `__is_convertible`, east-specifiers, `__libcpp_datasizeof`);
  [temp.param]p10 default accumulation opens the input-stream gateway
  (`num_get<char>`). 15 new negative-controlled gates; six new probes;
  `/dupaudit` recorded 3 families (live one = task #80). Suite 856 →
  885. Fork unchanged (1.0-madc.0.63.0). NEXT: the cin/input cluster
  (istringstream ctor parse is the frontier), #71 ref-qualified members
  (map/optional), stream runtime residuals re-verified at HEAD.

- **v0.63.0 (2026-07-30): the libc++ parity-lane burn-down (task #34, P2.7
  in progress).** The flavored suite lane (`run_tests.sh --stdlib=libc++`)
  is first-class with its failing set banked in
  `docs/parity/libcxx-failset.txt`; the lane went 534/282 → **747/108**
  across ~50 oracle-verified fixes, every step set-diffed with ZERO
  regressions. THE STRING WALL FELL (`std::string c = a + b` correct under
  libc++; cout smoke runs). Four flavor-INDEPENDENT silent wrong answers
  fixed: pointer-target alias templates de-pointered (`using up = U*` gave
  U — every rebind in every header), ctor-less classes dropped their copy
  argument (uninitialized spill temp), empty non-primary bases laid out
  past the object (Itanium EBO offset 0), `--emit=c11` flattened bit-field
  widths. Out-of-line defs bind by SIGNATURE; ref-qualified methods
  ([dcl.fct]p6); east-cv template args; operator-> rewrites on
  member/call-result receivers; mangled-direct declines unlinkable
  symbols; `__is_final`. Suite 807 → 856. Fork release 1.0-madc.0.63.0
  (zero-length-array diagnostic parity). NEXT: the map/set family at
  map:629 (mem-init naming a template-param private base, task #72), the
  vector family's emission layer, stream residuals.

- **v0.62.0 (2026-07-29): the `<string>` frontier burn-down (task #17 in
  progress).** libc++ `<string>` PARSES CLEAN (anonymous-aggregate
  enclosing-alias resolution — the last parse blocker);
  `__compressed_pair` compiles+runs (ref-cast member access, receiver
  addressing, REAL derived-to-base conversion via `upcast_class_ref_addr`
  — silent wrong storage at nonzero offsets before); constant-expression
  NTTP args fold (bail() adopted `skip_template_id_suffix`; the plain-
  `depth` counter the gate could not see is deleted); numeric_limits
  values EXACT both flavors (five stacked constant-fold defects; the
  [basic.scope.class] member-alias shadow fix un-hijacks `__base` from
  libc++'s real `__function::__base`). 8 new gates; fulltest 807/0/0/9,
  `--exe` 791/0, `--obj` 791/0, packed 807/0/0/9. Fork unchanged.
  NEXT (banked in task #17): the `__compressed_pair` template-ctor
  selection ("no matching constructor (__default_init_tag,
  __default_init_tag)"), member-template explicit-NTTP body emission,
  is_modulo's NTTP-expression instantiation key — then `<iostream>`
  (P2.5), containers (P2.6), the flavored parity lane (P2.7).

- **v0.61.0 (2026-07-29): the stdlib-flavor switch (task #16, P2.3).**
  The std ABI inline namespace follows the PARSED stdlib config —
  `_GLIBCXX_USE_CXX11_ABI` (1 ⇒ `__cxx11` on the cxx11-tagged components)
  / `_LIBCPP_ABI_NAMESPACE` (e.g. `__1`, on every component) — pushed
  into the mangler from all three define-write sites (directive, forest
  PP replay, CLI `-D`); `__cxx11` now lives ONLY in the mangler. All
  canonical std:: spellings, `itanium_mangle_std_var` (`_ZSt4cout` vs
  `_ZNSt3__14coutE`), and the marshalling carrier follow the flavor
  (clang++-18 oracle; pre-cxx11 `Ss` form under `=0`).
  `cir_native_link_env` de-conflated: per-flavor C++ runtime DT_NEEDED
  (`madc_stdlib_flavor::link_libs`, probed from each toolchain's own
  empty link at build time — no flavor→SONAME table); `__APPLE__` stays
  purely platform. The libc++ native legs UNLOCK: both `_libcxx` gates
  pass `--exe`/`--obj` end-to-end. fulltest 798/0/0/9, `--exe` 782/0,
  `--obj` 782/0, packed 798/0/0/9. Fork unchanged. NEXT: #17 (P2.4,
  libc++ `<string>` end-to-end — incl. cout-under-`std::__1` symbol
  round-trip and the inline/abi-tagged method resolution strategy).

- **v0.60.0 (2026-07-29): is_destructible, both flavors, every lane.**
  `__is_destructible` joins the trait-builtin registry (libc++'s
  `__has_builtin` branch serves directly, incl. deleted/member-deleted/
  private destructors); libstdc++'s SFINAE/declval chain fixed at four
  layers (deleted-dtor recording; deleted-dtor CALL rejection = the SFINAE
  substitution failure; call-result pseudo-dtor receivers reach the
  explicit-dtor arm — calls are staged on the operator stack; member
  templates capture per-param DEFAULT token runs enforced per
  [temp.deduct]/8, failing candidates fall to the ellipsis catch-all on the
  REGISTRATION owner). The forest capture POISONS instead of baking a
  constant past an unenforceable SFINAE (the [traitfold] lesson's
  alias-node twin); format v36 carries the default runs. Gates:
  `testdestructible` + `testdestructible_libcxx`, exact on g++13 ==
  clang++-18. fulltest 798/0/0/9, `--exe` 780/0, `--obj` 780/0, packed
  798/0/0/9. Fork unchanged. Filed: FT-lane deduction-aware defaults,
  array-type template args, SFINAE access control, deleted-member-flag
  freeze carriage.

- **v0.59.0 (2026-07-29): the trait-engine release.** gcc13's
  `__and_/__or_/__not_` SFINAE machine + the constructible/assignable trait
  family fold correctly in every lane: [temp.deduct]/8 unused-alias-arg
  validation with baked-ref (`DataDefREF`) trait args;
  `__is_constructible`/`__is_nothrow_constructible` builtins (tri-state,
  honest declines); lexer-erased `noexcept` re-lexed as `throw()` into
  `FuncDef::noexcept_spec` (patterns + forest); variadic bases
  real-instantiate at the base-clause resolution — vector reallocation takes
  g++'s move_iterator lane, and the class-wide sticky arming that broke the
  c++20 legs was bisected out and replaced with base-specifier-scoped
  arming; instantiation keys split pointer from reference args
  (`X<int*>` ≠ `X<int&>`, forest v35); trait folds REFUSE dependent args —
  the freeze's capture parse had frozen `__bool_constant<0>` as
  is_assignable's pattern base (packed-battery catch, bind-gate
  `[traitfold]`). Gates: `testtraitassign`, `testconstructible`,
  `testtplargkey`, `testvariadicstatic`. fulltest 796/0/0/9, `--exe` 779/0,
  `--obj` 779/0, packed 796/0/0/9. Fork unchanged. KG: DupFamily
  `trait_builtin_dispatch_twins` recorded open (the live/local trait
  dispatch pair took three lockstep double-edits this branch).

- **v0.55.0 (2026-07-28): class statics bind to their real Itanium symbols,
  and the emitter gets ONE name owner.** Static data members of library
  classes carry their ABI symbol (`std::numpunct<char>::id` ==
  `dlsym("_ZNSt7__cxx118numpunctIcE2idE")`, byte-identical to g++); non-type
  template args encode as literals. The regression it exposed became a
  campaign: SEVEN instances of "one rule, half its domain" fixed around
  `var_emit_name` — definitions, ctor receivers, deref arms, eval captures,
  the host-shim target, asm-labeled functions (`testasmlabelfn`) — now held
  by `check-var-emit-name-bypass.sh` in fulltest. A /dupaudit merge gate also
  consolidated the qualifier-before-`::` classifier, adopted the two
  spelling scanners the July 27 sweep missed, fixed the `operator~`/`,`
  parse-key collision, and made free operators mangle their Itanium code in
  every scope. dlfcn builtins declare real POSIX pointer types. Open, with
  reducers: `Gap{builtin_redecl_half_adopt}` (getenv under real headers),
  GAP B (`std::use_facet`), `Gap{ns_unary_operator_dispatch}`.

- **v0.54.0 (2026-07-27): six C++ correctness fixes, four SILENT.** One
  reducer chain: a qualified name in an OPERAND position was returned bare
  instead of continuing its postfix chain, so `(int)N::f(x)` became `(int)(x)`
  at exit 0 and `(int)N::arr[1]` reached the lambda-introducer dispatch; under
  it, `(int)S::f(4)` reported `undeclared identifier 'S'` because the operand
  path carried a narrow copy of the class-qualifier rule the shared resolver
  already owned; under that, a static data member read as `0` from inside its
  own class body, because storage was registered only by the out-of-class
  definition, which is parsed *after* member-function bodies (g++ creates the
  decl in the class body and lets the definition complete it — madc's
  `DECL_IN_AGGR_P` is `vfEXTERN`, whose completion `addVariable` already had).
  A nested type is now a member of its enclosing scope with `struct` spelled
  too. Suites 769/0/0/9, `--exe` 753/0, `--obj` 753/0, packed 769/0/0/9.
  Two labelled placeholders shipped: system-header class statics still fold
  (needs the Itanium `storage_alias_name` for class statics — NEXT), and two
  scopes each declaring `struct Inner` still collide.

- **Slice B class-KIND parse-once (2026-07-15): B2 COMPLETE; B3 NEXT.** The
  standalone design in
  `docs/plans/2026-07-14-class-kind-parse-once-DESIGN.md` inventories the
  complete `TokenSTRUCT`/`TokenCLASS` output contract and specifies one-time
  dependent pattern capture, immutable structural `ClassPattern` nodes,
  substitution through shared registration/layout/mangling helpers, and a
  pattern-vs-sole-parse ratchet. Eligibility is decided before instantiation:
  an ineligible pattern uses today's token parser as its only tallied lane; an
  admitted pattern rolls back and fails loudly rather than retrying through the
  parser. B0 @`c6f05b48` landed the shared registration/completion helpers,
  transactional journal, class-lane counters, ratchet, and vector census. B1
  captures and normalizes primary/partial `ClassPattern`s once, fingerprints
  them, and carries/restores them directly through the owner-approved existing
  class-template `TEMPLATE_PAYLOAD` in CIR forest format v28. Bind-time reparse
  is absent. After owner approval of B1, B2 admits the narrow structural subset:
  unary concrete/template-param types, aliases, scalar/pointer/array members,
  simple declaration-only methods, and forward completion. Structural and
  forced-legacy metadata, runtime, C11, MIR, layout, and GCC/Clang Itanium
  symbols agree; an admitted failure rolls back and fails loudly without a
  parser retry. Gates: live and packed release **696/0/0/16**, bind **18/18**,
  tsubst **10 hit / 0 fallback**, compiler warnings **0**, source warning census
  **712/0**. The exact class census is pattern 3, parse 48604, cache 99334,
  opaque 24431. Next: B3 closes the measured vector declaration/type surfaces
  and must materially shift `testsubscript` parses to the pattern lane while
  reducing both live and bound instantiate/total time.
- **Bound forest RECORDS decode (2026-07-14, `cbcb79b6`): CLOSED, merged to
  `develop` (independently verified).** Callgrind found the new bound CIR
  regression below `CirFrozenForest::unit_segment`: whole-frame
  `byteplane_inv` cost 404.9 M instructions even though the consumer touched
  only a small subset of the 566,522 loaded records. The reader now exposes
  decoded transformed bytes, forest segments retain RECORDS columnar, unit
  load validates all child/connector bounds from the linkage planes, and only
  touched rows are reconstructed. Quiet-host `cir build` falls
  **0.361 -> 0.270 s** (final live 0.227 s); the tracked packed wall median
  falls **0.715 -> 0.593 s**. The binary remains exactly **9,708,520 bytes**
  with 240 units; fulltest and packed suite are **695/0/0/16**, bind gate
  **18/18**. The broad bind restore path remains structural work: 95% of
  recordable aggregates are needed by this workload, refuting an early-demand
  filter.
- **Family-D forest drain gaps (2026-07-14): CLOSED.** Function-local class,
  nested-function, and dependent-pattern hoists now use deterministic
  enclosing-emission-symbol x source-name x per-body-ordinal identities. The
  local-class carry remains enabled, the reducer holds at **308** pack drops,
  and both the live fulltest and packed release suite are **695/0/0/16** with
  every forest gate green, including `[subbind]`. See
  `docs/plans/2026-07-14-CODEX-HANDBACK-local-class-identity.md`.
- **Version:** `0.42.0` (per `VERSION`) — the **inline un-erasure**
  release on `develop` (ELF-completion S4 follow-through): `inline` is
  a real C++ specifier carrying vague linkage (keyword registry, every
  decl position, `inline namespace` on the keyword path). User-header
  inline functions — S4's `sumv` boundary — AND C++17 inline variables
  merge `STB_WEAK` across TUs (function and data); a dynamic init runs
  once per merged image behind a linkonce once-guard (g++ COMDAT-init
  model). `static inline` stays internal; C modes keep the erasure;
  `--emit=c11` renders portable `__attribute__((weak))`. Fork
  untouched (still `1.0-madc.0.41.0`). Fulltest + packed
  **754/0/0/9**; `--exe`/`--obj` **738/0**.
  `master` remains at v0.38.0 pending `/promote`.
  v0.41.0 was the **ODR/linkonce weak** release (ELF-completion slice
  4): the C++ vague-linkage set (template instantiations, in-class
  bodies, vtables, typeinfo, synthesized members) emitted linkonce and
  captured `STB_WEAK` — identical per-TU copies merge at native links,
  internal and external ld alike; fork binding enum
  (GLOBAL/WEAK/LINKONCE — only interposable WEAK suppresses inlining,
  gcc parity); bindings survive binary + text MIR round trips; fork
  release `1.0-madc.0.41.0`.
  v0.40.0 was the **ctor/init-array** release (ELF-completion slice
  3): per-TU initializers ride a real `SHT_INIT_ARRAY` section
  (gcc-shaped, RELRO lead; external `ld` collects it natively), the
  two-ctor-TU merge fence lifted, `DT_INIT` retired, guarded
  `__madc_sys_init_once`, and the latent c2mir `-g` debug-capture
  use-after-free fixed; fork release `1.0-madc.0.40.0`.
  v0.39.0 was the **AOT hardening + ELF-completion** release: PIE
  executables by default (`-no-pie` escape, PT_PHDR load-bias law),
  multi-object linking (`.o` caches link/run, `-r` = `ld -r`, four
  project obj_skips lifted, `--obj` lane 737/0), Full RELRO +
  non-exec stack on every image (addrpool = the GOT leads the RW
  segment under PT_GNU_RELRO, BIND_NOW as statement of fact),
  DT_DEBUG (gdb probes interface restored), and multi-`.o` DWARF
  merge (multi-CU; external-ld links were corrupt too — fixed by
  relocatable debug sections); fork release `1.0-madc.0.39.0`.
  v0.35.0 was the **small-binary + family-D** release: the packed release binary drops
  **101 MB → 9.26 MB** (<10 MB owner target; blob 3.8 MB) via per-segment
  zstd (pack L15 / dev codec-default), the snapshot-v2 segment-transform
  vocabulary (CHILDREN u32-delta 1.68→0.09 MB, RECORDS byte-plane-80
  2.05→0.89 MB with an SSE2 16×16 tile transpose — the planned ZDICT
  dictionary was measured on real pack frames and refuted), and
  owner-approved intern-spine compression in the release pack only
  (3.74→0.81 MB, ~7 ms/process owned-buffer rebind; dev freezes stay
  zero-copy). configure now REQUIRES libzstd-dev. The family-D drain-gap
  campaign lands in the same release: pack drops 483→308 (reducer) / 346
  (release), stable function-local hoist identity (enclosing emission
  symbol × source name × ordinal), ranked-callee operand typing, and the
  live-correctness ladder (`*this = v`, `*this->ptr() = c`, stream
  manipulators, chained arrows, catch-param grammar, contextual
  `operator bool`, transitive vbase offsets). fulltest **695/0/0/16**,
  packed suite **695/0/0/16**, bind gate 18/18. Bound testsubscript 0.70 s
  worst-case (decode cost of the compressed pack; no-include TUs
  unaffected). v0.34.0 was the **pack-time drain** release: Phase 2 rung 1
  of the embedded-header forest CLOSED — deferred header bodies drain to
  fixpoint at pack time and freeze fully evaluated, guarded by a pack-side
  c2mir check gate (fork `c2mir_check_tree` @062dd97) with error-tolerant
  reverts; first-ever fully-green packed suite (681/681); dual dev/packed
  binaries; timing trend in `docs/perf/forest-timings.tsv`. v0.33.0 was
  the **parse-once** release: the template re-parse deprecation campaign
  (Phase 5) is COMPLETE. Every member-template instantiation — first or
  repeat — takes its body from tsubst over the one saved pattern tree (the
  g++ model); the re-parse fallback machinery is deleted outright
  (`materialize_tsubst_skipped_body`, captured-span tokens, first-eager flag,
  −123 lines), and a coverage gap is a LOUD pre-c2mir error, never a silent
  re-parse. Suite burndown **312 HIT / 0 FALLBACK (100%)**, zero `[why:]`
  reason-classes; pre-acceptance eligibility census EMPTY across the suite.
  Also fixes the map-iteration SIGSEGV (class-typed for-init declarator lost
  its injected construction; new `testmapiter`) and emits typed dtor forward
  protos so `--emit=c11` container output compiles unpatched under gcc.
  fulltest **677 passed / 0 failed / 0 timed out / 16 skipped**. v0.32.0 was
  the rung-1 interning capstone (`TokenIdent::str` dropped); v0.31.0 the
  tag-arithmetic retirement; v0.30.0 the set-wall release.
  `master` still holds the v0.24.0 asmjit/Gecko backend at full C89 coverage;
  develop is **not** promoted to master until the CIR path reaches feature
  parity.
- **Two-tree front-end performance work (local branch, 2026-06-26):** Phase 3
  tsubst consumption and Phase 4 scalar widening are env-gated behind
  `MADC_XTEST_DEP_PARSE`. The local branch now records direct parser-resolved
  TYPE template args on concrete member-template `FuncDef`s (`tsubst_type_args`)
  and CIR consumes those args directly, retiring signature-based binding recovery
  and covering body-only type params. It also records direct TYPE parameter-pack
  elements in `tsubst_type_arg_packs`, and the first CIR fan-out slices now handle
  direct value-pack call arguments such as `sink(args...)` plus direct
  reference-pack call arguments such as `Args&... args` / `sink(args...)` and
  direct expression-pattern packs such as `sink((args + 1)...)` by expanding
  marked list children during `tsubst_cir`. Follow-up pack slices handle the
  simple forwarding-call pattern `sink(std::forward<Args>(args)...)`, cover the
  same structural path for `sink(std::move<Args>(args)...)`, re-resolve
  copied callee ids from concrete explicit template args, link covered
  member-template constructors to the same Tree-1 tsubst path, and admit covered
  system-header placement-new bodies including scalar `_Up` construction and
  simple class `_Up` construction with scalar/pointer constructor pack elements
  in allocator-style `new ((void*)p) _Up(std::forward<Args>(args)...)`. The
  current slice also covers direct `__destroy(T*)` helper bodies by deferring
  pointee-class inspection until after substitution, and fixes multi-buffer
  builtin/intrinsic lookup so split system-header/user parses can capture those
  helpers as Tree-1 recipes. Recent slices cover local non-pack nested namespace
  function-template calls such as `sink(nn::ident(v))` by re-resolving copied
  callee ids from substituted argument types, by-value class-object constructor
  packs inside allocator-style placement-new bodies, including singleton and
  multi-element packs, value-returning class-reference constructor-pack shapes,
  and local reference-returning identity-forwarding class-reference constructor
  packs. The 2026-06-25 local keystone extends copied dependent-call handling
  from re-resolution-only to local instantiation-on-miss: the tsubst/copy path
  now instantiates missing nested namespace function templates with
  concrete-typed synthetic params, rebuilds the copied call against the concrete
  callee, and rewrites copied reference-slot arguments back to the concrete pack
  pointer slots. The first non-pack system-header dependent-call slice now admits
  simple scalar/pointer calls, including reserved `__*` helper names, only when
  the substituted args/return are concrete non-class shapes and the resolved
  callee has a materializable body or external symbol; copied calls mark the
  resolved callee reachable so lazy system-header body emission follows the
  ordinary call path. Real system-header reference-forwarding/object-address
  packs now have the first direct placement-new aperture: each expanded nested
  call must resolve through `resolve_copied_dependent_call` and return the
  same/derived class expected by the constructor reference parameter. The next
  forwarding aperture also admits a different returned class when the target
  reference class has a single-argument converting constructor for it and the
  copied body materializes the converted target temp before the outer
  constructor call. A 2026-06-26 robustness slice also balances
  `parseFunction`'s temporary parameter compound scope on dependent-pattern
  parse errors, so malformed retained member-template pack declarators report
  cleanly instead of leaving a dangling `compounds` entry. The follow-up
  pointer-parameter-pack slice teaches deduction and substitution the direct
  `Args*... ps` spelling, binding `Args` to each pointee type and fanning out
  the declaration as `T0* ps__0, T1* ps__1`; the scratch destructor-pack probe
  now parses and runs under `MADC_XTEST_DEP_PARSE`. The follow-up
  destructor-pack slice admits `_Destroy_aux`-style member-template bodies named
  `__destroy` only when the retained body itself contains a direct
  `__destroy(T*)` marker, so concrete pointee substitution can reuse the
  existing destructor/no-op lowering while iterator/object-address destructor
  forms still fall back. The current perf-round instrument adds
  `--show-stats` body engagement counts (`H hit / F fallback`) so real workload
  coverage can be ranked before further widening. The allocator construct/destroy
  cluster then moved flag-on `testsubscript` to **21 hit / 14 fallback**. The
  follow-up vector slices materialized copied scalar free-operator bodies and
  dependent nested member-template bodies: `std::allocator_traits::destroy<_Up>`
  now re-resolves its inner member-template call, instantiates the concrete body,
  and rematerializes dependent explicit destructors after substitution. Flag-on
  `tests/testvector.mad` now reports **14 hit / 1 fallback**; the only remaining
  vector fallback is `std::__cxx11::basic_string::_M_construct<_InIterator>`
  (`template-id '<' in body`). That `_M_construct` shape is the next tractable
  target, followed by a vetted `__uninitialized_copy`/pair-ctor pass. Broader
  system-header destructor/object-address packs still fall back.
  Validation at the committed checkpoint is green both
  flag-off and flag-on at **670 passed / 0 failed / 0 timed out / 18 skipped**;
  `test_cir` is **92 test cases / 1137 assertions / 4 skipped**. Phase 4 is
  roughly **74% implemented** by coverage weight, not session count.
  **2026-06-25:** the generic `is_type_dependent` predicate (the
  `type_dependent_expression_p` / pt.cc:30357 analogue) landed and is COMMITTED
  (`62409d08`, green both gates) — the step-C spine the per-construct
  `tsubst_eligible` catalog retires onto; the trajectory now pivots from growing
  that catalog to generic resolver-reentry. **Keystone update:** the local
  tsubst/copy path now instantiates nested function templates, not just existing
  overloads (g++'s `tsubst → finish_call_expr` model), and the local
  `std::forward`/`std::move` callee-name match is removed. Next work before
  deleting the re-parse fallback is to retire the remaining system-header
  dependent-call bail and catalog entries one construct at a time: real
  system-header destructor packs, class-valued placement-new argument packs
  beyond the direct/converted returned-class apertures, object-address
  forwarding last, broader dependent-call packs, and template-id body surfaces.
- **Backend:** `madc parser → cir_node (MC11-IR) → c2mir → MIR` is the **sole**
  backend. The asmjit JIT and the Gecko parser/MIR-transpiler were both removed
  (commits `42e9b6e`, `64f44b3`). There is no `--backend=jit`; `--backend=mir`
  aliases to cir. Builds against the **madc MIR fork**
  (github.com/derekbsnider/mir, branch `develop`, pinned by `MIR_COMMIT`).
- **★ SMAUG 1.8 boots, runs, and is playable** through the CIR path: it boots to
  a live server (`Realms of Despair ready … port 4000`) and a client can create
  a character, navigate the world, and fight (the Newgate serpent fight runs).
  The project's north-star goal — running a real C89 codebase end-to-end —
  is now demonstrated on CIR.
- **CIR baseline (2026-06-11, `feature/template-instantiation-claude` @ `01528ed`):** **572 integration/unit pass / 0 fail / 0 timeout / 18 skip** (`<=>` compliance track COMPLETE incl. = default comparison synthesis ([class.compare.default]) — defaulted member operator<=> yields all six comparisons; ordering-vs-ordering ==/!= work. (`<=>` track functionally complete: token + <compare> types + hidden-friend bodies + REWRITTEN candidates ([over.match.oper] — r!=0, reversed ==, relationals via (x<=>y)@0; the only-<=>/== class idiom gives all six comparisons); remaining polish: = default comparison generation. (`<=>` slice 3a: the token lowering itself works — builtin byte-select into a category temp + class-operand operator<=> dispatch, testspaceship_realhdr 8 g++-verified shapes; remaining: rewritten candidates + = default comparisons, cpp-support.md P2.15. (`<=>` slices 1–2b landed: the C++20 three-way-comparison token is std-gated, the `<compare>` category types live from the REAL header, and hidden-friend operator bodies hoist/compile/dispatch — `r < 0` on `std::strong_ordering` calls the TU-local friend; free-operator dispatch + literal-0 null-pointer-constant scoring + friend-function access grants are general mechanisms, g++-verified by testfreeop/testhiddenfriend/testcompareops_realhdr; remaining: the `<=>` token lowering + rewritten candidates, cpp-support.md P2.15. Template-instantiation batch COMPLETE: 2a fn-template empty-pack elision — std::stof/stod/stold; 2b-i `"pre" + s` exported mangled-direct; 2b-ii `a + "literal"` free-operator **BODY instantiation** + the char_traits explicit-spec instantiation-key fix — `std::char_traits<char>::length` silently folded to 0 before; 2c loud no-ctor-match error — a class declaration whose initializer matches no ctor is now a compile error instead of a silent construction drop, plus the reference-arg ctor-scoring fix it surfaced and the generic `.expect_err` compile-error test convention; 2d reference operands resolve as the referenced class in all operator-resolution surfaces — `cout << s` with a `const string&` parameter and `a + b` on reference params now work; previous 2026-06-11 develop baseline was 557/0/0/18 — eval leftovers B+A0+A: DSL string VALUE compares, MadValue/MadArray → one `madc::value`, mangled-direct `<ns_madc>` + user-call-site scope capture; libmadc in-process eval runs on CirJitSession);
  **gcc.c-torture 1567 of 1652 in-scope (95.0%)** with 26 compile-failed,
  29 runtime-failed, 0 timed out under the post-audit baseline; 33 class-(c)
  tests (gcc-internal / torture-only / UB) formally skipped per
  `docs/parity/failset-classification.md` (user-signed 2026-06-11) via
  `docs/parity/torture-skip-manifest.txt`. The promote gate is **all 41
  class-(a) standard-C failures fixed (≥1608 of 1652 in-scope)**; the old
  "match asmjit 1645" wording is retired (capability sets diverged: asmjit
  passes 82 of CIR's 88 failures; CIR passes 34 of asmjit's 40 failures).
  Class-(b) GNU extensions (14 tests) are roadmap items, not gate blockers.
  The clean `develop` rebuild emits no compiler warnings. The class-(a)
  failures are the active CIR coverage worklist — see Track 1.3.
- **C++17 real-header `std::map` / tuple salvage branch (2026-06-22,
  `wip/map-cxx17-salvage-codex` @ `3534b44` plus local fixes):** the previous
  dirty session is preserved at `failed/2026-06-22-map-cxx17-attempt-codex`
  commit `3534b44`; live work continues on the salvage branch. Map bring-up is
  deliberately C++17-first: `testmap` uses `find/end` rather than C++20
  `contains`, and map canary flags are `--std=c++17 --no-embedded-headers`.
  Focused `testmap` and `teststdmapint` pass. The interrupted handoff
  regressions are recovered: `testforeach2`, `testtuple`, `testfstream`,
  `testloop`, `testmadcevalexpr`, `testmadcevalexprctx`, and
  `testmadcevalexprtyped` are green. C++20 comparison canaries remain green
  under per-test C++20 flags. The local fixes keep body-bearing `void` std
  function templates (`std::_Destroy`, `std::destroy_at`) on the real-header
  body-instantiation path and disambiguate duplicate flattened member C field
  names. The const/reference template-argument spelling and derived-to-base
  nested-template deduction fixes are now enabled by default after external
  method declarations gained typed pointer returns and scalar-reference argument
  addressing. Latest local handoff fixes generic explicit-template overload
  selection for namespace calls such as `std::forward<T>` and const-qualified
  class/struct visibility in CIR emission. The previous `std::get` scoped-alias
  wall is now fixed generically with same-DataDef typedef-alias preservation and
  concrete partial-specialization completion from the opaque template path. The
  later undefined local `basic_string...__o15` wrapper was moved forward by
  generic CIR reference-return/constructor-argument handling. Focused
  `testmap` now passes after a generic anonymous aggregate layout fix:
  `DataDefSTRUCT` records anonymous struct/union groups and CIR emits unnamed
  anonymous aggregate members, so libstdc++ `std::basic_string` keeps its
  `_M_local_buf`/`_M_allocated_capacity` anonymous union instead of flattening
  them as sequential fields. Latest salvage fixes recover C++20 `<compare>`
  constructor binding by preserving scoped enum constant DataDefENUM storage,
  and recover eval render overloads by letting exact object identity bind
  `madc::array`/`madc::value` to `array&`. Fulltest was rerun twice but is noisy
  under the runner's default 5-second per-test cap on this host: one run reported
  **657 passed, 4 failed, 2 timed out, 18 skipped** and another reported
  **650 passed, 3 failed, 10 timed out, 18 skipped** with shifting unrelated
  timeouts; isolated timeout candidates pass sequentially under the default cap.
  Stable functional reds are now `testcontainerdtor`, `testmadc_ns`, `testset`,
  and `testsubscript`. Remaining work: retire the broader map/set/container
  branch reds, fix `tmp/te_direct.mad`'s direct `std::get`
  declaration-initializer call loss, and clean up non-fatal libstdc++
  pointer-type diagnostics in the map path.
- **C++ model — proven on the old backend, being re-established on CIR:**
  ctors/dtors, operator overloading, references, `new`/`delete`, single
  inheritance, vtables, SJLJ exceptions + unwinding, access control, const
  enforcement. These all worked on the asmjit backend; CIR parity is the
  current push.
- **C++ library object model cleanup (merged to `develop`, 2026-06-05):**
  the retire-std-hardcoding gate is at **0 offending lines** on `develop`. The
  intended model is the g++/clang++ one: library classes are declared in
  standard/embedded headers and compiled through the same object, overload,
  mangling, ctor/dtor, and retbuf machinery as user classes. Core compiler and
  runtime code must not special-case concrete C++ library classes or objects
  such as `std::string`, iostream/istream/ostream, sstream, containers, or user
  classes; the exceptions are the mangler, header text, tests, and the
  auto-include trigger map. Auto-includes are now a default `--std=madc`
  convenience only; explicit `--std=c` and `--std=c++` modes require the
  appropriate headers. The cleanup also recovered `std::cin >>` via real
  libstdc++ declarations/operators and advanced generic real-header parsing far
  enough to handle the iostream/istream/ostream class machinery. The remaining
  release-prep work is warning-clean validation and driving the final fulltest
  reds/timeouts to green. Follow-ups stay generic: preserve include-guard/macro
  state before broad real system-header PCH regeneration, finish class/member
  alias resolution for real iostream aliases (`char_type`, `iter_type`,
  `iostate`, `__streambuf_type`, `__ostream_type`, `__ios_type`, and friends),
  and continue closing the real-header `cout`/stream operand walls.
  `std::string` construction (`std::string s("hello")` → `hello len=5`),
  mutation (`s += …`, `s = …`, `a + b`, `s[i]`), `size()`, real `<iostream>`
  output including `cout << unsigned long` / `s.size()`, `std::getline`, and the
  `tmp/fs_out.mad` ofstream write canary now run through real headers. Call-symbol
  derivation is now unified onto a single `CirBuilder::call_emit_symbol` resolver
  (precedence `emit_symbol ?: local_emit_name ?: var_emit_name`) guarded by
  `scripts/check-call-emit-symbol.sh` — no more per-site re-implementations; this
  unblocked binding inline extern-template members (`cout << unsigned long` →
  `_ZNSolsEm`). Remaining real-header walls: `cout << std::string` (the free
  `operator<<` must take the class rhs as a const reference, not a pointer), the
  free-`std::`-fn `emit_symbol` migration (retire the call-site re-mangle + the
  `__ns_` shim gate), and the per-red ingredients for `testfstream` / `testloop` /
  `testdefer`, which remain open.
- **libmadc:** C++ embedding API (security policy, structured diagnostics,
  engine-owned IO). In-process compile/exec/`eval` **runs on CIR→c2mir→MIR**
  via `CirJitSession` (2026-06-10), the script-level `madc::eval_*` surface is
  declaration-only mangled-direct through `<ns_madc>`, the script `array` IS
  the public `madc::value` (A0 unification, 2026-06-11), and runtime-eval
  scope capture fires at the user call site (int/real/array/string locals,
  per-family engine gates). Remaining: package C — `register_function`,
  `get/set_global`, string call marshalling, fork/limits, the policy tail
  (the 38 `test_libmadc_program` skips; see
  `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`).
- **AOT (native object/executable):** **FEATURE-COMPLETE ON ELF as of
  v0.39.0** — `madc -c` emits ELF `.o` caches, `madc -o` emits PIE
  executables (`-no-pie`/`-shared`/`-r` as with gcc), all **assembled by
  MIR itself** (no external toolchain; owner directive). The full rung
  ladder R1–R6 + PIE + multi-object linking + Full RELRO/NX + DT_DEBUG +
  multi-`.o` DWARF merge is landed: `.o` caches link and run (`ld -r`
  shape included), every image is Full RELRO + NX with the addrpool as
  the GOT, and `-g` debug info survives multi-object links (multi-CU).
  Slice 3 (v0.40.0) adds the platform ctor model: per-TU initializers
  ride `.init_array` (`DT_INIT_ARRAY`; `DT_INIT` retired), lifting the
  two-ctor-TU merge fence in every native lane. Slice 4 (v0.41.0) adds
  ODR/linkonce weak: the C++ vague-linkage set captures `STB_WEAK`
  (fork binding enum GLOBAL/WEAK/LINKONCE), so identical per-TU copies
  merge at links — internal and external ld alike. The inline
  un-erasure (v0.42.0) closes S4's `sumv` boundary: `inline` is a real
  specifier routed into vague linkage, covering user-header inline fns
  AND C++17 inline variables (once-guarded dynamic init). Plan +
  landing blocks:
  [2026-07-19-mir-aot-elf-plan.md](2026-07-19-mir-aot-elf-plan.md);
  `run_tests.sh --exe` / `--obj` are the live AOT arbiters (738/0 each).
  Slice 5 `.mir.rodata` split is **DEFERRED (owner decision
  2026-07-25** — RELRO subsumed its hardening value; capture-side
  two-pool split not worth the format change), closing the
  ELF-completion track. **Mach-O/ARM64 track (Track 6.3, ACTIVE — the
  next `/promote` milestone): axis A COMPLETE (v0.43.0) — the fork's
  build-time target selection + full aarch64 PIC-addrpool object
  capture/ELF relocs, and the emit-only `bin/madc-aarch64-linux` cross
  compiler; gate A green under qemu-aarch64. Axis B writer + cross
  madcs COMPLETE (v0.44.0) — `mir-macho.c` emits ad-hoc-signed
  MH_EXECUTE PIEs for arm64 AND x86-64 behind the `MIR_object` seam;
  emit-only `bin/madc-{x86-64,arm64}-macos`; Gate B green vs
  clang+ld64.lld references (llvm-18 oracle, independent signature
  re-hash); Gate B-final GREEN on BOTH owner Macs (identical output,
  exit 28 — AMFI accepted the MIR-generated signature). madc-on-macOS
  Route 1 Phase 1 COMPLETE (v0.45.0) — hosted arm64/x86-64 darwin madc
  binaries (JIT + native Mach-O AOT) with the embedded darwin C
  prelude; G2 green on Apple hardware, all lanes, both arches
  ([2026-07-25-madc-on-macos-plan.md](2026-07-25-madc-on-macos-plan.md)).
  Forest-carriers S1 COMPLETE (v0.46.0) — hosted binaries ship PACKED:
  darwin groves cross-frozen in the container, embedded as a
  `__MADC,__forest` section via `-sectcreate` (no re-signer on the
  build path), section read-back; grove bind == live parse on Apple
  hardware, both arches. Forest-carriers S2 COMPLETE (v0.47.0) —
  emitted-pack: `--pack-forest=<container>` embeds a frozen container
  in emitted native executables (ELF trailer / Mach-O `__MADC,__forest`
  section laid by the fork writer INSIDE the emit-time signature — no
  re-signer anywhere on the product path); Mach-O file-probe read-back
  arm; full native loop (freeze → pack-emit → AMFI → read-back) green
  on Apple hardware, both arches; darwin `--freeze-run` was already
  green (no self-rewrite). Forest-carriers S3 COMPLETE (v0.48.0) —
  carrier discovery chain (self-image → `<exe>.forest` sidecar →
  `$MADC_FOREST`, S4/S6 slots reserved), `--with-forest=embedded|
  sidecar|none` configure axis, failure-policy knobs
  (`forest_missing_policy` silent/loud/strict + `enable_external_forest`
  in the RegistrationPolicy sandbox family); full shape × platform
  matrix green (Linux arbiter through BOTH carriers 756/0/0/9; Mac 7/7
  legs both arches). Forest-carriers S4 COMPLETE (v0.49.0) — the SHARED
  shape: forest-in-library discovery arm (`dladdr` → the libmadc image;
  `<lib>.forest` sidecar behind it; image arms deliberately ungated by
  `enable_external_forest`, so a sandboxed strict host still binds),
  `--enable-shared` thin-CLI configure axis with the release pack
  targeting `lib/libmadc.so`, and the forest knob family on the public
  embedding API (`enable_forest_bind` / `forest_missing` /
  `enable_external_forest`; `Program::forest_bind_enabled` folded into
  `RegistrationPolicy`); permanent `forest_library_gate` (9 legs incl.
  the `enable_external_forest=false` negative test S3 owed) + thin-CLI
  parity 756/0/0/9 + `--enable-shared` product arbiter 756/0/0/9
  ([2026-07-25-forest-carriers-plan.md](2026-07-25-forest-carriers-plan.md)).
  Forest-carriers S5 COMPLETE (v0.50.0) — **`-static-libmadc`**: the
  C-lane runtime became dual-build C11 sources (`src/rt/rt_except.c`,
  `src/rt/rt_vla.c`) that the host build compiles into libmadc AND madc
  compiles through c2mir at pack time into **AOT ledger** MIR modules,
  carried in a new optional forest-container segment (so they ride every
  carrier, read independently of the grove bind — the ledger is
  target-specific but dialect-agnostic). At emit the needed modules are
  pulled transitively before the link and the cover analysis verifies
  the image is madc-free; distinct refusals for "this build ships no
  ledger" vs the Tier-B symbol list. Also fixed the copy-relocated
  libc-data cover bug (`stderr` in `bin/madc`'s own `.bss` had been
  forcing a needless `libmadc.so.0` on every AOT program that touched
  it). Permanent `forest_ledger_gate` (14 checks, baseline-per-program
  so nothing passes vacuously) + PRODUCT path proven: packed
  `bin/madc-release` (240 units + 2 ledger modules) emits a try/catch
  program with zero `libmadc` DT_NEEDED that runs under an empty library
  path; fulltest 756/0/0/9, `--exe` 740/0, packed arbiter 756/0/0/9.
  **Forest-carriers S6 COMPLETE (v0.51.0) — the carriers track is DONE:**
  `madc.ini`, completing the settings precedence rule **CLI >
  environment > madc.ini > baked defaults**, enforced in one visible
  place. Keys `std` / `forest` (discovery **arm 5**, the last one) /
  `include` (repeatable, after every `-I`) / `cpu-limit` / `mem-limit`;
  lookup `./madc.ini` → `$XDG_CONFIG_HOME/madc/madc.ini` →
  `<sysconfdir>/madc.ini` with the first existing file winning outright
  (never merged); relative paths resolve against the config FILE's
  directory. The parser is STRICT — an unknown key or malformed line is a
  hard error naming file:line, never a warning that half-applies — and
  hand-written rather than a TOML dependency (auditable, and it keeps the
  self-hosting bar low). New `--config=<file>` / `--no-config`; the
  reader is a CLI feature (libmadc never consults a config file) and the
  forest key rides `enable_external_forest` with the sidecar/env arms;
  `configure --disable-config-file` removes the path entirely. Suite
  hermeticity: `run_tests.sh` and both pack scripts pass `--no-config`.
  The reader is **schema-blind and shared** (`madc::cfg::config_file`) —
  it owns the format, each consumer registers its own keys, so madcdat and
  madcdis-based tools reuse it instead of copying a parser (the same split
  `madcdis/snapshot.h` makes as a content-blind container). Permanent
  `forest_config_gate` (39 checks / 18 legs, every settings leg paired
  with a baseline that would fail without the file) + `test_config_file`
  (19 cases, incl. a reuse suite driving the reader as a different
  application). Batched with it: the installed `madcdis/snapshot.h` now
  compiles downstream (`madc_pch.h`, which its public signatures need, was
  never installed), and `docs/build.md` — which still documented asmjit as
  a build requirement — was rewritten.
  **MH_OBJECT `.o` FLAVOR DONE (2026-07-26, axis B step 4 — axis B is
  complete):** `madc -c` for a Mach-O target writes a real `MH_OBJECT`
  that **`ld64.lld` links** (mixed with a clang TU too), and
  `MIR_object_read` reads one back, so `-c` → link, the two-TU merge and
  `-r` all work on darwin. Read-back is proven EQUIVALENT — the `.o`
  path's image disassembles identically to the direct emit, which is how
  a real bug surfaced (Mach-O's single `PAGEOFF12` vs ELF's two kinds:
  the opcode sniffing dropped `sf`, reading every `add #imm12` back as a
  scaled load). One merge implementation, two container fronts, via a
  format-neutral input view. Gate: `make -C src machogate`, 30 assertions
  over both arches.
  **`-static-libmadc` IN THE `.o` LINK LANE DONE (2026-07-26) — the S5
  stated boundary is lifted:** the runtime enters as one more
  relocatable (pulled into a private object-mode context, generated,
  emitted, then merged into the same builder as the inputs — through the
  MERGE, because that is where symbol unification lives, and it is the
  read-back path both containers already gate). The cover analysis now
  takes the reference LIST rather than its source, so the source lanes
  pass context imports and this lane passes the merged builder's UNDEF
  names. Fixed on the way: the AOT-ledger carrier now opens header-only
  (the ledger is a container-global segment; `complete_open` binds the
  frozen pool into live parse state a link-only lane has no reason to
  own). What the lane actually hit was the host-call adapters — a `.o`
  keeps its `__madc_shim_*` surface by default and its 12 `madc_value_*`
  imports are Tier B — so the build can now say the artifact will never
  be host-called: **`-fno-eval-shims`** (the shape `-fPIC` has).
  `forest_ledger_gate` leg 9 flipped from asserting the refusal to
  proving the lane, two objects deep, against the libmadc-linked
  baseline as oracle.
  REMAINING: the S5 Mac hardware legs
  (the darwin cross pack now carries the ledger and refuses without one,
  but running an emitted Mach-O needs the owner's Mac); the value ABI as
  Tier-A C11 runtime, which is what a host-callable
  `-shared -static-libmadc` plugin needs (that combination refuses today
  for the same Tier-B reason, independently of the `.o` lane); the
  in-process `.o` loader on Apple hosts (needs
  `MAP_JIT` + the Mach-O parse; refuses loudly today); the darwin
  **dylib** packaging shape and the existing-signed-binary re-signer
  remain consciously deferred residue; P2 libc++ STD-ABI script-lane
  flavor.
  Plan:**
  [2026-07-25-macho-arm64-plan.md](2026-07-25-macho-arm64-plan.md).
- **Legacy reference (asmjit backend, pre-removal):** GCC-torture parity reached
  ~97.9% and ~475 integration tests passed. Retained only as the parity target
  the CIR path is climbing back to — NOT the current state.

---

## Track 1: Language Core

*Make the compiler correct, clean, and fast.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 1.1 | C foundation (GCC parity) | — | **DONE** (97.9% on the old backend; the CIR target) | — |
| 1.2 | Code cleanup Phase A — dispatch table, AST visitor, file split | 2-3 wk | **DONE** (v0.20.1) | [code-cleanup.md](code-cleanup.md) |
| 1.3 | **CIR coverage — drive `cir_node` (MC11-IR) → c2mir → MIR to the re-defined promote gate** | ongoing | **Active — the parity-to-master gate, RE-DEFINED 2026-06-11** (user-signed failset audit, [failset-classification.md](../parity/failset-classification.md)): gate = all 41 class-(a) standard-C failures fixed (≥1608 of 1652 in-scope; currently 1567), 33 class-(c) gcc-only/torture-only tests formally skipped, 14 class-(b) real-world GNU extensions as roadmap items. "Match asmjit 1645" is retired (capability sets diverged: CIR passes 34 tests asmjit fails). Class-(a) collapses to ~12 root causes, headlined by K&R old-style definition parsing (23 tests, std-gated < C23, never C++) and implicit-decl forward-call binding (5 tests). Integration baseline 557/0/0/18 (2026-06-11). | — |
| 1.4 | Code cleanup Phase B — parser dereference/subscript unification | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.5 | Code cleanup Phase C — macro system, token hierarchy | 3 wk | Ready | [code-cleanup.md](code-cleanup.md) |
| 1.6 | **SIMD — add a minimal generic-vector extension to MIR (types + insns + per-target codegen) and a c2mir `vector_size` front-end** | large | **In progress (raise the floor)** — MIR branch `feature/simd-vector-support-codex` at `2ffebff` now has a partial MIR `v128` floor plus c2mir `vector_size` support, expression-valued `vector_size` arguments, leading GNU vector attributes in declarations and compound-literal type names, C2MIR `__builtin_abort`, `__builtin_memcmp`, `__builtin_copysignf`, and `__builtin_nan` lowering to libc/libm calls, all 37 GCC c-torture execute vector-construct files checked in and passing under C2MIR `-ei`/`-eg`, including exact `pr92618`, `pr94524-{1,2}`, `pr53645`, `pr53645-2`, `pr109040`, `simd-{1,2,4,5,6}`, `pr65427`, `pr60960`, `pr70903`, `pr85169`, `pr108292`, `scal-to-vec{1,2,3}`, `20050316-{1,2,3}`, `pr105613`, `pr72824-2`, and `fp-cmp-cond-1` runtime coverage plus empty GNU asm barrier parsing, narrow address-taken register rvalue extension, union-alias preservation through array subscripts, and one-lane unsigned `__int128` vector equality broadened to one-lane `__int128` vector arithmetic, bitwise, unary, comparison, shift, compound, inc/dec, and div/mod helper-call lowering, Clang `ext_vector_type` support including non-power-of-two logical lane counts, same-element-count `__builtin_convertvector` across supported vector widths, non-`v128` integer vector operation lowering through scalar lanes, non-`v128` same-size vector casts through memory-backed block copies, same-size integer scalar/vector reinterpret bitcasts, GNU declaration-spec vector attributes, mixed-signedness vector shift-count type compatibility, mixed-source-width `__builtin_shufflevector` support, packed `v128` f32/f64 arithmetic/comparison opcodes, packed `v128` i8/i16/i32 add/sub and comparison opcodes, packed `v128` i64 add/sub opcodes, packed `v128` i8/i16/i32 multiply plus i8/i16/i32 and i64 scalar-count and lane-count shifts, qword vector comparison scalar-fallback masks, packed `v128` i64 equality/order, scalar-condition vector conditionals, GCC vector inc/dec lowering, and x86-64 `v128`/`v64`/`v32`/`v16`/`v8` integer-vector ABI support, plus `MIR_T_V128` text/binary data I/O support, and one-lane `__int128` vector div/mod helper-call lowering; no known <=16-byte SIMD gap remains, but Track 1.6 is still partial; design for **upstream** to vnmakarov/mir | — |

**Track 1.6 (SIMD) raises the *floor*, not just c2mir.** MIR today has no vector
type/insns (locals are `i64/f/d/ld` only), so real SIMD-in-JIT requires adding
vectors to MIR itself + per-target codegen (x86-64 SSE/AVX, aarch64 NEON, …) +
interpreter support + ABI/serialization, plus a c2mir front-end for GNU
`vector_size` / generic vector ops. **Design it for upstream** — it benefits MIR
directly (WASM→MIR, a stated MIR future goal, *requires* SIMD since WASM has
fixed-width SIMD; every MIR target has a vector ISA; it lifts ~11 deferred SIMD
torture tests). Keep it a **minimal generic-vector core** to fit MIR's
lightweight ethos. Interim until it lands: madc **scalarizes** for the JIT and
**emit-C → gcc/clang** for real SIMD (AOT). Feeds Track 6.2 (macOS NEON). See
the lowering-vs-raising rule (`.claude/rules/`) and ADR 0001.

2026-06-05/06 checkpoints: `/workspace/mir` branch
`feature/simd-vector-support-codex` is at `2ffebff`, not yet pinned by madc's
`MIR_COMMIT`. `6257780` adds the first c2mir front-end slice with distinct
memory-backed GNU `vector_size` types, brace initialization, scalar
indexing/lvalue writes, block copy, and memory-shaped param/return plumbing.
`2194f8c` adds MIR `v128`, `vmov`, `vaddi32`/`vsubi32`, vector bitwise ops,
signed `v4i32` comparisons, interpreter/x86-64 codegen, `mir-tests/test17.mir`,
and C2MIR lowering for signed `v4i32` arithmetic/bitwise/unary/scalar
splats/comparisons. `eceffd0` adds unsigned `v4u32` equality/inequality.
`516db72` adds same-size `v128` vector-to-vector casts. `3a7bbbd` adds
unsigned `v4u32` ordering comparisons by biasing operands and reusing the
signed compare. `240f838` adds `v4i32`/`v4u32` vector shifts via lane-wise
scalar MIR lowering for vector/scalar counts and compound shifts.
`99c19a6` adds `v4i32`/`v4u32` vector multiply, divide, and modulo via
lane-wise scalar MIR lowering, including compound assignment coverage.
`52940dd` adds C2MIR builtin recognition and lane-wise lowering for
`__builtin_convertvector` and `__builtin_shufflevector` on `v128` vectors with
4- and 8-byte arithmetic lanes. `0305f1d` adds C2MIR builtin recognition and
lane-wise lowering for GCC `__builtin_shuffle` on same-type `v128` sources
with signed/unsigned runtime mask modulo handling. `2982f38` extends C2MIR
lowering to `v128` integer vectors with 1-, 2-, 4-, and 8-byte lanes, covering
signed/unsigned small-lane arithmetic, bitwise, unary, comparisons, shifts,
compound assignment, same-type shuffles, `uint16x8_t`, and Clang-style
`__builtin_vectorelements`. `40661db` adds C2MIR support for width-changing
`__builtin_shufflevector` results from `v128` sources. `62f8f31` adds
`__builtin_shufflevector` support for non-`v128` source vector widths through
memory-backed scalar lane copies. `84377c2` adds generic non-`v128` GCC
`__builtin_shuffle` support for same-type vectors through memory-backed scalar
lane copies and runtime mask modulo handling. `665dbbb` adds C2MIR `v128`
floating-lane arithmetic and comparisons through memory-backed scalar lane
lowering, covering `v4sf` add/mul/unary/scalar splats/compound ops and `v2df`
division/comparison masks. `1deb2be` extends that floating-lane lowering to
non-`v128` float/double vectors, covering `v2sf`, `v8sf`, and `v4df`
arithmetic/scalar splats/unary/comparison masks. `56e331b` adds x86-64 `v128`
vector parameter/return ABI support across C2MIR prototypes, MIR generated
calls/prologues/returns, interpreter FFI, and interpreter shims. `6de64b4`
adds x86-64 `v64` vector parameter/return ABI support by classifying top-level
8-byte vectors as one SysV SSE eightbyte through the existing `blk2:8` /
`MIR_T_D` path, with `v2si`, `v2sf`, and `v4hi` coverage. `8a16ed6` adds
x86-64 `v32` integer-vector parameter/return ABI support by classifying
top-level 4-byte integer-element vectors as one SysV integer eightbyte through
the existing `blk1:4` path; coverage includes `v1si`, `v2hi`, `v4qi`, and the
GCC-memory-ABI `v1sf` control. `fe49fde` adds MIR packed `v128` f32 arithmetic
opcodes (`vaddf32`, `vsubf32`, `vmulf32`, `vdivf32`) with interpreter support
and x86-64 SSE `addps` / `subps` / `mulps` / `divps` codegen; C2MIR selects
them for `vector_size(16)` float arithmetic while preserving scalar-lane
fallback for other floating vector widths and double lanes.
`ffa02b6` adds MIR packed `v128` f32 comparison opcodes (`veqf32`, `vnef32`,
`vltf32`, `vlef32`) with interpreter support and x86-64 SSE `cmpps` codegen;
C2MIR selects them for `vector_size(16)` float comparisons and lowers `>` /
`>=` by swapping operands into ordered `<` / `<=`. `52a75bb` adds MIR packed
`v128` f64 arithmetic opcodes (`vaddf64`, `vsubf64`, `vmulf64`, `vdivf64`) and
comparison opcodes (`veqf64`, `vnef64`, `vltf64`, `vlef64`) with interpreter
support and x86-64 SSE2 `addpd` / `subpd` / `mulpd` / `divpd` / `cmppd`
codegen; C2MIR selects them for `vector_size(16)` double
arithmetic/comparisons while preserving scalar-lane fallback for non-`v128`
double vectors.
`798f18d` adds MIR packed `v128` i8/i16 add/sub opcodes (`vaddi8`, `vaddi16`,
`vsubi8`, `vsubi16`) with interpreter support and x86-64 SSE2 `paddb` /
`paddw` / `psubb` / `psubw` codegen; C2MIR selects them for
`vector_size(16)` byte/word add/sub while preserving scalar-lane fallback for
8-byte integer lanes.
`3f287d0` adds MIR packed `v128` i8/i16 comparison opcodes (`veqi8`, `veqi16`,
`vgti8`, `vgti16`) with interpreter support and x86-64 SSE2 `pcmpeqb` /
`pcmpeqw` / `pcmpgtb` / `pcmpgtw` codegen; C2MIR selects them for
`vector_size(16)` byte/word equality and ordering comparisons, including
unsigned ordering through lane sign-bit biasing, while preserving scalar-lane
fallback for 8-byte integer lanes.
`730b50d` adds MIR packed `v128` i16 multiply opcode (`vmuli16`) with
interpreter support and x86-64 SSE2 `pmullw` codegen; C2MIR selects it for
`vector_size(16)` signed/unsigned short multiplication and compound
multiplication while preserving scalar-lane fallback for byte, dword, and qword
integer multiply/div/mod.
`9fb836d` adds MIR packed `v128` i16 scalar-count shift opcodes (`vlshi16`,
`vrshi16`, `vurshi16`) with interpreter support and x86-64 SSE2 `psllw` /
`psraw` / `psrlw` codegen; C2MIR selects them for `vector_size(16)`
signed/unsigned short scalar-count shifts and compound shifts while preserving
scalar-lane fallback for vector-count shifts and other lane widths.
`2ec7b5d` adds MIR packed `v128` i32 scalar-count shift opcodes (`vlshi32`,
`vrshi32`, `vurshi32`) with interpreter support and x86-64 SSE2 `pslld` /
`psrad` / `psrld` codegen; C2MIR selects them for `vector_size(16)`
signed/unsigned int scalar-count shifts and compound shifts through the
generalized 16-/32-bit integer-lane shift path while preserving scalar-lane
fallback for vector-count shifts and unsupported lane widths.
`4dcf378` adds C2MIR support for scalar-condition vector conditional
expressions with matching vector true/false arms. Vector conditional results
now lower through the memory-shaped aggregate path, covering assignment and
vector-conditional rvalue indexing. GCC and clang C reject vector-condition
ternary/logical forms; GCC also rejects scalar/vector mixed ternary arms, so
this checkpoint follows the GCC-compatible matching-vector-arm subset.
`b84da0d` generalizes C2MIR `__builtin_convertvector` beyond the earlier
`v128`-only gate. Same-element-count conversions across supported arithmetic
vector lane widths now lower through generic memory-backed vector lanes,
covering `v64` conversions and `v128`-to-`v256` same-element-count widening.
`3146f66` widens C2MIR integer vector operation lowering beyond the `v128`
gate. Arithmetic, bitwise, shifts, unary operations, and comparisons for
supported non-`v128` integer vector widths now lower through scalar lanes, with
`v32`, `v64`, and `v256` fixture coverage.
`09b79af` generalizes C2MIR same-size vector reinterpret casts beyond the
earlier `v128`-only gate. Non-`v128` vector casts now lower through
memory-backed block copies while `v128` keeps the register move path, with
`v64` and `v256` bitcast fixture coverage.
`9f6132e` adds C2MIR same-size integer scalar/vector reinterpret bitcasts.
Integer-to-vector casts write the integer representation into vector storage,
vector-to-integer casts load from materialized vector storage, same-size
float/pointer scalar casts remain rejected to match GCC/clang, and
`c-tests/new/vector-size.c` now covers `v32` and `v64` scalar integer/vector
bitcasts in both directions.
`3b25f0a` extends C2MIR `__builtin_shufflevector` to same-element-type sources
with different vector widths. It validates source indexes against the combined
lane count and copies lanes from materialized sources using independent source
element counts. GCC accepts this mixed-source-width form; Clang rejects it, so
the checkpoint is recorded as GCC extension coverage.
`9de5b22` adds MIR packed `v128` i32 multiply opcode (`vmuli32`) with
interpreter support and x86-64 SSE4.1 `pmulld` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned int multiplication and compound
multiplication while preserving scalar-lane fallback for other integer lane
widths and div/mod.
`3bdf0e4` adds MIR packed `v128` i64 add/sub opcodes (`vaddi64`, `vsubi64`)
with interpreter support and x86-64 SSE2 `paddq` / `psubq` codegen. C2MIR
selects them for `vector_size(16)` signed/unsigned long-long add/sub and
compound add/sub while preserving scalar-lane fallback for other unsupported
64-bit integer operations.
`c96ac07` adds MIR packed `v128` i64 scalar-count shift opcodes (`vlshi64`,
`vurshi64`) with interpreter support and x86-64 SSE2 `psllq` / `psrlq`
codegen. C2MIR selects them for `vector_size(16)` signed/unsigned long-long
left shifts and unsigned long-long right shifts while signed long-long right
shifts remain on scalar-lane fallback because SSE2 has no arithmetic qword
right-shift instruction.
`3f33ff6` fixes the C2MIR scalar-lane fallback for qword vector comparisons:
8-byte comparison lanes now form all-ones masks in 64-bit temporaries with
64-bit subtract-from-zero, so `v2i64` / `v2u64` equality, inequality, and
ordering comparisons produce full-lane masks in generated mode.
`92accb7` adds MIR packed `v128` i64 equality opcode (`veqi64`) with
interpreter support and x86-64 SSE4.1 `pcmpeqq` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned long-long equality and inequality while qword
ordering comparisons stay on the scalar-lane fallback.
`7c169e7` adds MIR packed `v128` i64 ordering opcode (`vgti64`) with
interpreter support and x86-64 SSE4.2 `pcmpgtq` codegen. C2MIR selects it for
`vector_size(16)` signed/unsigned long-long ordering comparisons, including
unsigned ordering through lane sign-bit XOR biasing.
`dad14bc` synthesizes packed signed `v128` i64 scalar-count right shifts in
C2MIR with the existing `vurshi64`, `vxor`, and `vsubi64` floor. The permanent
`vector-size.c` fixture now covers the count-zero edge for `v2i64 >> 0`.
`2ed2c4a` extends x86-64 ABI classification for top-level v8/v16 integer
vectors, passing `vector_size(1)` / `vector_size(2)` integer vectors through
the existing integer-register `blk1` path instead of `blk0` / `rblk` memory
ABI; `vector-size.c` now covers `v1qi`, `v2qi` / `v2uqi`, and `v1hi`
parameter/return cases.
`e4e096b` synthesizes packed `v128` i8 scalar-count shifts in C2MIR with the
existing word-shift, mask, XOR, and byte-subtract vector floor; `vector-size.c`
now covers signed and unsigned `v16qi` / `v16uqi` left/right scalar-count
shifts plus compound signed right shift.
`29775cd` synthesizes packed `v128` i8 multiplication in C2MIR with the
existing word multiply, byte mask, word shift, and OR vector floor;
`vector-size.c` now covers signed and unsigned `v16qi` / `v16uqi` multiply and
compound multiply.
`0bf8e7c` adds GCC vector prefix/postfix inc/dec support for integer and
floating vector types in C2MIR. Prefix/postfix lowering reuses the existing
integer/float vector arithmetic paths with a splatted one, and postfix old
values are preserved when vector block-move assignments provide the result
destination. `vector-size.c` now covers `v4si`, `v16uqi`, `v4sf`, `v8si`, and
`v8sf` inc/dec cases.
`3a63473` adds C2MIR recognition for Clang `ext_vector_type` and
`__ext_vector_type__` attributes for power-of-two element counts. C2MIR maps
the element count to the existing byte-size vector representation, preserves
qualifiers, and reuses the current vector lowering paths. `vector-size.c` now
covers `clang_i4`, `clang_uh8`, and `clang_f4` extended-vector cases.
`5966c1d` splits C2MIR vector storage size from logical element count, allowing
Clang non-power-of-two extended vectors such as `ext_vector_type(3)` to keep
their logical lane count while matching Clang's rounded storage/alignment. The
same logical lane count feeds `__builtin_vectorelements`,
`__builtin_convertvector`, and `__builtin_shufflevector` result construction.
`vector-size.c` now covers `clang_i3` and `clang_uc3` size/alignment, logical
element count, and lane arithmetic.
`3d9b8af` adds GCC/clang parity for mixed-signedness vector shift-count
operands. C2MIR now accepts vector shift counts whose storage size, logical
lane count, and lane width match the shifted vector even when signedness
differs, while preserving lane-wise scalar lowering for non-uniform vector
counts. `vector-size.c` now covers `v4si` by `v4ui`, `v4ui` by `v4si`,
`v8hi` by `uint16x8_t`, and compound mixed-signedness vector-count left shift
cases.
`fc493fd` preserves GNU declaration-spec attributes and applies `vector_size`
and `ext_vector_type` after base type resolution. C2MIR now accepts GCC/clang
spellings such as `typedef signed char __attribute__((__vector_size__(16))) V;`
instead of dropping the attribute before the typedef declarator. `vector-size.c`
now covers that typedef spelling with signed-byte scalar compound modulo in the
pr94524-style shape.
`07dd396` parses attribute arguments as constant expressions and evaluates
`vector_size` / `ext_vector_type` arguments through the existing
constant-expression checker. C2MIR now accepts GCC/clang spellings such as
`__attribute__((__vector_size__(2 * sizeof (long long)), __may_alias__))`, and
`vector-size.c` now covers that spelling plus pr92618-style casted
vector-pointer stores that write all 16 bytes.
`fbb47f3` lowers C2MIR `__builtin_abort` to a void zero-argument call to libc
`abort`, matching GCC/clang lowering and avoiding the previous unresolved
`__builtin_abort` import. `c-tests/new/builtin-abort.c` covers the generic
builtin, and exact GCC torture cases `c-tests/gcc/pr92618.c`,
`pr94524-1.c`, and `pr94524-2.c` now pass under C2MIR `-ei` and `-eg`.
`48cd7be` accepts GNU empty asm statement barriers. C2MIR now parses
`asm` / `__asm` / `__asm__` statement syntax with qualifiers, operands, and
clobbers, rejects non-empty templates/output/goto asm, evaluates input
operands, and emits no MIR instruction for empty templates. Exact GCC torture
cases `c-tests/gcc/pr53645.c` and `pr53645-2.c`, plus focused
`c-tests/new/empty-asm.c`, now pass under C2MIR `-ei` and `-eg`.
`ff01f80` extends C2MIR `force_val` handling for narrow address-taken
register-backed scalar lvalues; `char` and `short` rvalues are now sign- or
zero-extended after byte/word pointer writes, fixing exact GCC `pr109040.c`.
Coverage adds `c-tests/gcc/pr109040.c` and focused
`c-tests/new/narrow-reg-address.c`.
`fbe5efb` lowers C2MIR `__builtin_memcmp` to an imported libc `memcmp` call
returning `int`, validates pointer/pointer/integer argument types, and avoids
the previous unresolved `__builtin_memcmp` symbol. Coverage adds focused
`c-tests/new/builtin-memcmp.c` plus exact GCC SIMD cases `c-tests/gcc/simd-5.c`,
`pr65427.c`, and `pr60960.c`.
`033732f` preserves leading GNU attributes in declaration specifiers and
type-name specifier/qualifier lists, covering macro-expanded vector forms in
ordinary declarations and compound literal type names. Coverage adds exact GCC
SIMD cases `c-tests/gcc/scal-to-vec1.c`, `scal-to-vec2.c`, and
`scal-to-vec3.c`.
`95e52f9` preserves union aliases through array subscripts so MIR O2 DSE keeps
union-width stores that are later read through array members. Coverage adds
exact GCC SIMD `c-tests/gcc/20050316-2.c`, which passes C2MIR `-ei` and `-eg`.
`626f75e` adds C2MIR `__int128` / `unsigned __int128` spelling and narrow
memory-shaped scalar handling, then lowers one-lane unsigned `__int128` vector
equality/inequality by comparing and storing the low/high 64-bit halves.
Coverage adds exact GCC `c-tests/gcc/pr105613.c`, which passes C2MIR `-ei` and
`-eg`.
`59117d8` lowers C2MIR `__builtin_copysignf` and `__builtin_nan` to checked
libm imports, clearing the IEEE vector-search blockers from exact GCC
`c-tests/gcc/pr72824-2.c` and `c-tests/gcc/fp-cmp-cond-1.c` under C2MIR `-ei`
and `-eg`; coverage also adds focused `c-tests/new/builtin-fp.c`.
`c69f4da` imports the remaining 21 exact GCC c-torture vector fixtures found by
the vector-construct scan, so all 37 GCC execute tests that mention vector
constructs are now checked in under `c-tests/gcc` and pass C2MIR `-ei` / `-eg`.
`55c65ee` adds MIR text and binary I/O round-trip support for `MIR_T_V128`
data items by serializing each vector element as 16 byte values. Focused
coverage now exercises textual scan/output in `mir-tests/scan-test.c` and
binary write/read in `mir-tests/io.c`.
`e4a8945` adds MIR `v128` lane-count shift opcodes for i8/i16/i32/i64 lanes:
`vlshvi*`, `vrshvi*`, and `vurshvi*`. C2MIR selects these opcodes for matching
vector-count operands, the interpreter executes them directly, and x86-64
generated mode lowers them through scalar lane loads/shifts/stores. Coverage
adds direct MIR scan/execute checks in `c-tests/mir/vector-shift-count.mir` and
C frontend checks in `c-tests/new/vector-shift-count.c`.
`360fdb5` adds C2MIR one-lane `__int128` and `unsigned __int128` vector
lowering for add/sub/mul, bitwise ops, unary ops, equality/ordering
comparisons, scalar-count and vector-count shifts, compound assignment, and
GCC vector inc/dec through low/high 64-bit halves. Coverage extends
`c-tests/new/vector-size.c`.
`2ffebff` adds C2MIR one-lane signed and unsigned `__int128` vector division
and modulo through `__divti3`, `__udivti3`, `__modti3`, and `__umodti3`
helper-call imports. C2MIR and the MIR binary runners now resolve those helpers
for saved MIR/BMIR execution, and `c-tests/new/vector-size.c` covers exact
small results plus high-half identity checks.
`/workspace/mir` `timeout 900 make test` passed at `2ffebff` with
interpreter/O0 `Tests 1121, Success tests 2242` and generated-mode
`Tests 1125, Success tests 2250`, focused `make scan-test` and `make io-test` passed
for the new `v128` data I/O coverage, the new checked-in exact vector copies
passed GCC native plus C2MIR `-ei` / `-eg` at `c69f4da`, and focused
builtin-fp and one-lane unsigned `__int128`
vector reducers passed GCC/clang assembly and native validation plus C2MIR
`-ei` / `-eg`,
focused
union-array alias reducers and adjusted array parameter probes passed C2MIR
validation, focused prefix vector-attribute cases passed
GCC/clang assembly and native validation plus C2MIR `-ei` / `-eg`, exact
`scal-to-vec1.c`,
`scal-to-vec2.c`, and `scal-to-vec3.c` passed C2MIR `-ei` / `-eg`, focused
`__builtin_memcmp` reducers passed GCC/clang native and
assembly validation plus C2MIR `-ei` / `-eg`, exact `simd-5.c`, `pr65427.c`,
and `pr60960.c` passed GCC/clang native validation plus C2MIR `-ei` / `-eg`,
generated MIR showed `import memcmp`, `memcmp_p`, and `call memcmp_p` calls,
focused empty-asm barrier reducers passed GCC/clang native
validation and C2MIR `-ei` / `-eg`, generated MIR for the focused fixture has
the input-operand call with no asm marker, exact GCC `pr109040.c` and focused
narrow-register reducers passed GCC/clang assembly/native validation plus
C2MIR `-ei` / `-eg`, focused `interp-test17` and
`gen-test17` passed, generated MIR showed `vmuli32`, focused v4i32 multiply
reducers passed GCC/clang assembly/native validation and C2MIR interp/gen
validation, and GCC/clang `-msse4.1` assembly showed `pmulld`. Focused v2i64
add/sub reducers passed GCC/clang assembly/native validation and C2MIR
interp/gen validation; GCC/clang assembly showed `paddq` / `psubq`, and
generated MIR showed `vaddi64` / `vsubi64` in both the focused reducer and
`c-tests/new/vector-size.c`. Focused v2i64 scalar-shift reducers passed
GCC/clang assembly/native validation and C2MIR interp/gen validation;
GCC/clang assembly showed `psllq` / `psrlq`, generated MIR showed `vlshi64` /
`vurshi64` for left and unsigned-right shifts. Focused signed v2i64
scalar-right-shift reducers passed GCC/clang `-msse4.2` assembly/native
validation and C2MIR interp/gen validation; GCC used a `pcmpgtq` / `psrlq` /
`psllq` / `por` sign-fill sequence, clang used `psrlq` / `pxor` / `psubq`, and
generated MIR showed the C2MIR `vurshi64` / `vxor` / `vsubi64` synthesis
including `v2i64 >> 0`.
Focused v2i64/v2u64 comparison reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR now uses 64-bit
`sub` mask formation for qword comparison lanes instead of 32-bit `subs`.
Focused v2i64/v2u64 equality reducers passed GCC/clang `-msse4.1`
assembly/native validation and C2MIR interp/gen validation; GCC/clang assembly
showed `pcmpeqq`, generated MIR showed `veqi64` for equality plus `vxor` for
inequality. Focused v2i64/v2u64 ordering reducers passed GCC/clang `-msse4.2`
assembly/native validation and C2MIR interp/gen validation; GCC/clang assembly
showed `pcmpgtq`, generated MIR showed `vgti64` for signed ordering and `vxor`
bias plus `vgti64` for unsigned ordering.
Focused v8/v16 integer-vector ABI reducers passed GCC/clang assembly/native
validation, C2MIR interp/gen validation against GCC-built and clang-built
shared libraries, and generated MIR now uses `blk1:1` / `blk1:2` arguments and
integer return registers instead of `blk0` / `rblk` memory ABI.
Focused v16qi/v16uqi scalar-shift reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR showed `vlshi16`,
`vurshi16`, and `vsubi8` selection for byte-shift synthesis.
Focused v16qi/v16uqi multiply reducers passed GCC/clang assembly/native
validation and C2MIR interp/gen validation; generated MIR showed `vand`,
`vurshi16`, `vmuli16`, `vlshi16`, and `vor` selection for byte-multiply
synthesis.
Focused vector inc/dec reducers passed GCC native validation and C2MIR
interp/gen validation for integer and float vectors. Clang rejects vector
inc/dec forms, so this checkpoint is recorded as GCC extension coverage.
Focused Clang `ext_vector_type` reducers passed Clang native/assembly
validation and C2MIR interp/gen validation. GCC ignores this Clang-only
attribute as expected. C2MIR interp/gen validation also passed for the full
`vector-size.c` fixture after adding the extended-vector cases. Focused Clang
odd-lane reducers for `ext_vector_type(3)` passed native/assembly validation
and C2MIR interp/gen validation. An odd-lane `__builtin_shufflevector` /
`__builtin_convertvector` reducer also passed Clang native/assembly and C2MIR
interp/gen validation. The full MIR `timeout 900 make test` passed after the
logical-lane change.
Focused mixed-signedness vector shift-count reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. Generated MIR for
the reducer showed lane-wise scalar `lshs` / `urshs` operations for the
non-uniform vector-count cases, not the low-64-bit scalar-count packed shift
opcodes. The full `vector-size.c` fixture passed GCC native validation and
C2MIR interp/gen validation with the mixed-signedness vector-count cases.
Focused declaration-spec vector-attribute reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. The exact GCC
`pr94524-1.c` and `pr94524-2.c` torture sources now pass exact runtime
validation after C2MIR lowers `__builtin_abort` to libc `abort`. The full
`vector-size.c` fixture passed GCC native validation and C2MIR interp/gen
validation with the declaration-spec vector-attribute case. The full MIR
`timeout 900 make test` passed after the memcmp checkpoint with `Tests 1090,
Success tests 2180` plus bootstrap checks.
Focused expression-valued `vector_size` reducers passed GCC/clang
native/assembly validation and C2MIR interp/gen validation. The exact GCC
`pr92618.c` torture source now passes exact runtime validation after
`__builtin_abort` lowering. GCC and clang assembly showed full 128-bit vector
stores for the casted vector-pointer store shape, and the full `vector-size.c`
fixture passed GCC native validation and C2MIR interp/gen validation with the
constant-expression attribute case. The full MIR `timeout 900 make test`
passed after the memcmp checkpoint with `Tests 1090, Success tests 2180` plus
bootstrap checks.
`/workspace/madc` fulltest hit the known failing set. The aggregate harness
reported 486 passed / 4 failed / 1 timed out / 55 skipped with
`testfortypedcomma` classified as `TIMEOUT` in this run. GCC/clang
packed-small-integer multiply and scalar-shift reducers matched the packed
word/dword shapes, and GCC/clang scalar-condition vector ternary reducers plus
C2MIR interp/gen reducers passed. GCC/clang
same-element-count convertvector reducers and C2MIR interp/gen reducers also
passed; focused GCC/clang non-`v128` integer-vector reducers and C2MIR
interp/gen reducers passed; focused GCC/clang non-`v128` vector-cast reducers
and C2MIR interp/gen reducers passed, with MIR dumps showing scalar lane
conversion, integer-operation lowering, and non-`v128` cast block copies;
focused GCC/clang scalar integer/vector bitcast reducers and C2MIR interp/gen
reducers passed, negative same-size float/pointer scalar-vector controls
rejected, and MIR dumps showed direct integer scalar/vector reinterpret stores
and loads; focused GCC mixed-source-width shufflevector reducers and C2MIR
interp/gen reducers passed, and the Clang rejection control still rejects that
mixed-source form.
Focused lane-count shift validation passed native GCC, C2MIR `-ei`, C2MIR
`-eg`, direct MIR `-ei`, direct MIR `-eg`, `make scan-test`, and
`make io-test`; generated MIR from the C fixture showed all twelve
`vlshvi*` / `vrshvi*` / `vurshvi*` opcodes.
Focused one-lane `__int128` vector reducers passed GCC/clang native validation
where the frontend accepts the operators, C2MIR `-ei`, and C2MIR `-eg`; the
full `vector-size.c` fixture passed GCC native validation plus C2MIR
interp/gen validation with the full one-lane `__int128` vector operator set.
Saved MIR `-ei` / `-eg` and saved BMIR interp/gen validation also passed for
the helper-call div/mod imports.
`git diff --check` is clean. Vector-condition ternary/logical semantics remain outside current
C2MIR C coverage because GCC and clang C reject those forms.
No known <=16-byte SIMD gap remains after the one-lane `__int128` vector
div/mod helper-call checkpoint. Later gaps include 32-byte-and-larger vector ABI support
beyond the covered stack-passed `pr109040` case requiring the broader AVX/YMM
or generic-vector MIR floor, broader MIR vector opcodes, registers,
interpreter support, codegen, and further optional per-target packed lowering.

**Track 1.3 is the central workstream.** It is the sole backend, so its
coverage *is* the bar for promoting `develop → master`. SMAUG 1.8 now boots,
runs, and is playable on this path (a real-world end-to-end proof); the ~95
remaining integration failures are the worklist between here and parity. Build
the `.mc11`/`.c` renderer + the gcc-`-fverbose-asm` fidelity gate + the
`cir_node`-vs-`c2m -d` differential to make those failures mechanical and
localizable, then reimplement eval/exec + REPL on MIR.

**Dependencies:** 1.2 before 1.3. **1.3 (full CIR parity) gates promotion to
master and unblocks Tracks 3, 5, 6, and AOT.**

---

## Track 2: C++ Support

*Extend from C scripting convenience to practical C++ OOP.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 2.1 | Constructors & destructors (RAII foundation) | 3-5 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.2 | Operator overloading completion | 2-3 d | **DONE** | [cpp-support.md](cpp-support.md) |
| 2.3 | References `T&`, const enforcement | 1 wk | **Mostly done** | [cpp-support.md](cpp-support.md) |
| 2.4 | `new` / `delete` | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.5 | Single inheritance | 1-2 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.6 | Virtual functions / vtables | 2-3 wk | **DONE** (v0.21.0) | [cpp-support.md](cpp-support.md) |
| 2.7 | Exception handling (SJLJ) | 3-4 wk | **Mostly done** (v0.21.0) — Phase A + B (scalar throw/catch + RAII unwind); **object/class-typed catch OPEN** | [cpp-support.md](cpp-support.md) |
| 2.8 | Quality of life | Ongoing | **Started** — access control, auto token position | [cpp-support.md](cpp-support.md) |
| 2.9 | Generic extern class ctor/dtor | — | **DONE** (v0.21.0) — replaces per-type switch boilerplate | — |
| 2.10 | **Single-name local instantiations (flattened→Itanium-mangled)** | 1-2 wk | **Planned** | — |
| 2.11 | **Self-host: madc compiles its own C++11 source** (ultimate dogfood) | large | **Planned (2026-06-26)** — feature audit done | failure-driven `selfhost` harness |

**2.3 remaining:** pointer-to-const enforcement (`*p` writes), const methods.
**2.7 remaining:** exceptions are SCALAR-ONLY (int/double/cstr/`catch(...)`).
Throwing/catching user-class or `std::` exception objects, and inheritance-aware
`catch (const Base &)` of a derived throw, are unsupported — the SJLJ runtime
carries no thrown object + catch dispatch is an integer-tag chain, not an RTTI
type match. Tracked as **P1.1e** in [cpp-support.md](cpp-support.md).
**2.8 remaining:** enum class, auto type deduction, broader real-iostream
output replacement, scope-level destruction.
**2.11 — Self-hosting (the ultimate dogfood test).** Audit (2026-06-26) of madc's own
`src/`+`include/` C++11: heavy templates/variadics, `dynamic_cast` (871), range-for (386),
lambdas (158), `std::move`, decltype, full STL (vector/map/set **+ stack/queue/deque/list**),
streams (sstream/fstream), `<algorithm>` (`std::sort` w/ lambda comparators), `<functional>`
(`std::function`×16), `unique_ptr`×32. Real pure-virtual/abstract = **0 uses** (not a blocker).
Gaps to build, hardest-first: **`std::function`** (type erasure over lambdas) → **`unique_ptr`**
(move-only RAII) → **`<algorithm>`** over iterators → **stack/queue/deque/list** → full
**stringstream/fstream** classes → decltype/alias-template/enum-class/`=default`/`=delete`
coverage. First step = a failure-driven `selfhost` harness: run madc (parse/sema) on each
src/include file, pass/fail → the empirical gap list; climb smallest TU → `parser.cpp`.

**2.10 — name every madc-local template instantiation by its Itanium mangled
name, retiring the flattened-key scheme.** Today madc carries TWO naming schemes
for the same entity: libstdc++-exported symbols are referenced mangled-direct
(`_ZNSt6vectorIiSaIiEE…`, via `madc_mangle`), while madc-monomorphized local
bodies (class-template instances like `vector<int>` + nested types, free-fn-
template instances `__ns_std__Destroy`/`__addressof`, member-template instances)
get flattened keys (`vector_int32_t_std__allocator_int32_t_…`). Carrying two
names for one entity is a standing source of confusion and drift (the mangler
should be the single name source). Unifying on the mangled name everywhere
(symbol table, emitted C, call sites, struct tags) gives: (a) `--emit=c11`
diffability against g++; (b) **free linker dedup** — a local instantiation whose
mangled name coincides with a libstdc++ weak export ODR-merges automatically, so
the "exported vs inline-only" decision disappears (always mangle; emit a body
only when nothing else defines it). **Cost/risk:** mangler completeness —
correct Itanium for nested types, member templates, and substitution compression
(`S_`/`S0_`); a wrong name becomes a link error or a silent wrong-symbol bind, so
migrate one category at a time behind the full gate. The member-template
convergence (Phase 2.10's first consumer — emit a local body under the mangled
name `itanium_mangle_member_template_sub` already computes when the owner is
local/not-exported) establishes the pattern.

**Dependencies:** All met. 2.1-2.7 complete.

---

## Track 3: Build Infrastructure

*Pre-compiled headers, modules, and portable builds.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 3.1 | PCH Phase 1 — post-lexer token serialization | 2 wk | **Partial** | [precompiled-headers.md](precompiled-headers.md) |
| 3.2 | PCH transition — replace text-embedded stubs | 2-3 wk | Blocked on parser | [precompiled-headers.md](precompiled-headers.md) |
| 3.3 | PCH Phase 2 — AST serialization | 4-6 wk | Future | [precompiled-headers.md](precompiled-headers.md) |
| 3.4 | PCH Phase 3 — C++20-style modules (.madm) | 6-8 wk | Future | [precompiled-headers.md](precompiled-headers.md) |
| 3.5 | Project build driver v1 (`--project`) | — | **DONE** | [madc-project-build-driver](../superpowers/plans/2026-06-08-madc-project-build-driver.md) |

**Dependencies:** 1.4 (parser cleanup) unblocks 3.2. 1.5 (token cleanup) before 3.3.

- **Unified front-end representation refactor (FULL DESIGN, later-stage optimization):**
  [2026-06-09-frontend-representation-refactor.md](2026-06-09-frontend-representation-refactor.md)
  is the comprehensive design that **details and reframes 3.1/3.3/3.4** (Phase-2 is now
  pre-PARSE `cir_node` AST, not just pre-lex; Phase-3 modules = the embedded forest) **and adds
  the Track-1 front-end prerequisites they depend on**: flat value-record token buffer + index
  cursor + source-stack (PoC: 3.6–4.5× over `deque<TokenBase*>`); an arbitrary-precision
  out-of-line value pool (fixes `__int128`/`_BitInt`, currently a 64-bit alias); `uid` as the
  universal handle with madc metadata in `uid`-keyed side-arrays (the c2mir-blind superset) +
  `uid`→MIR for debug info; static-immutable/project-volatile forest with materialize-on-resolve;
  and an optional, fenced c2mir hook seam (HIR/LIR; OSR/deopt + polyglot dynamic execution are
  flagged research-grade). Subsumes [2026-06-09-embedded-header-forest-design.md](2026-06-09-embedded-header-forest-design.md)
  and [2026-06-09-lazy-member-body-instantiation-plan.md](2026-06-09-lazy-member-body-instantiation-plan.md)
  (LANDED). **After** the current real-header correctness work, not before.

- Project build driver v1 landed (`--project compile_commands.json`, multi-TU compile+link+JIT-run of `main`). Replaces the need for a hand-written umbrella translation unit (like SMAUG's `SMAUG.mad`) for multi-file C programs. Deferred follow-ons: Makefile-subset reader + a link-description section (compile_commands.json carries no link rule); native `.madproj`; other-ecosystem readers; honoring `ProjectTU.working_dir` for include resolution (SMAUG will need it); `--project` + `--emit=c11` (project mode currently ignores `--emit`); real object-code-to-disk (parity-recovery item — asmjit on master had it); parallel/incremental build + manifest auto-detection. **Next concrete step: SMAUG bring-up via a generated `compile_commands.json` (separate follow-on plan).**

---

## Track 4: Embedding & Library

*Make madc usable as a library in other programs.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 4.1 | libmadc C++ API | — | **DONE** | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.2 | C ABI shim (`extern "C"`) | 2-3 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.3 | Fork-based isolation / worker mode | 3-4 wk | Partial | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.4 | Node.js integration | 4-6 wk | Future | [libmadc-phase4.md](libmadc-phase4.md) |
| 4.5 | Rust bindings (madc-sys + madc crate) | 2-3 wk | Future | [perry-rust-integration.md](perry-rust-integration.md) |

**Dependencies:** 4.2 before 4.4 and 4.5.

- **Type table + value ABI (DESIGN AGREED 2026-06-12):**
  [2026-06-12-type-table-value-abi-design.md](2026-06-12-type-table-value-abi-design.md)
  — one segmented uint32-typeid table (primitives / system-forest / project
  segments) as the canonical type identity, plus a 32-byte `madc_value`
  interchange struct (16-byte payload inlines every madc primitive incl.
  `__int128`/`_Complex`/`v128`; SSO; refcounted cells; gradual-typing flags
  LOCKED/COERCE/NULLABLE; re-tag unrestricted by default). **Eval package C is
  the first consumer**; the cir_node `datadef` side-array (refactor P3), forest
  type-ref serialization (P4), and the tag-arithmetic retirement are later
  campaigns on top. NaN-boxing (5A.5) stays internal-madcdis-only.

---

## Track 5: Data Substrate & Storage

*Three-tier data architecture: core substrate (madcdis) + external
drivers (madcdat) + language-conventional interfaces.*

### Track 5A: madcdis — Core Data Substrate (`libmadcdis`)

*Dependency-free core data substrate. The interfaces and implementations are
currently delivered through `libmadc`; the standalone `libmadcdis.so` split
remains planned. Pools, values, datasets, relations, query IR, typed flows,
raw channels, processes, and standard dependency-free drivers belong here.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5A.1 | Library restructure — split madcdis from madcdat | 2-3 wk | **Partial** (ownership split; standalone library pending) | [madcdis-plan.md](madcdis-plan.md) |
| 5A.2 | DataSet/Relation/Query/Schema/Mapper → `include/madcdis/` | 1 wk | **DONE** | [madcdis-plan.md](madcdis-plan.md) |
| 5A.3 | DataSource moves from libmadc to madcdis | 1 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.4 | Memory pools (arena, slab, size-class, intern) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.5 | Value system — NaN-boxing, refcounting, interning | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.6 | Multiplicity dedup for collections | 2 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.7 | Column encoding catalog (dict, RLE, FoR, delta, prefix, GCD) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.8 | mem:// and shm:// pool-backed drivers | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.9 | Federated query planner (core, capability-aware) | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.10 | GQL as canonical query language + SQL/Cypher lowering | 4-6 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.11 | Derivation relations (keyframe aggregation, retention) | 3-4 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.12 | COW snapshots (fork-based, page-level) | 2-3 wk | Planned | [madcdis-plan.md](madcdis-plan.md) |
| 5A.13 | Lazy Cursor/Sink/Flow + ABI-compatible streaming extensions | — | **DONE** @cd1f19c6 | [2026-08-07-data-channel-streaming-process-flow-plan.md](2026-08-07-data-channel-streaming-process-flow-plan.md) |
| 5A.14 | Raw channels + format bridge + explicit Process (`memory/file/FIFO/TCP/UDP/UDS/exec`) | — | **DONE** @cd1f19c6 | [2026-08-07-data-channel-streaming-process-flow-plan.md](2026-08-07-data-channel-streaming-process-flow-plan.md) |
| 5A.15 | Standard dependency-free record drivers (DSV/FLR/VLR); DSV native streaming | — | **DONE** @079ca8c3/@533947e1 | [madcdat-plan.md](madcdat-plan.md) |

### Track 5B: madcdat — External Storage Drivers (`libmadcdat`)

*Optional companion library. Depends on libmadcdis. External backends.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5B.1 | Library restructure — libmadcdat depends on libmadcdis | 1-2 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.2 | Optional external-library file/storage integrations (`snapshot://` remains planned) | Ongoing | Planned; DSV/FLR/VLR moved to core 5A.15 | [madcdat-plan.md](madcdat-plan.md) |
| 5B.3 | Keyed local DB drivers (BDB, GDBM, QDBM) | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.4 | SQLite driver | — | **DONE** | [madcdat-plan.md](madcdat-plan.md) |
| 5B.5 | Network DB drivers (MySQL, PostgreSQL) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.6 | Graph DB drivers (FalkorDB, Neo4j) | 3-4 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |
| 5B.7 | External-library service drivers (libcurl HTTP/HTTPS/REST/FTP/S3, MCP, mail) | 4-6 wk | Planned; raw TCP/UDP/UDS complete in core | [madcdat-plan.md](madcdat-plan.md) |
| 5B.8 | Structured text adapters (SMAUG areas, mbox, TOML) | 2-3 wk | Planned | [madcdat-plan.md](madcdat-plan.md) |

### Track 5C: Language-Conventional Interfaces

*Multiple syntactic surfaces over the same data substrate.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 5C.0 | Script-facing channel surface (`madc::channel` in `<ns_madc>`, `exec://` scheme, tcp/exec/httpget suite legs) | — | **DONE** v0.72.0 | [2026-08-08-track5c-script-channels-plan.md](2026-08-08-track5c-script-channels-plan.md) |
| 5C.1 | C-native core API (DataSet, Cursor, Query builder) | 2-3 wk | **Partial** | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.2 | C++23 ranges integration (madc::linq::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.3 | Ruby-style trailing blocks (madc::ruby::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.4 | Python comprehensions (madc::python::) | 3-4 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.5 | Objective-C brackets (madc::objc::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.6 | ORM-style records (madc::orm::) | 2-3 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| 5C.7 | Native query sub-grammars (sql::, cypher::, gql::) | 4-6 wk | Planned | [madc-interfaces-plan.md](madc-interfaces-plan.md) |

**Library structure:**
```
libmadc          (core: compiler, runtime, embedding API)
  ↑
libmadcdis       (optional: data substrate — pools, values, datasets, query, planner)
  ↑
libmadcdat       (optional: external drivers — BDB, GDBM, SQLite, MySQL, etc.)
```

**Dependencies:**
- The host-facing Track 5 core is active and validated through `libmadc`.
  Compiler-integrated language surfaces still depend on complete CIR coverage
  for their chosen C++ standard.
- 5A.1-5A.3 (restructure) first — moves existing code to new library boundary
- 5B.1 follows 5A.1 — madcdat depends on madcdis
- 5A.4-5A.5 (pools, values) before 5A.7-5A.12 (column encoding, COW, derivation)
- 5C.1-5C.2 (library-only surfaces) independent of compiler work
- 5C.3-5C.7 (compiler-integrated surfaces) require Track 9 (multi-syntax)

**Research:** [madcdis-memory-research.md](madcdis-memory-research.md) — design lineage from SMAUG, Lucene, modern arenas, refcounting

---

## Track 6: Platform Support

*Run madc on more than just x86-64 Linux.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 6.1 | macOS/ARM64 MVP (via MIR — c2mir + MIR are already cross-platform) | 10-15 wk | **Complete** (v0.45.0 hosted binaries; v0.76.0 public tarballs) | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.2 | macOS SIMD (NEON) | 2-3 wk | Blocked on Track 1.6 (raise MIR) | [macos-arm64-port.md](macos-arm64-port.md) |
| 6.3 | macOS AOT (Mach-O writer + aarch64 cross-gen) | 4-6 wk | **Complete** (v0.76.0: `-o` for C and C++; deferred residue: `libmadc.dylib`, in-process `.o` loader) | [2026-08-07-macos-release-lane-plan.md](2026-08-07-macos-release-lane-plan.md) |
| 6.4 | Windows port (a working Windows build + release artifacts; GitHub-Actions release automation follows it) | large | **NEXT — plan drafted** (owner, 2026-08-11/12: mingw-w64 + libstdc++ + UCRT, Win64 only, full-suite scope) | [2026-08-12-windows-release-lane.md](2026-08-12-windows-release-lane.md) |

**Dependencies:** 1.3 (IR) dramatically reduces 6.1 effort.

---

## Track 7: Rendering Abstraction (`ui::`)

*Universal semantic rendering: teletype to Unreal Engine. WCAG by design.
Hardware × user preference × accessibility three-way negotiation.
JIT-time capability resolution for zero runtime overhead.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 7.1 | Semantic IR + Level 0 (text stream) | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.2 | Level 1 — curses/terminal backend | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.3 | Reactivity + compiler-tracked state deps | 2-3 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.4 | Level 2 — 2D graphics (Skia/Cairo) | 3-4 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.5 | Level 3 — Web backend (WebSocket + thin JS) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.6 | Level 3 — Native GUI (SDL2/GTK) | 4-6 wk | Future | [rendering-abstraction.md](rendering-abstraction.md) |
| 7.7 | Level 4 — GPU/3D (WebGPU/Metal) | Future | Future | [rendering-abstraction.md](rendering-abstraction.md) |

**Dependencies:** 2.1 (constructors) for widget lifecycle. 7.1-7.2 can
start after 1.2 (cleanup) makes the parser ready for `render` blocks.

---

## Track 8: Tooling (madcide + libmadcedit)

*A Turbo-C style IDE and reusable editor library, built in madc.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 8.1 | libmadcedit core — piece table, cursor, undo, CUA keys | 3-4 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.2 | libmadcedit curses rendering | 2-3 wk | Blocked on 7.2 | [madc-ide.md](madc-ide.md) |
| 8.3 | Syntax highlighting + keybinding profiles (Vim, Emacs, Turbo-C) | 2-3 wk | Future | [madc-ide.md](madc-ide.md) |
| 8.4 | madcide shell — file tree, tabs, build, errors | 3-4 wk | Blocked on 8.2 | [madc-ide.md](madc-ide.md) |
| 8.5 | Advanced — find/replace, split views, go-to-def | Ongoing | Future | [madc-ide.md](madc-ide.md) |

**Dependencies:** 7.1-7.2 (rendering Level 0-1). Config via TOML + madc scripts.

---

## Track 9: Multi-Syntax Support

*Write madc programs in Python, Ruby, or Rust syntax. Controlled via
`#pragma syntax python`. Syntax is skin-deep — AST, compiler, IR unchanged.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 9.1 | Syntax profile infrastructure + lexer config | 2-3 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.2 | Python-style indentation mode | 3-4 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.3 | Type annotation variants (suffix syntax) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |
| 9.4 | Ruby/Rust profiles | 2-3 wk ea | Future | [multi-syntax.md](multi-syntax.md) |
| 9.5 | Mixed-syntax files (`#pragma syntax`) | 2 wk | Future | [multi-syntax.md](multi-syntax.md) |

**Dependencies:** 1.2 + 1.4 (parser cleanup). Editor highlighting reuses profiles.

---

## Track 10: Future Language Evolution

*Safety, modern features, and long-term direction.*

| Phase | Work | Effort | Status | Plan |
|-------|------|--------|--------|------|
| 10.1 | Optional bounds checking (`--check-bounds`) | 1-2 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.2 | Ownership annotations (RAII-based) | TBD | Future | [future-considerations.md](future-considerations.md) |
| 10.3 | Go-style error returns (multi-return convention) | 1 wk | Future | [future-considerations.md](future-considerations.md) |
| 10.4 | `-O` optimization levels (`-O0..-O3` flag landed; MIR-gen level) | 2-3 wk | **Partial** | — |
| 10.5 | TOML parser (for config files) | 1-2 wk | Future | — |

**Dependencies:** 2.1 (RAII) before 10.2. Existing multi-return enables 10.3.

---

## Ideal Execution Order

Each step builds on the previous. Items at the same indent level can
run in parallel.

```
 COMPLETED:
 ──────────
 1.  Track 1.2  Code cleanup Phase A                    [DONE v0.20.1]
 2.  Track 2.1  Constructors & destructors              [DONE v0.21.0]
 3.  Track 2.2  Operator overloading                    [DONE v0.21.0]
 4.  Track 2.3  References T& + const enforcement       [MOSTLY DONE]
 5.  Track 2.4  new / delete                            [DONE v0.21.0]
 6.  Track 2.5  Single inheritance                      [DONE v0.21.0]
 7.  Track 2.6  Virtual functions / vtables             [DONE v0.21.0]
 8.  Track 2.7  Exception handling (SJLJ + unwinding)   [DONE v0.21.0]
 9.  Track 2.8  Access control + auto token position    [DONE]
10.  Track 2.9  Generic extern class ctor/dtor          [DONE v0.21.0]

 NEXT UP (recommended order):
 ────────────────────────────
11.  Track 1.3  CIR coverage — cir_node (MC11-IR) → c2mir → MIR  [ongoing]
     ├── ★ SMAUG 1.8 boots, runs, and is playable on this path (v0.25.0)
     ├── Burn down the ~95 CIR integration failures toward 0
     ├── Build the .mc11/.c renderer + gcc -fverbose-asm fidelity
     │   gate + cir_node-vs-`c2m -d` differential
     └── Then reimplement eval/exec + REPL on MIR (deferred)
     ** THE PARITY-TO-MASTER GATE — sole backend; nothing downstream
        ships, and develop is not promoted to master, until this reaches
        full parity with master's C89 coverage **

 ║── Track 7.1  Rendering: Semantic IR + Level 0         [2-3 wk]
 ║   └── render { } blocks, UINode, text output
 ║       Can start in parallel with Track 1.3

12.  Track 1.4  Code cleanup Phase B                    [3 wk]
     └── Parser dereference & subscript unification
         Unblocks: PCH transition, parser resilience

 ║── Track 7.2  Rendering: Level 1 curses backend        [3-4 wk]

13.  Track 8.1  libmadcedit core                         [3-4 wk]
     ├── Piece table, cursor, undo/redo, CUA keybindings
     └── Requires: Level 0 rendering (step 11b)

14.  Track 8.2  libmadcedit curses + syntax highlight    [4-6 wk]
     └── Requires: Level 1 rendering (step 12b)

15.  Track 8.4  madcide shell                            [3-4 wk]
     └── Self-hosting milestone: edit madc in madcide

16.  Track 3.2  PCH transition                           [2-3 wk]
     └── Replace text-embedded stubs with pre-compiled

17.  Track 6.1  macOS/ARM64 MVP (via MIR)               [10-15 wk]
     └── c2mir + MIR are already cross-platform; CIR coverage (step 11) first

18.  Track 4.2  C ABI shim                               [2-3 wk]

19.  Track 1.5  Code cleanup Phase C                    [3 wk]
     └── Macro system unification, token hierarchy flattening

20.  Track 7.3-7.6  Rendering Levels 2-3                [4-6 wk each]

21.  Track 4.3  Fork-based worker isolation              [3-4 wk]

22.  Track 3.3  PCH Phase 2 — AST serialization         [4-6 wk]

     ── CIR PARITY GATE ─────────────────────────────────────
     Track 1.3 (CIR coverage) must reach full parity before
     data work begins.

23.  Track 5A.1-3  madcdis library restructure            [3-4 wk]
     Track 5B.1    madcdat depends on madcdis             [1-2 wk]

24.  Track 5A.4-5  Pools + value system                  [6-8 wk]

25.  Track 5A.9   Federated query planner                [4-6 wk]

26.  Track 6.2  macOS SIMD (NEON)                       [2-3 wk]

27.  Track 4.4  Node.js integration                      [4-6 wk]
     Track 4.5  Rust bindings                            [2-3 wk]

28.  Track 3.4  Modules (.madm)                          [6-8 wk]

29.  Track 6.3  macOS AOT (Mach-O writer)               [4-6 wk]

30.  Track 7.7  Rendering: Level 4 GPU/3D               [future]

31.  Track 9    Multi-syntax (Python/Ruby/Rust modes)     [ongoing]

32.  Track 10   Safety, optimization levels              [ongoing]
```

**Recommended next:** Track 1.3 (CIR coverage) is the highest-leverage item and
the gate for promoting `develop → master` — it is the sole backend, so nothing
downstream (data substrate, ARM64 port, AOT) proceeds, and master is not
updated, until `cir_node → c2mir → MIR` reaches full C89 parity. SMAUG 1.8
already boots/runs/plays on this path; the ~95 remaining integration failures
are the worklist. Build the `.mc11`/`.c` renderer and the gcc-`-fverbose-asm`
fidelity gate to make them mechanical and localizable, then reimplement
eval/exec + REPL on MIR. Latent items surfaced during the SMAUG bring-up:
other signed `int`-returning libc fns on the `long` fallback (declare them
`int`), and the flaky `testfortypedcomma` (uninitialized 2nd declarator in a
multi-declarator for-init).

## The SMAUG Goal

The concrete test case driving Tracks 1-3 is compiling **and running** SMAUG
1.8 (~158K LOC C89) end-to-end. ★ **Achieved on the CIR path (v0.25.0,
2026-05-30):** SMAUG compiles through `cir_node → c2mir → MIR`, links, boots to
a live server (`Realms of Despair ready … port 4000`), and is playable — a
connected client creates a character, navigates the world, and fights (the
Newgate room-109 serpent fight runs). This matches and now exceeds the old
asmjit backend's startup → login → serpent-combat reach, on the sole supported
backend. Remaining: broader gameplay coverage and driving the CIR integration
worklist to parity. The port itself lives in the external
[MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo.

SMAUG does NOT need C++ features (Tracks 2, 8) — it's pure C. But the
C++ features make madc useful as a general-purpose scripting language
beyond the SMAUG port. The rendering abstraction (Track 7) would let
SMAUG target terminal, web, and GUI from the same game code.

## Plan Index

| Plan | File |
|------|------|
| **ADR 0001 — CIR/c2mir backend (why c2mir, not direct-MIR)** | [../adr/0001-cir-c2mir-backend.md](../adr/0001-cir-c2mir-backend.md) |
| Code Cleanup | [code-cleanup.md](code-cleanup.md) |
| C++ Support | [cpp-support.md](cpp-support.md) |
| Cross-Cutting Insights | [cross-cutting-insights.md](cross-cutting-insights.md) |
| Data Storage & Federation (legacy) | [data-storage-federation.md](data-storage-federation.md) |
| madcdis Core Substrate | [madcdis-plan.md](madcdis-plan.md) |
| madcdis Memory Research | [madcdis-memory-research.md](madcdis-memory-research.md) |
| madcdat External Drivers | [madcdat-plan.md](madcdat-plan.md) |
| Language Interfaces | [madc-interfaces-plan.md](madc-interfaces-plan.md) |
| Future Considerations | [future-considerations.md](future-considerations.md) |
| libmadc Phase 4 | [libmadc-phase4.md](libmadc-phase4.md) |
| macOS/ARM64 Port | [macos-arm64-port.md](macos-arm64-port.md) |
| Pre-Compiled Headers | [precompiled-headers.md](precompiled-headers.md) |
| Perry/Rust Integration | [perry-rust-integration.md](perry-rust-integration.md) |
| Rendering Abstraction | [rendering-abstraction.md](rendering-abstraction.md) |
| madc IDE & Editor | [madc-ide.md](madc-ide.md) |
| Multi-Syntax Support | [multi-syntax.md](multi-syntax.md) |
| Typed-Register IR (archived — asmjit-era) | [archived/typed-register-ir.md](archived/typed-register-ir.md) |
| Gecko+MIR Transpiler (archived — superseded by CIR) | [archived/transpiler-backend.md](archived/transpiler-backend.md) |
| Revival Plan (archived) | [archived/revival-plan.md](archived/revival-plan.md) |
