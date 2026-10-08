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
6. **Class shells by pattern — the lane is not cheaper; measured
   2026-10-08.** Two lanes build a class-template specialization
   (`parser.cpp`, the lane choice after `materialize_class_pattern`): the
   PATTERN lane captures a type-only `ClassPattern` once per template (on
   its second use) and serves each specialization by substitution
   (`instantiate_basic_class_pattern`); the PARSE lane clones the template
   body's tokens with the arguments spliced in and runs the class parser
   over them. testsubscript: 229 pattern / 282 parse. The 282 by reason:
   `dependent-value-expression` 163 (35 templates — non-type parameters
   and value-dependent bases: `enable_if`, `integral_constant`-derived
   traits, `_PCC`, `pair`), `pattern-parse-error` 78 (`__and_` ×53,
   `__or_` ×16 — variadic), `pattern-not-captured` 23 (first use, by
   policy), `unsupported-friend-definition` 8, `unnormalizable-type` 8,
   `requires-eager-body-parse` 2. BUT forcing every class onto the parse
   lane (`MADC_CLASS_PATTERN_FORCE_LEGACY=1`) moves all 229 and costs
   +0.11% instructions (3,323.3M → 3,326.9M): the pattern lane, capture
   included, is no cheaper than a re-parse. Widening it would gain ~0.
   The cost of a class instantiation is the per-member work BOTH lanes do
   (resolving member types, registering members, minting names and
   DataDefs, the nested instantiations that triggers) — not reading the
   body's tokens. That is what has to get cheaper (step 4, and the
   allocation churn), measured per lane before any design.
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

## Function-template partial ordering — DONE (2026-10-08)

Not in the June–July plans; found by the same profile.
`fn_templates_most_specialized_first` orders a name's function templates for
every call, and each comparison (`po_at_least_specialized`) re-extracted BOTH
templates' parameter signatures from their declaration tokens
(`po_param_spellings`, 10.1% of the forest compile; 6,647 comparisons on
testsubscript). The signature is a pure function of the declaration's tokens,
which are permanent, so `Program::fn_template_ordering_signatures` holds it
once per declaration (keyed by the token-pointer sequence; split into words
once). Parse-cost gate: `subscript.forest` −11.49%, `cxx_stl.forest` −9.97%,
`subscript.live` −4.44%, `cxx_stl.live` −2.53%, C and dialect rows flat.

## Interned names above the token layer (owner 2026-10-08: "replace std::string with the interned strings all over the place")

### Why
The June–July interning (rung 1) put `madc::dis::intern_table` under token
spellings and the bare-name lookup maps. Everything above that — `DataDef::name`,
`Variable::name`, `FuncDef`'s `emit_symbol` / `local_emit_name` / display and
namespace names, emit names, instantiation keys, substitution-map keys, the
struct registry — is `std::string`. Profile (testsubscript, forest, -O2): 1.5M
heap strings per compile, **831k of them copies of an existing string**
(`_M_construct<char*>`), 186k built from `const char*`; allocation ≈ 17% self,
`memcpy` 4.6%, `memcmp` 3.5%. `include/datadef.h` holds 86 `std::string` uses
and zero intern ids.

### The type: `madc::dis::istring` (include/madcdis/istring.h)
- A handle to ONE permanent, process-wide, deduplicated `std::string`: it holds
  `const std::string *` to a heap entry the table never frees; the table is an
  `std::unordered_set` of byte-view keys pointing at those entries, so a lookup
  from `const char *` + length builds no temporary string.
- Copy = 8 bytes. `==` / `!=` = pointer compare. Hash = the pointer.
- `operator const std::string &()` and `str()` return the interned string BY
  REFERENCE — every read-only use of a `std::string` field (`.c_str()`,
  `.size()`, `.find()`, `+`, passing as `const std::string &`) compiles
  unchanged and copies nothing. A MUTATING use (`+=`, `append`, `[i] =`) fails
  to compile and is rewritten as an assignment of the new value.
- Implicit construction from `const char *` / `const std::string &` interns
  (lookup; insert only if new), so `fd->emit_symbol = some_string;` is unchanged.
- `operator<` compares the BYTES, so a `std::map<istring, X>` iterates in the
  same order as the `std::map<std::string, X>` it replaces — emission order,
  diagnostics and every ordered walk are behaviour-preserving. A map whose order
  is unobserved moves to `std::unordered_map` keyed by the pointer, case by case.
- The empty string is a static entry; a default-constructed `istring` is empty.

### Why process-wide, not the per-Program `strpool`
Built-in `DataDef`s (`ddINT`, ...) are static and shared by every Program, and
DataDefs, Variables and FuncDefs cross Program boundaries (eval contexts, REPL
sessions, the forest's restored state). A per-Program id would compare wrong
across them — the rung-1 hand-off already kept `TokenDataType`'s spelling out of
the per-Program pool for exactly this reason. GCC's identifier table is likewise
one table for the compilation.

### Thread-safety contract (thread-safety.md)
Entries are IMMUTABLE once interned and never freed, so reading an `istring`
(compare, hash, `c_str`, the `std::string` reference) is safe from any thread
without synchronization. `intern` (construction from bytes) is synchronized
inside the table (one mutex around lookup-and-insert). An `istring` is a value:
operations on distinct objects are safe; a shared mutable `istring` variable
follows the C++ stdlib convention. No other state. Memory: bounded by the
number of distinct spellings ever interned (the same lifetime as the token
arena's permanent tokens).

### Migration — done as ONE flip (owner: "make a list of what _needs_ to be an std::string")
Every `std::string` in the compiler core (lexer, parser, CIR builder/dump/format/
emit, madc_cir, cir_freeze, type spelling, mangler, completion, keywords, pch,
and their headers) became `madc::dis::istring`; the compiler then named every
site that mutates, and only those went back. The keep-list — what stays
`std::string`, by kind:

| Kind | Why it is not a name |
|---|---|
| Text BUILDERS (`+=`, `append`, `push_back`, `erase`, `&s[0]`) | edited in place; interned once complete when stored as a name |
| Diagnostic / dump / emitted-C text (`why`, `err`, `msg` out-params, `std::string(x) + …` temporaries) | output text, built and printed once |
| Source text (`Source::_buf`, pushback, macro args/body, comments, `TokenStr::str`, `TokenREM::str`, include text) | the program's bytes, not identifiers |
| libmadc public API (`madc_program.cpp`, `madc_session.cpp`, the `void *` eval bridge in parser.cpp, `madc::value` object keys, the registration-policy mirror) | the C++ API boundary carries `std::string` (cpp-first-api.md) |
| Paths and link / CLI lists (`canonical_path_for_compare`, `madc_self_*_path`, DT_NEEDED / `-l` / `-I` / `-D` lists, runpath, embedded-header table) | OS-boundary text, used once per compile |
| `std::getline` targets, `_trailing_trivia`, completion `entry_rows_shown` | stream / UI text |
| Text PROCESSORS: the spelling splitter (`spelling_delim.h`), the mangler's internals (`madc_mangle.cpp` below its `madc_mangle.h` interface), scanner words, macro-expansion slices, literal / comment token factories | transient slices of text; a piece is interned only where it is KEPT as a name (`split_scope_names`, the name-shaped `split_template_id_parts`, the mangler's exported returns) |

Hazards the flip exposed — each now a gate check (below):
- A `void *` that carries a `std::string *` across the eval bridge, cast back
  as `istring *`, compiles and reads garbage. Bridge casts name `std::string`.
- A `const istring &` DATA MEMBER bound from a `std::string` argument binds a
  temporary that dies at the end of the constructor's full-expression
  (`InstLiveGuard` crashed this way). Hold an `istring` by value — it is a pointer.
- A function returning `const istring &` that may return its `const istring &`
  PARAMETER dangles when called with a `std::string`. Return `istring` by value.
- `istring(x.str())` on an istring `x` re-interns for nothing; `istring(a) + b`
  interns a temporary that is only concatenated. Both swept.
- A function or lambda that stores the ADDRESS of its `const istring &`
  parameter (`n.definition = &body;`) keeps a pointer to a temporary when the
  caller passed a `std::string` (the REPL's `?NAME` read freed memory this way
  once `define_map` held text again).
### Cost: intern ONCE, at the source — measured (parse-cost gate, Ir vs the pre-flip baseline)
The first flip changed the types but left the hot paths converting text to an
istring at every USE (1.5M interns on `c_headers`, ~340 Ir each: byte hash +
mutex + probe). It went RED on every workload (+6% … +35%). What turned it:

| Step | c_headers.live | cxx_stl.live | subscript.live |
|---|---|---|---|
| flip as-was | +34.6% | +19.0% | +17.0% |
| 8-byte-word hash (was FNV-1a per byte) | +24.4% | +13.1% | +10.6% |
| + hold, pool memo, lexer `wname`, macro-body idents | −2.9% | −2.6% | −3.2% |
| + text processors back to std::string | **−3.9%** | **−5.1%** | **−5.3%** |

The mechanisms:
- `istring_table::hold` — the CLI's tokenize + parse take the table's mutex
  ONCE; `intern` skips the per-call lock on the owning thread (an atomic owner
  id, not TLS: a `__tls_get_addr` in libmadc.so costs what the lock did).
  Released before the program runs (its threads may compile through eval).
- `intern_table::name(id)` — the per-Program token pool memoizes each
  spelling's istring, so `TokenIdent::spelling_name()` is a memo read: the
  process table sees each distinct spelling once per pool, not once per use.
- The lexer takes the identifier's name once (`wname`) and passes it to every
  probe; `make_ident(spelling_id)` stops re-pooling the spelling.
- A macro body's identifiers are interned once with its cached replacement
  tokens; parameter matches compare pointers; argument text stays std::string.
- `MADC_ISTRING_LITERAL("int")` — a literal handed to an istring parameter on
  a hot path interns once per call site.
- Next (when a profile names the site): a NON-inserting `istring_table::find`
  for probes of text that may never have been a name — a miss proves absence
  from every istring-keyed container without growing the table.

A gate (`scripts/check-istring-fields.sh`, fulltest) then fails any NEW
`std::string` field holding a name in the migrated structs, so the migration
cannot regrow.

### Second sweep: text that still reached the table, and headers read once
An audit of the table's contents after `cxx_stl` (every entry, longest first)
showed TEXT interned through `const istring &` parameters and returns —
the conversion is implicit, so it hid at the call: whole embedded headers
(`Source::str`), every object-like macro body (`define_map`'s value type),
every function-like macro expansion, `#if` conditions, macro-argument
pre-expansion, the `-dM` dump, error and log text, and the preprocessor's
per-POSITION literal check (`pp_literal_start` took the whole text as an
istring: one hash of the text per character examined). All went back to
`std::string`. Table after `cxx_stl`: 28,620 entries / 1.84 MB → 26,102 /
1.69 MB; entries holding a newline 106 → 1.

| Change | Measured |
|---|---|
| Include guards decided in the ONE lex (gcc's multiple-include rules: `push_conditional` / `do_endif`), not a second read + line scan of every system header on its first visit; the first-visit probe reads the opening line only and hands its bytes to the lexer | 876 / 876 lex verdicts equal the full-text verdict (differential run over the gate workloads + include tests); `#else`/`#elif` on the guard group now disqualifies it, as in gcc |
| Include search memoized per Program (`search_include_dirs`, gcc's per-directory cache — hits only; `add_include_dir` and a flavor change drop it) and `realpath` per distinct path once | `resolve_include_path` was 2.45% of `cxx_stl` |
| Despaced class index: inserts filed incrementally (no full-map rescan + address set per top-up); rebuilds empty buckets in place | `topup_index_` was 3.05% of `cxx_stl` (121 full rebuilds) |
| Interned fast paths: trim / strip-namespace / despace return the input or a slice; one-character spellings from a 256-entry table; operator tokens intern their spelling once per class | the top conversion sites of a line-attributed profile |
| Hash tail: fixed-size loads (two overlapping 4-byte reads for 4..7 bytes, first/middle/last byte for 1..3) — never a variable-length `memcpy` call | most names are under 8 bytes |

Parse-cost gate, Ir vs the baseline committed before the interning work:
cxx_stl.live −14.0%, cxx_stl.forest −14.1%, c_headers.live −13.5%,
dialect.live −1.2%, subscript.live −14.3%, subscript.forest −14.9%.
