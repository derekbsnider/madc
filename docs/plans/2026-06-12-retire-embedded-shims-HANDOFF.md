# HANDOFF — Retire embedded shims: real headers serve every mode

**Read this FIRST on resume/post-compaction.** Cold-start brief; assume you
remember nothing. Run `bash scripts/resume.sh` first (live git/build truth),
then read this top-down. The governing process document is
**`madc-header-partition-handoff.md` (repo root)** — the user has had to
re-point at it repeatedly; every decision here must trace to it. Its
companion memory: `project_retire_embedded_shims` +
`project_header_partition_architecture`.

---

## STATUS UPDATE 2026-06-13 (session 6, part 3) — MIR upstream sweep (PRs #437-440)

**Commits `56ee053` (MIR pin 545ad46→5df536f) + `eeed70a` (emit-C hygiene).**
Adopted 4 upstream vnmakarov/mir PRs onto fork develop (pushed) + one madc
refinement. Headline: **#438 fixed a LIVE x86-64 generator bug** — a struct
param before `...` made `va_arg` read the named struct's register-save slot
as a vararg (3-way oracle: `MIR_gen` gave 60/3.0/3018 vs gcc 330/3.8/3018).
Pinned by `tests/testvastruct.mad`; also fixed gcc-torture `pr117432.c`
→ **failset 52→51, ZERO regressions**. #437's 128MB code-holder reservation
**arch-gated OFF x86-64** (fork `5df536f`, proposed upstream): rel32 reaches
±2GB so it's unneeded there, and it OOM-crashed the leaky-VLA torture case
`20040811-1.c` by eating commit headroom (that VLA leak is a NEW known gap —
see claude_status: VLA not freed on backward goto). #439 C23 paramless
variadic; #440 block-arg copy (aarch64/riscv64/s390x/ppc64, inert on x86-64).
emit-C `eeed70a`: `safe_ident` (per-byte mnemonic flatten of operator
spellings) + DOTS-only param list → `()`, unlocking the 3-way c2m oracle for
the w2a faces (the JIT tree path is untouched — c2mir never sees emitted C).
Gates: fulltest 556/27 (failure list byte-identical, +testvastruct), unit
10/10, torture 1571/failset 51, cir-fidelity exit 0, SMAUG soak green.
Triage: docs/parity/mir-fork-community-patches.md (round 3).

## STATUS UPDATE 2026-06-13 (session 6, part 2) — w2a CIR FACES: 83 → 42 CHECK ERRORS

**Commits `dd27c5e` → `ba6dd30` → `87cc363`** (after the eval fix below).
All three gated identically: suite 555/27 failure-list byte-identical,
unit 10/10 binaries, torture failset = the 52-name baseline (1570
passed), SMAUG soak green. `vector<int> v;` (tmp/w2a.mad) went from 7
ctor no-match errors + 83 c2mir check errors to **42 check errors, all
7 no-matches cleared**. Root causes, all general mechanisms:

1. `dd27c5e` — **implicit copy ctor** ([class.copy.ctor]) in BOTH
   ctor-call builders via shared try_implicit_copy_construct, gated on
   new recursive class_trivially_copyable (class_needs_dtor was the
   wrong predicate — any object member counted as non-trivial, but
   move_iterator{__normal_iterator{int*}} is bit-copyable); arg typing
   via the promoted CirBuilder::ctor_arg_datadef (the ONE resolver).
   **Delegating ctors** ([class.base.init]p6): a mem-init naming the
   ctor's own class is delegation, never a base initializer (the alias
   clause matched `: vector(__rv,__m,true_type{})` against _Vector_base
   and fired 3 args at 2-param base ctors); the prologue is ONLY the
   delegation call. **Identity-return inference restricted**: the
   backward scan skipped non-identifier tokens, so make_move_iterator's
   `move_iterator<_Iterator>` return matched as bare `_Iterator` —
   return_override became the ARG type, __uninitialized_copy_a deduced
   wrong and silently REUSED the plain-copy instantiation. Now fires
   only when the return IS the bare param (std::move/forward keep it).
2. `ba6dd30` — **one aggregate-tag-kind owner** (class_tag_ref):
   ~20 sites hardcoded N_STRUCT while branching sites followed
   union_layout → class-parsed unions (_Temporary_value::_Storage)
   emitted mixed kinds ("kind of tag unmatched"); class_struct_def's
   DEFINITION now branches too. **ttVariable discriminates operator
   receivers** (prefix/postfix/binary): TokenMember/TokenCallFunc
   DERIVE from TokenVar; the downcast emitted an implicit-this member's
   bare name (`--current` → "undeclared identifier current/_M_current",
   16 errors). Reducer tmp/w12a.mad (plain user class — general bug).
3. `87cc363` — **type()-gated object classification**
   (is_class_object_value + object_arg_addr's member/var arms:
   TokenCallMethod passed the TokenMember downcast) and
   **trivially-copyable rvalue-call receivers materialize**: raw-call
   rvalues (`__y.base() - __x.base()`) fall to object_arg_addr's
   materializing tail (implicit-copy assign into a temp). The gate MUST
   be class_trivially_copyable: external sret calls already yield a
   temp lvalue inside translate_expr — routing them into the tail
   recursed object_arg_addr→class_ctor_call through the copy ctor's
   const-ref param (teststringplus SEGFAULT, caught by the suite gate).

**SYSTEMIC TRAP (watch for more):** the token hierarchy
TokenCallMethod : TokenMember : TokenCallFunc : TokenVar means EVERY
`dynamic_cast<TokenVar*>`/`<TokenMember*>` classification site is
suspect — gate on type()==ttVariable/ttMember. Remaining un-audited
downcast sites in cir_builder.cpp: ~566(fixed)/1104(fixed)/3299/3313/
4662/5276/5718/7684/8111/8218/9260.

**w2a REMAINING (42 check errors, next session's entry):**
17× "incompatible argument type for arithmetic type parameter" (biggest
— start here), 11× int-without-cast-for-pointer warnings, 6× lvalue-&,
5× "incompatible return-expr type in function returning struct/union",
3× too-few-arguments, 3× subscripted-value-not-array, 3× invalid
comparison operands, 1× "undeclared identifier __madc_objtmp_66"
(pending-stmt placement — likely the while/for condition-temp gap noted
in session 4). Reducers: tmp/w12a.mad, tmp/w12b.mad (both g++-matched
green), tmp/w12c.mad free. KNOWN SEPARATE BUG found en route: `N n2(arr
+ 3);` — array+int as a ctor arg types as INT (decay missing in that
position); sidestepped in w12b, unfixed.

---

## STATUS UPDATE 2026-06-13 (session 6) — EVAL SCOPE CAPTURE FIXED; UNIT SUITE FULLY GREEN

**Commit `83c0ba4`** (this branch). Queue item (a) closed: runtime-eval
scope capture no longer sweeps parse-time constants. Root cause was in
`is_runtime_eval_scope_supported_variable` (parser.cpp ~9381), NOT the
CIR lowering: bare-`vfCONSTANT` globals (glibc anonymous-enum constants
`_ISupper` et al., `PTHREAD_*`) have no declaration in the emitted
module — reads of them FOLD — so the TokenScopeContext by-name capture
emitted undeclared identifiers and the PARENT TU failed to compile at
every scope-access eval call site. Fix: the collector's predicate
excludes `vfCONSTANT`-without-`vfCONSTDECL` (a value, not runtime scope
state); const-DECLARED vars (real storage) stay capturable.

**Gates:** unit `test_libmadc_program` **132/0/11** (was 128/4; only
deferred-AOT skips remain — unit phase fully green again). Integration
**555/27** (+3: testmadcevalexpr/testmadcevalexprtyped/testmadcevalscope,
zero new; log tmp/runtests_s6a.log). Torture failset **byte-identical to
the 52-name baseline** (1570 passed, tmp/gcctest_s6.log). SMAUG soak
green (exit 124 + ready line).

**Eval cluster re-attribution:** the 3 still-failing eval tests are NOT
scope-capture: testmadceval + testmadcevalexprctx die on wall 5
(`_ZNSolsESo` — `cout << endl` overload mis-pick, same as
testmultiret/testrust); testmadc_ns dies on wall 2 (`std::vector<int>`
instantiation). Wall 3 as a distinct wall is CLOSED.

**Queue item (c) verified done:** no `cc_*.json` scratch manifests remain
in /workspace/MadSMAUG.

**REMAINING QUEUE:** (b) w2a CIR faces per the session-4 banner (implicit
copy ctor `__normal_iterator(__normal_iterator)` first, then the
`_Vector_base` 3-arg mem-init mis-route) — unblocks the ~12-test
container cluster + testmadc_ns; then wall 5 (`_ZNSolsESo`, +4 tests).

---

## STATUS UPDATE 2026-06-12/13 (session 5) — WALL 4 CLOSED; SMAUG BOOTS ON REAL GLIBC

**Commits `63e3efb` → `3b460ea` → `ea3078a`** (this branch). All three
individually gated, zero regressions; suite improved to **552/30** (+3:
testservent/teststat/teststatret), torture failset **byte-identical to the
52-name pre-drift baseline** (wall 4's 20101011-1/loop-2f/loop-2g recovered),
**SMAUG 1.8 boots end-to-end on REAL glibc and survives the soak** (the
canonical `MadSMAUG.sh` invocation; exit 124 + ready line).

1. `63e3efb` **STEP 0 namespace-stack refactor** (as planned): vector +
   RAII `NamespaceScope`; `current_namespace()` is a read accessor; the
   qualified-stmt flag pair is replaced by `stmt_callee_namespace` +
   `QualifiedCalleeScope` (head-resolution override via
   `active_cpp_lookup_namespace()`; parseCallFunc/Method clear the spent
   override — args read the untouched lexical stack).
2. `3b460ea` **unit-suite SEGFAULT root-caused + fixed**: Pass-1.9
   fixpoint-materialized bodies were defined module-tail UNDECLARED
   (both declaration passes had already run) → implicit-int K&R calls,
   truncated pointer returns, mis-wired struct args (the __madc_shim
   wild store; alone-pass/full-crash = stale-stack dependence). New Pass
   1.95 emits late protos + late externs. ALSO: the eval policy gate now
   keys on the tokenized SOURCE FILE (real header paths broke the old
   policy-header-name exclude). Unit suite: crash → **128/4**.
3. `ea3078a` **wall 4, the whole chain** (each a real-glibc construct the
   shims had hidden): FF/VT = whitespace · fn-ptr members in
   nested/anonymous aggregates (shared parse_fnptr_member_tail) · arity
   checks via Program::call_signature_funcdef (blind (FuncDef*) casts on
   DataDefFPTR were UB — order/cwd-shapeshifting failures) · multi-star
   returns (dd_peel_pointers ×3 emit sites) · C++-only predefines gated
   out of C modes (predefine_is_cpp_only) · **GNU dialects**
   (--std=gnu89..gnu17, gnu++NN; gnu_dialect modifier) with gcc-parity
   strictness (__STRICT_ANSI__ strict-only, __STDC_VERSION__ per C std) ·
   project driver no-std .c → gnu17.

**TRAP LEARNED (cost a 222-failure suite run, caught by the gate,
uncommitted):** lifting __STRICT_ANSI__ from STD_MADC/C++ modes opens
glibc's `!__STRICT_ANSI__` float regions → `__float128`/`_FloatN`
declarations madc cannot type. The strictness lift is C-gnu-modes-only
until __float128/_FloatN land (noted in strict_ansi_mode()'s comment).

**REMAINING QUEUE:** (a) eval scope capture sweeps real-header constants
(`_ISupper` enum constants, interference-size constexprs) into
TokenScopeContext — emits identifiers that don't exist in C; the 4
remaining test_libmadc_program failures (walls 3-adjacent; root-cause
located in collect_runtime_eval_scope_variables/its CIR lowering at
cir_builder ~8482). (b) w2a CIR faces per the session-4 banner below
(implicit copy ctor first). (c) `cc_skfirst.json`/`cc_bis*.json` scratch
manifests in /workspace/MadSMAUG — delete when done.

---

## SESSION-5 ENTRY PLAN (decided with user 2026-06-12, end of session 4)

**STEP 0 — NAMESPACE-STACK REFACTOR (do FIRST, fresh context):** replace
the single mutable `Program::current_namespace` string (135 refs, ~10
hand-rolled save/restore sites, some not exception-safe) with
`std::vector<std::string> namespace_stack` + an RAII guard
(`NamespaceScope`), the idiomatic twin of `class_scope_stack` (vector,
NOT deque — back-ops only, tiny depth, needs iteration; std::stack
forbids the enclosing-chain walk). `current_namespace` becomes a read
ACCESSOR (back() or empty) so the ~125 read sites don't change; the ~10
mutation sites become guards. DELETE the `qualified_stmt_callee_ns` /
`qualified_stmt_lexical_ns` flag pair: the statement-level qualified-call
(`php::foo(args)`) callee-namespace override moves OUT of lexical-scope
state (a parameter to head resolution / its own member), and argument
parsing just reads the lexical stack top. WHY FIRST: walls 3 (eval-TU ns
context) + 4 (sys headers) are namespace-adjacent — do not stack more
save/restore patches; the conflation (lexical scope vs qualification
override) caused both the 2023 clear() hack and session-4's bug. GATE:
full suite (549/33 byte-identical) + torture name-diff vs the 52-name
baseline (+ the 3 known wall-4 names), committed alone before any other
work.

**THEN session-4's queue:** w2a CIR faces (implicit copy ctor
`__normal_iterator(__normal_iterator)` — [class.copy.ctor] same-class
arg + no user copy ctor → implicit memberwise/bit-copy in
select_ctor_overload's no-match tail; then the
`_Vector_base(__normal_iterator, allocator*, integral_constant_bool_true)`
3-arg mis-route — read the MADC_DEBUG_CTORINIT NO-MATCH dumps) → walls
3/4/5 per the session-4 banner below.

---

## STATUS UPDATE 2026-06-12 (session 4) — w2c GREEN; w2a PARSES fully; 13 root causes

**Commits `c8870aa` → `03d5990` → `a6c9d72`** (this branch). `tmp/w2c.mad`
(`_Vector_base<int,allocator<int>> b;`) constructs + destructs END-TO-END
(exit 0). `tmp/w2a.mad` (`vector<int> v;`) now PARSES the complete real
`<vector>` chain and stops at CIR overload selection. Suite re-verified
TWICE at this state: **549/33/0/18, failset byte-identical to the
session-3 baseline** (tmp/runtests_s4a.log @ c8870aa, tmp/runtests_s4b.log
@ a6c9d72) — zero regressions incl. all polyglot-namespace tests.

The 13 root causes, in landing order (all general mechanisms):

1. **ref_returning_call_type types by the RESOLVED callee** (CIR): a
   late-bound overload set leaves tcf->var on an arbitrary member; the
   token's returns() said `allocator&&` where the re-rank winner returned
   `_Vector_impl&&` → the `_Vector_impl_data(allocator)` no-ctor-match.
2. **flush_pending_stmts** (new helper): ctor/dtor prologue+epilogue
   builders splice materialized temp decls into THEIR OWN list — they
   leaked into the NEXT translated function (undeclared `__a` in
   _Vector_base's dtor).
3. **translate_if flushes condition temps ahead of the IF** (both arms) —
   they landed inside the then/else block (undeclared objtmp). NOTE:
   while/for conditions NOT yet covered (temp would hoist wrongly —
   semantics: per-iteration construction; revisit when hit).
4. **`= default` DEFAULT ctor parses as `{}`** ([dcl.fct.def.default],
   defaulted_member_parses_empty): the prologue machinery IS the implicit
   definition. `= delete` + defaulted copy/move stay declaration-only —
   defaulted COPY/MOVE need memberwise synthesis (OPEN; vector's
   `_Vector_base(_Vector_base&&) = default;` will need it).
5. **class_method_call __retbuf ABI at the CALL SITE** (direct + vtable):
   by-value non-trivial class returns materialize a cleanup-tagged temp,
   pass &temp as the hidden LEADING arg; expression value = temp lvalue
   (`__x.get_allocator() == __a`).
6. **Empty mem-initializer = value-initialization** ([dcl.init]p8):
   scalar/pointer member zero-assign (`_Vector_impl_data() : _M_start()…`
   left garbage the dtor freed → abort).
7. **Union with class-only syntax delegates to the class parser**
   ([class.union]; parsing_cpp_union_class → DataDefCLASS::union_layout;
   layout + CIR emission already branch on it). Real vector's
   `union _Storage` in _Temporary_value.
8. **operand_value_datadef types CALL operands by the re-ranked winner**
   via new `Program::resolved_call_funcdef` — the ONE parse-side re-rank
   (parseCallFunc's arity block refactored onto it). Fixes
   `__relocate_a_1<auto,…>` deduced from __niter_base's bound placeholder.
9. **Class-template-id qualified EXPRESSIONS keep the resolved class**:
   parseStatement's decl probe consumed `allocator_traits<_A>` then handed
   parseExprStmt the BARE ident → "Unknown namespace 'allocator_traits'".
   Pass the resolved TokenDataType (dataType arm owns Type::member(...)).
   Reducer tmp/w8d.mad.
10. ***member dispatches operator*** on a CLASS member reached via
    implicit this (member twin of the variable arm) — move_iterator's
    `*_M_current`.
11. **`typename X::type{...}` in EXPRESSION position** ([expr.type.conv]):
    resolve the dependent type, Redo through the dataType arm (vector
    swap's `typename _Alloc_traits::is_always_equal{}`).
12. **Unqualified type-name functional-construction fallback** in the
    ident arm (`true_type()` inside std bodies): resolve through the one
    shared resolver, Redo.
13. **ARGUMENT LEXICAL NAMESPACE** ([basic.lookup]) — THE BIG ONE:
    parseCallFunc/parseCallMethod CLEARED current_namespace around every
    argument parse (a polyglot-era artifact: statement-level
    `php::foo(args)` carries the CALLEE's ns in current_namespace, args
    are user-scope). The qualified-stmt arm now RECORDS the lexical ns
    (qualified_stmt_callee_ns / qualified_stmt_lexical_ns) and argument
    parsing restores THAT. This is why `true_type()` was "undeclared"
    ONLY inside instantiated member bodies (ns='' mid-body).

**WHERE w2a STOPS (next session entry):** 7 untranslatable nodes, all
CIR-side overload selection:
- `no matching constructor '__normal_iterator(__normal_iterator)'` —
  the IMPLICIT COPY ctor: candidates are default + `(int* const&)` only;
  same-class arg must select implicit memberwise copy
  ([class.copy.ctor]) — likely fix in class_ctor_call_addr/
  select_ctor_overload's no-match tail: same-class arg + no user copy
  ctor → bit-copy (the class is trivially copyable).
- `__normal_iterator(move_iterator<…>)` and
  `_Vector_base(__normal_iterator, allocator*, integral_constant_bool_true)`
  — ctor-initializer arg ROUTING (3 args at a 2-param ctor: looks like a
  delegating-ctor or mem-init arg mis-split; instrument with
  MADC_DEBUG_CTORINIT and read the NO-MATCH dumps).

**Diagnostics added (all gated MADC_DEBUG_NS_RESOLVE /
MADC_DEBUG_CTORINIT):** unknown-ns + deref-fail + undeclared-ident dumps
with instantiation context + upcoming-token stream; deferred-body entry
prints owner/spelling/derived-ns; typedef error names the offending token
+ stream; verbose no-ctor-match candidate dump (hardened — an earlier
version crashed on a dangling string).

**Session reducer inventory additions (tmp/):** w8a-w8d
(allocator_traits qualified-expr; w8d = the 14-line repro), w9a-w9c
(true_type resolution; w9c exposes OPEN gap: static constexpr member
`value` — "Unidentified member 'value' in integral_constant_bool_true").

**OPEN gaps queued (hit but not yet blocking w2a):** defaulted copy/move
ctor memberwise synthesis · static constexpr data members
(integral_constant::value) · static member-template instantiation via
class-qualified call (w8d's residual: `import of undefined item
allocator_traits_…_destroy`) · while/for condition temp placement ·
global-scope (non-namespace) fn templates never instantiate (w7e/w7g).

**TORTURE (full run @ a6c9d72, tmp/gcctest_s4.log): 1567/37/18/0/63 — 3
names OVER the 52-name baseline: `20101011-1.c` (real `<signal.h>` chain,
"Expecting member name in anonymous struct definition", reducer
tmp/w10a.mad) and `loop-2f.c`/`loop-2g.c` (`<sys/mman.h>`, "unexpected
token type 10" = the wall-4 sys-header desync family). ATTRIBUTED by
rebuild-at-6cb9003: all 3 fail at session-3 HEAD too — session-2/3 drift
(those sessions never ran torture), NOT today's fixes (today = ZERO
torture regressions). Fold all 3 into wall 4; they are MERGE-GATE
blockers (zero-regression rule).

---

## STATUS UPDATE 2026-06-12 (session 2) — WALL 2 CORE BROKEN; one residual

**12 root-cause fixes landed this session** (WIP commit on this branch; all
general mechanisms, no shims). The `vector<int>` chain now gets through
`__alloc_traits` rebind, `std::move`/`__alloc_on_swap` instantiation, the
nested `_Vector_impl`/`_Vector_impl_data` classes, and the late-declared
`_M_impl` member. What landed, in dependency order:

1. **`__builtin_addressof`** registered as a core builtin (parser
   `populate_builtin_registry`, zero-param variadic convention, NULL sym;
   CIR already lowered it to N_ADDR by name).
2. **`resolve_member_chain_or_type`** — the ONE seam: every
   `resolve_declared_type_token` branch (incl. all template-id
   instantiation paths + the ns-qualified branch) now consumes
   `Tmpl<Args>::member` chains. With a **non-destructive first-segment
   probe** (member must be a REAL alias/template — expression-position
   `Type::static_member` and if-condition heads stay untouched; the opaque
   escape deliberately NOT probed).
3. **Global operator overload sets rank** — the 3 gates
   (`namespace_overload_set_accepts_more`, parse re-rank,
   `call_target_funcdef`) accept EMPTY namespace_name (set key
   "::operatornew"); declaration-only global operators bind Itanium
   mangled-direct: `operator_code` got nw/na/dl/da,
   `mangle_nested_function` got the global `_Z<code><params>` form →
   `_Znwm`/`_ZdlPvm`/`_ZdlPvmSt11align_val_t` resolve real libstdc++.
4. **Block-scope `using X = T;`** registers flat like a local typedef
   ([dcl.typedef]); block-scope typedef/using no longer LEAK into
   namespace_datatype_map (fn-template instantiation runs with
   current_namespace set — `__alloc_on_swap`'s `__pocs`).
5. **Ident → type re-dispatch**: an identifier naming a datatype_map type
   followed by `::` Redo's through the ttDataType arm (post-tokenization
   registrations: block-scope aliases in instantiated bodies).
6. **`operand_value_datadef`** (Program static): value view of
   reference-typed/vfREFERENCE operands; used by fn-template DEDUCTION
   arg typing AND both overload-ranking arg lists (parser + CIR). Fixes
   `__alloc_on_swap<allocator<int>*>` (pointer-model leak).
7. **`_Tp&&` deduction** — fn_template_deduce_param accepts amps==2
   (rvalue/forwarding refs deduce the VALUE type); std::move/forward
   instantiate.
8. **`DelimDepth` C++ angle rules** — `<` opens ONLY after a name-like
   token (ident/type/`template`); `>` closes only when not inside parens
   opened within the list (per-open paren-depth stack). Real
   `integral_constant<bool, _Tp(-1) < _Tp(0)>` no longer desyncs the
   template scanner (it ate type_traits lines 874→2141: ALL the
   remove_*/add_*/make_* transforms were silently lost).
9. **Reference-cast = no-op on the object**: `static_cast<T&&>(x)` parses
   the target via getReferenceType; CIR cast arm emits the operand lvalue
   unchanged (is_reference target). `ref_returning_call_type` helper:
   ctor-arg typing + `object_arg_addr` bind a ref-returning call's value
   as the object address directly (`&*` folds) — no temp-construct
   recursion (that was a stack-overflow segfault).
10. **Derived-to-base ctor binding** — score_arg_to_param scores a derived
    class arg to a base class param 3 (slicing via base copy/move ctor);
    `object_arg_addr` upcasts ref-returning-call receivers with base
    offset.
11. **Nested-class fixes**: struct member-type slot resolves through the
    one shared resolver (enclosing-class aliases per [basic.scope.class]);
    base-clause nested structs delegate to the class parser (predicate
    pre-guard dropped); NAMED `struct Q {...};` member-less definitions no
    longer inline as anonymous aggregates (`is_anonymous` gate); renamed
    nested classes' FIRST ctor carries local_emit_name
    (Class__SourceName ≠ Class__Class); implicit base default-ctor calls
    resolve via select_ctor_overload + ctor_call_symbol (not blind
    Class__Class composition).
12. **Deferred ctor mem-initializers** — [class.base.init] complete-class
    context: in-class ctor init-lists are token-CAPTURED
    (DeferredFunctionBody::ctor_init_tokens) and parsed with the deferred
    body at class completion via the extracted
    `parse_ctor_initializer_list` (out-of-class ctors still parse eagerly).
    Real `_Vector_base(..., _Vector_base&& __x) : _M_impl(...,
    std::move(__x._M_impl))` names the member declared AFTER the ctor.

Also: "Unidentified member" diagnostic now names member + class.

**WHERE IT STOPS (next session entry point):** `tmp/w2c.mad`
(`std::_Vector_base<int, std::allocator<int> > b;`) now fails ONLY with
`cir error: no matching constructor for call to
'_Vector_base..._Vector_impl_data(allocator_int32_t)'` — a ctor-INITIALIZER
mis-route at CIR time: something constructs the `_Vector_impl_data` BASE
with the `__a` allocator argument. `_Vector_impl`'s ctors registered
correctly (o2 = `(const _Tp_alloc_type&)` verified in --dump-cir).
Hypothesis space (verify, don't trust): (a)
`ctor_initializer_targets_base`'s alias clause
(`class_alias_lookup_cir(owner,"_Tp_alloc_type")` walks enclosing_class →
_Vector_base's alias = allocator; allocator does NOT derive from
_Vector_impl_data, so on paper it shouldn't match — CHECK what it actually
returns, esp. whether `enclosing_class`/`base_class` are even set on the
delegated nested class); (b) `class_ctor_initializer_stmts`' member loop
with flattened base members; (c) `class_member_construct` default-
constructing `_M_impl` with stale explicit args. Instrument
ctor_initializer_targets_base with a gated fprintf and run tmp/w2c.mad.
After w2c: w2a (`vector<int> v;`) is the next face up.

**Diagnostics added (gated)**: `MADC_DEBUG_TYPEDEF_PARSE` (TokenTYPEDEF
enter/record + USING-ALIAS record), `MADC_DEBUG_FNTPL` now also dumps
injected instantiation tokens when env `MADC_DEBUG_FNTPL_DUMP=<substr>`
matches the inst key. TokenTEMPLATE::parse DBG prints file:line — the
GAP-detection one-liner that found fix 8:
`grep "TokenTEMPLATE::parse() at" log | awk -F: '{...}'` (see git log).

**Session reducer inventory (tmp/, all default-mode)**: w2a..w2s
(vector/alloc_traits chain), w3a..w3l (__alloc_on_swap/using-alias),
w4a..w4m (nested _Vector_impl shapes), w5a..w5i (std::move,
remove_reference, full _Vector_impl replica w5a = GREEN), w6a/w6b
(declval/array-spec probes — green). w2c/w2a are the live walls.

**Follow-up noted (user question)**: whether madc-LOCAL template
instantiations should be NAMED with their Itanium mangling (instead of
`__ns_std_*`/flattened keys) for --emit=c11 diffability and
auto-resolution of library-exported explicit instantiations — naming
fidelity only, linkage semantics unchanged.

**PRE-EXISTING unit-test crash (verified NOT this session)**:
`bin/test_libmadc_program` SEGFAULTS in the full run (inside a JIT'd
`__madc_shim_*` during the string-call shim tests; backtrace:
`gdb -batch -ex run -ex bt bin/test_libmadc_program`). It kills
`make -C src fulltest` at the UNIT phase before integration tests run.
Verified by stashing this session's diff + rebuilding: the BASELINE
branch crashes identically — so this branch's recorded 549/33 came from
`bash scripts/run_tests.sh` directly. Run integration that way until
root-caused (own wall; state-dependent: single `-tc=` runs pass, the
full sequence crashes).

---

## 0. TL;DR

Branch **`feature/retire-embedded-shims-claude`** off develop @ `2832fc0`
(develop untouched). ALL bucket-3 shims are DELETED (23k lines):
`include/madc/` holds ONLY bucket-1/2 compiler headers
(float/limits/stdarg/stdbool/stddef/stdint) + madc-owned `ns_*`. Real
glibc/libstdc++ serve every mode **including default STD_MADC** (which now
presents as g++ — `presents_as_cpp()`). Real `<iostream>/<string>/<cmath>`
compile AND run g++-identically in default mode. Integration was 546/36
before the latest (uncommitted-at-writing) stream-boundary fix; expect
~548+/34− after. The work remaining is (a) the wall list in §4, (b) the
PROCESS conformance items in §5 that institutionalize the partition doc.

**User rulings (binding):**
- K&R-era recovery (old-style params, implicit-int defs) ONLY under
  explicit `--std=c78..c17`. Never STD_MADC, never C++ (`knr_supported()`).
- No shims, no per-case hacks, fix at the deepest layer — categorical.
- All bucket-3 hand-rolled headers stay deleted; never re-author them.

## 1. The partition model (from madc-header-partition-handoff.md)

A header is madc's ONLY if its correctness requires codegen-private facts
(size_t identity, va_list layout, limits, intrinsics). Bucket 1 = pure
compiler headers (madc supplies fully). Bucket 2 = layering shims that
`#include_next` to the system copy (stdint/limits/float). Bucket 3 =
EVERYTHING else — all glibc + all libstdc++ — consumed REAL and unmodified.
The authority for bucket 1/2 membership is `gcc -print-file-name=include`
(the `$OWN` dir), NOT the standard's freestanding list.

## 2. What landed (commit chronology on this branch)

- `fa25e7f` **K&R gate**: `Program::knr_supported()`; harness `--std=c17`;
  9 K&R-era tests got `.flags`. GATED GREEN (fulltest 582, torture 52-name
  baseline ZERO regr +1 fixed → `docs/parity/torture-failset-current.txt`
  now 52 names, SMAUG soak green).
- `13383b7` **presents_as_cpp()**: STD_MADC seeds `__cplusplus` (201703L
  floor) + `__GNUG__` like explicit C++ modes; C modes stay plain gcc.
  Pin tests: testpredefmacros (defined) / testpredefmacros_c17 (absent).
  GATED GREEN (same three gates).
- `2d61556` **the sweep**: all bucket-3 shims deleted; `#include_next`
  made positional (never consults named PCH/embedded caches); baked PCH
  table EMPTIED (stale single-mode `gcc -E` captures that shadowed real
  headers; `gen_precompiled_headers.sh` HEADERS=() with rationale; lookup
  machinery kept for the proper PCH track). Plus 3 root-cause fixes:
  typedef_emit_name chokepoint for extern-proto RETURN types
  (cir_builder ~11289); shim text-ctor requires `required_param_count()<=2`
  (cir_builder `class_text_ctor`); template DEFAULT-arg declarator
  suffixes `_Tp*`/`_Tp&`/`_Tp&&` fold into the arg type (parser ~2660,
  mirrors the explicit-arg star fold; the suffix used to LEAK into the
  live token stream).
- `bb8083b` **preprocessor root causes**: #if expands function-like
  macros WITH arguments (expandIfMacros); gcc's guard-aware
  multiple-include optimization for SYSTEM headers (guard-less
  bits/mathcalls.h re-tokenizes per `_Mdouble_` pass — float/ldouble math
  decls were silently lost) while user `"..."` includes keep require-once
  (testincludeonce); generic `__builtin_X -> X` libc-twin dlsym fallback
  (emit_symbol = twin; kills the grow-forever hand list); FP-classify
  builtin family as sizeof-dispatched statement-expr macros onto REAL
  glibc exports (`__fpclassify*`/`__isnan*`/`__isinf*`/`__finite*`).
- `1b91e9f` **SFINAE pre-check** ([temp.deduct]):
  instantiate_fn_template_binding resolves a substituted
  `typename Q::X<args>::member` RETURN type in a sandboxed token push
  BEFORE the body parse; unresolvable → silent candidate discard. Real
  <cmath>'s integer-only `__gnu_cxx::__enable_if` overloads no longer
  hard-error float calls.
- (latest) **instantiation stream-boundary fix** (parser
  instantiate_fn_template_binding tail): the injected token run is
  restored to `base_depth` UNCONDITIONALLY after the parse — an "ok"
  `__hypot3<float>` instantiation left 2 trailing inj tokens that the
  resumed outer parse consumed, shifting every later declaration
  ("__z undeclared" two functions later). Cleared testmathh +
  testieeehugeval. `#if MADC_DEBUG_FNTPL` now also reports any
  imbalance (the diagnostic that found this).

## 3. Diagnostics arsenal (all gated, compile with -D<flag>)

- `MADC_DEBUG_FNTPL=1` — fn-template instantiation outcomes + STREAM
  IMBALANCE reports (parser.cpp).
- `MADC_DEBUG_NS_RESOLVE` — unknown-namespace throws with instantiation
  depth (parser.cpp ~13755).
- `MADC_DEBUG_TYPEDEF_EMIT` — typedef_emit_name alias→tag decisions
  (cir_builder.cpp).
- `MADC_DEBUG_BASE_CLAUSE` — base-clause first-lookup resolutions
  (parser.cpp ~19545).
- `madc -E` — preprocessed token stream (the bisect substrate;
  tmp/m4_pp.txt is `#include <math.h>` in default mode).
- Reducers in tmp/ (gitignored), ALL default-mode no-flags unless noted:
  realios*.mad (iostream), p2.mad, c9/c11.mad (extern-proto string),
  d1-d3.mad (string by-value), v1-v6.mad (vector/iterator bases),
  m1-m4.mad (math.h), h1-h4.mad (hypot shape), pfx1/pfx2.mad
  (m4_pp.txt prefixes), bisect.sh (prefix bisector).

## 3.5 VERIFIED WORKING (do not re-litigate; re-prove with these exact commands)

All in DEFAULT mode (no flags) unless noted — that is the point of the campaign:

```bash
# Real <iostream>/<string>/cin/getline/cerr, g++-byte-identical:
printf '#include <iostream>\n#include <string>\nint main(){ std::string s; std::cin >> s; std::cout << "got: " << s << std::endl; return 0; }\n' > tmp/ok1.mad
echo hello | bin/madc tmp/ok1.mad          # -> "got: hello", exit 0
# Real <math.h>/<cmath> incl. float pass + SFINAE abs/sqrt + hypot3:
printf '#include <math.h>\n#include <stdio.h>\nint main(){ printf("%%f\\n", HUGE_VAL); return 0; }\n' > tmp/ok2.mad
bin/madc tmp/ok2.mad                        # -> inf, exit 0 (stderr noise = wall 7)
# Real <vector> HEADER parses+compiles (instantiation = wall 2):
printf '#include <vector>\nint main(){ return 0; }\n' > tmp/ok3.mad
bin/madc tmp/ok3.mad                        # exit 0
# K&R gating (user ruling):
printf 'int f(a,b) int a; int b; { return a+b; }\nint main(){ return f(1,2)==3?0:1; }\n' > tmp/ok4.mad
bin/madc --std=c17 tmp/ok4.mad; echo $?     # 0 (accepted)
bin/madc tmp/ok4.mad; echo $?               # 1 (rejected in dialect)
# Suite baseline at branch HEAD: 549 passed / 33 failed / 0 timed out / 18 skipped.
# Phase 0+1 gates (recorded green 2026-06-12): torture 1570/34/18 = the 52-name
# baseline docs/parity/torture-failset-current.txt; SMAUG --project soak green.
```

The 18 pre-campaign `--no-embedded-headers` tests (testfstream/testloop/
testdefer/test_extern_polymorphic/*_realhdr/3-way gates) all still pass.

## 3.6 THE EXACT 33 FAILURES at HEAD (tmp/runtests_p2h.log), by cluster

- **Container instantiation (12)** — wall 2: testvector testvectorptr
  testmap testset testcontainerdtor testtemplatecontainer
  testtemplatestring testsubscript testsubscriptarrow testsubscriptmember
  teststruct3 test3eqclass. First error: `vector<int> v;` → "Expecting
  type after 'typedef'" inside the monomorphized real template body.
- **String-class behaviors (5)** — likely same root as containers (real
  basic_string member-template instantiation): teststdstringconv
  teststringglobal teststringref teststringrel testrefreturn.
- **madc eval surface (6)** — wall 3 (_ISupper ctype enums in the eval
  TU): testmadceval testmadcevalexpr testmadcevalexprctx
  testmadcevalexprtyped testmadcevalscope testmadc_ns.
- **sys headers parse (3)** — wall 4: teststat teststatret testservent.
- **operator<< mangle (2)** — wall 5 (_ZNSolsESo): testmultiret testrust.
- **sstream (1)** — `__byte_op_t` undeclared: testsstream.
- **foreach/php array (2)**: testforeach2 testforeachref ("too few
  arguments" class — check shim/trampoline interplay with real string).
- **misc (2)**: testprefer (prefer directive + real headers),
  testrubycharsshadow.

## 3.7 TRAPS REDISCOVERED THIS SESSION (cost real time; don't repeat)

- **Log truncation**: `cmd | tail -N > log` in a background task loses
  the failset head. ALWAYS `cmd > tmp/x.log 2>&1` then inspect.
- **tmp/*.madh shadowing**: find_filesystem_precompiled_header includes
  the CURRENT SOURCE DIR in its candidates — stale .madh files next to
  tmp/ reducers silently hijack `#include <...>`. `rm tmp/*.madh` first.
- **Stale-binary fulltest lie**: a fulltest summary that contradicts a
  by-hand run of the same binary = NAS mtime staleness. `make -C src
  clean` + full rebuild, then re-run by hand before trusting either.
- **Error-position misattribution**: errors from header-origin tokens
  print the MAIN file's name with the header's line number (e.g.
  "tmp/x.mad:3567"). The line number belongs to the real header — find
  it with `grep -n` in the suspect header, or via `madc -E` output.
- **Exit codes through pipes**: `bin/madc x | head; echo $?` reports
  head's status. Use `>/dev/null 2>&1; echo $?`.
- **DBG() is dead on worker threads** (thread_local) — use the gated
  `#ifdef MADC_DEBUG_*` fprintf diagnostics (§3) instead.
- **Throw prints unconditionally** (throwbuf::sync → stderr) even when
  the exception is caught and tolerated — printed error ≠ fatal error;
  check the EXIT CODE.

## 4. REMAINING WALLS (attack order; per-fix METHOD in §6)

1. ~~typename dependent return types~~ CLEARED @1b91e9f.
   ~~math param-scope leak~~ CLEARED @d11f5a3 (stream boundary).
   ~~class-scope alias in hidden-friend bodies~~ CLEARED @ the
   parse_hoisted_friend_operator owner-scope fix ([class.friend] lookup:
   hoisted parse runs with the owner class pushed on class_scope_stack).
   Real `#include <vector>` now PARSES AND COMPILES clean (tmp/v1.mad).
2. **Real container template INSTANTIATION** — `vector<int> nums;`
   (testvector:8) now fails with "Expecting type after 'typedef'" while
   monomorphizing the REAL vector template body (a typedef inside the
   instantiated body doesn't resolve). This is the new face of the
   container cluster (testvector/vectorptr/map/set/containerdtor/
   templatecontainer/templatestring/subscript* — ~12 tests). See memory
   `project_template_instantiation` (Borland monomorphize is THE model;
   string already works this way). Separately testsstream fails earlier
   on `__byte_op_t` undeclared (std::byte operator machinery in real
   <sstream>/<ostream> chain) — reduce independently.
   ALSO: a known LATENT gap from the same friend machinery — hidden
   friend NON-operator definitions (`friend iterator mk(...) {...}`) are
   skipped but never hoisted (tmp/w1.mad: "mk undeclared" at use).
   libstdc++ uses hidden-friend swap() widely; generalize the hoist
   predicate from operator-definitions to ANY friend definition with a
   body.
3. **testmadceval\*** (6 tests) — emitted eval code references `_ISupper`
   etc.: glibc ctype.h's anonymous enum constants don't reach the child
   eval TU. Likely the eval-TU synthesis (`<ns_madc>` path) needs the
   same real-header include context as the parent.
4. **teststat/teststatret/testservent** — parse error in real
   sys/stat.h chain under default mode ("unexpected token type 10" near
   EOF = stream desync; instrument like wall 1 — possibly another
   boundary/recovery leak).
5. **testmultiret/testrust** — bogus mangled import `_ZNSolsESo`
   (ostream<<ostream by value — overload resolution mis-pick on the
   real-header operator<< set; reduce `cout << <multi-ret-call>`).
6. **--emit=c11 hygiene** (non-blocking): `operatornew[]__o5`,
   `operator""s` leak as raw C identifiers in emitted text (JIT tree
   unaffected). safe_ident()-class fix at emission.
7. Stderr NOISE from caught/discarded instantiation attempts
   (throwbuf::sync prints unconditionally): wrap candidate-scoring
   instantiation in a diagnostics-suppressed mode so SFINAE discards are
   silent (currently they print scary-but-harmless errors, e.g. m1's
   "cannot dereference non-pointer type"). Principle: a DISCARDED
   candidate prints nothing; the CHOSEN candidate's errors are real.

## 5. PROCESS CONFORMANCE (institutionalize the partition doc — overdue)

These make the model self-enforcing instead of memory-dependent:

- **P1. Step-1 discovery gate**: new `scripts/check-header-partition.sh`
  — enumerate `gcc -print-file-name=include`, record GCC version +
  listing checksum in `docs/parity/header-partition-baseline.txt`;
  verify `include/madc/` ⊆ {bucket-1/2 names from $OWN} ∪ {ns_*}; FAIL
  on any bucket-3 reappearance. Wire into `make -C src fulltest` next to
  check-no-std-hardcoding.sh. THIS is the unfakeable "shims stay dead"
  contract.
- **P2. Step-4 macro parity**: madc has NO `-dM` yet (gap). Add
  `--dump-macros` (trivial: dump define_map/macro_map after init), then
  diff against `gcc -dM -E -x c /dev/null` and `g++ -dM -E -x c++` for
  the macros real headers branch on; record the accepted-diff baseline
  in docs/parity/. (gen_predefined_macros.sh captures build-time values;
  the diff verifies nothing load-bearing is missing.)
- **P3. Acceptance oracle (partition doc "Acceptance tests")**: freeze
  the C smoke (stdio/stdlib/string/stdarg/stddef/limits) and C++ smoke
  (type_traits/utility/tuple/vector/string/memory) as permanent
  tests/*.mad fixtures in BOTH default and --std=c++17 modes, once wall
  2 falls.
- **P4. Bucket-2 conformance**: current stdint.h/limits.h/float.h are
  FULL shims; the doc prescribes thin `#include_next` chaining shims.
  Convert + verify madc's #include_next semantics against each.
- **P5. Step-5 builtins checklist**: enumerate the `__is_*`/`__has_*`
  intrinsics the installed libstdc++ calls (command in the doc) and
  track implemented-vs-missing in docs/parity/ (drives <type_traits>
  conformance work).

## 6. METHOD (mandatory — unchanged)

Per fix: reduce (tmp/, NO flags = default mode is the point) → attribute
(gcc + clang + stock `/workspace/mir/c2m FILE -ei`; for madc-path bugs use
`--dump-cir`, NOT emit-C-as-truth) → DEEPEST-layer fix, no shims, no
per-name special cases → rebuild (`touch` the .cpp first — NAS mtime trap;
clean-rebuild if results look impossible) → re-probe reducers →
`make -C src fulltest` (cap: `( ulimit -t 3600; timeout 3000 ... )`, ONE
heavy job at a time) → full torture ALONE, failset-name diff vs
`docs/parity/torture-failset-current.txt` (52 names) → SMAUG soak
(`cd /workspace/MadSMAUG/runtime/area && timeout 50 /workspace/madc/bin/madc
--project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000`; exit 124
+ "Realms of Despair ready at" = good) → commit on THIS branch → update the
STATUS block in docs/plans/2026-06-12-retire-embedded-shims-plan.md.
Background long runs (`run_in_background`), capture FULL logs to tmp/
(never `| tail` into the log — it truncates the failset!).

## 7. MERGE GATE (do not merge to develop before ALL of)

fulltest 100% green (582+ incl. re-greened tests) + both check gates +
P1 partition gate · torture zero regressions vs 52-name baseline · SMAUG
soak · `bash scripts/run_tests.sh --exe` (shared-codegen surfaces moved)
· mirrors synced (claude_status.json head, CHANGELOG, ROADMAP, KG via
scripts/kg_query.sh, agent memory) · user approval (develop is the
shared stable branch).

## 8. Why the failures were "new" (user question, answered 2026-06-12)

Only 18 tests ever ran `--no-embedded-headers` (iostream/fstream/string/
compare families, under --std=c++17). vector/map/set/sstream and the
whole madc-dialect surface (eval, php arrays, foreach) had ONLY ever run
against the embedded shims. The sweep put all 582 tests through real
headers in default mode for the first time; every failure is a latent
real-header bug, not a regression of proven coverage.
