# Session Handoff / Rehydration — 2026-05-30 (std::string + c2mir cleanup)

**Read this first after compaction.** Then: `MEMORY.md` index + the linked memories,
`claude_status.json`, this file. Live git/test state is operational truth.

## Where everything lives (TWO repos)
- **madc**: `/workspace/madc`, branch **`feature/cir-stdstring-claude`** (off `develop`).
  Build: `make -C src` (configured `--enable-madcdat=no`). Test: `make -C src fulltest`.
  Single test: `./bin/madc tests/NAME.mad`. Emit C: `./bin/madc --emit=c11 FILE.mad` (debug renderer, incomplete for some nodes).
- **MIR fork**: `/workspace/mir`, branch **`feature/cleanup-attribute`** (the one madc links via `/workspace/mir/libmir.a`).
  Build: `make -C /workspace/mir`. Test suite: `make -C /workspace/mir c2mir-test` (must stay green: 1075 tests/2150 success). c2m binary: `/workspace/mir/c2m`. Run a c2m test: `c2m FILE.c -ei` (file BEFORE -ei; args after -ei go to the program) or `-eg` (gen). gcc is canon: `gcc -std=gnu11 -O0 FILE.c && ./a.out`.

## ⏩ RESUME HERE (next task) — container std-lib headers, then delete ns_stl
Real template instantiation (Borland monomorphize) is BUILT + validated for 4-byte,
8-byte (long/double), AND pointer type args (commits ba62e64 capture, 704eada
members, 8229ae8 methods, 8652455 pointer args). `Name<T>` clones+substitutes the
captured body tokens, injects to the parse deque, re-parses via TokenCLASS at
top-level scope → normal DataDefCLASS (reuses class-model lowering).
tests/testtemplate.mad covers int/long/double/char* and passes.
**The 8-byte-return bug from the prior handoff DOES NOT REPRODUCE** on current code
(verified 2026-05-31, int/long/double all store+fetch correctly) — it was resolved
by the top-level-scope method-registration fix (8229ae8); the prior note was a stale
intermediate observation.
ENABLING LANGUAGE FEATURES DONE THIS SESSION (all 0-regression, on
feature/cir-stdstring-claude) — the scalar container is fully proven end-to-end:
- Pointer type args `Box<char*>` (8652455).
- **Reference return types** `T& method()` / `T& operator[]` (ef699e2): a reference
  is a strict pointer — method returns the address, call site reads `*p`/stores
  `*p=rhs`, byte-identical to g++. tests/testrefreturn.mad.
- **Range-for over an instantiated class** (22cc794): `for (T x : c)` -> index loop
  on c.size()/c[i] (translate_foreach_class). tests/testtemplatecontainer.mad proves
  the FULL scalar container (ctor/dtor/push_back+realloc/size/T& operator[]/range-for).
- **Placement new** `new (addr) T(args)` (079d0c5): in-place construction (what
  std::vector uses for elements), scalar+class+string. tests/testplacementnew.mad.
  Parser dispatch broadened (parser.cpp ~8607) to fire on `(` / builtin-type too.

String element ACCESS infra DONE (commit 8be26f2): class_subscript_addr (bare
operator[] call = element address), is_string_subscript, is_string_object_value +
string_obj_arg now handle string-element subscripts -> cout/assign/range-for wire
through. No regression (371). Placement copy-construct (push_back) works via
placement new.

**IMMEDIATE NEXT TASK — the LAST gate for vector<string>: emit `string` as a
CONCRETE std::string-sized TYPE, SURGICALLY (not globally).**
ATTEMPTED + STASHED (stash@{0} "WIP concrete __madc_string type"): defined
`struct __madc_string { long _w[string_obj_words()]; }` (size from
sizeof(std::string), NOT hard-coded — user requirement) emitted once at module
top, and made append_type_specs(dtSTRING) emit it instead of `char`. RESULT: types
become correct (`struct __madc_string *data` strides 32; operator[] returns
`struct __madc_string *`; `slot = data + len` correct) BUT it **REGRESSED 7 tests**
(371->364: testphp/perl/rust/regex/teststdstringconv/testmultiret/…). Reason: a
GLOBAL append_type_specs(dtSTRING) change is too blunt — dtSTRING flows through that
path in many contexts (string PARAMS, returns, extern protos) that are coordinated
elsewhere to be char*/void* (param_decl emits string params as void*; the wrappers
take void*). So emit the concrete struct ONLY where stride/layout matters — a
string POINTER (string* member/local, e.g. container `data`) and a string ELEMENT/
member — and KEEP the char*/void* behaviour for bare string params/values. Likely:
gate on is_pointer-to-string in the declarator path, leave append_type_specs's
dtSTRING scalar case alone. Reconcile with string_storage_decl (declared vars =
long[W]); both must agree on size (string_obj_words()).
ALSO blocking the header (separate pre-existing PARSER member-resolution gaps found
this session — fix or avoid): (a) `&member[i]` address-of a member subscript doesn't
resolve the member (parser.cpp ~6861) — use `data + i` pointer arith; (b)
`(cast)member` (e.g. `(long)data`) doesn't resolve the member either; (c) the
SECOND bug exposed even with correct types: `data = (T*)malloc(...)` in the ctor
MISLOWERS as a std::string operator= (`aSEPKc` on `&data`) although data is a
string* pointer — the string-assign interception (cir_builder.cpp ~3343,
is_string_object_value(top->left)) misfires on a string* member; must exclude
string-POINTER lvalues. Validate with tmp Vec<string> (push_back via placement new +
`T* slot=data+len`; operator[]; cout; range-for), don't regress 371.
THEN flip: write include/madc/vector (T* data; len/cap; realloc; push_back via
`T* slot = data + len; new (slot) T(v);` — NOTE `&data[i]` source syntax hits a
separate pre-existing parser bug: address-of a MEMBER subscript doesn't resolve
the member, parser.cpp ~6861; use `data + i` pointer arithmetic instead; T&
operator[]; size; ~dtor), remove the tkVECTOR lexer keyword, repoint
testvector/testsubscript, delete ns_stl.cpp. Then map/set.
Pre-existing gaps: array data members (int data[4]) unsupported in class bodies;
`&member[i]` address-of (use member+index). Plan:
docs/plans/2026-05-30-template-instantiation.md. Memory:
[[project_template_instantiation]]. Gotcha: method names that are madc keywords
(set/map/list/vector) collide — use other names.

## CURRENT TEST STATE
- madc integration: **368 passed / 55 failed / 1 flaky-timeout (testfortypedcomma; sometimes shows as a fail) / 56 skipped** (was 334/88 at the start of the 2026-05-30 session; **+34 net, 0 regressions**; unit tests green). Goal = drive failures→0 for develop→master parity (Track 1.3).

## ⚠️ ARCHITECTURE (user-confirmed — READ [[project_cpp_mangled_direct]] AND [[project_template_instantiation]])
Split by whether libstdc++ EXPORTS the type (verified via `nm -D`):
- **Exported (std::string, ostream/istream, cout/cin)** → DIRECT mangled-libstdc++
  calls on real objects (cout-path model). **std::string DONE**: symbols now
  GENERATED by the substitution-aware mangler (commit 947dd96), resolved via
  dlsym(_Z). The mangler (commits 24cc764+4f5b9c7) generates exact libstdc++
  symbols for string/vector/map/set/sstream, validated vs c++filt.
- **Inline templates (vector/map/set)** → NOT exported, CANNOT be dlsym'd. Need
  REAL **template instantiation** in madc (Borland monomorphize → emit member
  bodies as cir_node → c2mir compiles, calling exported leaves _Znwm/__throw_*).
  madc has NO template support today; `vector<int>` is a hardcoded keyword +
  ns_stl wrapper. **ns_stl is the disallowed stdlib-wrapper vehicle — to be
  replaced by template instantiation, then deleted.** Full plan:
  docs/plans/2026-05-30-template-instantiation.md. DO NOT re-propose wrappers /
  g++ subprocess / dlsym'ing vector members (the repeated wrong turn).
- stringstream `ss<<` currently uses streamout_*+sstream_ostream (commit 2f60ed1);
  ostream is exported so its operator<< can go mangled-direct like cout (the
  base-offset adjust is the one real subtlety). MadArray/MadValue are madc's OWN
  C++ types → a libmadc facility (wrappers OK there).
- User-defined classes use the flat ClassName__method scheme (madc compiles them);
  the template instantiation engine REUSES that class-model lowering.

## DONE THIS PM SESSION (367, +33) — all committed on feature/cir-stdstring-claude
- Qualified std:: streams; MadArray objects; Cfront temp materialization for
  const-char*->string args; std::string operator=/+=; range-for over MadArray;
  rust::match->switch (3ba6da9, 08b0008, 2c9a4c7).
- STL containers vector/map/set (fd32441+308f549, subagent).
- stringstream objects + `ss<<` (2f60ed1) — WRAPPER-based, retire per architecture.
- std::string -> mangled-direct libstdc++ (57a9da3) — the architecture keystone.
- **C++ class model core** (merge 8a2177f, subagent): structs/methods/ctor-dtor/
  inheritance/new-delete/operators/vtables/refs/string-members. +15.
- **STL containers DONE** (commits fd32441+308f549, opus subagent in worktree, reviewed+cherry-picked+validated by me): vector<T>/map<K,V>/set<T> via the runtime-object model — recovered ns_stl.cpp (real std::vector/map/set wrappers) + parser instantiation + NEW CIR lowering (is_container_object, container_method_call, container_subscript_read/_assign, translate_foreach_vector; string results via scope-temp + (fill, string_cstr) comma-seq). Passing: testvector testset testmap testsubscript testmadc_ns. list<T> still stubbed (unused).
- madc unit tests: green. MIR c2mir-test: green (incl. bootstrap, which we FIXED).

## 2026-05-30 PM SESSION — P1 progress (+12, branch feature/cir-stdstring-claude)
Six features, all on the runtime-object model, committed (3ba6da9, 08b0008, +match):
- **Qualified std:: stream chains** — stream_ident_kind recognizes the hidden
  globals `__std_cout`/`__std_cerr`/`__std_endl` so `std::cout << x` is a stream
  chain, not N_LSH. (testns)
- **MadArray (`array`) objects** — generalized the string storage/ctor into
  `obj_storage_decl`/`obj_default_ctor_call` (shared); `array` = opaque buffer +
  madarray_construct + cleanup(madarray_destruct); array/array& params -> void*.
  (testphp)
- **String-object arg coercion (Cfront temporaries)** — a const char* value
  passed to a string-OBJECT param is materialized into a scope-lived temp
  std::string via `string_obj_arg` + `m_pending_stmts` (flushed by translate_block
  before the statement; cleanup-attr destructs). A real object passes by address.
  Mirrors the old transpiler's emit_ns_arg. Applied to ALL calls via callee
  FuncDef param types. (testphp array_push, testperl, testrust, …)
- **std::string operator= / += ** — `s = "lit"`/`s = t`/`s += …` on a string
  object lvalue -> string_assign / string_assign_cstr / string_append(_cstr).
  (testlang, test, test5)
- **Range-for over MadArray** — `for (T x : arr)`; TokenFOREACH is a sibling of
  TokenFOR (was missed). Loop var lives in the enclosing scope (parser puts it
  there), so translate_foreach emits the index loop + per-iteration element fill
  (php_array_get / php_array_get_int). (testforeach)
- **rust::match -> switch** — OR-list patterns become case labels, `_` ->
  default, each arm auto-breaks. (testrustmatch)
Newly passing: testns testphp testperl testrust testregex testrubycharsshadow
testinclude test test5 testforeach testlang testrustmatch.

### REMAINING P1 sub-projects (each a cohesive unit; cir_builder.cpp-heavy so
### serialize or use worktrees+cherry-pick, not parallel same-file edits)
- **stringstream** (testsstream) — needs a stringstream OBJECT (obj model) + a
  proper `stringstream*->ostream*` base-offset-adjusting wrapper (the virtual-base
  issue AGENTS.md flags) + printstream. The streamout_* wrappers
  (madc_mir_backend.cpp) take an ostream* and already work for any stream.
- **for_each + fn-ptr / lambda callback** (testforeach2 testlambda) — std_for_each
  passes a std::string* per element to a `void(*)(void*)` callback; by-value string
  param + auto fn-ptr + lambda. Overlaps the lambda cluster. (testforeach2 SIGSEGVs
  in the callback's `cout << name`.)
- **file streams** (testcin testfstream testloop) — `ofstream`/`ifstream`
  `.open()/.good()/.close()` + `ofs << x` (file-stream operator<<, same ostream*
  wrapper mechanism as stringstream).

## WHAT WAS DONE THIS SESSION (all committed)
### MIR fork — two clean, ordered PRs (enhance fork now, upstream to vnmakarov later; fork untouched ~2yr = safe window)
- **`fix/ldouble-determinism`** (off base 42471db): `mir.c put_ldouble` wrote uninitialized 80-bit-long-double padding (6 bytes) → non-deterministic binary MIR → bootstrap self-consistency test FAILED. Fix: zero the union + `u.u[1] &= 0xffff` (x86). Latent core-MIR bug, triggered by the fork's _Complex work (203 ldouble ops in c2mir.c). All 6 bootstrap variants pass. UPSTREAM-WORTHY. See [[project_mir_ldouble_determinism]].
- **`feature/cleanup-attribute`** (STACKED on the fix): full GNU `__attribute__((cleanup(fn)))` in c2mir. Mechanism: CHECK records `fn(&var)` stmts as extra ops on the jump node via `record_cleanups_until(jump,stop_scope)`; GEN emits them before the jump (return value already in a reg → no temp). Phases: fall-through (`emit_scope_cleanups` at N_BLOCK check end), return (`add_return_cleanups`, stop=NULL), break/continue (stop=loop/switch enclosing scope, trackers `curr_loop_scope`/`curr_loop_switch_scope`), goto (label scope via `S_LABEL` symbol `aux_node` + `common_ancestor_scope` NCA + `process_goto_cleanups` post-pass for forward gotos). Op indices: N_RETURN=2, N_BREAK/CONTINUE=1, N_GOTO=2. 5 tests in `c-tests/new/cleanup-*.c`, all byte-identical to gcc interp+gen. GOTCHA: c2m has no `__GNUC__` so glibc empties source-level `__attribute__` — c2mir tests use `__mirc_attribute__`; **madc builds the N_ATTR node directly so it's unaffected**. See [[project_c2mir_cleanup_attr]].

### madc — std::string as a real object (branch feature/cir-stdstring-claude)
A madc `string` = a real std::string OBJECT (g++ canon, user decision): storage `long buf[4]` (8-aligned), address via `string_obj_addr(name)`→`(void*)name`. Runtime wrappers in `src/madc_mir_backend.cpp` (string_construct/_cstr, string_destruct, string_cstr, string_length, string_assign/_cstr, string_append/_cstr, string_clear) operate on the void* object ptr. DONE:
- Decl + construct (`string_storage_decl`, `string_ctor_call`), in `translate_block`.
- **Destruction via the cleanup attribute** — `string_storage_decl` tags the buffer with `__attribute__((cleanup(string_destruct)))` (N_ATTR built in tree); c2mir destructs on EVERY exit path. Manual destruct removed. Verified leak-free on early return (valgrind).
- char* coercion: `string_cstr_arg` (string obj → `string_cstr((void*)s)`) at char*-expecting builtin args (printstr/puts).
- Stream: `cout << s` → basic_string `operator<<` (in `translate_stream_chain`).
- **Params**: `string&`(dtSTRINGref)/by-value(dtSTRING) → `void*` in `param_decl`; `is_string_object_value` recognizes string& params.
- **Methods**: `.c_str/.length/.size/.empty/.clear` → wrappers (`string_method_call`, dispatched on `ttCallMethod`=TokenCallMethod which IS-A TokenMember). `need_output_extern` gained a return-spec param so `string_length` returns `long`.
- KEY HELPERS in `src/cir_builder.cpp`: `is_string_object` (dtSTRING, not ptr), `is_string_object_value` (declared string var OR string& param; EXCLUDES literals/`__literal__`/ternary-of-literals — this distinction was a SIGSEGV bug we fixed), `string_obj_words`, `void_ptr_type`, `string_storage_decl`, `string_obj_addr`, `string_ctor_call`, `string_cstr_arg`, `string_method_call`. Provenance marker `synth_from_origin` on cir_node (src/cir_node.h) for future reverse-render suppression.
See [[project_cir_stdstring_model]]. Plan: docs/superpowers/plans/2026-05-30-cir-stdstring-lowering.md.

## THE 88 FAILURES — clusters (run `bash scripts/run_tests.sh 2>&1 | grep FAIL` to refresh)
- **C++ class model: ~23** — ctor/dtor/except*/virtual/inherit/operover/new/access/method/ref/class. Proven on OLD asmjit backend, NOT re-established on CIR. `cleanup` attribute now gives us RAII dtors.
- **namespace/STL/string: ~15** — testns/testphp/testperl/testrust/testlang/testmap/testset/testvector/testsstream/teststdstringconv(maybe now passing)/testregex/testrubycharsshadow/testrustmatch/testforeach/testforeach2/testsubscript. String-heavy: string objects passed to php::/std::/STL fns, cout chains, stringstream.
- **complex: 7** (testcomplex*), **struct/compound init: 7**, **VLA: 6** (testvla*), **fnptr: 5** (testfnptr*), **misc**: test/test4/test5 (undefined `version` global — a builtin-global/Task-1.7 issue, NOT params), testcin, testfstream, testdefer, testlambda, testgccconversionprefix/u32todouble/uint32realcoerce, testnegzerostatic, testlargesizeofquery, testsignedbitfieldassignexpr, testcomputedgoto, testmacrodefhead, testnestedvarargs, testautoincludestdheaders.

## PLAN (prioritized for test-suite recovery — develop→master gate)
**P1 — namespace/STL/string cluster (~15 tests). DO FIRST.** Natural continuation; we have string objects+params+methods. Work: coerce string OBJECTS into call sites of php::/perl::/python::/ruby::/js::/rust::/std:: functions (they take string/string&/char* params — apply string_cstr_arg or pass object ptr per param type), fix cout/stringstream chains with string objects, and STL container element handling (vector<string>/map). Investigate each failing test's emit (`--emit=c11`) + diff vs expected. Likely also needs: namespace-fn signatures with string params (param_decl already does string→void*), and string args to varargs/printf-family (extend the builtin char* coercion to general/varargs calls — currently only printstr/puts). MEDIUM effort, high count. Take the call-site/coercion complexity myself; subagent the repetitive per-namespace tests with liberal context.
**P2 — C++ class model (~23 tests). BIGGEST cluster.** Re-establish on CIR: ctor/dtor (use cleanup attr for dtors!), single inheritance + vtables, operator overloading, new/delete, references, access, exceptions (SJLJ). Large multi-feature. Establish ONE sub-feature myself (e.g. ctor/dtor), then fan the rest to subagents (worktrees) with liberal context. Reference: the OLD asmjit backend's C++ model (git history on master) + docs/rules/c11-transpiler.md (class→struct+vtable+static-fns lowering).
**P3 — small independent clusters (complex 7 / struct-init 7 / VLA 6 / fnptr 5 / misc). SUBAGENT-FRIENDLY, parallelizable.** Each is a few tests, mostly disjoint. Fan to parallel subagents (worktrees) with liberal context; I verify + integrate. The `version` global (test/test4/test5) + Task 1.7 global strings is a quick one.
**Pure std::string completion (returns/concat `s+t`/operators =,+=,==)**: parity-completeness, NO current failing test needs it → do on-demand when a P1/P2 test requires it, else last.

### Working method for clusters (proven this session)
- Disjoint code → parallel subagents in **worktrees** (madc: `git worktree add -b wip/X /workspace/madc-X feature/cir-stdstring-claude`; configure+build in it; cherry-pick back — clean for disjoint code). Same-file/coupled → do myself or sequence.
- Always: subagents model=opus, LIBERAL context (full background+helpers+gotchas+build/test+gcc-canon), TDD, gcc is canon, fulltest must not regress the pass count.

## RULES/CONVENTIONS (don't relearn)
- gcc/clang is canon. Build per-change as separate ordered PRs on the MIR fork (fixes before dependent features). Commit msgs end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Scratch in tmp/. No `&&` chains in Bash. `make -C src fulltest` after changes.
- MIR fork PRs: PR1 fix/ldouble-determinism, PR2 feature/cleanup-attribute (stacked). Future MIR work stacks similarly.
