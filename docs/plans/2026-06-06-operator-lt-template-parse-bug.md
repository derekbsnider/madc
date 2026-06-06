# Handoff — `operator<` mis-parsed as a template-id (container-header parser gap)

Date: 2026-06-06. Branch: `feature/realhdr-parse-gaps2-claude` (off `develop`).
Status: **root-caused + reduced; fix NOT landed (multi-part, multi-scanner).**

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
