# REHYDRATION HANDOFF — real-header track + struct/class unification directive

## ★ FULL HANDOFF (READ FIRST) — 2026-06-07, compaction checkpoint ★

**Branch** `feature/realhdr-parse-gaps2-claude` (off `develop`, **local only, NOT
pushed**; `develop` untouched). **HEAD `e63e639`**, working tree **clean**.
**fulltest baseline 528 / 4 / 0 / 26** — the 4 reds are PRE-EXISTING/unrelated
(`testdefer`, `testfstream`, `testlargesizeofquery`, `testloop`). Do NOT push;
do NOT promote develop→master (parity gate).

**Mission:** make madc parse REAL system C++/libc headers (toward C23/C++23),
through the one `cir_node`/MC11-IR → c2mir → MIR backend. Immediate goal: the
**string / iostream / sstream / fstream** family.

**THE BIG FINDING:** madc's embedded stub headers in `include/madc/`
(string/vector/map/set/algorithm/streams) **shadow the real system headers** (the
`#include <...>` path checks embedded before the filesystem) and break their
include chains — they are an ACTIVE blocker, strongly motivating the
retire-std-hardcoding campaign. New diagnostic **`--no-embedded-headers`**
(commit `f71a472`, reuses the existing `registration_policy`/
`is_embedded_header_allowed()` gate; a disallowed embedded header falls through
to the real filesystem header) makes madc use real headers so the track can
proceed.

**This session's commits (all gated: fulltest 528/4/0/26 · gcc.c-torture failset
IDENTICAL 88, 0 regr · SMAUG boots clean):**
- `d1f28f4` nested method-bearing struct + trailing declarator (STRUCT body)
- `97eeeeb` …same (CLASS body)  · `840eb38` restore nested fwd-decl regressed by 97eeeeb
- `fcca0a3` `-E` char-literal reconstruction (instrument fix)
- `1f8e3fd` **`madc -E`** (preprocess-only) + `docs/plans/2026-06-07-parser-pp-architecture-research.md`
- `2e853de` `decltype(<)` in trailing return (was consumed to EOF)
- `f71a472` `--no-embedded-headers` · `1cc70f0` recursive-macro loop (`#define A A`)
- `a2f387c` macro-expansion depth-guard backstop
- `be5a08f` multi-line `/* */` comment on a directive line
- `de24e23` `#` stringize of an empty macro arg → `""`

**METHODOLOGY (proven this session):** `-E` bisection — `bin/madc --std=c++17
[--no-embedded-headers] -E probe.mad > pp.cpp` then `bin/madc --std=c++17
--emit=c11 pp.cpp` gives the REAL derail line (the live-include path reports bogus
`.mad` coords). Reduce to a one-liner → confirm gcc AND clang accept it → fix the
DEEPEST layer (reuse existing lexer/parser machinery, don't reimplement) → re-gate
(fulltest + torture failset-diff + SMAUG soak). USER PRINCIPLES re-affirmed:
reuse existing mechanisms FIRST; the parser already knows comments/etc. — route
through it; get the parser working now, optimize later.

**REAL-HEADER FRONTIER (with `--no-embedded-headers`):** `type_traits`/`utility`
parse fully. The streams + string advanced through six root causes —
decltype-`<` → stub-shadowing → recursive-macro-loop → multi-line-comment →
`__need_XXX`/asm-label(empty-arg `#`) → **NOW: template instantiation**.

**NEXT TARGET (deeper area — fresh start):** all 4 streams + `string` derail
instantiating `basic_string_view<char>` → `parser.cpp:2644` "Expecting a type
argument to **iterator**<>". `basic_string_view` has `using iterator =
const_iterator;` (a member TYPE ALIAS, not a template); madc's template-id parser
(it assumes `identifier <` = template-id and demands type args) misparses a
bareword `iterator <…>`. Standalone `iterator<tag,char>` and `S<wchar_t>` parse
fine → specific to the real instantiation = **template-id-vs-type-alias
disambiguation** (needs name-lookup before assuming `<` opens a template-id).
**RESEARCH DONE + BUG SITE VERIFIED — see `docs/plans/2026-06-07-template-id-disambiguation-research.md`.**
Clang does name-lookup FIRST: `identifier <` is a template-id ONLY if the name
resolves to a TEMPLATE (`isTemplateName`/`getAsTemplateNameDecl` → non-null only
for `isa<TemplateDecl>`); a typedef/alias → `annot_typename`, never the template
path. **madc bug (VERIFIED): `parser.cpp:2935–2942`** in
`resolve_declared_type_token` — an already-resolved type token (`ttDataType`)
followed by `<` is re-dispatched to `instantiate_template_id(spelling)` →
`find_template(spelling)` (flat spelling lookup), so a member alias `iterator`
gets hijacked by the same-spelled `std::iterator` template. **Minimal fix:** don't
template-instantiate an already-resolved `ttDataType` whose binding is a
non-template alias; make `find_template` scope/binding-aware (spelling-only
dispatch belongs only on the unresolved-identifier path ~2999). Reducer +
fix detail in the research doc. Go test-gated.

**Verify on resume:** `git rev-parse --short HEAD` (e63e639) · `make -C src` (clean)
· `make -C src fulltest` (528/4) · `bash scripts/probe_real_headers.sh` (§5 frontier)
· torture diff vs parent for any parser/cir change · SMAUG soak
(`cd /workspace/MadSMAUG && MADC=/workspace/madc/bin/madc MADC_CPU_LIMIT=0
MADC_MEM_LIMIT=0 timeout 600 ./MadSMAUG.sh <port>` → "ready … port", 0 errors).
Memory index: `project_parser_pp_architecture` (this track) +
`project_struct_is_class`. Detailed per-fix history is in the UPDATE blocks below
(NEWEST-4 … NEWEST).

---

# REHYDRATION HANDOFF — real-header track + struct/class unification directive

Date: **2026-06-07** (late session). Branch **`feature/realhdr-parse-gaps2-claude`**
(off `develop`, **local only, NOT pushed**). HEAD **`bb829ed`**. Working tree
**clean**. `parser.cpp` = **24,383 lines**. fulltest baseline: **526 passed /
4 failed / 0 timed out / 26 skipped** (the 4 reds are PRE-EXISTING and unrelated —
`testdefer`, `testfstream`, `testlargesizeofquery`, `testloop`).

> This is a **self-contained** rehydration document. After reading it you can
> resume without re-deriving anything. It supersedes
> `docs/plans/2026-06-07-parser-restoration-HANDOFF.md` (that one covers the
> earlier parser-restoration sub-thread) for *current* state. The one-screen
> index is the memory file `project_parser_restoration.md`; the struct/class
> design directive lives in `project_struct_is_class.md`.

---

## UPDATE 2026-06-07 (NEWEST-4) — real-header preprocessor frontier: 5 fixes, streams now reach template instantiation

Continued the `--no-embedded-headers` (real system headers) push. Each blocker
was a GENERAL lexer/PP/parser bug, found by `-E` bisection + gcc/clang oracle,
each its own validated commit (fulltest 528/4/0/26 · gcc.c-torture IDENTICAL 88,
0 regr · SMAUG boots clean). Commit chain this block:
`2e853de` decltype-`<` · `f71a472` `--no-embedded-headers` · `1cc70f0`
recursive-macro loop · `a2f387c` depth guard · `be5a08f` multi-line comment ·
`de24e23` empty-arg stringize.

- **`be5a08f`** — multi-line `/* */` comment opening on a directive line
  (`#endif /* … \n … */` in real `<stddef.h>`, the `__need_XXX` symptom). The
  directive handlers char-scanned to the newline, bypassing the lexer's `case
  '/'` comment handling. Fix: `consume_directive_line_tail()` runs the active
  directive tail THROUGH the lexer (loop `getToken()` to the EOL token, reusing
  `case '/'`); `skipConditionalBlock` keeps a char-level skip since inactive
  branches must NOT be tokenized/expanded. Deduped ~11 hand-rolled loops.
- **`de24e23`** — `#` (stringize) of an EMPTY macro arg gave a literal `#` not
  `""`. A function-like macro called with fewer args than params left the
  missing params unbound. Hit glibc's `__asm__(__ASMNAME("n"))` →
  `__STRING(__USER_LABEL_PREFIX__)` (prefix empty → inner `#` sees no arg) →
  `__asm__(# "n")` → "Expecting ')' after asm label". Fix: bind missing params
  to "" (C semantics).

### NEXT blocker — template instantiation (`iterator<>`), a DEEPER area
All 4 streams + string now derail parsing **`basic_string_view<char>`**:
`parser.cpp:2644` "Expecting a type argument to **iterator**<>". `basic_string_view`
has `using iterator = const_iterator;` (a member TYPE ALIAS, not a template) —
madc appears to misparse a bareword `iterator <…>` as a template-id during the
string_view-region instantiation, and the arg fails to resolve. Standalone
`iterator<tag,char>` and `S<wchar_t>` both parse fine, so it's specific to the
real instantiation (template-id-vs-type-alias disambiguation / template
instantiation fidelity — cf. [[project_template_instantiation]]). This is a
bigger fix than the lexer/PP ones above; start fresh here. Reduce from
`basic_string_view`'s member-alias + `std::reverse_iterator<const_iterator>`
usage (lines ~18548-18550 of the real `<string_view>`/`<bits/...>`).

Frontier summary (real headers, `--no-embedded-headers`): decltype-`<` →
stub-shadowing → macro loop → multi-line comment → `__need_XXX`/asm-label →
**now `iterator<>` template instantiation**. type_traits/utility parse fully.

---

## UPDATE 2026-06-07 (NEWEST-3) — decltype-`<` fixed; embedded stubs shadow real headers; recursive-macro loop fixed + depth guard

HEAD progresses `2e853de` → `f71a472` → `1cc70f0` (+ depth-guard commit pending).
Key shift: **the embedded stub headers in `include/madc/` shadow the real system
headers and break their include chains** — confirmed by a new diagnostic flag.

- **`2e853de`** — the `decltype(<)` streams-gate fix (a `<` in a trailing-return
  `decltype` was parsed as a template-id open → consumed to EOF in
  `skip_template_nonclass_declaration`; now only tracks angles at paren/square
  depth 0). `stl_function.h` parses standalone; `std::less<void>` shape parses.
- **`f71a472`** — **`--no-embedded-headers`** diagnostic flag. Reuses the EXISTING
  gate (`registration_policy.restrict_headers_to_allowlist` +
  `is_embedded_header_allowed()`), NOT a new switch; a disallowed embedded header
  now FALLS THROUGH to the real filesystem header instead of throwing. (Process
  lesson: reuse existing mechanisms FIRST — I initially added a parallel bool, reverted.)
- **`1cc70f0`** — **recursive-macro infinite loop FIXED.** `#define A A` (and
  mutual `A`<->`B`) recursed forever → SIGSEGV. Root cause: `Source::get()`
  popped a macro's blue-paint pushback frame the instant its last char was
  consumed, BEFORE the re-lexed token's `macro_disabled()` check, so a
  single-token expansion lost its paint. Fix: delayed-pop + clear stale frames
  when pushback drains. fulltest 528/4/0/26, torture identical, SMAUG boots
  clean, chained expansion intact (`A->B->42`). Hidden because shallow stubs
  never exercised it; real headers do.
- **depth guard (pending)** — `Source::pushback_depth()` + a `>4096` backstop at
  the top of `_getToken` turns any FUTURE runaway expansion into a clean
  diagnostic instead of a stack crash (per user suggestion).

### Embedded-stub finding + NEXT blockers
With `--no-embedded-headers` + the loop fix, the WHOLE target set gets far past
the old stub blockers. Current real-header frontier:
- **`__need_XXX`** (all 5 streams + string): `463:16 use of undeclared identifier
  '__need_XXX'`. ROOT (found): it's inside a **multi-line `/* */` comment** in
  real `stddef.h` (`#endif /* … \n || __need_XXX was not defined before */`) —
  madc lexes the comment TEXT as code. **A multi-line block-comment lexer bug.**
  NEXT TARGET.
- **`memory`** → `reinterpret_cast target is not a type` (separate).
- `vector`/`map`/`set`/`algorithm` — secondary.

Retire-std-hardcoding (delete the stubs, use real headers) is now strongly
indicated: the stubs are an ACTIVE blocker, not just tech debt.

---

## UPDATE 2026-06-07 (NEWEST-2) — streams gate ISOLATED to a one-liner: `<` in a trailing-return `decltype`

HEAD now **`840eb38`**. Two more fixes + the streams gate pinned to a minimal
reducer. All commits validated (fulltest 528/4/0/26; gcc.c-torture failset
IDENTICAL = 88, 0 regressions).

- **`fcca0a3` — `-E` char-literal reconstruction.** `token_spelling()` had no
  `ttChar` case → char literals reconstructed as EMPTY, so `-E`/`--dump-source`
  dropped `'\0'`/`'\x7f'` etc. This had **contaminated my own bisection** (a
  phantom failure in glibc's `btowc` inline). Char literals parse fine LIVE
  (verified vs g++); purely a reconstruction gap. **The `-E` instrument is now
  faithful** — trust it for reparse bisection.
- **`840eb38` — restore nested forward declaration** (`class L { class facet; };`),
  a LATENT REGRESSION from `97eeeeb`: the definition-only+re-feed path wrongly
  applied to the `;` forward-decl case (TokenCLASS already consumes the `;`, so
  re-feed mis-fired). Tests/torture didn't cover nested fwd decls. Fixed by
  splitting the `;` case onto the original simple parse path; definition-only +
  re-feed now only for `{`/`:`. **Lesson: a green fulltest+torture does NOT cover
  nested-C++-class edge cases — reduce + check gcc/clang for those by hand.**

### THE STREAMS GATE (precisely isolated — surgical next target)
All 5 streams fail at `use of undeclared identifier 'less'` because
`<bits/stl_function.h>` derails with **"Unexpected end of data"**, so `std::less`
never registers. Reduced to ONE line (gcc/clang accept; madc fails):
```cpp
struct X { template<typename A,typename B> auto f(A a,B b) const -> decltype(a<b) { return a<b; } };
```
**Root cause:** a `<` inside a `decltype(...)` in a **trailing-return-type**
position is parsed as a template-argument-list opener → the parser consumes to
EOF. Proof by bisection: `-> decltype(a+b)` OK, `-> decltype(a>b)` OK, a
STANDALONE `decltype(a<b);` stmt OK — **only `-> decltype(a< … )` fails.** This is
research **lesson #5** (the `<` angle-bracket ambiguity; madc lacks a
`greater_than_is_operator_p`-style guard in trailing-return/type context). FIX
LOCATION: the trailing-return `->` type parse must parse the `decltype(...)`
operand as an EXPRESSION (where `<` is less-than), not via a type/template-id
balancer. The general decltype-as-type resolver is `parser.cpp:2975`
(`is_decltype_identifier`); find where the method trailing-return (`->`,
`TokenDeRef`) collects its type and route its `decltype` through the expression
path. Fixing this unblocks `less` → likely all 5 streams at once.

### Other target-header gates (independent of the above)
- **`string` 42:9** "Expecting type in class definition" — not yet bisected.
- **`<memory>` 2019** `template<...> struct array;` — `array` is a hardcoded
  builtin token (`TokenARRAY`, the lone std-name straggler in `datatokens.h`);
  retire-std-hardcoding-campaign work. `<memory>` is a transitive include but
  the streams' direct gate is `less`, so do `decltype(<)` first.

---

## UPDATE 2026-06-07 (NEWEST) — `madc -E`, PP/parser RESEARCH, NESTED-STRUCT-METHOD fixes; `<memory>` now hits the template/std-hardcoding layer

Branch HEAD now **`97eeeeb`** (`feature/realhdr-parse-gaps2-claude`, local, NOT
pushed; `develop` untouched). Mission: get the **string / iostream / sstream /
fstream** family parsing (en route to "parse anything clang can"). Worked the
real-header track via a new instrument + `-E`-bisection of `<memory>` (a
prerequisite the streams pull in).

### Shipped this block
1. **`madc -E`** (commit `1f8e3fd`, `src/madc.cpp`) — preprocess-only dump
   (expand `#include`/`#define`/macros, print token stream, stop). Reuses the
   `dump_source` path (madc preprocesses during `tokenize()`). Content-only for
   clean diff vs `gcc -E | grep -v '^#'`. **This is the bisection instrument.**
2. **PP/parser architecture research** (`1f8e3fd`,
   `docs/plans/2026-06-07-parser-pp-architecture-research.md`) — source-grounded
   study of gcc(libcpp+cp)/clang/c2mir. 5 convergent lessons; the big one is
   **recursive declarators** (one node per `*`/`[]`/`()`) as the structural fix
   for the multi-star/fnptr-decay bug class. Also: struct≡class confirmed; the
   multiarch include path was a FALSE alarm (already in the generated sys list —
   my earlier "230→53" conflated a non-`--std` run).
3. **fix `d1f28f4` — nested method-bearing struct + trailing declarator
   (STRUCT body).** `struct Outer { struct Inner { void f(){} } member; };`
   failed; gcc/clang accept it. The struct-body parser routed the nested struct
   to the data-only `parse_nested_aggregate_body` (no methods). Fix: when the
   nested body needs the class parser, delegate the DEFINITION to TokenCLASS in
   a new **definition-only mode** (`Program::class_definition_only`, returns at
   `}` without consuming a trailing declarator); the outer member loop claims
   the declarator. No parallel implementation.
4. **fix `97eeeeb` — same, CLASS body** (the real `<memory>` blocker:
   `struct __uses_alloc0 : __uses_alloc_base { struct _Sink {...} _M_a; }`).
   When the outer aggregate has a base/methods it's the CLASS parser; its
   nested-type path delegated+`continue`d, dropping the trailing declarator
   (even data-only). Fix: definition-only parse (TokenSTRUCT now honors the
   flag too), then **re-feed the tag as a bare type name** so the normal member
   path reads `Inner m;`. Reuses the one member path.

**Validation (both fixes):** fulltest **528/4/0/26** (the 4 known reds); full
gcc.c-torture failset **IDENTICAL** to parent (88 FAIL, 0 regressions — the
fixes are gated by `cpp_struct_body_needs_class_parser` which is false in C
mode, so C/torture/SMAUG can't route). SMAUG soak = final confirmation
(C89-unaffected by construction).

### `<memory>` progress + NEXT blockers (bisect with `-E` then reparse for the REAL line)
`<memory>` advanced past ALL nested-struct gaps; now derails at
`template<typename _Tp, size_t _Nm> struct array;` →
**`Expecting template class name`**. ROOT CAUSE (reduced): **`array` is a
hardcoded builtin type token** — `TokenARRAY tkARRAY` (`datatokens.h:48`,
`TokenDataType("array", ddARRAY)`), registered in `datatype_map`
(`lexer.cpp:1854`) + `madc_ns["array"]` (`parser.cpp:7081`). So `struct array`
lexes `array` as a keyword, not an identifier. This is **legacy
std-hardcoding** (same class as `dtSTRING`/`tkVECTOR`), NOT a template-parser
bug — it belongs to the **retire-std-hardcoding campaign**, and is the next
target. (A bare `template<...> struct arr;` forward decl parses fine.)

Other known gaps found while bisecting (gcc/clang accept all; not yet fixed):
- **Chained method calls** `o.member.set(42)` → "chained method call not yet
  supported" (codegen gap, separate).
- **`Outer::Inner` qualified nested-type name** → "'Inner' is not a static
  member of 'Outer'" (nested-type name resolution; bare `Inner` member works).
- **`<cstddef>` `max_align_t`** → "not a declaration in '::'" (declarable, like
  the Bucket-B libc decls).
- **Deferred (§6):** the two ~2300-line body parsers (TokenSTRUCT vs TokenCLASS)
  still duplicate nested-aggregate handling — these two fixes patched both;
  full unification remains the right long-term move.

`-E` bisection recipe: `bin/madc --std=c++17 -E probe.mad > pp.cpp` then
`bin/madc --std=c++17 --emit=c11 pp.cpp` gives the REAL line (the live-include
path reports bogus `.mad` coords). Reduce → confirm gcc+clang accept → fix
deepest layer → re-bisect.

---

## UPDATE 2026-06-07 (latest) — STRUCT METHODS + SMAUG C89 WARNING AUDIT DONE

Branch HEAD now **`f52c3ad`** (`feature/realhdr-parse-gaps2-claude`, **local, NOT
pushed**; `develop` untouched). Two arcs landed this session, all test-gated.
**Live state:** fulltest **528/4/0/26** (4 known pre-existing reds); **FULL
gcc.c-torture failset BYTE-IDENTICAL to the pre-session parent** (1566 passed, 89
fail — I ran BOTH the parent `98c9a20` and the final build full and `comm`-diffed:
**0 regressions** across every commit); **SMAUG boots clean** to "Realms of Despair
ready … on port 4023", **0 errors**.

### Arc 1 — struct member methods (job 1 of §6); commits `2dbfa8e`, `952d6d2`, `5939707`
A `struct` body now parses member functions/operators/ctors/dtors/access labels.
Mechanism: **widened `cpp_struct_body_needs_class_parser`** (the pre-existing
struct→class delegation gate, parser.cpp ~13653) to detect a plain member function
(`name(` — at class scope always a function) and a typed `operator`, so
method-bearing structs route to `TokenCLASS::parse` like ctor/base-bearing ones
already did. **Struct path UNTOUCHED** (data-only structs — all of C/SMAUG — never
route). Brought the class body parser to **parity** so nothing drops: comma
declarators (`int x,y;`), bit-fields (single+comma, named+unnamed), leading+trailing
member `__attribute__`; hoisted `parse_bitfield_width` to a shared `Program` method.
Gate false-positives (attribute *contents* `aligned(8)`/`mode(byte)`, array-dim
`[sizeof()]`, NSDMI `= f()`) caused 12 transient torture regressions — all found via
the parent-vs-mine failset diff and fixed (skip `__attribute__((...))` balanced
parens, sqdepth + member_seen_eq guards). Tests: `teststructmethod`,
`teststructmethodattr`.

### Arc 2 — SMAUG C89 warning audit: 886 → 11 warnings (98.8%); commits `7810f3b`, `c13c931`, `735c719`, `f52c3ad`
The SMAUG boot emitted 886 **c2mir** warnings (NOT madc-parser warnings — all from
`/workspace/mir/c2mir/c2mir.c`, reflecting the types in madc's emitted node_t tree).
Audited them; **found and fixed 4 real, GENERAL madc bugs** (not SMAUG-specific).
Methodology that nailed each: a **3-way reducer — gcc, clang, AND c2m-direct
(`/usr/local/bin/c2m`) vs madc**; when gcc/clang/c2m are silent but madc warns, the
bug is madc's lowering, not c2mir strictness or source looseness.
1. **`7810f3b` (Bucket B):** embedded `stdlib.h`/`stdio.h` left
   malloc/calloc/realloc/fopen/fgets/getenv on the dlsym fallback (return typed
   `long`); declared their real `void*`/`char*` returns (`unsigned long` for size_t,
   as string.h does; FILE is `void`). 886→700.
2. **`c13c931` (Bucket A — THE over-strict bug):** a Form-1 function typedef
   (`typedef bool SPEC_FUN(CHAR_DATA*)`) param `SPEC_FUN *p` emitted as `SPEC_FUN **p`.
   `CirBuilder::explicit_star_count` (cir_builder.cpp ~2066) **summed** the implicit
   function-decay star with the explicit declarator `*` — they're the same pointer
   level, must not stack. Changed `+=` to **max**. 700→17. (gcc/clang/c2m all silent
   on the reducer; madc now matches.)
3. **`735c719`:** `add_stdio()` registered stdin/stdout/stderr as `ddINT64` globals
   (value right, integer type wrong) → typed as `ddVOIDptr` (FILE=void); declared
   `inet_ntoa`/`inet_ntop` (char* returns) in `arpa/inet.h` (+`#include <netinet/in.h>`
   for `struct in_addr`). 17→15.
4. **`f52c3ad`:** `TokenCast` emitter (cir_builder.cpp ~5684) peeled ONE pointer
   level and appended ONE `*`, so `(char **)` collapsed to `(char *)` (bit SMAUG's
   `CREATE(dest,char*,n)` macro + every `T**` cast). Now peels/emits ALL DataDefPTR
   levels (fptr keeps the single-pointer fallback). 15→11.

**Remaining 11 SMAUG warnings are GENUINE C89 looseness that gcc -Wall also flags**
(9 `grub.c` `int*` vs `bool*` arg mismatches; 2 `magic.c` int/pointer comparisons —
my reducers could NOT reproduce them in madc, confirming SMAUG-specific). madc is
now correctly **aligned with gcc**, NOT over-strict — do NOT silence these (would
hide real bugs; user's "don't ignore warnings"). User's goal ("handle legacy code
well, not be more strict than gcc") is met.

**Known minor latent bug found but NOT fixed (rare, out of scope):** a *global*
`FT **g` (pointer-to-function-pointer via a function typedef) under-counts to one
`*` on the var path (the var path doesn't record explicit stars for fnptr aliases).
Vanishingly rare, not in SMAUG, orthogonal to the param fix.

### NEXT (real-header track — struct-methods advanced `memory` 81:54→81:79 but did NOT clear streams)
The streams' `'less'` is the **`stl_function.h` DERAILMENT** — `std::less` is a
struct template with a base, but the header parse derails ("Unexpected end of data",
both madc-pp and gcc-pp → a PARSER bug) in the `less<void>` specialization (partial
specs / nested struct templates / `__ptr_cmp<>{}` / `constexpr auto … -> decltype`).
struct-methods was a PREREQ but this template bug kills `std::less` registration
independently — **this is the next target for the streams.** Other live probe
blockers (§5): `string`/`map`/`set` `36:9`/`42:9` (preprocessor), `vector` `2119:22`
(namespace parse), `string_view` `768:45` (iterator<> template arg), `algorithm`
`'abs'` (declare in `<cstdlib>` like the Bucket-B fns). Deferred §6 work: full
**unification/dedup** of the two ~2300-line body parsers + the `is_nontrivial_class`
re-gate (§6.4).

### Verify-state on resume
```bash
cd /workspace/madc
git rev-parse --short HEAD          # f52c3ad (or later)
git branch --show-current           # feature/realhdr-parse-gaps2-claude
git status --short                  # clean
make -C src 2>&1 | grep -iE 'error|warning'   # clean
make -C src fulltest 2>&1 | tail -2 # 528 passed / 4 failed
bash scripts/probe_real_headers.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g'   # §5 frontier
# SMAUG soak (boots to "ready ... port 4023", 0 errors):
cd /workspace/MadSMAUG && MADC=/workspace/madc/bin/madc MADC_CPU_LIMIT=0 timeout 600 ./MadSMAUG.sh 4023 2>&1 | grep -cE 'warning --|ready at address|error:'
# full torture failset diff (the regression gate for struct/class & cir changes):
#   1. git checkout <parent> -- src/parser.cpp src/cir_builder.cpp include/madc.h ; make -C src ; run_gcc_testsuite.py > parent.log
#   2. git checkout HEAD  -- (same) ; make -C src ; run_gcc_testsuite.py > now.log
#   3. comm -13 <(failset parent) <(failset now)  → MUST be empty
```

---

## 0. TL;DR — what to do on resume

1. **Run the verify block (§2)** to confirm live state matches this doc.
2. **The next high-leverage target is STRUCT MEMBER METHODS** — the single
   root cause behind the remaining streams blocker (`'less'`) and `memory`
   (`81:54`). The **user's design directive (§6)** is now canonical: *struct and
   class differ ONLY by default access* (struct public-by-default, class
   private-by-default); a "simple" struct and "simple" class (no virtual
   methods/dtors) are **virtually identical**. So the fix is to let a `struct`
   body parse exactly like a `class` body (methods, ctors, dtors, `operator()`,
   access labels, bases) — ideally by **unifying `TokenSTRUCT::parse` with
   `TokenCLASS::parse`** (one path parameterized by default access), keeping
   trivial structs on the native-struct path so the C torture suite + SMAUG do
   not regress. Full design, prior-attempt history, and the safety guarantee are
   in §6 and the memory `project_struct_is_class`.
3. This is a **core, regression-sensitive** parser change (struct parsing is
   load-bearing for C). Go **test-gated**: `make -C src fulltest` after each
   sub-step (must stay 526/4/0/26 — exactly those 4 reds), plus the **full
   gcc.c-torture failset-diff** and a **SMAUG soak** before declaring done. Prior
   *blanket* struct=class attempts caused **65–120 torture regressions** (§6).
4. Lower-risk alternatives if you want a quick win first: `map`/`set` `36:9`
   (preprocessor), `algorithm` `'abs'` (§5), or `vector` `2119:22` (namespace
   parse). But struct-methods unblocks the most headers at once.

Methodology (NON-NEGOTIABLE, from AGENTS.md): **gcc AND clang are both canon** —
reduce a failing case to a minimal repro and compare against `gcc -S
-fverbose-asm -O0` / `clang -S -O0` (or `g++ -E` for preprocessor) BEFORE
forming a hypothesis. **Fix at the deepest layer**, never shim. **Think twice,
code once.** **Understand what exists before assuming it doesn't** (search the
24k-line parser first). The user explicitly said this session: *"don't be afraid
to fix existing madc functionality to work more correctly"* and *"Rule #2 — fix
at the deepest layer."*

---

## 1. Environment, build, backend

- Repo `/workspace/madc`. Branch `feature/realhdr-parse-gaps2-claude` off
  `develop`. **Do NOT push; do NOT promote to master** (parity gate — see
  `.claude/rules/branching.md`). `develop` is untouched by all this work.
- Backend: `madc parser → cir_node (MC11-IR == c2mir node_t) → c2mir → MIR → JIT`.
  Sole backend (asmjit removed). The MIR dependency is the **fork** at
  `/workspace/mir` (branch `develop`, pinned by repo-root `MIR_COMMIT`), NOT
  upstream — carries native `_Complex`, `__attribute__((cleanup))`, ≤16-byte
  SIMD, scope-depth decl layout fix, ABI fixes.
- Build / test:
  ```bash
  make -C src               # build bin/madc + lib (regenerates embedded headers — see §4.7)
  make -C src fulltest      # unit (doctest) + all integration tests; MUST stay 526/4/0/26
  bash scripts/check-no-std-hardcoding.sh   # the retire-std-hardcoding gate (wired into fulltest)
  ```
- Useful flags: `--std=c++17` (or `--std=c++11`), `--emit=c11` (render the
  cir_node tree as C = "what c2mir sees"), `--dump-source` (madc's PREPROCESSED
  token stream — for preprocessor bugs), `--dump-cir` (dump the tree), `-v`
  (verbose DBG trace — invaluable for "which function threw").
- Scratch goes in `tmp/` (gitignored). Reducers from this session live there but
  are not committed — recreate from this doc as needed.

---

## 2. Verify-live-state block (run first on resume)

```bash
cd /workspace/madc
git rev-parse --short HEAD            # expect bb829ed (or later if work continued)
git branch --show-current             # feature/realhdr-parse-gaps2-claude
git status --short                    # expect clean
make -C src 2>&1 | grep -iE 'error|warning' | head     # clean build
make -C src fulltest 2>&1 | tail -3   # 526 passed / 4 failed / ...
bash scripts/probe_real_headers.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g'   # §5 table
# struct-method gap (the next target) still present:
printf 'struct S { int m(int a){return a;} };\nint main(){ S s; return s.m(5); }\n' > tmp/_sm.mad
bin/madc --std=c++17 tmp/_sm.mad   # -> "Expecting ';' after struct member"  (the gap)
```

---

## 3. Project context / north star (why any of this matters)

madc ("My Advanced Dialect of C") is a C/C++ dialect that parses source into a
`cir_node` tree (the MC11-IR, which derives from c2mir's `node_t`) that c2mir
compiles to MIR. The long arc (see `docs/plans/madc-vision-and-invariants.md`,
`docs/adr/0001-cir-c2mir-backend.md`) is a **polyglot transpiler** through that
one IR. The **north star is C23/C++23 compliance** (`project_north_star_c23_cpp23`),
and **anything off that path is drift** (`feedback_correct_over_shortcuts` —
shortcuts are *categorically unacceptable*; the user has re-enforced this many
times; RED-FLAG tells = about to hardcode a literal / add a wrapper-shim /
special-case higher up / think "good enough for now" → STOP, fix the deepest
layer).

This branch sits in two converging campaigns:
- **retire-std-hardcoding** (`project_string_as_class`, `project_cpp_mangled_direct`):
  madc hardcodes ONLY C/C++ primitives; every other type is a COMPOSED
  DataDefCLASS from a `#include` header — no per-type code / `dt*` tags /
  wrappers / `_Z…` literals; the mangler is the single symbol source. Gate:
  `scripts/check-no-std-hardcoding.sh` (green).
- **real-header parsing** (this doc): madc parses the REAL system C++/libc
  headers (retiring curated stubs), so `#include <vector>` etc. resolve to
  libstdc++/glibc like clang does — no per-class machinery. The instrument is
  `scripts/probe_real_headers.sh`.

The long-running concrete goal is to compile SMAUG 1.8 (~158k LOC C89; the port
lives in the separate **MadSMAUG** repo). SMAUG must stay working (C structs
plain) — it's the canary for any struct/class change.

---

## 4. Everything fixed this session (newest → oldest) — mechanism + anchor + commit

All on `feature/realhdr-parse-gaps2-claude`, all fulltest-green at the stated
count, all with the 4 known reds and no others.

### 4.1 `bb829ed` docs — pthread/east-const recorded; struct-methods flagged
Docs only.

### 4.2 `0c9523e` fix(parser): CV-qualifiers after the base type in a typedef (east-const)
`typedef int const X;` / `typedef int volatile X;` failed ("use of undeclared
identifier 'X'"). **Root cause:** `TokenTYPEDEF::parse` (parser.cpp ~17053)
consumed a LEADING `const`/`volatile` (the skip loop ~17094) and a following
`*` (the pointer loop), but NOT a CV-qualifier sitting *between the base type and
the alias name*. So after resolving `base_dd=int`, `nextToken()` read `const` as
the alias (a keyword → `alias="const"`), leaving `X` dangling. **Fix:** replaced
the pointer-only loop (was at ~17187) with a star+CV-qualifier loop using the
existing `is_post_pointer_qualifier_token()` (defined parser.cpp:1019) — mirrors
the loop in `parseDeclaration` (~21986). Now `int const X`, `unsigned int const
X`, `int const *X`, `int * const X` all parse. Variable decls already handled it;
only the typedef path didn't. **Test:** `tests/testtypedefcvqual.mad` (`7 11 3 5
9`). fulltest 526/4/0/26. Found while writing the pthread types (§4.3).

### 4.3 `7da5b96` fix(headers): define opaque pthread types in embedded pthread.h
**Root cause:** `include/madc/pthread.h` declared only `pthread_t` (`#define
pthread_t int64_t`) and left `pthread_mutex_t`/`cond_t`/`key_t`/`once_t`/`attr_t`/
etc. "deferred" = **undefined**. libstdc++'s `<bits/gthr-default.h>` does
`typedef pthread_mutex_t __gthread_mutex_t;` (and the rest), which failed with
"Expecting type after 'typedef'" — the parser is *correct* to reject a typedef of
an unknown type; the **stub was incomplete**. This gated ALL 5 streams headers
via the chain `<ostream>` → `ios_base.h` → `bits/locale_classes.h` →
`ext/atomicity.h` → `bits/gthr.h` → `bits/gthr-default.h`. **Fix:** declared the
opaque POSIX thread types with glibc x86-64 sizes (`typedef union { char
__size[N]; <align>; } pthread_X_t;` for the aggregates; `pthread_key_t`=unsigned
int, `pthread_once_t`=int, `pthread_spinlock_t`=`volatile int`). Contents stay
opaque (the real pthread ops go through libc via dlsym); only size/alignment +
type-name matter for parsing. **GOTCHA:** I first wrote `typedef int volatile
pthread_spinlock_t;` (qualifier AFTER `int`) which failed — that postfix-qualifier
gap is exactly bug §4.2 (fixed separately); use `volatile int`. fulltest 525→
(then 526 after the §4.2 test).

### 4.4 `a4037f3` fix(headers,build): declare ctype functions + parse-time embed-gen
**Two coupled fixes.**
- **Header:** `include/madc/ctype.h` only *listed* the ctype functions in a
  comment ("available via dlsym fallback") — never declared them. Real
  `<cctype>` does `namespace std { using ::isalnum; ... }`, and a
  using-declaration needs a global-scope declaration to bind to (else "'isalnum'
  is not a declaration in '::'"). This was the dominant blocker for `<string>` +
  all 5 streams. **Fix:** declared the 15 prototypes (`int isalnum(int);` …
  `toascii`) — real return type `int` per the embedded-headers rule.
- **Build (completes `5a27a22`):** the `.PHONY gen_embedded_headers` recipe (my
  earlier footgun fix) regenerated `embedded_headers.cpp` at *recipe* time —
  TOO LATE: make stats `embedded_headers.o` against the file's mtime when it
  evaluates the dependency graph, BEFORE the recipe-phase regeneration updates
  it, so a changed/added/deleted embedded header did NOT rebuild the object until
  the NEXT make. This **masked the ctype edit behind a stale binary** (I chased a
  ghost for several probes). **Fix:** regenerate at Makefile **PARSE time** —
  `EMBED_GEN := $(shell ../scripts/gen_embedded_headers.sh >/dev/null 2>&1; echo
  done)` near `MADHDRS` (src/Makefile ~17), and removed the `.PHONY` rule. The
  generator is idempotent (writes a temp, `cmp`, only `mv` on real change — see
  §4.7), so this neither churns mtimes nor forces rebuilds, and a stub edit lands
  in `embedded_headers.cpp` + the binary in ONE `make` (verified).

### 4.5 `e392a17` fix(parser): allocation-operator arity + noexcept template-id stripping
Cleared the `Too many parameters` blocker on `<memory>`/`<vector>`/`<string>`.
**Two fixes:**
- **operator new/delete overload arity (the real blocker):** libstdc++'s
  new_allocator calls the C++17 *aligned* `operator new(size, align_val_t)` (2
  args), but madc collapses EVERY operator-new/delete overload onto the single
  name `operatornew`/`operatordelete` (`parseOperatorId`, parser.cpp ~8349) and
  registers only the 1-arg form, so the 2-arg call tripped BOTH the comma-count
  check (`parseCallMethod` ~8508) and the post-call arity check (`parseCallFunc`
  ~8352). **Fix:** added `static bool is_overloaded_allocation_operator(const
  std::string&)` (parser.cpp ~8103, recognizes the 4 allocation-operator names —
  general to the language, NOT a per-symbol hack) and used it to accept extra
  args in `call_accepts_extra_args` (covers parseCallFunc), the parseCallFunc
  post-check, and parseCallMethod. The concrete overload is the runtime
  allocator's concern, not the parser's.
- **noexcept(...) with a template-id condition (found en route; a real bug):**
  `noexcept` was a FUNCTION-LIKE macro (`macro_map["noexcept"]`, params=`{__expr}`,
  empty body), so `noexcept(is_nothrow_constructible<T, Args...>::value)` split
  on the comma inside `<...>` — which the C preprocessor does NOT group — into
  TWO macro args ("Too many parameters" at macro expansion). **Fix:** strip
  `noexcept` by **balanced-paren consumption** in the lexer's `getToken()` word
  handler (lexer.cpp ~3609, right before the `__attribute__` branch), like
  `__attribute__`/`_Alignas`; removed the `define_map["noexcept"]` and
  `macro_map["noexcept"]` entries (lexer.cpp ~1024). **Test:**
  `tests/testnoexcepttemplatecond.mad` (`7 12`) — note it uses `class` not
  `struct` for the member, because struct-methods is the open gap (§6). vector
  advanced 690→2119.

### 4.6 Locale stub retirement (`6bae117` → `39bd07a` → `05add19`) + footgun (`5a27a22`)
- `6bae117` added `struct lconv` + `setlocale`/`localeconv` prototypes to the
  embedded `locale.h` stub (it only had `LC_*` constants). Cleared `'lconv' is
  not a declaration in '::'`.
- `39bd07a` then **retired the `locale.h` stub entirely** — the restored parser
  now parses the REAL system `<locale.h>` end-to-end (madc predefines
  `_GNU_SOURCE`, predefined_macros.cpp:451), so deleting `include/madc/locale.h`
  makes `#include <locale.h>` fall through to the system header (embedded headers
  are tried first, then system — see §4.7 resolution order). One real header
  covers `struct lconv` + setlocale/localeconv + the FULL `locale_t`/`uselocale`/
  `newlocale` POSIX-2008 API at once — mapping to real libc like clang instead of
  chasing symbols into a drifting stub. **This is the first instance of the
  "retire curated stubs → real headers" arc.** clocale went green; string/streams
  advanced past the entire locale `using`-chain.
- `05add19` fixed a **stale-generated-file bug**: `39bd07a` deleted the source
  but left the regenerated `embedded_headers.cpp` UNSTAGED, so HEAD still baked
  the locale.h stub into the binary (a clean build would have kept shadowing the
  real header). Root cause = the `embedded_headers.cpp: $(MADHDRS)` rule only
  regenerated on a NEWER listed header; a DELETION triggers no regen. Committed
  the corrected generated file.
- `5a27a22` was the first attempt to fix that footgun (`.PHONY` always-run gen +
  idempotent generator). It made deletions/additions reflected but had the
  one-pass bug fixed properly in `a4037f3` (§4.4).

### 4.7 The embedded-header mechanism (you WILL touch this)
- Embedded headers live in `include/madc/` (e.g. `ctype.h`, `pthread.h`,
  `sys/cdefs.h`, `stdarg.h`). `scripts/gen_embedded_headers.sh` bakes them into
  `src/embedded_headers.cpp` (a `std::map<string,string>` keyed by path relative
  to `include/madc/`, e.g. `"sys/cdefs.h"`). The generator is now **idempotent**
  (temp file + `cmp`, only replaces on change) and runs at **Makefile parse
  time** (§4.4). `src/embedded_headers.cpp` IS committed and shows in `git
  status` after a stub edit — commit it too.
- **`#include` resolution order** (lexer.cpp ~2072–2123, for `<...>`): (1)
  filesystem PCH (`.madh` on disk), (2) embedded PCH
  (`find_precompiled_header`, baked in `src/precompiled_headers.cpp`), (3)
  embedded TEXT (`find_embedded_header` → the stub), (4) system path
  (`resolve_include_path`). So an embedded stub is used in preference to the real
  header; deleting the stub falls through to the system header. (Embedded PCHs
  exist for some headers, e.g. `pch_ctype_h` — but they currently "PCH fail" and
  fall through to embedded text; don't be surprised by the `-v` trace
  "precompiled … PCH failed, trying embedded text … (embedded)".)
- **MADHDRS gap (latent):** `MADHDRS = $(wildcard ../include/madc/*)` (Makefile
  ~17) only globs TOP-LEVEL files — subdir stubs like `sys/cdefs.h` aren't in the
  list. Not currently biting (parse-time gen runs regardless), but note it.

### 4.8 Earlier this session (the bug-B segment — already on the branch below these)
- `5ea75a2` fix(parser): **namespace-scope `template_map`** (bug B / char_traits).
  `template_map` was `map<string, TemplateDef>` keyed by BARE name, so a
  same-named template in a 2nd namespace OVERWROTE the first (`std::char_traits`
  clobbered `__gnu_cxx::char_traits`). Now `map<string, vector<TemplateDef>>`
  (per-namespace variants), all selection through a new `find_template(name,
  ns_hint)` helper (parser.cpp ~9447), `register_template()` owns insert/merge,
  `instantiate_template_use()` gained `ns_hint` and COPIES the chosen def
  (vector-realloc dangling-ref safety), and the mangled key folds in the
  namespace ONLY on a >1-variant collision (zero churn for std::). Test
  `tests/testtemplatenamespacescope.mad`.
- `4f4fbd2` refactor(parser): collapsed 10 hand-rolled `alias_use→use` template-id
  probe sites onto one `instantiate_template_id(name, tb, ns_hint)` seam
  (parser.cpp ~2270) so the namespace hint flows uniformly — addressed the user's
  "competing methods → amalgamate" note.

### 4.9 Even earlier (real-header fixes from the prior session segment, on the branch)
- `c2126dc` fix(parser): namespace-qualified types as struct members (`a::T *p;`
  in a struct body) — `TokenSTRUCT::parse` member loop falls through to
  `resolve_declared_type_token` when the bare lookup misses and `::` follows.
  Test `tests/testqualifiedmembertype.mad`. (Bug A of the char_traits bisection.)
- `68dee85` feat(headers): fleshed out embedded `sys/cdefs.h` with glibc
  no-attribute-fallback macros (`__nonnull`, `__attr_access`, `__wur`,
  `__attribute_*__` family, the inline family `__extern_inline` etc.). Cleared
  the `<wchar.h>` K&R-misparse wall.
- `0665b86` fix(cir+headers): array-typedef emitter bug — `struct_behind()` in
  `cir_builder.cpp` (the top-level decl emit driver) didn't peel
  `DataDefCArray`, so a 2nd array typedef of a tagged struct re-emitted the body
  → c2mir "tag redeclaration"; plus added `__gnuc_va_list` to embedded
  `stdarg.h`. Test `tests/testarraytypedefstruct.mad`.

---

## 5. Current real-header probe (HEAD bb829ed) — the live frontier

`bash scripts/probe_real_headers.sh` — two columns: **`madc`** = madc's OWN
preprocess+parse; **`pp`** = gcc-preprocessed (macros expanded, `# line` stripped)
so madc only PARSES. **`pp` pass + `madc` fail = a madc PREPROCESSOR gap; BOTH
fail = a PARSER gap.** The `pp` column is essentially ALL-GREEN now (the
restoration's unified angle-scanner killed the old `<map>`/`<set>` parser
blocker), so remaining `madc`-column failures are preprocessor or
near-parser / instantiation issues.

```
HEADER         madc (own preprocess)                                pp (parser-only)
type_traits    OK                                                   OK
utility        OK                                                   OK
char_traits    OK                                                   OK
iosfwd         OK                                                   OK
cctype         OK                                                   OK
clocale        OK                                                   OK
ostream        172:17 use of undeclared identifier 'less'           OK   <-- struct-method (std::less)
istream        172:17 'less'                                        OK
iostream       172:17 'less'                                        OK
sstream        172:17 'less'                                        OK
fstream        172:17 'less'                                        OK
string         42:9 Expecting type in class definition              OK   <-- preprocessor (class-def)
map            36:9 Expecting type in class definition              OK   <-- preprocessor
set            36:9 Expecting type in class definition              OK   <-- preprocessor
memory         81:54 Expecting ';' after struct member              OK   <-- struct-method
vector         2119:22 Expecting '{' after namespace name           OK   <-- namespace parse
string_view    768:45 Expecting a type argument to iterator<>       OK   <-- template-arg resolution
algorithm      52:13 'abs' is not a declaration in '::'             OK   <-- global using of libc fn
```

**Blocker analysis / threads, by leverage:**

1. **`'less'` (5 streams) + `memory` 81:54 — STRUCT MEMBER METHODS.** `std::less`
   is a `struct` with `operator()`; `<system_error>`'s `error_category::operator<`
   body calls `less<const error_category*>()(this,&__other)` (madc-preprocessed
   line ~10305). madc rejects member functions in a `struct` ("Expecting ';'
   after struct member"), so `std::less` never registers → "undeclared 'less'".
   `memory`'s 81:54 is the same gap (a struct member function). **This is the §6
   target — the highest-leverage move.** Likely unblocks all 5 streams + memory.

2. **`string`/`map`/`set` `36:9`/`42:9 Expecting type in class definition`** —
   `pp` column is OK, so this is the madc PREPROCESSOR (a `_GLIBCXX_*`/macro gap
   producing a malformed class body). Use the two-column method: `g++ -E` the
   failing inner header, diff `bin/madc --dump-source`, find the unexpanded/
   mis-expanded macro. (These may also partly resolve once struct-methods land —
   verify after.)

3. **`algorithm` `52:13 'abs' is not a declaration in '::'`** — a global
   `using ::abs;` of a libc function (`<cstdlib>`); `abs` isn't declared at `::`
   in madc's view. Mirror of the (now-fixed) `lconv`/`isalnum` pattern — either
   declare `abs`/`labs`/`llabs`/`div` etc. in the embedded `<stdlib.h>` stub (the
   ctype approach), or retire that stub for the real header if it parses.

4. **`vector` `2119:22 Expecting '{' after namespace name`** — a new, deep
   namespace-declaration parse issue (vector got this far only after §4.5).
   Reduce at the failing construct under `--dump-source`.

5. **`string_view` `768:45 Expecting a type argument to iterator<>`** — a
   template-arg resolution gap in a nested `iterator<>` use; its own thread.

**Probe-one-header recipes:**
```bash
# madc own-preprocess:
printf '#include "/usr/include/c++/13/ostream"\nint main(){return 0;}\n' > tmp/_x.mad
bin/madc --std=c++17 --emit=c11 tmp/_x.mad 2>&1 >/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -m1 'error:'
# preprocessor-vs-parser classify:
g++ -std=c++17 -E /usr/include/c++/13/ostream 2>/dev/null | grep -v '^#' > tmp/_pp.cpp
bin/madc --std=c++17 --emit=c11 tmp/_pp.cpp 2>&1 >/dev/null | grep -m1 'error:'
# which function threw (verbose):
bin/madc -v --std=c++17 --emit=c11 tmp/_x.mad 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -B6 'error:' | tail
# what c2mir would see:
bin/madc --std=c++17 --dump-source tmp/_x.mad 2>/dev/null | sed -n 'START,ENDp'
```
**Cross-include line attribution is UNRELIABLE** (the parser reports the top file
with an inner line number, e.g. `ostream:172` is really a `<system_error>` line).
Bisect by INCLUDE (probe each `#include` of the failing header individually —
that's how every blocker this session was localized), not by trusting `file:line`.

---

## 6. THE NEXT TARGET — struct member methods / struct≡class unification

### 6.1 User design directive (canonical — 2026-06-07)
> *"we want struct vs class to follow the same rules that modern C++ does, where
> the only real difference between a struct and a class is that a struct is
> public-by-default and a class is private-by-default … and thus a 'simple' class
> and a 'simple' struct (ones without virtual methods and destructors) are
> virtually the same."*

So: a `struct` body must parse exactly like a `class` body — member functions,
constructors, destructors, `operator()`/operator overloads, access-specifier
labels (`public:`/`private:`/`protected:`), base-class lists — the ONLY
differences being **default access** (struct public, class private) and **default
inheritance access** (struct public, class private). A trivial struct and trivial
class (no virtual methods/dtors, no bases) are byte-identical and must share the
native struct code path (layout, by-value param ABI, aggregate/brace init,
copy) — this is verified true in g++ (a method changes nothing; only
std::string-member / user ctor/dtor / virtual / base add object ABI).

### 6.1b User refinement (2026-06-07) — the difference is ALREADY mostly implemented
> *"AFAIK we already implemented that handling difference between class and struct
> … I just imagine there may be some code that is depending on the datatype /
> tokentype rather than if it is trivial or not."*

Take this as the working hypothesis: the struct-vs-class *semantics*
(default-access, and struct→class promotion on object members) are LARGELY DONE
(§6.2). So this is NOT a big new feature — it is two bounded jobs:
1. **Let a `struct` body PARSE member functions/ctors/dtors/operators** (today
   `TokenSTRUCT::parse` only parses data members → "Expecting ';' after struct
   member"). The cleanest form is unifying the body-parse with `TokenCLASS::parse`
   (§6.3-A), but it may be as small as routing the struct member loop through the
   same member-declaration handler the class path uses.
2. **AUDIT for code that branches on the TYPE/TOKEN kind rather than on
   TRIVIALITY** — i.e. anything keyed on `DataDefSTRUCT` vs `DataDefCLASS`,
   `btStruct` vs `btClass`, `TokenType`/keyword `tkSTRUCT` vs `tkCLASS`, or
   `is_object()`/`as_class_instance()` (true for ANY class) where it SHOULD key on
   `is_nontrivial_class()` (object members / user ctor-dtor / virtual / base).
   Those are the spots that make a trivial struct-class diverge from a plain
   struct (the source of the prior 65–120 torture regressions, §6.4). Grep
   starting points: `DataDefSTRUCT`, `DataDefCLASS`, `btStruct`, `btClass`,
   `is_object(`, `as_class_instance(`, `tkSTRUCT`, `tkCLASS`, `basetype()`.

### 6.2 Current state of struct/class in madc (verify before editing)
- `class` parsing: `TokenCLASS::parse` (parser.cpp ~15295+; `do_typedef`
  detection ~15306) creates a `DataDefCLASS` (private default), and ALREADY
  handles member functions, ctors/dtors, access labels, bases, virtual, RTTI,
  multiple/virtual inheritance (all landed earlier — see
  `project_cpp_parser_correctness`, `project_multiple_inheritance`).
- `struct` parsing: `TokenSTRUCT::parse` (parser.cpp **13716**) runs a SEPARATE,
  simpler member loop that parses **data members only**. It reads `<type> <name>`,
  then expects `,` or `;`; when a `(` follows the name (a method), it throws
  **"Expecting ';' after struct member"** at parser.cpp **14674**. It does NOT
  call `parseFunction` or handle ctors/dtors/operators/access-labels. **THIS is
  the gap, and it CONFIRMS the user's hypothesis** (§6.1b): the struct/class
  difference is two separate BODY PARSERS, not a fundamental missing feature.
- **Exact reuse anchors (class side, what the struct path should adopt):**
  `TokenCLASS::parse` (parser.cpp **15299**) is the rich body parser — it detects
  member functions and calls `pgm.parseFunction(...)` (parser.cpp **15849 / 15903
  / 15960 / 16274** for methods / ctor-dtor / conversion-op / general member),
  inside a **deferred-function-body sink** (set at ~15711–15712, restored at
  ~16399; `enqueue_deferred_function_body` ~15197, `parse_deferred_function_body`
  ~15212). The struct→class promotion finalize (object-member case) is at
  `TokenSTRUCT::parse`'s closing-`}` (~parser.cpp 11185 per the old note — VERIFY;
  it may have moved). **The fix = route the struct member loop through the same
  member-declaration/method handler the class path uses (ideally unify the body
  parse, parameterized by default access), set PUBLIC default access for
  struct-origin classes, and promote a struct to DataDefCLASS when it gains any
  C++ object feature (method/ctor/dtor/virtual/base) — exactly mirroring the
  existing object-member promotion.**
- **Struct→class PROMOTION already exists** (develop @ `78d1b27`, also on this
  branch): at `TokenSTRUCT::parse`'s closing-`}` finalize (~parser.cpp 11185 per
  the old note — VERIFY the current line), if any member `is_object()` by value
  (e.g. a `std::string` member; pointers excluded), the parsed `DataDefSTRUCT`
  is promoted to a `DataDefCLASS` (slice-copy `static_cast<DataDefSTRUCT&>(*ddc)
  = *dds`, repoint `struct_map[tag]=ddc`) so the existing class lifecycle (member
  ctors at decl + RAII dtor) applies. This is why a `struct` with a std::string
  member works but a `struct` with a *method* does not — promotion triggers on
  object MEMBERS, not on member FUNCTIONS (TokenSTRUCT::parse can't even parse a
  member function to know it's there).

### 6.3 The recommended approach (two options; both must be triviality-safe)
The directive points at **unifying the two parse paths**. The restoration also
flagged this as deferred polish: *"dedup TokenSTRUCT::parse vs TokenCLASS::parse
(~2,300 near-identical lines)."* Two ways to get there:

- **(A) UNIFY (preferred, matches the directive):** make `TokenSTRUCT::parse`
  and `TokenCLASS::parse` ONE parse path parameterized by `default_access`
  (struct=public, class=private) and default-inheritance-access. Concretely:
  extract the class body parser into a shared `Program` method that both
  keyword-tokens call with the default-access argument. The struct path then
  gets member functions/ctors/dtors/operators "for free." A `struct` with any
  C++ object feature becomes a `DataDefCLASS` (public default); a trivial struct
  stays on the native struct path (see triviality guarantee §6.4).
- **(B) EXTEND PROMOTION (smaller, incremental):** teach `TokenSTRUCT::parse` to
  PARSE member functions/ctors/dtors/operators (and access labels) in a struct
  body, and promote to DataDefCLASS when any C++ object feature is present (a
  member function, ctor, dtor, virtual, or base) — extending the existing
  object-member promotion. Less code churn than a full unify but duplicates the
  member-function parsing logic (the thing the dedup wants to eliminate).

Given the directive ("the only real difference is default access"), **(A) is the
right end state**; (B) is a stepping stone if (A)'s blast radius feels too large
in one go. Either way the access-default must be set correctly (madc enforces
access control — a `struct` member accessed from outside must be public, or you
get a private-access error).

### 6.4 THE SAFETY GUARANTEE (why this is hard, and how prior attempts failed)
**Blanket "every struct is a DataDefCLASS in non-C-mode" was tried THREE times
and reverted — 65 to 120 gcc.c-torture REGRESSIONS each.** Root cause: the
gcc.c-torture C programs (and SMAUG) run in `STD_MADC` (the default — the runner
passes no `--std`), so they ALL became classes, and **madc's class path WRONGLY
DIVERGES from the struct path for TRIVIAL types** at: aggregate/brace init
(treated as a constructor arg), by-value struct PARAMETER ABI, copy-assign
synthesis, bitfields/packing/alignment, stdarg/`va_arg`, and type-emission
(e.g. `__madc_va_list_tag redeclaration` from the class-emission loop). That
divergence is a **real latent bug**: a trivial public class is byte-identical to
a struct, so the class path should not diverge for it.

**Therefore the unification is safe ONLY if trivial-class-behaves-identically-to-
struct.** The mechanism `DataDef::is_object()` returns true for ANY `btClass`
(datadef.h ~181), so trivial struct-classes hit object-specific handling. The
CORRECT fix (preferred over object-member-gating): introduce
`is_nontrivial_class(dd)` = a class with (object members OR user ctor/dtor OR
vtable/virtual OR base class), and **re-gate the OBJECT-SPECIFIC handling**
(init-via-ctor, by-value param copy/passing, copy-assign synthesis) on
`is_nontrivial_class` instead of `is_object()`/`as_class_instance()` — so a
trivial class falls through to the native struct path at init/param/copy.
Already-safe sites (gated on triviality, no-op for trivial classes): member
ctors (`class_has_object_members`), retbuf return (`class_needs_dtor`).

**Validation REQUIRED before declaring done (HIGH blast radius):**
- `make -C src fulltest` after each sub-step (526/4/0/26).
- **FULL gcc.c-torture failset-diff = ZERO regressions** (this is what proves
  trivial-struct-as-class is safe; the prior attempts each added 65–120). The
  torture harness + how to diff failsets is in `docs/parity/` and
  `claude_status.json` (the parity campaign — `project_cir_parity_campaign`).
- **SMAUG soak** — must boot clean (its ~158k LOC of C structs must stay plain).
  SMAUG should declare a C std (`--std=c99`/`c89`) so its structs stay
  `DataDefSTRUCT`; verify it does (a directive in SMAUG.mad or the invocation).

### 6.5 Concrete starting reducer
```bash
printf 'struct S { int m(int a){return a;} };\nint main(){ S s; return s.m(5); }\n' > tmp/_sm.mad
bin/madc --std=c++17 tmp/_sm.mad         # currently: "Expecting ';' after struct member"
# the std::less shape that blocks the streams:
printf 'struct less { bool operator()(int a,int b) const { return a<b; } };\nint main(){ return less()(1,2)?0:1; }\n' > tmp/_less.mad
bin/madc --std=c++17 tmp/_less.mad       # currently fails on the struct operator()
```
Compare against `g++ -S -fverbose-asm -O0` on the trivial-struct-method case to
confirm it lowers identically to the equivalent class (Rule #1). Then implement
(A) or (B), re-gate triviality (§6.4), and run the full validation.

---

## 7. Methodology / rules to honor (condensed from AGENTS.md + memory)

- **GCC and clang are BOTH canon.** Reduce → compare disassembly/`-E` → hypothesis
  → fix the DEEPEST layer → never shim. (`gcc-methodology.md`,
  `clang-methodology.md`, `feedback_two_canon_compilers`.)
- **`make -C src fulltest` after every change.** Never leave the tree red beyond
  the 4 known reds. The no-std-hardcoding gate is wired in — keep it green.
- **No hardcoding specifics into general machinery; no special-casing specific
  C++ classes.** madc must map `#include` details to libc/libstdc++ like clang
  (the user's explicit, repeated constraint). The struct/class fix must be
  general (the language's struct/class semantics), not a per-header patch.
- **Shortcuts are categorically unacceptable** (`feedback_correct_over_shortcuts`)
  — the RED-FLAG tells are listed in §3. When tempted to hardcode a literal / add
  a wrapper / special-case higher up / think "good enough", STOP and fix the
  deepest layer. (Hardcoded stream `_ZSt` literals once caused DAYS of drift.)
- **Don't ignore warnings** (`feedback_dont_ignore_warnings`) — analyze every
  madc build warning AND every g++ warning on emitted C; ignored warnings have
  hidden real bugs (a g++ warning once hid a `length()`-returns-void error).
- **emit-C vs g++ oracle** (`feedback_emitc_gcc_parity_oracle`): a gcc-compiled
  `--emit=c11` of a program must match the g++-compiled original C++; divergence
  = a madc bug.
- **Commit early, small, self-contained.** Feature branch off `develop`,
  `-claude`-owned. **Do NOT push; do NOT promote to master** (parity gate).
  Commit before any scripted bulk transform so revert is clean. Co-author trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **The retire-stub pattern (validated this session):** many `include/madc/`
  stubs now shadow real headers the restored parser CAN handle. Retiring one
  (delete the file; the build regenerates `embedded_headers.cpp`) maps that
  include to real libc/libstdc++ and clears whole `using ::name;` chains at once
  — PROVIDED the real header (and everything it pulls in, in context) parses.
  Always gate with fulltest + full probe; REVERT if it surfaces an earlier parser
  bug (as the `ctype.h` retirement did — it surfaced a `bits/types.h`
  typedef-in-context issue, so fleshing the stub was chosen instead). The
  alternative — flesh the stub with the missing declarations (like `struct
  lconv`, the ctype prototypes, the pthread types) — is the bounded fix when the
  real header doesn't yet parse cleanly in context.

---

## 8. Restoration context (DONE — background, not a TODO)

The parser restoration (the user's prior mandate) is **COMPLETE** on this branch:
- `parseExpression` went 3,577 → ~172 lines (original shunting-yard shape);
  `Program::parseExpression` is at parser.cpp ~12924. The four big switch-arms are
  extracted methods: `parseExpr_dataTypeArm` (~9443), `parseExpr_symbolArm`,
  `parseExpr_identifierArm`, `parseExpr_operatorArm`, via the **ExprStep
  protocol** (`enum class ExprStep { Break, Continue, Redo, Done, Return };` in
  madc.h ~1547 — maps each arm's inline break/continue/goto/done/early-return
  1:1). The extraction technique was "compiler-as-oracle": after moving an arm
  body verbatim into a method, the compiler flags arm-level `break;`/`continue;`
  as illegal → convert exactly those to ExprStep returns → byte-identical.
- Free `static f(Program &pgm, …)` helper functions went **138 → 2** (the 2 that
  remain — `register_std_namespace_spec`/`register_madc_namespace_spec`,
  parser.cpp ~5874 — MUST stay free; they're registered as function-pointer
  callbacks). NOTE: a grep for `name(Program &pgm` matches ~32 lines, but those
  are the legit `TokenX::parse(Program &pgm)` framework parse-interface methods,
  NOT the threading anti-pattern.
- **Coordinated free-fn→method conversion recipe** (reuse for any future
  conversion): strip forward decls FIRST; sig-replace BEFORE body xform; use a
  STRING/CHAR/COMMENT-AWARE brace matcher (naive `{`/`}` counting runs away on
  braces in literals/comments — it corrupted sigs once); `pgm.`→``, cluster calls
  `C(pgm,`→`C(`, bare `pgm`→`*this`; strip def-side default args (move to the
  in-class decl); assert no `Program &*this`; let the compiler catch leftovers.
- Optional remaining restoration polish: **dedup `TokenSTRUCT::parse` vs
  `TokenCLASS::parse`** — which is now the SAME work as the §6 struct/class
  unification (they converge); `operator<` resolution end-to-end.

Full restoration detail: `docs/plans/2026-06-07-parser-restoration-HANDOFF.md`
and `project_parser_restoration` memory.

---

## 9. Commit list this session (newest first; branch feature/realhdr-parse-gaps2-claude)

```
bb829ed docs(plan): pthread types + east-const typedef fixed; struct-methods identified as root cause
0c9523e fix(parser): accept CV-qualifiers after the base type in a typedef (east-const)   [test testtypedefcvqual]
7da5b96 fix(headers): define opaque pthread types in embedded pthread.h (unblocks gthr/streams)
51b30fb docs(plan): isalnum cleared; 'typedef'-in-context dominant
a4037f3 fix(headers,build): declare ctype functions (clears 'isalnum') + parse-time embed-gen
fd2675d docs(plan): Too-many-parameters fixed; isalnum now dominant
e392a17 fix(parser): allocation-operator arity + noexcept template-id stripping   [test testnoexcepttemplatecond]
46af14f docs(plan): locale stub retired, footgun fixed, ctype deferred
5a27a22 fix(build): regenerate embedded_headers.cpp on every build (catch deleted stubs)  [footgun v1]
05add19 fix(headers): regenerate embedded_headers.cpp to actually drop locale.h
39bd07a feat(headers): retire embedded locale.h stub — map <locale.h> to real libc
6bae117 feat(headers): declare struct lconv + setlocale/localeconv in embedded locale.h (superseded by 39bd07a)
fabfdc0 docs(plan): bug B RESOLVED; probe table + next threads
4f4fbd2 refactor(parser): one instantiate_template_id seam for the alias|class template-id probe
5ea75a2 fix(parser): namespace-scope template_map so same-named templates don't collide   [bug B; test testtemplatenamespacescope]
e91858f docs(plan): comprehensive rehydration handoff (the prior doc)
c2126dc fix(parser): resolve namespace-qualified types as struct members   [bug A; test testqualifiedmembertype]
68dee85 feat(headers): flesh out embedded sys/cdefs.h with glibc attribute + inline macros
0665b86 fix(cir+headers): array typedef of a tagged struct must not re-emit the body; add __gnuc_va_list
```
(Earlier on the branch: the parser-restoration steps 1–25 and the `#include_next`
/ wchar work — see the restoration handoff.)

Regression tests added this session: `testtemplatenamespacescope` (`1 2`/`10 20`/
`21`), `testnoexcepttemplatecond` (`7 12`), `testtypedefcvqual` (`7 11 3 5 9`),
plus earlier `testqualifiedmembertype` (`7 7`), `testarraytypedefstruct` (`7 11`).

---

## 10. Gotchas / hard-won learnings (do not relearn)

- **Stale binary after a stub edit** (cost me several confused probes): before
  the `a4037f3` parse-time-gen fix, editing an embedded header + a single `make`
  did NOT relink the binary (the recipe-time regen was too late for the same-make
  object rebuild). Fixed now — but if you ever see a stub edit "not take effect,"
  do a clean rebuild and check `embedded_headers.cpp` actually changed
  (`grep -c <symbol> src/embedded_headers.cpp`) and that the `.o`/binary relinked.
- **`git checkout <file>` reverts to HEAD** — I accidentally clobbered an
  uncommitted `ctype.h` edit this way trying to drop a one-line probe. Commit or
  stash before `git checkout`; never `git checkout` over uncommitted work
  (`feedback_never_lose_code`).
- **Cross-include `file:line` is unreliable** — bisect by `#include`, not by line.
- **madc `struct` cannot have member methods** (the §6 gap) — use `class` (with
  `public:`) in any test that needs a method until §6 lands.
- **`noexcept`, `__attribute__`, `_Alignas`, `__extension__`, CV-quals, `inline`,
  `constexpr`** are stripped/ignored by the lexer — `noexcept`/`__attribute__`/
  `_Alignas` by BALANCED-PAREN consumption (comma-safe), the rest as empty
  defines. Don't reintroduce a function-like macro for anything whose argument
  can contain a top-level (template `<...>`) comma.
- **`<...>` is NOT a preprocessor grouping** — a function-like macro called with a
  template-id argument splits on the inner comma. This bit `noexcept` (§4.5) and
  is a general trap for any macro madc defines.
- **madc predefines `_GNU_SOURCE=1`** (predefined_macros.cpp:451) — real glibc
  headers expose their GNU extensions (uselocale, etc.) under madc.
- **The 4 pre-existing fulltest reds** are `testdefer`, `testfstream`,
  `testlargesizeofquery`, `testloop` — NOT caused by any of this work; "green"
  means exactly those 4 and no others.

---

## 11. Pointers

- AGENTS.md / CLAUDE.md (rules index), `.claude/rules/*` (bare rules),
  `docs/rules/*` (reasoning).
- `docs/plans/madc-vision-and-invariants.md` (I1–I8 invariants),
  `docs/adr/0001-cir-c2mir-backend.md` (backend decision).
- `claude_status.json` (canonical snapshot), `docs/parity/` +
  `project_cir_parity_campaign` (gcc.c-torture failset-diff harness — needed for
  §6.4 validation), `docs/test-status.md`.
- Memory index `/home/dev/.claude/projects/-workspace-madc/memory/MEMORY.md`.
  Most relevant memories: `project_parser_restoration` (one-screen index +
  points here), `project_struct_is_class` (THE struct/class design + prior-attempt
  history — read in full before §6), `project_north_star_c23_cpp23`,
  `feedback_correct_over_shortcuts`, `feedback_two_canon_compilers`,
  `feedback_dont_ignore_warnings`, `feedback_emitc_gcc_parity_oracle`,
  `project_cpp_parser_correctness`, `project_multiple_inheritance` (class machinery
  that the struct path will inherit on unify), `project_string_as_class`,
  `project_cpp_mangled_direct`.

---

## 12. One-paragraph resume script

On resume: read this doc + `project_struct_is_class` (full) +
`project_parser_restoration`. Run §2 to confirm HEAD `bb829ed`, clean tree,
fulltest 526/4/0/26, and the §5 probe. Then start §6: implement struct member
methods via struct≡class unification (one body-parse path parameterized by
default access — struct public, class private), making it SAFE by ensuring a
trivial class behaves identically to a struct (re-gate object-specific handling on
`is_nontrivial_class`, §6.4). Reduce with §6.5, compare to g++ -S (Rule #1), fix
the deepest layer, and validate with fulltest + the FULL gcc.c-torture
failset-diff (ZERO regressions) + a SMAUG soak before declaring done. Do not
push; do not promote to master.

END OF HANDOFF.
