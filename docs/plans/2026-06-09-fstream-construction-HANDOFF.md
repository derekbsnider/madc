# HANDOFF — real libstdc++ `<fstream>`: construction works, dtor crash is the wall

**Read this FIRST on resume / post-compaction. Assume you remember NOTHING (a
`/compact` is effectively a `/clear` — do not rely on conversational memory).**
Run `bash scripts/resume.sh` first (live git/build truth), then read this top to
bottom, then the governing design corpus in §2.

> ## ⚑ 2026-06-09 (turn 3, FINAL) MILESTONE — real `<string>` + `<iostream>` COMPILE+RUN
> **On `feature/cpp-detection-idiom-claude`, HEAD `aef0366`. ALL GATES GREEN: fulltest
> 543/4 (known reds only — testcout_realhdr GREEN, the 542/5 detection-idiom regression
> HEALED), gcc.c-torture 1566/31/57/1 (UNCHANGED), canaries OK.** Three commits:
> `2173ae0` lazy member-function-body instantiation ([temp.inst]) · `11ac1bc` extern-template
> external-binding (basic_string<char> ctor→C1/dtor→D1) · `aef0366` Pass 1.9 re-run the
> reachability fixpoint after synth-dtors (define late-ODR'd deferred library dtors).
> VERIFIED: `std::string line; line.size()` → exit 0; real `<iostream>` testcout →
> "This is a test, x = -1" (= g++). FULL detail: plan doc §9
> (`docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md`).
> **THIS WIP CHAIN IS NOW GREEN + GATED — landing-ready onto the green tip
> `feature/header-partition-claude` (fast-forward) when the user OKs.** (Don't push remote
> without asking.)
> **⚑ UPDATE 2026-06-09 (turn 4 / Codex): string construction + mutation NOW WORK —
> task-#25's "s+= still fails" is SUPERSEDED.** Later same-day commits `c932003`
> ("advance real string mutation path") + `c9fd222` cleared the basic_string.h:3486
> wall. VERIFIED at HEAD `c9fd222`: `std::string s("hello")` → `hello len=5`; `s += "..."`,
> `s = "..."`, `a + b`, `.size()` all compile+run. The `allocator_traits` partial-spec /
> `_M_local_data` `__detected_or_t` diagnosis below is resolved — do NOT re-chase it.
> **REAL NEXT WALL: (1) `std::getline`** → `'getline' is not a member of namespace 'std'`
> (unbound — bind the real libstdc++ `std::getline(istream&, string&)` overloads), **then
> (2) `<fstream>`** ofstream/ifstream typedefs + `open()`/`operator<<` + `ios_base`/
> `basic_ostream` hierarchy (inc-5/inc-6) → testfstream/testloop/testdefer. NOTE those
> three reds' SOURCES use bare `ofstream` via `using namespace std` + only
> `#include <iostream>` (no `#include <fstream>`) — old-shim reliance; closing them the
> real-header way may also need the test sources to `#include <fstream>`.
>
> ## ⚑ 2026-06-09 (turn 3) CURRENT STATE — (superseded by the MILESTONE banner above)
> **NEW WORK: lazy member-function-body instantiation ([temp.inst] conformance) —
> LANDED + committed `2173ae0` on `feature/cpp-detection-idiom-claude`.**
> Full plan + confirmed results + the two next frontiers:
> **`docs/plans/2026-06-09-lazy-member-body-instantiation-plan.md` (READ §8).**
> - WHY: madc eagerly parsed every member body at class instantiation → force-parsed
>   `basic_string::_M_local_data` (+ its `pointer_traits<pointer>` wall) for a size()-only
>   program. g++ instantiates member DEFINITIONS only on ODR-use. Fixed: defer
>   system-header member bodies (`Program::deferred_lazy_bodies`), materialize on demand
>   in cir_builder's existing reachability fixpoint. Build clean.
> - EFFECT (probed): **testcout (real `<iostream>`) ADVANCES PAST** the `_M_local_data`
>   `Unknown namespace 'pointer'` wall (clean WIP hits it; lazy dissolves it — testcout
>   never odr-uses `_M_local_data`). **str1 still hits it, correctly** — `std::string`'s
>   dtor genuinely odr-uses `_M_local_data`, so lazy defers but it materializes.
> - TWO NEXT FRONTIERS (plan §8): (1) testcout: `allocator_int32_t___dtor` undefined
>   (call-vs-definition mismatch / late-instantiation synth-dtor gap; the `int` is REAL —
>   `char_traits<char>::int_type==int`). (2) str1 HEADLINE: `basic_string<char>` is
>   non-polymorphic so `is_externally_defined()` bails at `!has_vtable` (parser.cpp:6316)
>   → madc emits its ctor/dtor → reaches `_M_local_data`. FIX = implement **`extern
>   template`** handling (madc has NONE — grep-confirmed) and bind extern-template
>   instantiations' members to libstdc++ `C1`/`D1`/mangled (generalize 6b5d4ea/38d9152 to
>   non-poly). Do NOT blanket-bind all system non-poly instantiations (vector<int> isn't
>   exported by libstdc++). This is the next major piece — its own focused session.
> - §12.7/§12.8 (detection-idiom + pointer_traits) are SUPERSEDED as the "next layer" by
>   plan §8 (pointer_traits was a symptom; the real chain is lazy + extern-template). The
>   detection idiom itself still stands (proven).
>
> ## ⚑ 2026-06-09 (turn 2) STATE — (superseded by turn 3 for "next step"; branch facts current)
> **TWO BRANCHES (everything is COMMITTED — no fragile stashes hold this work):**
> - **GREEN TIP** = `feature/header-partition-claude` @ **`d7344ac`** (30 ahead of develop,
>   LOCAL/UNPUSHED). fulltest **543/4** (known reds testdefer/testfstream/testlargesizeofquery/
>   testloop); real-header `cout` + `ofstream` RUN. `bin/madc` on this branch matches it.
> - **WIP** = `feature/cpp-detection-idiom-claude` @ **`9fc2ce1`** (= green tip + 2 commits):
>   the `__void_t` SFINAE **detection idiom + nested template-id partial-spec unifier**,
>   IMPLEMENTED and **PROVEN** (g++ oracle reproduced — see §12.7). NOT on the green tip
>   because mid-chain it regresses `testcout_realhdr` (fulltest 542/5): real `<string>` is a
>   multi-layer chain. **To continue: `git checkout feature/cpp-detection-idiom-claude && make -C src`.**
> - **DONE earlier this session (on the green tip): ofstream dtor SIGSEGV FIXED** (`ecfc856`):
>   array-of-class-object member dims dropped in `member_node`'s `is_class_object` path
>   (ios_base `_Words _M_local_word[8]` → -112B → C1 overflowed the slot; confirmed via the
>   POST-CHECK c2mir tree). Real `<fstream>` ofstream constructs + writes `hello42` + destructs cleanly.
> - **LIVE FRONT = real `<string>`** (gates `getline` → testfstream/testloop). Detection idiom
>   DONE (§12.7). **NEXT LAYER (§12.8): `std::pointer_traits<char*>` INSTANTIATION** —
>   `instantiate_template_id` returns NULL for it in `basic_string::_M_local_data()`; the
>   expr-arm dispatch (parser.cpp:11984) is fine, the pointer_traits instantiation is the gap.
> - User steering 2026-06-09: implement the **REAL detection idiom** (NOT a shim — Rule #2, and
>   do not even *offer* shims). Lazy member-typedef instantiation is a *follow-up*, not a
>   substitute. "A test not using feature X" is NOT a reason to drop X. Keep g++ as oracle.
> - NOTE: §0 TL;DR + §3–§5 below describe the EARLIER (ofstream-dtor) milestone of this
>   session and have stale HEAD/count numbers — the authoritative current state is THIS banner
>   + §12. The dtor "NEXT WALL = basic_string.h:3486" in §0 is SUPERSEDED by §12.

> **THE PROCESS LESSON THAT GOVERNS THIS WORK (do not skip).** This campaign's
> recurring failure: a fresh session rehydrates on the routed handoff only,
> re-derives designs/building-blocks that already exist, and that re-derivation IS
> the duplicate code the campaign exists to delete. Before planning ANYTHING:
> (1) read the design corpus (§2); (2) `grep` for the capability before assuming
> it's missing — treat "I think X is absent" as a search task, not a fact;
> (3) PROBE before theorizing — this session I theorized a struct-identity split
> and an undersizing root cause, both wrong; a 2-minute instrumented probe each
> time would have saved an hour. See `[[feedback_rule4_check_own_prior_work]]`.

---

## 0. TL;DR

Branch **`feature/header-partition-claude`**, HEAD **`ecfc856`**, working tree
clean, **26 commits ahead of `develop`, local only (UNPUSHED — the user's call; do
NOT push without asking).** Gates green every commit: **fulltest 543/4** (known
reds testdefer/testfstream/testlargesizeofquery/testloop), **gcc.c-torture
1566/31/57/1** (run ALONE), `<iostream>` + `test_extern_polymorphic` run.

**UPDATE 2026-06-09: the dtor crash is FIXED (commit `ecfc856`) — real libstdc++
`<fstream>` ofstream now CONSTRUCTS + WRITES `hello42` + DESTRUCTS cleanly, EXIT 0.**
Root cause WAS undersizing, confirmed at the deepest layer (§5 below, now resolved):
`member_node`'s class-object early-return path emitted an EMPTY declarator list,
dropping the N_ARR dims for any `Class arr[N]` member. `ios_base`'s `_Words
_M_local_word[8]` collapsed to one `_Words` → ios_base lost 112B → libstdc++'s C1
overflowed madc's stack slot → dtor nil-dispatch. Confirmed via the POST-CHECK
c2mir tree (`--dump-cir-checked`: empty `LIST()` vs the scalar sibling's `ARR()/I()4`),
NOT `--emit=c11`. Fix = honor `mdims` in the class-object path like the scalar path.
General bug (not stream-specific). **NEXT WALL = the ifstream/`std::getline`/
`std::string` INPUT path** (`basic_string.h:3486` cluster — a synthesized basic_string
method body, 6 c2mir check errors; the :3486 origin is a fallback stamp, real fault
is the instantiated body). See §4/§8 (W2-remaining `std::string` operator>>/getline).

**(historical, now resolved) This session got real libstdc++ `<fstream>` ofstream from a 12-error compile wall
to: CONSTRUCTS via libstdc++'s real `C1` ctor + WRITES `hello42` correctly to a
file.** The one remaining wall WAS: the **destructor SIGSEGVs at scope exit** (after the
writes succeed) — **NOW FIXED, see the UPDATE above**. (Original note, kept for the
diagnostic trail: root cause was NOT confirmed at write time — see §5, and heed the warning that
`--emit=c11` text is NOT layout truth.

**User's standing steering (2026-06-08):** (1) optimization is TABLED — get all the
real std headers WORKING first (correctness/coverage over speed); next targets past
`<iostream>` are `<fstream>` then `<sstream>`. (2) Embedded-header packaging (B-half)
is ON HOLD. (3) The big perf lever (demand-driven instantiation + AST-compressed
lazy-load PCH) is TABLED until everything works. (4) No shims, fix at the DEEPEST
layer, every discriminator DATA-DRIVEN (never `namespace=="std"`). (5) Clean up
background shells/processes when done.

---

## 1. HOW TO REHYDRATE (do this before touching anything)

1. `bash scripts/resume.sh` — live branch/HEAD/build/runaway-process truth.
2. Read this handoff fully.
3. Read the design corpus (§2) — do NOT re-derive it.
4. `git log --oneline --reverse develop..HEAD` — this session's 24 commits (listed §3).
5. Skim memory `[[project_header_partition]]` (the live campaign status; this
   handoff is its detailed twin).
6. Cap EVERY run: `( ulimit -t 120; timeout 180 <cmd> )`, ONE heavy job at a time.
   Background `-v` output to a FILE then grep — interactive `-v | grep` truncates
   and has lied about capture counts repeatedly. `--emit=c11` is NOT a correctness
   oracle (it skips c2mir checking AND, as found this session, can text-render a
   layout differently from the real tree). Validate by RUNNING or by reading the
   tree. NAS mtime trap: `touch src/<f>.cpp` before `make`; clean-rebuild if results
   look impossible.

---

## 2. GOVERNING DESIGN CORPUS (READ — do not re-derive)

The header-partition campaign = **madc ships only compiler-freestanding + its own
`ns_*` headers, and CONSUMES the real glibc/libstdc++ headers** (eventually as one
pre-lexed compressed embedded package), retiring the hand-tooled stdlib shims.

- **`~/.claude/plans/clever-scribbling-dove.md`** — the approved campaign plan.
  Build half **B1–B6**, runtime half **R1–R5**, **M** (shim retirement), **A**
  (acceptance oracle). §8 tracks each.
- **`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`** — W1–W5
  (W1 mangler, **W2 non-member operator resolution**, W3 ABI-from-declaration,
  W4 extern header globals, W5 auto-include map).
- **`docs/superpowers/plans/2026-06-02-mangler-nonmember-template-ops.md`** — the
  W1 mangler-completeness plan (`itanium_mangle_std_free_template`).
- `docs/plans/madc-vision-and-invariants.md` — invariants I1–I8.
- Prior handoff `docs/plans/2026-06-08-header-partition-W2-perf-HANDOFF.md`
  (the `<iostream>`-runs + perf milestone; this handoff continues it).
- Memory: `[[project_header_partition]]`, `[[project_cpp_mangled_direct]]`,
  `[[project_template_instantiation]]`, `[[project_multiple_inheritance]]`,
  `[[feedback_rule4_check_own_prior_work]]`, `[[feedback_correct_over_shortcuts]]`,
  `[[feedback_emitc_gcc_parity_oracle]]`.

Branch facts (from git, not assumption): `feature/header-partition-claude` is the
canonical tip (24 ahead of develop). `retire-std-hardcoding` is already merged into
develop — not a competing branch. No fork to reconcile.

---

## 3. THIS SESSION'S WORK — the fstream-construction arc (8 fixes, last turn's 4 bolded)

All gated 543/4 + 1566/31/57/1 + canaries. Prior commits (4fa746e..a82085b) are the
`<iostream>`-runs + perf arc (see the 2026-06-08 handoff). This session added:

| Commit | What it fixed |
|---|---|
| `39878b7` | **Instantiation cache / `is_complete`.** The C++ class-def parser (`TokenCLASS::parse`) never set `is_complete` after parsing a `{...}` body (unlike the C struct parser). So instantiated C++ class templates stayed `is_complete=false`; `is_incomplete_class_datadef` (parser.cpp:2325) matched them on the empty-aggregate heuristic (true for typedef-only traits like `iterator_traits<T>`), and `instantiate_template_use`'s cache re-instantiated them on EVERY reference (`iterator_traits<uint32_t>` 6×). Fix: `TokenCLASS::parse` sets `ddc->is_complete=true` after layout finalize (cir ~18006/parser ~18092); `is_incomplete_class_datadef` honors `is_complete`. Empty `<iostream>` instantiations 398→344, zero dups. |
| `e6beebc` | **By-value plain-struct emission ordering.** `emit_class_member_deps` only hoisted by-value CLASS members before their owner; plain struct/union members (DataDefSTRUCT) were skipped. `basic_filebuf` embeds `pthread_mutex_t` (`_M_lock`, anon union holding `struct __pthread_mutex_s __data`) by value → emitted before `__pthread_mutex_s`'s def → c2mir "field __data has incomplete type". Fix: new `CirBuilder::emit_struct_with_deps` (cir 2904) hoists plain struct/union member types too (recurse members, emit named body; anon aggregates inline so only deps hoist). |
| `e4fca20` | **vptr slot in typedef-def-point classes.** A polymorphic class emitted via `typedef struct C{...} alias;` (template instantiations: basic_ios, basic_ofstream) had its body emitted by `typedef_decl` via `anon_members_list` (plain-struct member list, NO vptr), dropping the `__vptr` slot. Fix: extracted `class_struct_def`'s field builder into `CirBuilder::class_member_list` (cir 2762); `typedef_decl`'s inline-body path uses it for a class with `has_vptr_slot`. |
| **`6b5d4ea`** | **Mangled-direct construction/destruction for externally-defined classes (THE KEYSTONE).** madc was SYNTHESIZING ctors for libstdc++-owned classes (member-wise body + vbase chain), which can't reproduce the ABI. New pass (cir ~8740, before global-ctor + body translation) binds, for each `is_externally_defined` class: the dtor → real `D1`, and — for TEMPLATE-INSTANTIATION classes only (canonical spelling has `<`) — each ctor → real `C1` via `itanium_mangle_ctor_sub`(canonical, param spellings). `class_ctor_call` (cir 4051, ~line 4180) skips the vbase-construction chain when the ctor is bound (`emit_symbol` set). **GATING CAUGHT:** binding ALL external classes broke `std::bad_alloc` (inline/defaulted ctor, NO exported `_ZNSt9bad_allocC1Ev` → dangles at link) → hence the `<`-only gate; concrete classes keep madc's vptr-init ctor. A wrong bind fails LOUDLY at MIR-link. |
| **`38d9152`** | **Combined-typedef hoist + external complete-dtor.** (a) `emit_struct_with_deps` hoisting a struct via bare `struct_def` stranded the alias of a COMBINED `typedef struct __pthread_internal_list{...} __pthread_list_t;` at its later position → `__pthread_mutex_s`'s member `__pthread_list_t __list` referenced an undefined alias → "unknown type __pthread_list_t". Fix: pre-scan maps each struct tag → its combined-typedef TopDecl (`m_combined_typedef_alias`); the hoist emits the WHOLE combined decl (body+alias) and records the alias (`m_hoisted_combined_aliases`) so Pass 0 skips re-emitting. (b) `class_complete_dtor_symbol` (cir 3685) returns the real `D1` for `is_externally_defined` classes (the synthesized `Cls___dtor_complete` wrapper is never emitted for them → was "undefined item …__dtor_complete" at the cleanup attribute). **ofstream now COMPILES + LINKS.** |
| **`22c5b53`** | **W2 free-operator resolution for DERIVED streams.** `deduce_free_stream_call` (cir 4552) only matched the lhs's own head spelling, so `ofstream out; out<<"x"` (basic_ofstream != basic_ostream) rejected the free `operator<<(ostream&,const char*)` and mis-bound the MEMBER `operator<<(streambuf*)`, passing the `const char*` as a streambuf* → SIGSEGV in `__copy_streambufs`. Fix: `try_free_operator_call` (cir 4614) now deduces against the lhs class AND its non-virtual bases (`collect_self_and_base_spellings`, cir 4597 — spelling+offset, most-derived first) + adjusts the passed stream ptr to the base subobject (offset 0 for single-inheritance streams; `adjust_to_base` lambda). Both operator and `std::endl`-manipulator paths. **`out << "hello" << 42` now WRITES "hello42" correctly.** |

---

## 4. CURRENT fstream STATE (precise)

Reducers in `tmp/` (gitignored): `tmp/fs_out.mad` (ofstream-only write), `tmp/fstream_test.mad`
(full: ofstream write + ifstream getline), `tmp/iosrepro2.mad` (minimal array-of-object
repro), `tmp/fssz.mad` (sizeof probe — currently errors, see §5).

- **ofstream construction:** `std::ofstream out("f")` lowers to the EXACT g++ symbol
  `_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode((&out),"f",16)`.
  libstdc++ builds the whole hierarchy. ✅ (= g++)
- **ofstream operator<<:** `out << "hello" << 42` binds the free
  `operator<<(ostream&,const char*)` (via base deduction) + member `operator<<(int)`.
  WRITES "hello42" to the file. ✅
- **ofstream destructor:** ✅ FIXED (ecfc856) — destructs cleanly, EXIT 0. (Was a
  SIGSEGV at scope exit from the array-of-class-object undersizing; see §5.)
- **ifstream / getline:** NEXT WALL — but it is NOT getline-specific. **Even a bare
  `std::string line;` (no getline, no fstream) hits the same `basic_string.h:3486`
  cluster** (6 c2mir check errors). ROOT CAUSE LOCATED (2026-06-09, via `--dump-cir`
  on `tmp/str1.mad`): in the real-libstdc++ `<string>` instantiation, basic_string's
  POINTER/SIZE members resolve to **opaque unreduced alias-template names instead of
  concrete types**:
    - `_Alloc_hider::_M_p` (should be `char*`) → `STRUCT() ID()
      __detected_or_t_value_type____pointer__Char_alloc_type` (a struct, not a pointer).
    - `_M_string_length` (should be `unsigned long`) → `STRUCT() ID()
      allocator_traits_…_Size_…_type` (an `allocator_traits<>::size_type` alias).
    - `_M_local_buf` IS correct (`char[16]`).
  Because `_M_p` is a struct not a pointer, every method that does `_M_p[n]`,
  `_M_p == x`, `&_M_p[n]`, or returns it fails → the 6 errors (subscript / comparison
  ×2 / lvalue-`&` / struct-return). **THE GAP = alias-template + detection-idiom
  resolution**: `allocator_traits<allocator<char>>::pointer`/`size_type` flow through
  libstdc++'s `std::__detected_or_t<…>` / `__detector` SFINAE machinery, which madc is
  NOT reducing to the concrete `char*`/`size_t`. This is a substantial template-
  metaprogramming piece (sibling of the `__are_same`/trait-builtin + R5/C2 namespaced
  template-id work), NOT a quick fix — it also gates campaign-M `<string>`-first
  retirement and all of W2-remaining (`std::string` operator>>/getline). Reducer:
  `tmp/str1.mad` (minimal: `std::string line; line.size();`). Suggested next move:
  brainstorm the detection-idiom resolution approach before coding (alias-template
  instantiation + `__detected_or`/`__detector` SFINAE → concrete member type).

Run to reproduce the crash:
```bash
( ulimit -t 120; timeout 180 bin/madc --std=c++17 --no-embedded-headers tmp/fs_out.mad )
# writes "hello42" to tmp/fs_out_scratch.txt then SIGSEGV
```

---

## 5. THE DTOR CRASH — RESOLVED 2026-06-09 (commit `ecfc856`)

**RESOLUTION:** undersizing WAS the root cause, and it was a real c2mir-tree bug
(not just `--emit=c11`). `member_node`'s `is_class_object` early-return path
(cir_builder.cpp ~2235) emitted `node2(N_DECL, id(...), list())` — an EMPTY
declarator — dropping the `mdims` computed just above. So EVERY `Class arr[N]`
member collapsed to one element. `ios_base`'s `_Words _M_local_word[8]` (the `_Words`
elem has a ctor → `is_class_object` true) lost 7×16=112B; ofstream went 512→~400;
libstdc++'s real `C1` overflowed madc's stack slot by 112B → `D1` walked
`_M_word`/`_M_local_word`/callbacks off the corruption → nil-dispatch SIGSEGV.
**Confirmed via the POST-CHECK c2mir tree** (`--dump-cir-checked` → `_M_local_word`
had empty `LIST()`, sibling scalar `__pad1` had `ARR()/I()4`) — the JIT consumes the
node_t tree directly (`c2mir_compile_tree`), so this was a genuine tree bug, and the
`--emit=c11` scalar render was the same emit path being consistently wrong. **Fix:**
the class-object path now builds its declarator with the same N_ARR dims (+ flexible
handling) the scalar path uses; `_M_local_word` now emits `ARR()/I()8`; ofstream
writes "hello42" AND destructs cleanly (EXIT 0). Gates: fulltest 543/4, torture
1566/31/57/1, canaries — all green. The methodology that cracked it (per the user's
steering): g++ `-S -fverbose-asm` for the 512-byte slot + offset-0 calls, then the
`--dump-cir-checked` POST-CHECK tree (NOT `--emit=c11`) to confirm the dropped dim.

--- ORIGINAL (diagnostic trail, now superseded by the resolution above) ---

**Do NOT trust the earlier "ofstream is undersized" claim — it is UNCONFIRMED and
may be a `--emit=c11` text artifact.** Here's exactly what was found and what wasn't:

**Finding via `--emit=c11`:** an array of an OBJECT-class member renders as a SCALAR.
`ios_base` has `enum { _S_local_word_size = 8 };` and `_Words _M_local_word[_S_local_word_size];`
where `_Words` has a ctor (`_Words() : _M_pword(0), _M_iword(0) {}`). madc emits
`struct ios_base___Words _M_local_word;` with **no `[8]`**. Minimal reducer
`tmp/iosrepro2.mad` reproduces it; a TRIVIAL `_Words` (no ctor) emits `[8]` correctly
(`tmp/iosrepro.mad`). If real, this undersizes `ios_base`/`ofstream` → libstdc++'s
C1/D1 overflow madc's stack slot → crash. (g++ sizes: ofstream=512 ifstream=520
filebuf=240 ios=264 ostream=272.)

**WHY IT'S UNCONFIRMED (the contradiction):** I instrumented `member_node`
(cir_builder.cpp ~2189) — it RECEIVES the correct `is_array=1 count=8 dims=[8]`
(MNPROBE), and `addMember` logs "count 8, total 144". `member_node`'s mdims block
(~2194) + the N_ARR emit (~2360) SHOULD therefore emit `[8]`. `struct_def` (cir 2401)
AND `anon_members_list` (cir 2158) BOTH go through `member_node`. So the c2mir TREE
probably HAS `[8]` (correct) while only the cir_emit_c TEXT drops it. **If the tree is
correctly `[8]`-sized, the dtor crash is NOT undersizing** and I chased a partial red
herring. The contradiction (member_node has `[8]`, emitted text is scalar) was NOT
resolved — candidates: (a) a 2nd non-`member_node` emitter produces the scalar and
wins via `emitted_structs` dedup; (b) cir_emit_c's declarator renderer drops N_ARR for
an object-typed member. Neither proven.

**NEXT STEPS (fresh eyes — diagnose via RUNTIME, not `--emit=c11` text):**
1. **gdb the JIT SIGSEGV.** `out<<"x"` crashes at the scope-exit dtor though "x"
   writes. Get the real faulting frame/address. (Earlier minimal backtrace was 2
   frames, no symbols — need a real build/gdb attach.)
2. **Get madc's REAL `sizeof(std::ofstream)`** and compare to g++'s 512. BLOCKED by a
   separate parser bug: `sizeof(std::ofstream)` / `sizeof(std::basic_ios<char>)`
   errors "'X' is not a member of namespace 'std'" *in `sizeof` context* though the
   same type parses elsewhere — a small namespaced-template-id-in-`sizeof` gap; FIX
   THAT FIRST (it's also independently useful), or read the size off the c2mir tree
   / `DataDefCLASS::size`.
3. **If the tree IS correctly sized**, the crash is the D1 binding / vptr / vbase
   layout: madc places the `basic_ios` virtual base at `+248` in ofstream
   (construction emitted `(char*)&out + 248`) — compare to g++'s actual vbase offset
   (`g++ -fdump-lang-class` or read the vtable's vbase-offset slot). Mangled-direct
   construction of a class WITH virtual bases REQUIRES madc's layout to byte-match
   libstdc++ (the MI/vbase layout engine S1–S4 exists — `[[project_multiple_inheritance]]`
   — but may not match libstdc++ exactly for the stream diamond).
4. **Separately (real bug regardless):** if cir_emit_c text-renders array-of-object
   members as scalar, `--emit=c11` portable-C output is wrong even when the JIT tree
   is right. Worth fixing; reducer `tmp/iosrepro2.mad`. Trace `member_node`'s N_ARR
   into the cir_emit_c declarator renderer.

---

## 6. KEY FILE ANCHORS (verify with grep — line numbers drift)

- `src/cir_builder.cpp`:
  - `class_member_list` 2762 · `class_struct_def` just below it (wraps it).
  - `emit_struct_with_deps` 2904 · `emit_class_member_deps` ~2867 (now recurses into
    plain struct/union members; routes class members to `emit_class_struct_with_deps`).
  - `member_node` ~2172 (the member emitter; mdims block ~2189–2202, N_ARR emit ~2360).
  - `struct_def` 2401 · `anon_members_list` 2158.
  - `class_complete_dtor_symbol` 3685 (returns D1 for external) · `class_dtor_symbol`
    ~3648 (returns `emit_symbol` when set).
  - `class_ctor_call` 4051 (vbase-chain skip when `ctor->emit_symbol` set, ~line 4180).
  - `deduce_free_stream_call` 4552 · `collect_self_and_base_spellings` 4597 ·
    `try_free_operator_call` 4614 (candidate loop ~4628/4636; `deduce_any` +
    `adjust_to_base` lambdas; manipulator path + operator path).
  - The ctor/dtor binding pass: ~8740 (`itanium_mangle_ctor_sub` call at 8760), just
    before `collect_global_ctors`. Mirrors Pass 1.5 (vtable, ~8862) / Pass 1.6 (dtor
    synth) which gate on `is_externally_defined()`.
- `src/cir_builder.h`: `class_member_list` decl; `emit_struct_with_deps` decl;
  `m_combined_typedef_alias` (map struct-tag → combined-typedef TopDecl) +
  `m_hoisted_combined_aliases` (set) near `m_ambiguous_typedef_aliases` (~132).
- `src/parser.cpp`: `is_incomplete_class_datadef` 2325 (honors `is_complete`);
  `TokenCLASS::parse` data-member array parse ~17845, `addMember` ~17924, sets
  `ddc->is_complete=true` ~18092 (after `build_vtable_groups`); `bind_declared_cpp_symbol`
  16481 (only fires for `declaration_only` — that's why inline-bodied stream ctors
  weren't bound at parse, hence the cir_builder pass).
- `src/madc_mangle.cpp`: `itanium_mangle_ctor_sub` 831 (`_ZN…C1…`),
  `itanium_mangle_dtor_sub` 839 (`_ZN…D1Ev`), `itanium_mangle_std_free_template`
  (the `_ZStls…`/`_ZSt4endl…` generator W2 consumes).
- `include/datadef.h`: `DataDefCLASS` — `is_complete` 283, `is_externally_defined()`
  ~710, `from_system_header` ~733, `has_vptr_slot`, `bases` (BaseSpec{base,offset,
  is_virtual} ~655), `is_anonymous` 286.
- `DataDefCLASS::is_externally_defined()` (parser.cpp): has_vtable && canonical
  spelling && every vtable slot bodyless-external && whole base chain external —
  OR `from_system_header` (override for inline-virtual-default facets/streams, 12398f2).

---

## 7. METHOD + COMMANDS + GATES (mandatory)

```bash
bash scripts/resume.sh
make -C src 2>&1 | grep -iE 'error:|warning:'                  # clean build, NAS: touch first
make -C src fulltest 2>&1 | grep -E 'passed,|FAIL:'            # 543/4 known reds
python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc | tail -2  # 1566/31/57/1 ALONE
bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad             # "This is a test, x = -1"
bin/madc --std=c++17 --no-embedded-headers tests/test_extern_polymorphic.mad  # what=std::bad_alloc / name=St9bad_alloc
( ulimit -t 120; timeout 180 bin/madc --std=c++17 --no-embedded-headers tmp/fs_out.mad )  # writes hello42 then SIGSEGV
```
- Per change: reduce → compare g++ AND clang → DEEPEST-layer fix → rebuild → re-probe
  → fulltest (known reds only) → torture failset ALONE → commit. RUN to validate
  (NOT `--emit=c11`).
- These four fulltest reds PREDATE all this work: testdefer, testfstream,
  testlargesizeofquery, testloop (the first three go green when the real-`<fstream>`
  shim retirement lands, campaign M).
- Anything touching struct/ctor/dtor/class-layout emission has HIGH blast radius
  (C path + SMAUG) — torture ALONE every time; the C path is structurally untouched
  because every new behavior gates on `is_externally_defined()` / `from_system_header`
  / `<`-in-spelling, all false for C.

---

## 8. BROADER CAMPAIGN STATUS (every workstream)

**retire-std-hardcoding W1–W5:** W1 mangler DONE (develop). **W2 non-member operator
resolution DONE** + now matches derived streams (this session); REMAINS: `std::string`
`operator<<`/`operator>>`/`getline` (the basic_string input-path cluster), friend
operators + full ADL (task #19). W3 ABI-from-declaration PARTIAL. W4 extern globals
PARTIAL. W5 auto-include map PARTIAL.

**clever-scribbling-dove R1–R5:** R1 per-`--std` macros + `__has_*` NOT DONE. R2
libstdc++ auto-load DONE (develop). R3 retire `array` keyword NOT DONE (blocks
tuple/array). R4 trait-builtin breadth PARTIAL. R5 real-header sema: `<iostream>`
DONE+RUNS; `<fstream>` construction DONE, dtor-crash blocker (§5); `<string_view>`/
`<memory>` namespaced-template-id-const-capture gap still pending.

**B1–B6 (pre-lexed embedded package):** NONE DONE — **ON HOLD per user** (part of the
tabled optimization picture; the future direction supersedes the pre-LEX-raw-token
framing → fully parse each header to an AST tree, store COMPRESSED, lazy-load).

**M (shim retirement):** NOT DONE. `<iostream>` runs against the real header (candidate
to start M with, carefully). `scripts/check-no-std-hardcoding.sh` is the grep-gate
(wired into fulltest).

**A (acceptance oracle):** NOT DONE. `testcout_realhdr` is the first end-to-end
real-header regression test.

**TABLED (do NOT start without the user re-opening it):** demand-driven / lazy
template instantiation (task #24 — the 398-eager-instantiations lever; route concrete
header template-ids to placeholders, force-complete at ODR-use; delicate, a missed
completion-force = incomplete-type miscompile); the AST-compressed-lazy-load PCH.

---

## 9. TASK LIST (reconcile on resume; IDs from the session task tracker)

- DONE: R2 auto-load (#7), ref-return &call (#16), W2 non-member operators (#17),
  std::endl mangled-direct (#20), PERF scoped instantiation-cache fix (#21).
- IN PROGRESS: R5 real-header sema (#14); **Real `<fstream>` compiles+runs (#22)** —
  constructs+writes; dtor crash is the wall (§5).
- PENDING: R1 per-std macros+`__has_*` (#8), A acceptance oracle (#9), B1/B2 (#10),
  B3-B6 ON HOLD (#11), R3 retire `array` (#12), R4 trait breadth (#13), M shim
  retirement (#15), friend+full-ADL (#19), Real `<sstream>` (#23), TABLED
  demand-driven instantiation (#24).

---

## 10. WHY NONE OF THIS IS A SHIM (the user's standing constraint)

Every discriminator is DATA-DRIVEN (`is_externally_defined`/`from_system_header`/
`is_system_header_path`/`has_vptr_slot`/`<`-in-canonical-spelling), never
`namespace=="std"` or a name test (Rule #7). Construction/destruction bind to the
REAL libstdc++ `C1`/`D1` via the mangler that GENERATES the Itanium symbol (never a
hardcoded `_ZNSt…` literal — that drift cost days). The `<`-only ctor gate is the
"explicitly-instantiated template" proxy (libstdc++ exports those; concrete inline
ctors like bad_alloc's it does not — a wrong bind fails LOUDLY at link, never
silently). The instantiation-cache fix and the emission-ordering fixes are
correctness fixes (O(n²)→O(1); definition-before-by-value-use), not shortcuts. The
dtor crash will be fixed at the deepest layer (real layout / D1 binding), NOT papered
over.

## 11. OPEN ITEMS
- **Unpushed:** branch is 24 commits ahead of `develop`, local only. Pending the
  user's push decision — do NOT push without asking.
- `tmp/` scratch (fs_out.mad, fstream_test.mad, iosrepro*.mad, fssz.mad, *.c, *.err) —
  gitignored.
- The `sizeof(std::ofstream)`-in-sizeof-context parser bug (§5 step 2) is a small
  standalone gap worth fixing (unblocks the layout diagnosis + is generally useful).

See `[[project_header_partition]]` (live status), `[[feedback_rule4_check_own_prior_work]]`,
`[[project_cpp_mangled_direct]]`, `[[project_multiple_inheritance]]`,
`[[feedback_emitc_gcc_parity_oracle]]`. Supersedes
`docs/plans/2026-06-08-header-partition-W2-perf-HANDOFF.md` for current state (that one
keeps the `<iostream>`-runs + findVariable-perf granular history).

---

## 12. REAL `<string>` — root-cause map + the detection-idiom plan (2026-06-09)

Getting real libstdc++ `<string>` to compile (the gate for `getline`, and thus
testfstream/testloop the right way) was traced end-to-end this session. Layers peeled,
in order, each by PROBING (not theorizing) + the g++ oracle:

### 12.1 What's DONE / what's the wall
- **DONE — array-of-class-object member dims** (`ecfc856`, §5). Unrelated to string but
  was the ofstream dtor fix.
- **PREREQUISITE, STASHED (`stash@{0}`, NOT committed)** — nested template-id partial-spec
  unification. `allocator_traits<allocator<char>>` must select the partial spec
  `allocator_traits<allocator<_Tp>>` (`pointer=_Tp*`, `size_type=size_t`) instead of the
  primary (`pointer=__detected_or_t<…>`). madc's `unify_spec_pattern_arg` (parser.cpp
  ~10405) only matched `[cv] PARAM [*]*` and exact-spelling; a NESTED template-id pattern
  (`allocator<_Tp>` vs concrete `std::allocator<char>`, namespace-qualified) fell through
  to the primary. Fix = `Program::unify_nested_spec_pattern_arg` (string-based: strip ns,
  split top-level args, recurse; deduce `_Tp=char` via `resolve_named_datadef`) wired as a
  fallback in `match_partial_specialization`'s loop. **Correct, but it regresses `cout`
  ALONE** because it makes basic_string complete far enough to force the next layer →
  do NOT commit it without the detection idiom. (+115 parser.cpp / +8 madc.h.)
- **THE WALL — the `__void_t` detection idiom for `iterator_traits`.** madc EAGERLY
  instantiates every basic_string member typedef incl. `reverse_iterator`, whose base
  `iterator<typename iterator_traits<_It>::iterator_category, …>` (stl_iterator.h:137)
  needs `iterator_traits<__normal_iterator<char*,string>>::iterator_category`.

### 12.2 g++ ORACLE (the targets to reproduce — `tmp/itchain.cpp`)
- `iterator_traits<__normal_iterator<char*,string>>::iterator_category` = **`std::random_access_iterator_tag`**
- `…::value_type` = **`char`**
- `iterator_traits<char*>::iterator_category` = `random_access_iterator_tag`
- Chain: `__normal_iterator::iterator_category` (its member typedef) → `iterator_traits<char*>`
  (pointer partial spec, works via the flat `_Tp*` unifier) → `random_access_iterator_tag`.

### 12.3 The libstdc++ construct (C++17, stl_iterator_base_types.h:155-178)
```cpp
template<typename It, typename = __void_t<>> struct __iterator_traits {};           // primary, empty
template<typename It> struct __iterator_traits<It,
    __void_t<typename It::iterator_category, typename It::value_type,
             typename It::difference_type, typename It::pointer, typename It::reference>>
  { typedef typename It::iterator_category iterator_category; … };                   // SFINAE spec
template<typename It> struct iterator_traits : public __iterator_traits<It> {};      // inherits
```
`__void_t<Args...>` = `void` IFF all `Args` are valid types. So the partial spec is
selected IFF `It` has all five nested member types.

### 12.4 FOUR PIECES (all in `src/parser.cpp`)
1. **DONE/exists** — base-class type-alias lookup: `resolve_class_type_alias` (1676)
   already recurses `bases[]`/`base_class`/`enclosing_class`. So once `__iterator_traits<It>`
   has the alias, `iterator_traits<It>::iterator_category` finds it through the base.
2. **MISSING (the core blocker)** — `__iterator_traits<It>` is **never instantiated**:
   `match_partial_specialization` is NEVER called for `"__iterator_traits"` (verified by
   probe). It's left an incomplete/dependent placeholder. Two sub-causes to untangle:
   (a) `iterator_traits<__normal_iterator<…>>` is itself instantiated as dependent-surface
   (so its base clause isn't really processed), and/or (b) the dependent template-id base
   `__iterator_traits<_Iterator>` isn't getting `_Iterator` substituted + instantiated.
   Investigate `instantiate_template_use`'s dependent path, `materialize_dependent_member_type`
   (3082, the path `resolve_typename_type_token`@3024 takes when the owner is dependent),
   and the base-clause loop (16857-16966; opaque-base fallback at 16952 — NOTE: probe showed
   `__iterator_traits` base does NOT hit 16952, so it resolves to *something* — likely a
   forward/incomplete that's never completed).
3. **MISSING** — default template-arg materialization: the primary has 2 params
   (`It`, `typename=__void_t<>`); an instantiation `__iterator_traits<It>` must materialize
   the 2nd as `void` → type_args `[It, void]` so arity matches the partial spec's 2 pattern
   slots (`<slot0:_Iterator><slot1:__void_t<typename _Iterator::iterator_category,…>>`).
4. **MISSING** — `__void_t<…>` pattern-slot evaluation in `match_partial_specialization`:
   a slot whose pattern outer-name is `__void_t` matches the concrete `void` IFF every arg
   inside (e.g. `typename _Iterator::iterator_category`), with `_Iterator` substituted from
   the slot-0 deduction, resolves to a valid type (SFINAE member-existence check via
   `resolve_class_type_alias`/member-chain). This is ADDITIVE (no `__void_t` spec matches
   today) so low regression risk on its own. `__void_t` is used pervasively (alloc_traits
   `_Ptr`/`_Diff`/`_Size`, chrono, refwrap, functional_hash) → foundational for the campaign.

### 12.5 Sequencing + gates
Implement 2→3→4 (1 is done), restore `stash@{0}` alongside, validate each step against
`tmp/str1.mad` and the §12.2 g++ oracle (`random_access_iterator_tag`), then gate HARD:
fulltest (known reds), gcc.c-torture **alone** (1566/31/57/1), and the cout+ofstream
canaries (this whole area gates on `is_externally_defined`/`from_system_header`/`<`-spelling,
all false for C — but partial-spec selection touches ALL templates, so torture is mandatory).
Lazy member-typedef instantiation (the user's architecture direction, currently tabled #24)
is a worthwhile FOLLOW-UP for perf/correctness but is NOT a substitute for the feature.

### 12.6 Anchors (verify with grep — drift)
- `unify_spec_pattern_arg` 10405 · `unify_nested_spec_pattern_arg` (the stashed addition,
  ~10489) · `match_partial_specialization` ~10560 · the matching loop's per-slot unify call.
- `resolve_typename_type_token` 3024 (the `::member` walk on a typename-qualified type) ·
  `resolve_class_type_alias` 1676 (base-recursive) · `materialize_dependent_member_type` 3082.
- base-clause instantiation in `TokenCLASS::parse` 16857-16966.
- Reducers (tmp/): `str1.mad` (`std::string line; line.size()` — minimal trigger),
  `str2.mad`, `str3.mad` (getline). g++ oracle `tmp/itchain.cpp`.

### 12.7 PROGRESS 2026-06-09 (turn 2) — detection idiom IMPLEMENTED + proven; chain advanced 2 layers
Three additive parser changes (≈215 insertions; in the working tree / WIP — NOT on the
green tip, see below). All discovered via probe-then-fix against the g++ oracle:
1. **Nested template-id partial-spec unification** — `unify_nested_spec_pattern_arg`
   (parser.cpp ~10546) + wired into `match_partial_specialization`. So
   `allocator_traits<allocator<char>>` selects the partial spec (string-based: strip ns,
   split top-level args, recurse; deduce via `resolve_named_datadef`).
2. **`typename` is NOT consumed in `consume_template_type_arg_qualifiers`** — that was a
   wrong turn, REVERTED. The real `typename X<T>::member` arg handling lives in
   `resolve_typename_type_token` (3024), which works once the member resolves.
3. **`__void_t` detection idiom** — `eval_void_t_detection_slot` (parser.cpp ~10590,
   decl in madc.h) wired as the 3rd fallback in the match loop. A `__void_t<Args...>`
   pattern slot matches concrete `void` (or another `__void_t<...>`) IFF every Arg —
   shape `typename PARAM::member[::member]`, with PARAM substituted from the slot-0
   deduction in `ded` — resolves via `resolve_class_type_alias` (real SFINAE member-
   existence). Unevaluable Arg shapes (decltype, `template rebind<>`) return false →
   empty primary = today's behavior (no regression). GOTCHA found: `template_token_fragment`
   concatenates WITHOUT spaces, so the arg renders `typename_Iterator::iterator_category`
   (glued) — strip the leading 8-char `typename` keyword regardless of spacing.

**EFFECT (verified):** `__iterator_traits<It, void>` now selects its `__void_t` partial
spec → gets the 5 member aliases → `iterator_traits<It>` inherits them through its base
(piece 1, `resolve_class_type_alias` base-recursion, already existed) → the
`iterator<typename iterator_traits<_It>::iterator_category,…>` base of `reverse_iterator`
RESOLVES. str1 advanced TWO layers: past `iterator<>` AND past the typename args.

**NEXT LAYER (str1 now stops here):** `Unknown namespace 'pointer'` — in
`basic_string::_M_local_data()` the body `std::pointer_traits<pointer>::pointer_to(*_M_local_buf)`
is a **template-id-qualified static call in EXPRESSION context**; madc's expr parser
(parseExpr identifier-arm, site **parser.cpp:11947** in `parseExpr_*`) captures the inner
template ARG `pointer` as the `::` qualifier instead of treating `pointer_traits<pointer>`
as a class scope. The expr-arm handles `ClassName::member` (`resolve_expression_class_scope`
@~11931) but not `Template<Arg>::staticmember`. That's the next feature to add (instantiate
the template-id, then resolve the static member on it — mirror what
`resolve_typename_type_token` does for the type-context case).

**WHERE THE WIP LIVES:** committed on branch **`feature/cpp-detection-idiom-claude`** (off
`feature/header-partition-claude`@`28f7d4d`); the header-partition tip stays GREEN at
`28f7d4d`. The detection idiom is correct + proven; it does NOT yet make str1/cout-realheader
compile (the pointer_traits layer remains), so it is intentionally NOT on the green tip.
To continue: `git checkout feature/cpp-detection-idiom-claude`, rebuild, attack site 11947.
Reducers: tmp/str1.mad (minimal), tmp/itchain.cpp (g++ oracle: iterator_category =
random_access_iterator_tag — CONFIRMED reproduced through the detection idiom).

### 12.8 NEXT-LAYER refinement (2026-06-09 turn 2) — it's pointer_traits INSTANTIATION, not the expr arm
Probed the `Unknown namespace 'pointer'` site (parser.cpp:11947) further. The expression
arm for `std::Template<args>::member` ALREADY EXISTS (parser.cpp ~11984: when
`template_declared_in_namespace(member, ns)` and peek `<`, it calls
`instantiate_template_id(member, member_tb, ns)` then `resolve_class_qualified_expression`
for the `::member`). Probe `[PTP] ns=std member=pointer_traits peekLT=1 tdn=1` confirms it
ENTERS that block. So the dispatch is fine — the failure is that
`instantiate_template_id("pointer_traits", "std")` returns NULL for
`std::pointer_traits<char*>` → member_dd stays NULL → fall-through mis-parses `<pointer>`
as less-than and `pointer` becomes a stray `::` qualifier.

So the REAL next sub-problem: **instantiate `std::pointer_traits<char*>`**. libstdc++'s
`pointer_traits` (bits/ptr_traits.h) has a partial spec `pointer_traits<_Tp*>` (with
`static pointer pointer_to(...)`), plus the primary uses the detection idiom (`_Ptr::element_type`
via `__detected_or`). For `pointer=char*`, the `_Tp*` partial spec applies (handled by the
flat `_Tp*` unifier — should work) — so check WHY instantiate_template_id returns NULL:
likely the primary `pointer_traits<_Ptr>` body (which references `__detected_or_t<...>`
member aliases like `element_type`/`difference_type`/`rebind`) is what's instantiated, or
the partial spec isn't selected here. Probe `match_partial_specialization("pointer_traits",...)`
and `instantiate_template_id` for pointer_traits<char*> next. g++ oracle: pointer_traits<char*>
::pointer = char*, ::element_type = char, pointer_to(r) returns char*. (bits/ptr_traits.h.)
Reducer tmp/str1.mad still stops here.

---

## ⚑ 2026-06-09 (turn 5) — HEADER PARTITION: shim-bypass classifier landed; incremental path chosen

**Direction (user-confirmed):** retire the hand-rolled SYSTEM-header shims; madc
provides ONLY bucket-1/2 freestanding compiler headers and CONSUMES real
glibc/libstdc++ (see `madc-header-partition-handoff.md` + memory
`[[project_header_partition_architecture]]`). Do NOT hand-author embedded
libstdc++ (the prior "finish embedded <fstream> inc-5/inc-6" plan is RETRACTED —
it was the vendoring anti-goal).

**LANDED (commit `65d2d67`, gated fulltest 543/4, default mode untouched):**
data-driven embedded-header classifier `embedded_header_is_system_library_shim()`
(parser.cpp) + `RegistrationPolicy.bypass_system_library_headers` +
`madc_compiler_owned_include_dir` (gen_sys_includes.sh). `--no-embedded-headers`
now bypasses ONLY system-library shims (glibc/libstdc++ twins) to the REAL header,
KEEPING madc-own (`ns_*`/`__madc__`, no real twin) and freestanding
(`stddef`/`limits`/`float`, resolve in the compiler-owned dir) embedded. Fixes the
old all-or-nothing that dropped ns_php.

**Two blockers to PHYSICAL shim deletion (deferred per user's incremental choice):**
1. STD_MADC deliberately omits `__cplusplus` (lexer.cpp:1481) → real libstdc++
   fails in default mode; only `--std=c++` works.
2. Embedded `<string>` mixes the libstdc++-binding basic_string (shim) with
   madc-DIALECT helpers (`to_string(s,42)` 2-arg, `stoi`) → deleting it loses them
   unless relocated to a madc-own header.

**CHOSEN PATH (incremental):** get the 4 reds green via real headers in
`--std=c++17 --no-embedded-headers` mode (the proven testcout_realhdr path), fixing
real-header consumption bugs; defer physical shim deletion + the STD_MADC flip.

**NEXT — RC#1 (free-function overload sets), the gating fix for getline:**
`getline(inf,line)` under real headers → "Incorrect number of parameters: expected
3 got 2" (parser.cpp:9523/9558). Root cause: `register_skipped_namespace_template_function`
(parser.cpp ~22565) drops every overload after the first
(`ns.find(name)!=ns.end() → return`) — namespace_map is one Variable per name, no
overload sets. libstdc++ declares ~6 getline overloads (2-param + 3-param, lvalue +
rvalue&&, + char/wchar_t specs). FIX = free-function overload sets: register all
overloads + select by arity/arg-types at the call site (model on class-method
`findMethodOverload`, parser.cpp ~6336). HIGH blast radius (namespace-function
resolution used everywhere) → gate fulltest + torture ALONE every iteration.
Then RC#2: free-function-template call lowering mishandles reference args/temps
(`__madc_objtmp_N` undeclared / lvalue-`&` errors). Reducer: tmp/loop_real.mad.

testfstream also needs its non-standard bits rewritten to standard C++
(`to_string(s,42)`→`std::to_string`, `strlen(string)`/`system(string)`→`.c_str()`).
testlargesizeofquery is a SEPARATE track (uint32→64 array-dim widening; niche/risky).
