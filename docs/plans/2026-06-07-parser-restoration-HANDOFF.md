# REHYDRATION HANDOFF — Parser restoration (read this FIRST after compaction)

Date: 2026-06-07. Branch: **`feature/realhdr-parse-gaps2-claude`** (off `develop`,
**NOT pushed** — local only). Working tree **clean** (all work committed).
fulltest baseline: **521 passed / 4 failed / 0 timed out / 26 skipped**
(the 4 reds — `testdefer`, `testfstream`, `testlargesizeofquery`, `testloop` —
are PRE-EXISTING, unrelated to this work).

This document is self-contained. After reading it you should be able to continue
the parser-restoration work without re-deriving anything. Verify live state with
the commands in §11 before editing.

---

## 0. TL;DR — what to do on resume

We are **restoring madc's parser to its original method-based, state-machine
architecture**, which Claude+Codex degraded over time. The active sub-thread is
**collapsing the ~13 hand-rolled `<…>` angle-bracket balancers onto shared
primitives** (`DelimDepth` + `delim_scan_step`/`delimStepStream`, and the
operator-id helpers). This both removes duplication AND fixes the `operator<`
template mis-parse in one place. 6 scanners done; a handful remain (§7).

Each step: small, **`make -C src fulltest` after every change** (must stay
521/4/0/26), mirror the original's idioms, **net lines and free-function count
trend DOWN**. Next options in value order are in §14.

---

## 1. The mission and the user's vision (the WHY — do not lose this)

The user's words (2026-06-06/07):
- *"this 1000 line parser needs to be broken down into (ideally reusable) parts.
  Most C++ parsing it doing similar things and I'm worried that the parser isn't
  reusing enough code."*
- *"Well written parsers written in C++ work more like state machines with
  stacks, and methods can access all this internal to the class without having
  to pass huge parameter lists around."*
- *"The original pre-Claude code version of madc … worked this way … you and
  Codex both introduced a lot of suck and dirty hacks that made the code very
  difficult to maintain."*

So this is a **RESTORATION to a design that already existed and worked**, not a
greenfield refactor. The user is (rightly) frustrated about the degradation. Be
honest, never force fake consolidation, never add new `pgm`-threading free
functions — *delete* them as primitives absorb their logic.

The user has twice approved continuing ("Keep going please", "yes, let's do #1",
"this is good work"). They value: correctness over shortcuts, honesty over
spin, methods-with-internal-state over parameter-threading, real reuse over
duplication. They explicitly do NOT want hand-tooled hacks.

---

## 2. The evidence: original vs degraded (concrete, measured)

| | Original `a22343e` ("Importing work to date") | Now (`HEAD`) |
|---|---|---|
| `src/parser.cpp` total | **1,401 lines** | **24,309 lines** (17×) |
| `Program::parseExpression` | ~175 lines (clean shunting-yard) | **3,580 lines** |
| free `static f(Program &pgm, …)` functions | **0** | **138** (was 139; step 1 removed one) |
| everything was a `Program::` method | yes | 96 methods + 138 free functions |
| `TokenSTRUCT::parse` / `TokenCLASS::parse` | (no classes yet) | 1,185 / 1,159 lines — near-identical member loops (duplication) |
| `parseDeclaration` | ~60 lines | 1,435 lines |
| hand-rolled inline `<…>`/delimiter balancers | (no templates yet) | **~13 copies** |

The original `Program::parseExpression(TokenBase *tb, bool conditional)`:
```cpp
TokenBase *Program::parseExpression(TokenBase *tb, bool conditional)
{
    TokenCpnd *code = compounds.empty() ? NULL : compounds.top();
    stack<TokenBase *> exStack;      // <-- local stacks: the state machine
    stack<TokenBase *> opStack;
    ...
    for ( done = false; !done && tb; tb = nextToken() )   // <-- member nextToken()
    {
        switch(tb->type()) {           // <-- switch dispatch
            case TokenType::ttInteger: exStack.push(tb); break;
            ...
        }
    }
}
```
State reached via **member methods** (`nextToken()`, `compounds.top()`), never
threaded through parameters. THAT is the target idiom.

To inspect the original: `git show a22343e:src/parser.cpp`,
`git show a22343e:include/madc.h`. (The original was an early C-subset parser —
no templates/operators yet — so there's no template code to copy verbatim; copy
the STYLE: methods on `Program`, local stacks, `nextToken()/peekToken()`-driven
loops, decomposed functions, zero `pgm`-threading.)

---

## 3. Architecture decisions made (locked, via AskUserQuestion)

1. **Host the reusable parsing layer as methods on `Program`** (NOT a separate
   `Parser` class). `Program` already holds the token stream (`std::deque<TokenBase*> tokens`,
   `_prv_token`), the scope stack (`class_scope_stack`), and the accessors
   `peekToken()/nextToken()/pushToken()/prevToken()` (include/madc.h ~1365–1426).
   The state machine substrate already exists; we add primitive methods + migrate
   the free functions onto them incrementally. A dedicated `Parser` class can be
   carved out LATER if ever.
2. **First phase = the angle/operator-id scanning primitive** — because it
   collapses the ~13 hand-rolled balancers AND fixes the `operator<` bug in one
   place. (This is the thread currently in progress.)

---

## 4. The shared primitives (created this session — the new reusable core)

All in `src/parser.cpp`. Line anchors drift; re-`grep` to confirm.

### 4a. operator-function-id recognition (parser.cpp ~1837)
```cpp
// Free helper — shared by stream AND vector scanners (some have no Program&).
static bool token_is_operator_id_start(TokenBase *t)
{
    return t && is_contextual_identifier_token(t)
        && contextual_identifier_name(t) == "operator";
}
// Templated (std::vector AND std::deque): # tokens the operator-id at toks[i]
// spans (keyword + symbol [+ closing )/] for operator()/operator[]]), 0 if none.
template<typename Seq>
static size_t operator_id_token_span(const Seq &toks, size_t i)
{
    if ( i >= toks.size() || !token_is_operator_id_start(toks[i]) ) return 0;
    if ( i + 1 >= toks.size() ) return 1;
    TokenBase *sym = toks[i + 1];
    if ( (sym->id()==tkOpBrk || sym->id()==tkOpSqr) && i+2 < toks.size() ) return 3;
    return 2;
}
```

### 4b. `DelimDepth` — the single home for delimiter bookkeeping (parser.cpp ~1865)
```cpp
struct DelimDepth {
    int paren = 0, square = 0, brace = 0, angle = 0;
    bool top() const { return !paren && !square && !brace && !angle; }
    void update(TokenBase *t) {        // pure delimiter switch (NO operator-id)
        if (!t) return;
        switch (t->id()) {
            case TokenID::tkOpBrk: ++paren; break;
            case TokenID::tkClBrk: if (paren>0) --paren; break;
            case TokenID::tkOpSqr: ++square; break;
            case TokenID::tkClSqr: if (square>0) --square; break;
            case TokenID::tkOpBrc: ++brace; break;
            case TokenID::tkClBrc: if (brace>0) --brace; break;
            case TokenID::tkLT:    ++angle; break;
            case TokenID::tkGT:    if (angle>0) --angle; break;
            case TokenID::tkBSR:   if (angle>0) angle = angle>1 ? angle-2 : 0; break;
            default: break;
        }
    }
};
// Index form: returns # tokens consumed (1, or an operator-id span). Templated.
template<typename Seq>
static size_t delim_scan_step(const Seq &toks, size_t i, DelimDepth &d)
{
    if ( size_t n = operator_id_token_span(toks, i) ) return n;
    d.update(toks[i]);
    return 1;
}
```
`DelimDepth` is **forward-declared in include/madc.h:28** (`struct DelimDepth;`)
so the method `delimStepStream` can take it by reference; it's DEFINED in
parser.cpp. (Reference param of an incomplete type is legal in a declaration.)

### 4c. operator-id consumer + stream balancer (Program methods)
- `std::string Program::parseOperatorId(TokenBase *operator_tok, std::vector<TokenBase*> *consumed=NULL)`
  (parser.cpp ~8391; decl madc.h ~1458). Consumes the operator SYMBOL token(s)
  after the `operator` keyword token, returns the canonical name ("operator<",
  "operator()", "operatornew", "operator[]", …). If `consumed`!=NULL, each
  consumed symbol token is appended (the keyword token itself is the caller's).
  Was the free `static parse_operator_name_after_keyword(Program&,…)` — converted
  in step 1; its 5 call sites updated to `parseOperatorId(x)` / `pgm.parseOperatorId(x)`.
- `bool Program::isOperatorIdStart(TokenBase *t)` (parser.cpp ~8440; decl madc.h
  ~1465) — delegates to `token_is_operator_id_start`.
- `void Program::delimStepStream(TokenBase *t, DelimDepth &d, std::vector<TokenBase*> *extra=NULL)`
  (parser.cpp ~8445; decl madc.h ~1470) — STREAM form: `t` already pulled via
  `nextToken()`; if it's an operator-id keyword, `parseOperatorId(t, extra)`
  consumes its symbol(s) from the stream (so a token-collecting caller re-emits
  the full id); else `d.update(t)`.
  ```cpp
  void Program::delimStepStream(TokenBase *t, DelimDepth &d, std::vector<TokenBase*> *extra)
  {
      if ( isOperatorIdStart(t) ) { parseOperatorId(t, extra); return; }
      d.update(t);
  }
  ```

**Two families:** INDEX scanners (walk a captured `vector`/the `deque` by index)
use `delim_scan_step`; STREAM scanners (pull via `nextToken()`) use
`delimStepStream`. Both share `DelimDepth::update` and the operator-id logic.

---

## 5. CRITICAL FACT — `operator` is the keyword token `tkOPEROVER`, not an identifier

`operator` lexes to a KEYWORD token `tkOPEROVER` (`TokenOPEROVER`, lexer.cpp:919,
registered in keyword_map at lexer.cpp:1800). BUT `is_contextual_identifier_token`
(parser.cpp ~16779) and `contextual_identifier_name` (parser.cpp ~16846) BOTH
treat `tkOPEROVER` as a contextual identifier whose name is `"operator"` (the
`case TokenID::tkOPEROVER:` arms). So `token_is_operator_id_start` /
`isOperatorIdStart` correctly catch it. (Verified: `_targ.mad` changed behavior
after migrating `collect_template_argument_spelling`.) Do not "fix" this — it's
working.

One scanner — `skip_template_nonclass_declaration` (parser.cpp ~18528) — has its
OWN operator handling (`if (t->id()==tkOPEROVER) consume_operator_spelling=true`).
Leave it unless you unify it; it already avoids the operator< miscount.

---

## 6. Scanners ALREADY migrated (the 6 commits, steps 1–6)

**operator-id-aware (steps 2–3) — each skips operator-ids via the shared helpers:**
1. `collect_template_argument_spelling` (parser.cpp ~1900) — template-arg instantiation; spelling + tokens_out.
2. `skip_template_id_suffix` (~18410) — skip a `<…>` suffix.
3. `collect_template_class_prefix` (~18965) — partial-spec `struct Name<args> : base`.
4. alias-target collector in `TokenTEMPLATE::parse` (~19631) — `using X = …;` target.
5. `template_id_suffix_end` (~2010, vector index).
6. `skipped_template_function_declarator_name_index` (~18622, vector index).
7. `trim_param_default` (~19214, vector index).
8. `Program::parseFunction` declarator scan (~21100, stream).

**Now ALSO on the `DelimDepth` balancer (steps 4–6) — full chain deleted:**
- `count_queued_call_arguments` (~8160, index) — comma count + break-on-paren-0.
- `split_upcoming_function_params` (~19130, index) — split params on top-level comma.
- `trim_param_default` (~19214, index) — top-level `=` trim.
- `skipped_template_function_declarator_name_index` (~18622, index) — top-level `(` name detect.
- `collect_template_class_prefix` (~18965, **stream** via `delimStepStream`).
- alias-target collector (~19631, **stream** via `delimStepStream`).

Find live balancer callers: `grep -n "delim_scan_step(\|delimStepStream(" src/parser.cpp`.

---

## 7. Scanners REMAINING to migrate (the next subtraction)

`grep -n "== TokenID::tkLT" src/parser.cpp` then check which `++…depth` follow.
As of step 6, the hand-rolled tkLT scanners NOT yet on the balancer:
- `collect_template_argument_spelling` (~1980) — has operator-id handling, but
  still hand-rolls its chain AND has special logic: `tkBSR` at `angle_depth==1`
  splits `>>` into a returned `TokenGT()` (so the outer `<…>` closes and the
  inner `>` is re-injected). TRICKIEST — `DelimDepth::update` collapses `>>` to
  angle-=2; this scanner needs the split. Migrate carefully or leave.
- `template_id_suffix_end` (~2051) — angle-only; returns the index where the
  `<…>` closes (custom return-on-angle-0). Could use `DelimDepth` for the depth
  and check `d.angle==0` after a `>`; angle-only so the other depths are unused.
- `skip_template_id_suffix` (~18423) — angle-only stream; `depth` starts at 1
  (the `<` already consumed). Has operator-id handling. Could use a `DelimDepth`
  pre-seeded `angle=1` + `delimStepStream`, loop until `d.angle==0`.
- `skip_template_nonclass_declaration` (~18528) — has its OWN `tkOPEROVER`
  handling; assess before touching.
- `skip_template_suffix_tokens` (~19192, vector, `idx++` fetch, angle-only).
- `Program::parseFunction` (~21101, stream) — has operator-id handling; still
  hand-rolls paren/square/angle. Could adopt `delimStepStream` (no token collect).
- `datatype_statement_starts_qualified_expr` (~23397, **deque, `i++` fetch**) —
  NOT yet operator-id-migrated. `i++` in the fetch means: `size_t n =
  operator_id_token_span(pgm.tokens, i-1); if (n) { i += n-1; continue; }` after
  the fetch, OR restructure.

NOTE the brace-only scanners are DIFFERENT and do NOT need this:
`cpp_struct_body_needs_class_parser` (~13501, brace-only + member detection,
already handles tkOPEROVER) and `class_body_enum_definition_follows` (~14940,
trivial first-`{`/`;`). These three + `count_queued` were what I WRONGLY called
"3 identical deque loops" — they are DISTINCT functions sharing only trivial
`for/fetch/null-check` boilerplate. Do NOT try to merge them.

---

## 8. The `operator<` bug — root-caused, TWO parts (one fixed-in-progress, one open)

**Symptom:** `operator<` mis-parsed as a template-id in angle-scan contexts; the
`<` is counted as opening a template-argument list, so the scanner runs past the
closing `>` and over-consumes the enclosing scope, surfacing far away (e.g.
"Expecting type in class definition" at the next `namespace`).

**Reduced cases (in `tmp/`, recreate as needed):**
- `R19` FAILS "Unexpected end of data":
  `bool operator<(int,int);` then
  `template<typename T, typename = decltype(operator<(0,0))> struct X {};`
- `R20` OK (control): same with `operator>`. The `<`/`>` asymmetry IS the bug.
- `R21` FAILS: `struct C{bool operator<(const C&)const;};` then
  `template<typename T, typename = decltype(C().operator<(C()))> struct X{};`
- `tmp/_targ.mad`: `Foo<decltype(operator<(0,0))> f;` — template-ARG instantiation.

**Part (a) — angle-scan over-consumption.** Being fixed by the scanner migrations
(§6) — each migrated scanner now skips operator-ids. R19/R21 exercise scanners
NOT yet migrated (template-param-default list parser; member-call), so they STILL
fail until those are migrated.

**Part (b) — operator-id RESOLUTION (OPEN, not started).** After the angle-scan
is fixed, a template-arg `decltype(operator<(…))` yields
`use of undeclared identifier 'operator'`. The expression parser only treats
`operator` as an operator-function-id **inside a class method**:
```
parser.cpp ~11465:
    if ( ident_tb->str == "operator"
      && code && code->method && code->method->owner_class )   // <-- too restrictive
    { member_lookup_name = parseOperatorId(tb); parsed_operator_name = true; }
```
For free / namespace / template / `decltype` context this gate is false, so
`operator` falls through as a bare identifier. Relaxing it needs: free-operator
name lookup (`findVariable("operator<")` — free operators register under
"operator"+sym, see parser.cpp ~5429) AND dependent-name tolerance in
uninstantiated template bodies (the SFINAE `decltype(operator<(…))` detects
*absence* — must not hard-error). The `parsed_operator_name` flag currently forces
a method-only path that throws "use of undeclared member" at parser.cpp ~12694.
This is intricate — design carefully; the operator-overload tests (`testoperator*`)
must not regress.

---

## 9. `stl_tree.h`'s blocker is a SEPARATE bug (NOT operator<)

The real `bits/stl_tree.h` (and thus `<map>`/`<set>`) fails madc-own parse at
"Expecting type in class definition". Via the TRACE_CM trace (§11) the
over-consumption is in `std::less<void>` (`bits/stl_function.h` ~620–643), at the
member alias-template:
```cpp
template<typename _Tp, typename _Up>
  using __ptr_cmp = __and_<__not_overloaded<_Tp,_Up>,
        is_convertible<_Tp, const volatile void*>,
        is_convertible<_Up, const volatile void*>>;   // line ~643
```
This is a member `using X = …<…>>` with nested `>>` — NOT operator< (the operator<
`__not_overloaded` specs at 619–637 are BEFORE it and now parse). A simple
R6-style member alias template parses fine; the 3-arg / `const volatile void*` /
trailing `>>` form does not. **Reduce this separately.** It blocks the container
real-header goal but is orthogonal to the operator< / restoration work.

---

## 10. Gotchas / hard-won learnings (do not relearn these)

- **`stdarg.h` `__gnuc_va_list` fix REGRESSES va_args (8 tests).** Real glibc
  `<wchar.h>` does `typedef __gnuc_va_list va_list;` after `#define __need___va_list`,
  so it needs `__gnuc_va_list` defined. Adding it to the embedded `stdarg.h` —
  even as a direct second typedef of the same struct — broke `testvarargs`,
  `teststdarg2`, etc.: madc's `va_start` intrinsic needs `va_list` to be THE
  defining struct-body typedef, not an alias. **Reverted; not needed** (the
  wchar.h chain is blocked later anyway by the glibc `sys/cdefs.h` preprocessor
  issue — deferred-libc). Don't retry without solving the va_start coupling.
- **`cut+append` bisection is UNRELIABLE.** Truncating the preprocessed file and
  appending a fixed `}` + `int main(){}` gives false signals (the append balances
  differently per cut). Use **deletion bisection** (delete complete depth-0-bounded
  ranges, check if the error CLEARS vs merely shifts by the deleted line count) or
  the **TRACE_CM** trace.
- **The "3 identical deque loops" were NOT duplicated** (§7) — verified by reading
  them. `count_queued_call_arguments` (4-delim + comma count),
  `cpp_struct_body_needs_class_parser` (brace-only + member detect),
  `class_body_enum_definition_follows` (trivial first-`{`/`;`). Don't merge.
- **Line attribution across `#include`s is unreliable** — errors report the top
  `.mad` file with an inner line number. Don't trust `file:line` across includes;
  bisect/trace instead.
- **Real-header worklist context** (separate track, see
  `docs/plans/2026-06-06-realhdr-preprocessor-HANDOFF.md` and the corrected
  `docs/plans/2026-06-06-operator-lt-template-parse-bug.md`): streams family is
  blocked by real libc `<wchar.h>` → glibc `sys/cdefs.h` not fully preprocessed
  by madc (`__nonnull`/`__attribute_nonnull__`/`__glibc_has_attribute` undefined
  because a conditional region is mis-skipped) — DEFERRED libc work. `#include_next`
  is DONE (commit `ed054db`). Containers are the in-scope parser gaps (this work).

---

## 11. Diagnostic tools & verification commands

```bash
# Build + full regression (MUST stay 521/4/0/26 after every change):
make -C src              # build bin/madc + libmadc
make -C src fulltest     # unit (doctest) + all integration tests

# Single header probe (parser vs preprocessor isolation):
printf '#include "/usr/include/c++/13/bits/stl_tree.h"\nint main(){return 0;}\n' > tmp/_t.mad
bin/madc --std=c++17 --emit=c11 tmp/_t.mad 2>&1 >/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -m1 'error:'

# Parser-vs-preprocessor: gcc-preprocess, strip '# line' markers, parser-only:
g++ -std=c++17 -E HDR | grep -v '^#' | bin/madc --std=c++17 --emit=c11 -
#   both columns fail at same construct => PARSER gap; madc-only fail => preprocessor.

# Two-column burn-down harness:
bash scripts/probe_real_headers.sh

# TRACE_CM — find class-member over-consumption (the technique that located the
# stl_tree.h / less<void> over-run). Insert at the TokenCLASS::parse member loop
# (the `while ((tn=pgm.peekToken()) && tn->id() != TokenID::tkClBrc) {` line):
#     #ifdef TRACE_CM
#     std::cerr << "CM " << (ddc?ddc->name:std::string("?")) << " id=" << (int)tn->id()
#               << " @" << (tn->file?tn->file:"?") << ":" << tn->line << "\n";
#     #endif
#   then:  touch src/parser.cpp; make -C src CXXFLAGS="-std=c++11 -Wall -DTRACE_CM"
#   run on the header; the line where the peeked token jumps past the scope is the
#   over-consuming member. REMOVE the trace before committing.

# operator< reducers (recreate in tmp/):  see §8.
```

---

## 12. Metrics

- `src/parser.cpp`: **24,309 lines** (original was 1,401).
- Free `static f(Program&,…)` functions: **138** (was 139; trending toward the
  original's 0).
- Restoration line trend: since the `DelimDepth` helper landed (step 4 onward)
  **+101 / −202 = net −101**; whole session restoration **+200 / −190 = +10**
  (was +112 at step 4 — the extraction overhead is paid off, now subtracting).
- 6 scanners on the shared balancer; the delimiter switch + operator-id logic each
  live in ONE place (were ~10–13 copies).

---

## 13. Commits this session (on `feature/realhdr-parse-gaps2-claude`, local/unpushed)

```
3e12c1b refactor(parser): stream DelimDepth step; collapse 2 stream scanners (step 6)
db370fa refactor(parser): adopt DelimDepth in 2 more index scanners (step 5)
d131b5f refactor(parser): extract DelimDepth balancer; collapse 2 copies (step 4)
2f9a5b2 refactor(parser): shared operator-id helpers + 4 more angle scanners (step 3)
6ebda6e docs(plan): correct diagnosis + record parser-restoration steps 1-2
14d201f refactor(parser): unify 4 angle-bracket scanners onto operator-id primitive (step 2)
193b6af refactor(parser): operator-id parsing as a Program method (step 1)
ddd50d3 docs(plan): root-cause handoff for operator< template-parse bug
ed054db feat(lexer): #include_next directive support
```
(Older real-header-track commits — `4c5c6f9` harness/handoff, the preprocessor
environment + brace-init/opaque-enum parser fixes — are on this branch / `develop`
below these. `develop` is untouched by this session.)

---

## 14. Next threads, in value order (pick one; user has been saying "keep going")

1. **Continue adopting `delim_scan_step`/`delimStepStream` in the remaining
   scanners (§7)** — pure line reduction; the user explicitly chose this ("#1").
   Easiest next: `skip_template_id_suffix`, `template_id_suffix_end` (angle-only),
   `parseFunction`, `datatype_statement_starts_qualified_expr`. Leave
   `collect_template_argument_spelling` for last (special `>>`-split logic).
2. **operator< RESOLUTION end-to-end** (§8 part b, parser.cpp ~11465) — makes
   free/template `operator<` actually work, not just scan. More visible/testable
   but intricate (free-operator lookup + dependent-name tolerance; guard
   `testoperator*`).
3. **The `__ptr_cmp` member-alias-template bug** (§9) — unblocks `stl_tree.h`/
   `<map>`/`<set>` real-header parse.
4. **The big pieces** (the user's original "break down the 1000-line parser"):
   decompose the **3,580-line `parseExpression`** into per-construct sub-methods;
   dedup `TokenSTRUCT::parse` vs `TokenCLASS::parse` (~2,300 lines, near-identical
   member loops); convert more of the **138 free functions → `Program` methods**
   (step-1 pattern: declare in madc.h, move body, `pgm.x`→`x`, update call sites,
   delete the free function — drives the count toward the original's 0).

---

## 15. Methodology / rules to honor (from AGENTS.md + this session)

- **GCC and clang are BOTH canon.** `gcc -S -fverbose-asm -O0` / `clang -S -O0`
  before forming a codegen/parse hypothesis.
- **Fix at the deepest layer; no shims.** **Think twice, code once.** **Understand
  what exists before assuming it doesn't** (search first — the codebase is huge).
- **`make -C src fulltest` after every change** — never leave the tree red.
- **No hard-coding specifics into general machinery**; enums/predicates over
  string compares.
- Restoration-specific: **mirror the original's idioms** (methods on `Program`,
  local stacks, `nextToken()/peekToken()` for state); **never add new
  `pgm`-threading free functions** — delete them as primitives absorb them; keep
  each step small + test-gated; **net lines + free-function count trend DOWN**.
- Commit early; feature branches off `develop`; this branch is `-claude` owned.
- Be honest about metrics and mistakes (e.g. the "3 identical loops" mis-read was
  corrected openly).

---

## 16. Broader campaign context (so you don't lose the forest)

This branch (`feature/realhdr-parse-gaps2-claude`) sits within the long-running
**retire-std-hardcoding** + **real-header parsing** arc. North star: madc parses
*real* system C++/libc headers (retiring curated stubs) and reaches C23/C++23
compliance, all through the one `cir_node`/MC11-IR → c2mir → MIR backend. The
parser restoration directly serves this: the container real-headers (`<map>` etc.)
are blocked by parser gaps, and a clean, reusable parser is the foundation. See
`AGENTS.md`, `docs/plans/madc-vision-and-invariants.md`, and the memory index at
`/home/dev/.claude/projects/-workspace-madc/memory/MEMORY.md` (esp.
`project_north_star_c23_cpp23`, `project_cpp_parser_correctness`,
`feedback_correct_over_shortcuts`, `feedback_dont_cling_to_legacy`).

`develop` is at `110e026`-era and untouched by this session. Do NOT promote to
master (parity gate). Do NOT push this branch unless asked.

---

END OF HANDOFF. On resume: read this fully, run the §11 verify commands, then
continue with §14 thread #1 (or whichever the user directs).
