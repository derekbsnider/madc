# Handoff — `operator<` mis-parse + parser restoration

Date: 2026-06-06/07. Branch: `feature/realhdr-parse-gaps2-claude` (off `develop`).
Status: **restoration underway (steps 1–2 landed); two distinct bugs separated.**

## UPDATE 2026-06-07 — corrected diagnosis + restoration started

The user's architectural point (the parser degraded from the original
method-based design: `a22343e` = 1,401 lines, **0** free functions taking
`Program&`; now 24,299 lines, **139** such free functions, ~13 duplicated
inline `<…>` balancers) is the real fix. Restoration begun, each step
fulltest-gated (521/4/0/26 throughout):

- **Step 1** (`193b6af`): `parse_operator_name_after_keyword` (free fn) →
  `Program::parseOperatorId` method. 139→138 free functions.
- **Step 2** (`14d201f`): add `Program::isOperatorIdStart()` + extend
  `parseOperatorId(consumed)`; migrate 4 angle scanners onto them
  (`collect_template_argument_spelling`, `skip_template_id_suffix`, the
  alias-target collector, `collect_template_class_prefix`). The operator-id
  over-consumption is now fixed in ONE primitive for those paths.

**TWO DISTINCT BUGS, separated by the trace (the earlier single-bug framing below was wrong):**
1. **operator< has two parts:** (a) angle-scan over-consumption — fixed in 4
   scanners (step 2), ~9 inline balancers remain (R19 template-param-default,
   R21 member-call, etc. — enumerate via `grep "== TokenID::tkLT" src/parser.cpp`);
   (b) operator-id **resolution** — after the angle fix, a template-arg
   `decltype(operator<(…))` now yields `use of undeclared identifier 'operator'`
   (expression parser only treats `operator` as an operator-id inside a class
   method, `parser.cpp:11465`). Still pending.
2. **stl_tree.h's actual blocker is NOT operator<.** Trace (TRACE_CM at the
   `TokenCLASS::parse` member loop) shows `std::less<void>` over-consumes at
   `bits/stl_function.h:643` — the **`__ptr_cmp` member alias-template**
   (`template<…> using __ptr_cmp = __and_<__not_overloaded<…>, is_convertible<…>,
   is_convertible<…>>;`), a `using X = …<…>>` with nested `>>`. The operator<
   `__not_overloaded` specs (619–637) are BEFORE it and parse fine now. Diagnose
   this member-alias-template scanner separately (note: a simple R6-style member
   alias template parses OK — the 3-arg / `const volatile void*` / trailing `>>`
   form does not; reduce it).

Below is the original (single-bug) writeup, partially superseded.

---

Status (original): **root-caused + reduced; fix NOT landed (multi-part, multi-scanner).**

## What this blocks

The real-header **container** family (`map`, `set` via `bits/stl_tree.h`) — a
genuine **parser** gap (both columns of `scripts/probe_real_headers.sh` fail).
Distinct from the streams family (deferred-libc `wchar.h`/`cdefs.h`) and from
`memory`/`algorithm` (preprocessor gaps, unclassified).

## Root cause

madc mis-parses the **operator-function-id `operator<`** (and `operator<<`,
`operator<=`). When `operator<` appears in a **template-argument / `decltype`**
context, an angle-bracket matcher counts `operator<`'s `<` as a
template-argument-list opener, scans past the closing `>`, and **over-consumes
the enclosing class scope**. It surfaces far away as
`Expecting type in class definition` at the *next* namespace
(`parser.cpp:15923`, the class-member loop hitting `namespace` where it expects
a member type).

Pinpointed with a class-member trace (insert at the `TokenCLASS::parse`
class-body loop `while ((tn=pgm.peekToken()) && tn->id()!=tkClBrc)`, ~`parser.cpp:15536`,
logging `tn->file:tn->line`; build `CXXFLAGS="... -DTRACE_CLASS_MEMBERS"`).
The over-consuming member is libstdc++ **`std::less<void>`** (`bits/stl_function.h`
~620–643), specifically the member partial-spec:
```cpp
struct __not_overloaded<_Tp, _Up,
  __void_t<decltype(operator<(std::declval<_Tp>(), std::declval<_Up>()))>>
  : false_type { };
```
`std::greater<void>` (uses `operator>`) parses fine — the `<`/`>` asymmetry
confirms it.

## Minimal reproducers

- `R19` (FAILS, "Unexpected end of data"):
  `template<typename T, typename = decltype(operator<(0,0))> struct X {};`
- `R20` (OK, control): same with `operator>`.
- `R21` (FAILS): `template<typename T, typename = decltype(C().operator<(C()))> struct X {};`

## The fix is multi-part (NOT one line)

1. **Angle-bracket scanners must treat `operator`+symbol as a name, not
   delimiters.** `collect_template_argument_spelling` (`parser.cpp:1869`) is ONE
   such scanner — a ~35-line change there (detect `is_contextual_identifier_token(t)
   && contextual_identifier_name(t)=="operator"`, consume `operator` + its symbol
   token, plus the closing `)`/`]` for `operator()`/`operator[]`, **without**
   touching `angle_depth`) eliminates the angle-miscount for template-*argument*
   instantiations. BUT the `stl_tree.h` over-consumption is in a **different,
   still-unpinned scanner**: the class-member **partial-specialization-args**
   parser inside `TokenCLASS::parse`. Find and apply the same operator-id skip
   there.
2. **Operator-id resolution in free/template/`decltype` context.** The
   expression parser only treats `operator` as an operator-function-id **inside a
   class method** (`parser.cpp:11465`, gate `code && code->method &&
   code->method->owner_class`). After (1), template-arg `operator<` parses but
   then errors `use of undeclared identifier 'operator'`. Relaxing the gate needs:
   free-operator name lookup (`findVariable("operator<")`, registered per
   `parser.cpp:5429`) **and** dependent-name tolerance for uninstantiated template
   bodies (the SFINAE `decltype(operator<(...))` detects *absence* — must not hard-error).
   Note the `parsed_operator_name` flag currently forces a method-only path that
   throws `use of undeclared member` at `parser.cpp:12694`.

Each piece compiles; the full fix was not landed (reverted to keep the tree
clean — only `ed054db` #include_next stands). `make -C src` baseline 521/4/0/26.

## Method / tools

`scripts/probe_real_headers.sh`; isolate via
`#include "/usr/include/c++/13/bits/stl_tree.h"`; trace via `-DTRACE_CLASS_MEMBERS`
at the class-body loop. Verify any fix with the R19/R20/R21 reducers AND the real
`stl_tree.h` (both columns), then `make -C src fulltest` (watch `tests/testoperator*`
for operator-overload regressions).
