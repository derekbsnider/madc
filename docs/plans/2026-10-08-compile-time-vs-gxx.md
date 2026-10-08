# Compile time vs g++ — catching up (2026-10-08)

**Owner (2026-10-08):** "the whole idea of the pre-compiled headers was to
compile FASTER than gcc." The benchmark is `tests/testsubscript.mad` (55
lines: `<iostream>`, `<vector>`, `<map>`, `<string>`; vector and map
subscripts), apples to apples: g++ compile + link + run against madc
compile + run.

## Where it stands (build container, release -O2 binaries, 5 interleaved runs)

| Path | Wall | Note |
|---|---|---|
| `g++ -x c++` compile + link + run | 0.34 s | parses every header live |
| madc, forest (default) | 0.57 s | headers bound from the packed forest |
| madc, `--no-forest-bind` | 0.76 s | headers parsed live |

The forest saves only 0.19 s, because it saves the part that was already
cheap. Phase breakdown (`--show-stats`, in-process):

| Phase | Forest | Live |
|---|---|---|
| lex | 0.016 s | 0.100 s |
| parse | 0.138 s | 0.294 s |
| template instantiation (inside parse AND the CIR build) | 0.242 s, 7,756 calls | 0.287 s, 11,878 calls |
| CIR build (tree → `cir_node`) | 0.236 s | 0.251 s |
| c2mir | 0.008 s | 0.006 s |
| other / link | 0.048 s | 0.043 s |
| total | 0.543 s | 0.715 s |

Callgrind of the forest path (symbolized -O2, 3.45 G instructions):
- `CirJitSession::build` 57% inclusive; its deferred-instantiation lambda
  (`CirBuilder::translate_module`) is 40%;
- `CirBuilder::copy_cir_subtree` (tsubst copying a pattern subtree per
  instantiation) is 28% inclusive;
- `TokenCLASS::parse` under instantiation is 32%: 282 class bodies still
  re-parsed, against 229 instantiated from their pattern. The ranked
  reasons are in `--show-stats`, for example `std::__and_ ×53
  [pattern-parse-error]` and `dependent-value-expression` across the type
  traits;
- allocation churn is about 19% self (`malloc`/`free`/`new`), plus `memcpy`
  4.6% and `memcmp` 3.5%;
- `CirFrozenForest::unit_segment` is 1.9% self, which is a lookup that
  should be nearly free.

## Review — planned vs implemented vs undone vs today (June–July 2026 plans)

The front-end performance work ran from 2026-06-08 to 2026-07-09. The
June 23 materialize-from-AST design set the goal: lex + parse + CIR build
at or under `g++ -fsyntax-only`, and the forest to beat g++. Every item
below is matched to today's profile (forest path, testsubscript, -O2,
3.45 G instructions).

| Lever (plan) | Planned | Implemented | Left undone | Today's profile |
|---|---|---|---|---|
| O(n²) scans (`parser-lookahead-audit`, `header-partition-W2-perf`) | find and remove quadratic scans | 45× (`be3f800`), 7.1× (`b2c1c75`), `findVariable` O(1) (`10fcef1`) | the forward-scan worklist was never triaged | the CIR builder has the same class of bug: `pending_function_body_available` / `find_ast_function_body` rebuild `func_emit_name` per element, 152,762 calls, **4.2%**. No plan covers it (`emit-symbol-unification` was for correctness) |
| Lexer (`front-end-performance-plan` P1/P2, token arena) | arena tokens, flat-buffer lexer | done: lex −37%, token stream as slot ids (`e8861ac` … `c76e59a`, `1a26d38`) | — | lex 0.016 s with the forest. **Solved.** |
| Interning (`arena-interning-HANDOFF`, `rung1-*`) | intern once, reuse everywhere | spellings and every bare-name lookup map (`68d152b` … `d706c521`, `5739e624`): −3.3% instructions, hashing −64% | the instantiation identity, `struct_map`, `namespace_map`, canonical spellings and the mangler were never in scope; the July 1 close-out already listed instantiation (72% of parse) as left over | **about 19% allocation churn**: 1.5M heap `std::string`s, mostly instantiation keys, emit names and copies of names |
| Instantiation identity (`materialize-from-ast` T2/T4, `representation-refactor` P5, `forest-design`'s (node, env) memo) | key instantiations by (template, canonical environment), pre-hashed and checked first; substitute by index, not name | only the class-pattern resolver memo (`parser.cpp:8956`) | the string key (`registered_mangled`) and the per-call `std::map<string>` substitution maps remain; the map-strategy doc notes the string key fails to dedupe | part of instantiation's **0.242 s** and of the churn |
| Body patterns (two-tree phases 0–5, `tsubst-*`) | parse each body once; instantiate by tsubst | done and unconditional: v0.33.0 (`7907577e`), body re-parse deleted (`eef9264f`), 100% hit | — but the phase-3 hand-off MEASURED it as net-neutral at -O2 ("recipe-build overhead ≈ instantiation savings") and handed speed to the forest | `copy_cir_subtree` **28%**: tsubst always deep-copies |
| Cheaper tsubst copy (`materialize-from-ast` T3) | copy-if-changed, dependence short-circuit | not done | all of it; c2mir writes `node->attr` on every node, so the copy cannot be skipped outright, only made cheaper | the 28% above |
| Class bodies by pattern ("hybrid B", §11) | member bodies by tsubst; the class SHELL stays re-parsed (a deliberate choice) | the class-pattern lane exists and is on (`parser.cpp:11228`) | 282 class bodies still re-parsed vs 229 from pattern; reasons: `std::__and_ ×53 [pattern-parse-error]`, `dependent-value-expression` | `TokenCLASS::parse` under instantiation **32%** |
| Lazy / DCE instantiation (`lazy-member-body`, `system-header-dce`) | instantiate bodies only when used | done and live | W2 §5.1: "don't eagerly instantiate templates inside system headers" (an empty `<iostream>` made 796 instantiations; called "the biggest remaining perf lever") was never landed | 7,756 instantiation calls for a 55-line program |
| Forest (`embedded-header-forest-*`, closed 07-09 at v27) | skip header decl-parse (primary); frozen instantiations (the "secondary prize") | decl-parse skipped; instantiations that exist at freeze time are frozen (`cir_freeze.cpp:3160`) | instantiations a TU makes (`map<string,int>`, the traits over its types) are redone on every compile; the measured wins were on C/SMAUG (−17%), not C++ | lex + parse 0.15 s (was 0.39 s live), but instantiation + CIR build 0.48 s, unchanged |
| DataDef arena (`forest-arena-native-scoping`, B3) | DataDefs as flat records keyed by type id, names interned | write-through only (`f0b21e0e` … `5baf2c0f`) | the read flip; `forest_arena_enabled` is off by default | the heap DataDefs and their string names are still what every lookup reads |

**In short:** the plans aimed at the right targets. The lexer and
bare-name lookups were finished. The levers aimed at instantiation (T2,
T3, T4, the (node, env) memo, frozen TU instantiations, fewer eager
instantiations in system headers) were designed but not landed. The
two-tree work that did land was built for correctness and measured
net-neutral for speed, and that's where today's time goes. One hot spot
has no plan at all: the CIR builder's unindexed emit-name scans.

## Plan — ranked by measured share; each step lands with the parse-cost gate

Already landed this session (parse-cost gate, `cxx_stl.live`): speculative
diagnostics no longer rendered (−1.62%), and the lexer's specifier check
table-driven (−1.10%).

1. **Benchmark first. DONE.** `testsubscript` is a parse-cost workload
   (`subscript.live`, `subscript.forest`; `scripts/parse_cost/subscript.*`
   link the test), and `scripts/perf_vs_gcc.sh <file> --pipeline` times
   g++ compile + link + run against madc on the forest and on
   `--no-forest-bind`, interleaved, median of N, every run's rc + stdout
   checked against the g++-built program. Every step below reports both.
2. **Index the CIR builder's function lookups. DONE.** One owner,
   `AstFunctionBodies` (`src/cir_builder.cpp`): the first bodied function in
   `prog->ast` whose source or emit name matches, served from one shared
   forward scan. The materialize fixpoint's `funcdef_map` loop holds one per
   pass (it never parses), replacing 678 full rescans. The scan builds the
   same emit names in the same order as before (`func_emit_name` can mint a
   flavor thunk). Parse-cost gate: `subscript.live` −8.26%,
   `subscript.forest` −3.80%, `cxx_stl.live` −3.41%, `cxx_stl.forest`
   −1.00%, C and dialect rows flat. Pipeline after it (median of 5): g++
   0.344 s, madc forest 0.564 s (1.64×), live 0.739 s (2.15×).
3. **`CirFrozenForest::unit_segment`** (1.9% self, 4.1% inclusive) is not a
   lookup: it is the forest's UNIT LOAD. The first node touched in a header's
   unit decompresses that unit's whole record set and validates every record
   and child link (39 calls on testsubscript; 1,299 zstd frames, 71.8 MB
   decoded; an empty `main` with `<iostream>` + `<string>` decodes 18.3 MB).
   The self cost is the whole-unit validation loop. The lever is loading
   less than a whole unit (finer-grained segments, or validation moved to
   pack time behind a checksum) — a forest-format design, owner ruling
   before code. Folds into the forest-load item under step 8.
4. **Instantiation identity, T4/T2 as designed** (the churn and part of
   the 0.242 s): key by (template id, interned canonical arguments),
   pre-hashed and checked BEFORE any key spelling is built; substitution
   maps keyed by parameter index. Core parser — its own focused session.
5. **Cheaper tsubst copy, T3 — DEMOTED by measurement (2026-10-08).**
   `copy_cir_subtree`'s 28% is inclusive, and most of it is not copying: the
   copy re-resolves each dependent call (`resolve_copied_dependent_call`,
   9.1% + 2.9% recursive; `tsubst_relower_deferred_construction` 5.9%),
   which calls back into the parser's instantiation
   (`instantiate_namespace_fn_template_for_call` 15.8%,
   `instantiate_member_fn_template_for_call` 5.9%). The copy's own work is
   outside the top 30 by self cost. Copy-if-changed would save little; the
   cost belongs to steps 4 and 6 (what an instantiation costs), and to the
   allocation churn spread across them (malloc/free/`std::string`
   construction ≈ 17% self).
6. **Class shells by pattern** (32%): widen the class-pattern lane per KIND
   (parse-once rule), starting with `pattern-parse-error` on the variadic
   `__and_` / `__or_`, then `dependent-value-expression`.
7. **Fewer eager instantiations in system headers** (W2 §5.1, never
   landed): a LIVE-path lever only. With the forest, everything the headers
   instantiate by being parsed is already frozen (measured below), so this
   step is weighed against `--no-forest-bind` and the C++ lanes that parse
   headers live.
8. **No hand-picked frozen instantiations (owner ruling, 2026-10-08).**
   Whatever the forest freezes is GENERIC: the instantiations the system
   headers make by being parsed, which benefit every program that includes
   them. A chosen set of program-shaped instantiations (`map<string,int>`,
   `vector<int>`) is out: it serves specific use cases, not most users. The
   program's own instantiations get cheaper through steps 4–6 instead.
   Measured 2026-10-08 (`--show-stats`, -O2 release): an empty `main` with
   `<iostream>` + `<string>` makes 3,573 instantiation calls and builds 213
   classes by parse when the headers are parsed live, against 215 calls
   (0.001 s) and 0 classes from the forest — the generic freeze works.
   `std::string` + `cout << s` also builds 0 classes (0.145 s total, against
   0.102 s for the empty `main`). The cost is the program's own template
   arguments: `vector<int>` alone 962 calls / 62 classes built (0.118 s
   total); `map<string,int>` alone 3,414 calls / 254 classes built, 153 of
   them by re-parse (0.323 s total). testsubscript uses `map<string,int>`
   and `map<string,string>`, so the two maps are most of its 0.58 s. The
   empty program's remaining 0.102 s is the forest load itself (restore
   0.039 s, decode 0.018 s): a separate item, after steps 2–6.

Target: the forest path under g++'s 0.34 s on testsubscript, with no growth
on the live path.

## Thread-safety

Measurement and caching changes stay per-Program; any cache added in steps
2–8 is per-Program state or immutable forest data (C++ stdlib convention,
`.claude/rules/thread-safety.md`).
