# RESTART HANDOFF — 2026-05-31 (read this FIRST after restart)

This is the authoritative rehydration entry point for the next session. It supersedes
the older `2026-05-30-SESSION-HANDOFF.md` (which predates the std::-types refactor).

## Read order on restart
1. This file (top to bottom).
2. `MEMORY.md` index + these memories (the WHY): `project_legacy_cpp_shortcuts`,
   `feedback_dont_cling_to_legacy`, `project_string_as_class`, `project_cpp_mangled_direct`,
   `project_template_instantiation`, `feedback_liberal_subagent_context`, `feedback_opus_subagents`.
3. The plan: `docs/superpowers/plans/2026-05-31-stdtypes-as-real-classes.md` (full task
   breakdown + EXECUTION COMPLETE section + the polish-followups list with statuses).
4. Then verify live state (below) before doing anything.

## ⚠️ RESTART CONSTRAINT
**SendMessage is NOT enabled** this session — you CANNOT continue any prior subagent by
its `agentId`. Dispatch FRESH subagents (Agent tool). That's fine: subagents always start
with isolated context; you construct exactly what they need from this doc + the plan.

## LIVE STATE (verify on restart)
- Repo: `/workspace/madc`, branch **`feature/cir-stdstring-claude`** (off `develop`). HEAD at
  handoff: **`c5eef3c`**. Tree clean (only pre-existing untracked files like `a.bmir`,
  `tmp/`, `.claude/` scratch — none of this refactor's).
- MIR fork: `/workspace/mir`, branch `feature/cleanup-attribute` @ `53cdb85` (the fork madc
  links via `/workspace/mir/libmir.a`). Untouched this session.
- Build: `make -C src` (binary `bin/madc`; ~1-2 min). Test: `make -C src fulltest`
  (~3-5 min). Single test: `bin/madc tests/NAME.mad`. Emit C: `bin/madc --emit=c11 FILE.mad`.
- **Baseline gate: `make -C src fulltest` = 376 passed** (55 stable pre-existing failures —
  vla/complex/exception/fnptr/struct-init, unrelated; + 1 flaky `testfortypedcomma` that
  flips fail↔timeout — IGNORE only that one). NEVER drop below 376.
- VERIFY ON RESTART: `git rev-parse HEAD` == c5eef3c (or later); `make -C src` clean;
  `make -C src fulltest` == 376; `git -C /workspace/mir rev-parse --short HEAD` == 53cdb85.

## THE WHY (do not re-litigate; the user was emphatic across many turns)
madc faked C++ niceties (std::string, vector/map/set) with **TEMPORARY SHORTCUTS** —
a special `dtSTRING` DataType with bespoke "string-object" lowering, hardcoded `tkSTRING`
token + `tkVECTOR/MAP/SET/LIST` keywords, and `ns_stl.cpp` wrappers — from before madc had
a real C++ framework. That framework now EXISTS (class model, template instantiation,
mangled-direct libstdc++ calls via the dlsym(_Z…) resolver, c2mir). The mandate: **RETIRE
the shortcuts; model std::string/vector/map/set as the real `std::`-namespaced class/template
types they are, DEFINED BY THEIR HEADERS (`#include <string>`/`<vector>`/…), not builtins.**
User feedback that drove this (saved to memory): *"you really tend to get hung up on legacy
artifacts and want to hold onto them like they're some sort of holy relic"* and *"these are
LEGACY ARTIFACTS… temporary shortcuts to shoehorn some C++ niceties into madc."* → Lesson:
when a special-case/legacy thing causes friction, MODEL THE REAL ABSTRACTION; delete the
shortcut, don't patch around it.

## WHAT'S DONE (this session — the refactor IS COMPLETE; 376 green; commits d0a4a76..c5eef3c)
std::string is a real C++ class; std::vector/map/set are real `#include`-defined `std::`
templates; the dtSTRING special-casing, the tkSTRING/tkVECTOR/MAP/SET/LIST tokens/keywords,
and ns_stl.cpp are GONE. Commit trail (chronological):
- `ed21e3b` invariant test · `9e5eceaf` FuncDef::emit_symbol + string methods→mangled ·
  `6101c3a` string ctor/dtor via class path · `d95503c`+`d8317c4` decl/construct/destruct via
  class path (struct string) · `e5aca81` methods via class path · `1a9c024` operators =/+= ·
  `3e78028` string members/pointers as `struct string` · `64c3d63` **string return-by-value**
  (via __retbuf) · `1d0b6c0` string ELEMENT ops (v[i] assign/stream/placement) → vector<string>
  works · `34c1e0e` `include/madc/vector` + **namespace{} block parsing** · `d45e72a` remove
  vector keyword · `457cdaa` delete vector container lowering · `70e5932` `include/madc/map`+`set`
  · `bcd428f` remove map/set/list keywords · `ef41596` delete ns_stl · `6390559` remove tkSTRING
  (std::-only) · POLISH: `b0f408c`+`0806e0e` operator==/!= via string_equals + map/set use == ·
  `4a06137`+`d281f6c` delete dead container lowering/types · `655b088` element destructors via
  __destroy intrinsic.

## THE HOW (architecture you must understand before touching step 4)
- **std::string is ALREADY a `DataDefCLASS`**: `typedef DataDefCLASS DDClass;` (include/datadef.h:659),
  `class DataDefSTRING : public DDClass`. So are DataDefVECTOR/MAP (now inert). `ddSTRING`
  (parser.cpp:3438) carries its methods/ctors/dtor.
- **Mangled-direct binding**: `FuncDef::emit_symbol` (include/madc.h, in `class FuncDef`). When
  non-empty, `CirBuilder::class_method_call` / `class_operator_call` (src/cir_builder.cpp) emit
  THAT symbol as the C call (a mangled libstdc++ symbol or a runtime wrapper) instead of
  `ClassName__method`; madc emits no body. `Program::add_string_methods()` (parser.cpp ~3695)
  binds string methods/ctors/dtor/operators to mangled symbols (generated by `src/madc_mangle.*`:
  `itanium_mangle_{member,ctor,dtor,operator}_sub`, `std_string_type()`), e.g. STR_CTOR0/_S/_CP,
  STR_DTOR, STR_ASGN_*/STR_APP_* statics in cir_builder ~:472-500. NOTE string& mangled-arg uses
  the substitution-compressed `RKS4_` form (verify exported symbols with `nm -D`/`c++filt`).
- **`struct string`**: concrete opaque storage `long _w[object_class_words()]` (sized from
  `sizeof(std::string)`, NEVER hard-coded), emitted once by `class_struct_def`. Strings in
  member/pointer/element/return position use this (so `string*` strides correctly).
- **Unified object addressing**: `object_var_addr` / `var_is_pointer_stored` / `string_obj_arg`
  (cir_builder ~:945-2271) — one rule: value lvalue→`&name`, pointer-stored param/ref/`T*`→`name`,
  member→`&obj.m`, subscript element `v[i]`→ the bare `class_subscript_addr` (the operator[] call).
- **string return-by-value**: `__retbuf` lowering (func returns `void` + hidden `struct string*
  __retbuf` first param; `return s` copy-constructs `*__retbuf`; caller passes a temp's address).
  Gated to madc-compiled funcs (is_string_returning_call / m_user_func_names). Parser fix
  `paren_group_is_function_def` disambiguates a string-returning fn-def from a ctor-call decl.
- **Containers = real templates**: `include/madc/vector|map|set` are `namespace std {
  template<...> class ... {...} }`, instantiated per use by the template engine
  (`instantiate_template_use` parser.cpp ~:1536, `TokenTEMPLATE::parse` ~:12701) → a normal
  DataDefCLASS → class-model lowering. Headers use `data + i` pointer arith (NOT `&data[i]` —
  parser gap), `new (slot) T(v)` placement copy-construct, `T& operator[]`, range-for via
  size()/operator[] (`translate_foreach_class`). `__destroy(data+i)` for element teardown.
- **`operator==`/`!=`**: bound to the `string_equals` runtime extern-C wrapper (src/madc_mir_backend.cpp;
  libstdc++'s == is an un-dlsym-able inlined template). `class_operator_call` handles int-returning
  bound operators. map/set compare keys with `==`.
- **`__destroy(ptr)` intrinsic**: registered as a core builtin (parser.cpp:5044), lowered in
  `CirBuilder::translate_expr` (cir_builder.cpp:3498) — strips ptr to element type T, runs
  `as_class_instance(T)`: object T → its class dtor (`class_dtor_symbol`), scalar T → no-op (`0`).

## ⏳ STEP 4 — REMAINING WORK

### 4A — COMPLETE OPERATOR OVERLOADING (user priority: "all overloadable operators mapped to functions")
The design is already the C++ way — every operator routes through `class_operator_call`
(binary) / `class_subscript_call` (`[]`) → a real `operator` METHOD/function (via
`FuncDef::emit_symbol` for bound libstdc++ ops, or `ClassName__operator<sym>` for user
classes). NO special-casing. But COVERAGE is incomplete (audited 2026-05-31). Gaps:
- **std::string `operator[]` NOT bound** → `s[1]` fails ("subscripted value is neither array
  nor pointer"). Bind `char& operator[](size_t)` on ddSTRING (mangled libstdc++ member,
  via the mangler) so string indexing routes through the operator[] method like vector/map.
- **std::string `operator+` NOT bound** → `a + b` concat fails ("invalid operand types of +").
  std::string `+` is a NON-MEMBER in libstdc++ (and may be an inlined template like `==` was →
  if un-dlsym-able, use an extern-C runtime wrapper `string_concat`, like `string_equals`).
- **`binop_overload_symbol` (src/cir_builder.cpp) is a PARTIAL table** — maps `== != < > <= >=
  + - * / % = +=` only. MISSING dispatch for: compound assigns `-= *= /= %= &= |= ^= <<= >>=`,
  bitwise `& | ^`, shifts `<< >>` (as arithmetic, distinct from the stream-chain `<<`),
  logical `&& ||`, UNARY operators (`- ! ~ ++ --` prefix/postfix), `operator()`, `operator->`.
  The class-member PARSER (parser.cpp ~11090) already ACCEPTS most of these (operator(),
  operator[], multi-char + single-char) — so the gap is in the CIR DISPATCH, not the parse.
- FIX: extend `binop_overload_symbol` to the full binary-operator table; add unary-operator
  dispatch (find where unary ops are lowered and route to `operatorX` when the operand is a
  class with that operator); route `operator()`/`operator->` similarly. Bind std::string's
  built-in `[]`/`+` (+ any other common ones: `at`? `<` for ordered map keys later?) as
  class operators with `emit_symbol`. ALL via the existing class-operator machinery — no
  special-casing, gcc/clang as canon for the libstdc++ symbols (`nm -D`/`c++filt`).
- TEST each: `s[i]`, `a + b`, user-class `operator-`/`operator*`/`operator()`/unary, etc.
- This is higher-value than 4B (string indexing/concat are everyday ops) and is mostly
  cir_builder dispatch + parser.cpp operator binding — LOWER risk than the 4B grammar changes.

### 4B — three parser gaps (FEATURE work, parser-grammar RISK)
They DO NOT FIX BUGS — the headers work today via workarounds. They improve elegance/usability
and would retire the workarounds. A parser-grammar change touches ALL 376 tests → highest
regression risk; do it DELIBERATELY, incrementally, subagent-driven, gating on 376 after every step.

Sequence by leverage (per the element-dtor subagent's analysis):
1. **KEYSTONE — method/operator on a subscript element** (`arr[i].method()`, `keys[i] == k`,
   `keys[i].c_str()`). Today the postfix chain doesn't continue with `.`/`->`/operator after a
   subscript primary. Anchors: `Program::parsePostfixChain` (src/parser.cpp:**6508**) builds
   `TokenSubscript`/`TokenSubscriptExpr` (madc.h) but doesn't chain a member/method after them;
   the CIR side ALREADY has `class_subscript_addr`/`is_string_subscript` (cir_builder.cpp) to get
   an element's address. Fix: allow a postfix `.`/`->`/operator after a subscript node in
   parsePostfixChain, producing a TokenMember/TokenCallMethod on the element. PAYOFF: removes the
   `K cur = keys[i]; cur == k` workaround in `include/madc/map` (lines ~50/72/82/93) and
   `include/madc/set` (~36/52) — they copy the element to a local because `keys[i] == k` won't
   parse. Also enables user `vec[i].method()`.
2. **pseudo-destructor syntax** (`(data+i)->~T()` / `obj.~T()`) — parse an explicit destructor
   call, lower it to the type's class dtor (`class_dtor_symbol`) or no-op for scalar (the SAME
   dispatch `__destroy` already does in cir_builder.cpp:3498). Once this lands, the `__destroy`
   intrinsic (parser.cpp:5044 + cir_builder.cpp:3498) can be RETIRED and the headers use native
   `(data+i)->~T()` teardown. Depends on (1) if using `data[i].~T()` form; `(data+i)->~T()`
   needs pointer-deref-pseudo-destructor parsing.
3. **unqualified sibling-method call inside a method** (`helper()` where helper is a method of the
   same class → `this->helper()`). Orthogonal, lowest priority. Lets map/set factor their linear
   search into a private `__find()` (currently inlined into each method). Also: `this.member`
   inside methods doesn't parse either (related — bare member-name resolution in a method body).

### HOW TO DO STEP 4 SAFELY (the method that worked all session)
- **Subagent-driven** (skill: superpowers:subagent-driven-development), FRESH opus subagents,
  ONE at a time (parser edits conflict — never parallel). Per task: implement → verify →
  spec/quality check → commit; gate `make -C src fulltest` == 376 before EVERY commit.
- **Be EXTRA liberal with subagent context** (the user's standing instruction — memory
  `feedback_liberal_subagent_context`): paste the WHY + architecture + exact anchors + rules +
  gotchas + build/test cmds + "don't regress 376". Subagents that understood the intent caught
  real issues all session; stingy prompts would re-introduce the legacy shortcuts.
- **gcc/clang is canon** (compare emitted C / `g++ -S`); deepest-layer fixes; NO shortcuts; NO
  new string special-casing (we just removed it all). If a parser change cascades beyond control,
  the subagent commits what's green (376) and reports DONE_WITH_CONCERNS — do NOT force it.
- Invariant tests to keep green: teststringclass, teststringreturn, teststringeq,
  testtemplatestring, testtemplatecontainer, testcontainerdtor, testvector, testmap, testset,
  testsubscript. Add a test per parser gap (e.g. `arr[i].method()`).
- **Risk note**: parser grammar is load-bearing for the whole suite. Each gap is its own
  commit, fulltest-gated. Start with (1) the keystone; reassess before (2)/(3).

## OTHER OPEN/CONTEXT (not blocking)
- Inert leftovers (deliberately kept; removing ripples for zero gain): `DataDefVECTOR`/`MAP`
  classes + `dtVECTOR/MAP/SET/LIST` enum tags (still referenced by enum-guarded branches in
  madc.h TokenSubscript ctor + parser dt-tables). `TokenOSTREAM` class-only orphan.
- `s == "lit"` (const char* RHS for string ==) falls through select_operator_overload (only the
  `const string&` overload is bound) — out of scope, no test needs it.
- User-class (non-string) return-by-value (`Pt makept()`) still "too many arguments" — the
  __retbuf lowering is string-only; generalizing it would close that + could subsume multi-return.
- Devbox: a transient `/tmp` ENOSPC was observed during one subagent's greps (commands succeeded
  on retry) — glance at the `/tmp` mount if grep/build act up.
- Bigger picture: this whole refactor serves Track 1.3 (CIR→master parity) and the SMAUG goal;
  `claude_status.json`/`docs/plans/ROADMAP.md` are the mirrored status surfaces (sync when done).

## ONE-LINE SUMMARY
std::string/vector/map/set are now real `std::` header-defined class/template types (legacy
dtSTRING/tkSTRING/tkVECTOR/ns_stl retired); 376 tests green. Remaining: 4A — COMPLETE operator
overloading (all overloadable operators → functions; bind std::string `[]`/`+`; extend the
binop dispatch table + unary/()/-> dispatch — user priority, lower risk); 4B — 3 optional
parser-grammar features (keystone = method/operator on a subscript element). Everything stays
method/function-based — no operator special-casing.
