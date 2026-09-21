# CTAD — class template argument deduction: design and sequencing

Status: **design settled by recon, not started.** Written 2026-09-18 after the
owner asked how gcc and clang handle this, in response to a suggestion to add
an `UnresolvedCtadNode` holding the argument expressions.

## Why CTAD — measured, not assumed

`scripts/run_gcc_testsuite.py --gxx-dirs` (added d61af6f42) measured each
standard's OWN additions. 0 runtime failures anywhere except 1 in C++14:

| standard | dirs | in scope | passing | rate |
|---|---|---:|---:|---:|
| C++11 | cpp0x + template | 1950 | 1369 | 70.2% |
| C++14 | cpp1y | 417 | 196 | 47.0% |
| C++17 | cpp1z | 308 | 118 | 38.3% |
| C++20 | cpp2a | 653 | 336 | 51.5% |

Cumulative C++17 (cpp0x+cpp1y+cpp1z+template under `--std=c++17`):
**1679/2675 = 62.8%** — the number to quote for "madc supports C++17".

**CTAD is 63 of C++17's 190 compile failures (33%).** It is ABSENT, not
partial:

    template<class T> struct A { T v; A(T x) : v(x) {} };
    A a(42);     // g++ -std=c++17: 42
                 // madc: error: use of undeclared identifier 'A'

The parser never recognizes `TemplateName var(...)` / `var{...}` as a
DECLARATION, so the name falls through to expression parsing. Aggregate CTAD
(`Agg a{1,2}`, C++20) fails identically — the missing declaration form is
shared by both, and is where the risk lives.

## Recon — gcc and clang AGREE

Sources: /workspace/gcc/gcc/cp (full), /workspace/llvm-clang-src/clang
(AST + Sema; Serialization not vendored).

**GCC — the placeholder IS an `auto` node** (gcc/cp/pt.cc:31090):

    /* ... represented as an 'auto' with the special level 0 and
       CLASS_PLACEHOLDER_TEMPLATE set.  */
    tree make_template_placeholder (tree tmpl)
    {
      tree t = make_auto_1 (auto_identifier, false, /*level=*/0);
      CLASS_PLACEHOLDER_TEMPLATE (t) = tmpl;
      TYPE_CANONICAL (t) = canonical_type_parameter (t);
      return t;
    }

    /* For a C++17 class deduction placeholder, the template it represents. */
    #define CLASS_PLACEHOLDER_TEMPLATE(NODE) \
      (DECL_INITIAL (TYPE_NAME (TEMPLATE_TYPE_PARM_CHECK (NODE))))
                                                -- gcc/cp/cp-tree.h:6718

No new tree code: a TEMPLATE_TYPE_PARM (the same node kind as `auto`) at
level 0, carrying the template.

**Clang — a deduced TYPE sharing `auto`'s base**
(clang/include/clang/AST/Type.h:5501):

    class DeducedTemplateSpecializationType : public DeducedType, ... {
      /// The name of the template whose arguments will be deduced.
      TemplateName Template;

`AutoType` and `DeducedTemplateSpecializationType` both derive from
`DeducedType`.

### The two conclusions

1. **The placeholder carries ONLY the template — never the argument
   expressions.** Both compilers keep the arguments in the initializer / call
   expression where they already live; deduction runs over them when the type
   resolves. Copying them into the placeholder gives that state two owners.

2. **Serialization is free because the node is not new.**
   `grep -c CLASS_PLACEHOLDER_TEMPLATE gcc/cp/module.cc` = **0**. GCC's module
   and PCH streaming special-case the CTAD placeholder NOWHERE — being an
   ordinary TEMPLATE_TYPE_PARM it rides the generic tree path. This is the
   answer to the frozen-forest question: reuse an already-serializable node
   kind and the frozen form needs no new record kind, no new streaming, no
   version bump.

## ⚠️ THE LANDMINE — guides are DISCARDED today, and re-registering them is a known regression

`skipped_template_decl_is_deduction_guide` (src/parser.cpp:58254) exists to
THROW GUIDES AWAY. From 4dc3f2e02 (2026-08-01):

> libc++ ships CTAD deduction guides (array:379, vector:969, string:365)…
> `register_skipped_namespace_template_function` treated them as bodyless
> function templates… and a namespace Variable named after the class template.
> That phantom VALUE shadowed the TYPE reading of the same name in every
> value-lookup lane — under `using namespace std`, `void f(array &ctx, ...)`
> read `array & ctx` as an expression: ns_madc:56 "use of undeclared
> identifier 'ctx'", **killing 4 of the 5 remaining madc-eval tests**.

It took two attempts — 915b129b guarded only the statement classifier.
Gate: `tests/testdeductionguide.mad` (green). The same commit anticipated this
work: *"guides can be stored when CTAD lands."*

**So the frozen forest holds NO guides at all today**, and the discard happens
in TWO places: the registrar AND its forest-restore twin
`recapture_free_overload_surfaces`.

## SEQUENCING

1. **Retain guides as a DISTINCT record kind** — parse path and
   `recapture_free_overload_surfaces` both. Distinct specifically so it cannot
   mint a namespace VALUE; `tests/testdeductionguide.mad` is the existing gate
   that proves it did not.
2. **Recognize the deduced-declaration form** (`TemplateName var(...)` /
   `var{...}`) and mint an `auto`-family placeholder carrying ONLY the
   template. ⚠️ `DataDefAUTO`/`ddAUTO` (include/datadef.h:2163) is a SINGLETON;
   GCC mints one per placeholder — that is the one real adaptation.
   Serialization then rides the existing forest TYPE ARENA.
3. **Guide formation** — CONSTRUCTOR-derived first (66 of the 128 cpp1z
   class-deduction tests have explicit ctors, 44 carry user guides; 63 failing
   total), AGGREGATE-derived after (~17 tests, all in cpp2a — only 1 cpp1z
   test mentions aggregates).

Deferred deduction inside a template rides the EXISTING parse-once tsubst
spine's **construction** KIND (`.claude/rules/parse-once.md`), not a new
deferral mechanism.

## DO NOT

- Do not store argument expressions in the placeholder (neither canon does).
- Do not add a new AST node — `.claude/rules/mc11-ir.md` is SET IN STONE
  (`cir_node` derives from c2mir's `node_t`; no competing tree type), and a new
  node would need new arena records + streaming that reusing `auto` avoids.
- Do not re-register guides as skipped function templates. That is 4dc3f2e02's
  exact regression, blast radius = every `using namespace std` value lookup.
- Do not lead with aggregate CTAD on the theory that "C++20 is 51% done" —
  that figure is a pass rate on a corpus exercising mostly pre-C++20
  machinery, and says nothing about aggregate deduction.

## RELATED DEFECT — the feature-test macros lie (fix independently)

`src/predefined_macros.cpp` defines 51 `__cpp_*` macros with ZERO `--std=`
gating. Two distinct problems:

- **`__cpp_deduction_guides = 201703L` is advertised while guides are
  discarded.** libstdc++ and libc++ both guard CTAD-dependent code on it, so
  madc tells headers to emit CTAD it cannot parse — in EVERY mode, including
  the C++11 lane. One-line fix, independent of implementing CTAD.
- **C++17 macros leak into C++11/C++14 modes** (madc reports 201703 under
  `--std=c++11`; g++ leaves it undefined). `if constexpr` is the same shape but
  genuinely WORKS, so this is mis-gating, not a lie — it violates the "--std=
  determines semantics" law while only making madc more permissive.
  Wants a standard-level column on the macro table. Own slice.
