# How GCC, Clang, and c2mir structure their preprocessor + parser — and what madc should adopt

**Date:** 2026-06-07. **Branch:** `feature/realhdr-parse-gaps2-claude`.
**Why:** a research pause in the real-header-parsing track. We have been
iron­ing out parser/preprocessor bugs one at a time (multi-star casts,
function-typedef pointer decay, `_GLIBCXX_*` macro gaps, `std::less<void>`
template derailment). This doc steps back and asks how the three reference
front ends we trust — **gcc/g++ (libcpp + gcc/cp)**, **clang/clang++**, and
**c2mir** (madc's *actual backend*) — solve the same problems, so we fix the
**class** of bug structurally instead of one symptom at a time.

Sources: the full gcc tree at `/workspace/gcc` (libcpp, gcc/cp, gcc/c-family),
the full c2mir source at `/workspace/mir/c2mir/c2mir.c` (madc's backend), and
clang 18.1.3 (binary + the official Internals Manual / doxygen). All three were
read at the source level (gcc, c2mir) or confirmed empirically + against docs
(clang). Citations are in the per-topic sections below.

> **New debugging instrument shipped alongside this doc: `madc -E`.** Preprocess
> only — expand `#include`/`#define`/macros and print the resulting token stream,
> then stop (no parse, no codegen). madc's lexer preprocesses during `tokenize()`,
> so the post-PP token stream *is* the preprocessed TU; `-E` reuses the
> `--dump-source` reconstruction over it. Content-only (no `# line` markers) so it
> diffs cleanly against `gcc -E | grep -v '^#'`. This is how every empirical
> finding below was obtained.

---

## TL;DR — the five convergent lessons

All three front ends independently agree on five things. Each maps directly to a
madc bug we have already hit:

1. **Clean phase 1–6 → 7 separation.** Preprocess the *entire* TU into one
   finalized token stream first; the parser is a pure consumer that never
   re-lexes raw source. (gcc buffers all tokens up front; c2mir pushes every
   expanded token into one `recorded_tokens` array then parses by index; clang
   streams on demand but through a single re-injectable `Preprocessor::Lex`
   seam.) madc already does this shape (lexer → `prog->tokens` → parser), which
   is good — the lesson is to **keep the boundary clean and make the token
   buffer mutable + backtrackable** (needed for #5).

2. **Declarators are recursive, one node per `*`/`[]`/`()` — never a flat
   star-count.** This is the single highest-leverage structural lesson. gcc:
   `cp_parser_declarator` consumes ONE ptr-operator, recurses, wraps in ONE
   indirection node (`parser.cc:26515/26557/26568`). c2mir: each `*` is its own
   `N_POINTER` node spliced into an ordered list, type built by walking it
   (`c2mir.c:4986/8021`). clang: `Declarator` = `DeclSpec` + a
   `SmallVector<DeclaratorChunk>` stored **innermost-first**. **Pointer depth =
   recursion depth / list length, counted by construction.** → This is the root
   cause of madc's `(char**)`-collapses-to-`(char*)` cast bug and the
   `SPEC_FUN *p`→`**p` function-typedef decay bug, both of which I fixed this
   session with *arithmetic band-aids* (`max` instead of `+=`; peel-all-levels).
   The structural fix is a recursive declarator model.

3. **`#include_next` = re-run the search for the same name starting at the
   directory *after* the one the current file was found in** — it is NOT a
   filename. gcc: `search_path_head` sets `dir = file->dir->next`
   (`files.cc:1106`). madc *does* implement this (`lexer.cpp:1607`
   `resolve_include_next_path`); c2mir does **not** (zero hits) — a gap madc must
   own for system headers, and already has.

4. **Predefined macros + the include search path are an explicit, ordered,
   target-aware configuration** — not a hardcoded guess. gcc merges quote →
   bracket → system → after into one `cpp_dir` chain (`incpath.cc:353`) including
   the **multiarch triplet dir**; it injects `__GNUC__`/`__cplusplus`/`__cpp_*`
   from `c-cppbuiltin.cc:904` keyed off the standard. libstdc++'s `_GLIBCXX_*`
   macros are *computed by the headers* from those base macros, so getting the
   base set + the search path right is a hard prerequisite. → Directly explains
   the empirical finding below: madc's default search path was missing the
   multiarch dir, so `<bits/c++config.h>` wasn't found and ~230 `_GLIBCXX_*`
   macros stayed unexpanded.

5. **struct ≡ class: one grammar production, differing ONLY in default access.**
   gcc: one `cp_parser_class_key`; the *only* divergences are default member
   access (`class.cc:8686`: private for `class`, public for struct/union) and
   default base access (`decl.cc:18591`). clang: one `ParseClassSpecifier`,
   class-key sets `AS_public` vs `AS_private`. c2mir: `class` is literally a
   synonym for `struct` (`c2mir.c:4694`). → Validates the user's struct/class
   directive and this session's struct-method work: route both through one
   record parser, carry one `is_declared_class` bool that picks the initial
   default access, branch on nothing else.

---

## 1. Translation-phase separation (PP vs parse)

| Front end | Model | Key evidence |
|-----------|-------|--------------|
| **gcc/cp** | Buffer ALL tokens up front, then parse from the buffer by pointer | `cp_lexer_new_main` drains the whole PP stream into `lexer->buffer` (`parser.cc:761`, loop `:791`); parser peeks/consumes via pointer arithmetic (`:1252`) |
| **c2mir** | Preprocess entire TU → one `recorded_tokens` VARR, then parse by index | `pre()` runs to completion (`c2mir.c:18822`) *before* `parse()` (`:18827`); `read_token` indexes the array (`:4013`) |
| **clang** | Lex on demand *through* the preprocessor — streaming, not buffered | Parser calls `Preprocessor::Lex(Token&)` one token at a time; `#include`/macros/conditionals resolved lazily one layer down |

**The split that matters:** gcc and c2mir fully materialize a token buffer (phases
1–6) before the parser (phase 7) runs. Clang streams, but crucially through a
*single re-injectable seam* — which is what lets it splice parser-synthesized
**annotation tokens** back into the stream (see §5). All three keep the parser
from ever re-lexing raw source.

**madc today:** the lexer preprocesses during `tokenize()` into `prog->tokens`,
and the parser consumes that — i.e. madc is already in the gcc/c2mir "buffer
then parse" camp. **Good.** Two refinements the research recommends:
- Keep the PP→parse boundary strict (no re-lexing in the parser).
- Make the token buffer **mutable and cheaply backtrackable** (save/restore a
  cursor index, like c2mir's `record_start`/`record_stop` at `:4020`, or gcc's
  `cp_parser_parse_tentatively`). madc needs this for §5.

---

## 2. Declarator parsing — the highest-leverage fix

**The bug class:** madc miscounts pointer levels because it infers depth
ad-hoc (a counter, or re-deriving from spelling) instead of modeling each
derivation as a node. Symptoms fixed this session as band-aids:
- `(char **)x` emitted as `(char *)x` — `TokenCast` peeled one level, appended
  one `*` (fixed `f52c3ad` by peeling/emitting ALL `DataDefPTR` levels).
- `SPEC_FUN *p` (function-typedef param) emitted as `SPEC_FUN **p` —
  `explicit_star_count` *summed* the implicit decay star with the explicit one
  (fixed `c13c931` by `max` instead of `+=`).
- Known-still-broken: global `FT **g` (ptr-to-fnptr via fn typedef) undercounts.

**How the canon compilers make this impossible:**

- **gcc** (`cp_parser_declarator`, `parser.cc:26515`): consume exactly ONE
  ptr-operator (`cp_parser_ptr_operator:27301`, one `*`/`&`/`&&` per call),
  **recurse** into `cp_parser_declarator` for the rest (`:26557`), then **wrap**
  the result in exactly ONE indirection node (`cp_parser_make_indirect_declarator:4682`).
  `int ***p` → three recursion frames → three wrappers. `int (*fp)(void)`
  interleaves a parenthesized declarator with the function declarator so `*`
  binds to the right sub-declarator. A cast `(int **)` reuses the *same* code on
  an abstract declarator.
- **c2mir** (`c2mir.c:4986` `pointer`): consumes one `*`, builds one `N_POINTER`,
  **recurses** if another `*` follows, `op_append`s itself onto the list. The
  consumer `check_declarator` (`:8021`) walks the `N_LIST` head-to-tail, one
  `struct type` per op, chained by recursive `append_type` (`:7968`).
  Function/array→pointer **decay** is an explicit `adjust_param_type` (`:7996`)
  that wraps in a fresh `TM_PTR` — exactly the function-typedef case madc got wrong.
- **clang**: `Declarator` holds a `SmallVector<DeclaratorChunk>` stored
  **innermost-first** ("TypeInfo #0 is closest to the identifier"); `ParseDeclaratorInternal`
  adds prefix chunks *after* recursing and postfix (`[]`,`()`) chunks in a loop;
  Sema folds chunks from #0 outward. Each `*` is exactly one `Pointer` chunk.

**Recommendation for madc:** adopt the recursive-declarator / ordered-derivation-node
model. Represent each `*`/`&`/`[]`/`(params)` as a distinct ordered node (this is
also exactly what c2mir's `check_declarator` *expects* as input, so it aligns
node-for-node with the backend). Then: multi-star is N nodes, `(*fp)(void)` is a
pointer node *inside* a paren wrapping a function node, function-typedef decay is
"a Function node under a Pointer node" — and the entire off-by-one-star bug class
becomes structurally impossible. Trailing return types (§5) get a natural home on
the Function node. **This is the single biggest structural change the research indicates.**

---

## 3. `#include` / `#include_next` / re-inclusion

- **gcc:** one merged `cpp_dir` linked list, order quote→bracket→system→after
  (`incpath.cc:353`), built to include the **multiarch triplet dir** and
  `/usr/include`. `#include_next` = `search_path_head` starting at
  `file->dir->next` (`files.cc:1106`). Header-guard / `#pragma once` caching via
  `MultipleIncludeOpt` avoids O(n²) re-lexing.
- **clang:** `HeaderSearch::SearchDirs` vector + boundary indices
  (`AngledDirIdx`, `SystemDirIdx`); `LookupFile(FromDir=...)` powers
  `#include_next`; `MultipleIncludeOpt` + `isPragmaOnce` + controlling-macro
  record skip re-reads.
- **c2mir:** has header search (`get_include_fname:2530`, dirs from
  `init_include_dirs:18640`) but **no `#include_next`** (0 hits) and **no
  pragma-once / guard caching on Linux** — relies on `#ifndef` guards. A gap
  madc cannot inherit.
- **madc today:** *does* implement `#include_next`
  (`lexer.cpp:1607 resolve_include_next_path`, "search only entries AFTER the
  current dir") — good, and ahead of c2mir.

**Empirical finding (via `madc -E`):** madc's *default* C++ search path was
missing the **multiarch dir** `/usr/include/x86_64-linux-gnu/c++/13`, where the
target-specific `bits/c++config.h` actually lives (`/usr/include/c++/13/bits/c++config.h`
does **not** exist). Result: `memory`'s transitive `#include <bits/c++config.h>`
found nothing, so **~230 `_GLIBCXX_*` macro uses stayed unexpanded** (gcc expands
all to zero). Adding `-I/usr/include/x86_64-linux-gnu/c++/13` dropped survivors
to ~53 and expanded all the structural ones (`NOEXCEPT`, `BEGIN/END_NAMESPACE_VERSION`,
`CONSTEXPR`, `STD_C`). → **madc should auto-discover and prepend the gcc multiarch
C++ include dirs to its default search path** (query `gcc -print-multiarch` /
`g++ -E -x c++ - -v`), the way every real build does. The residual ~53 survivors
(`_GLIBCXX_RESOLVE_LIB_DEFECTS` etc.) indicate a further include-chain/conditional
gap to chase next — but the multiarch path is the dominant fix.

---

## 4. Macro expansion + feature-test macros

- **Function-like vs object-like:** gcc gates function-like expansion on a `(`
  lookahead with backup (`funlike_invocation_p`, `macro.cc:1463`) — a macro name
  not followed by `(` stays a plain identifier. clang/c2mir do the same.
- **Rescanning** is cleanest as a **context/source stack** (gcc `cpp_get_token_1`
  loop `macro.cc:3006`; clang `TokenLexer` stack; c2mir pushback rescanner with
  `T_EOR` sentinels + `ignore_p` "blue paint" no-self-recursion). Pushing
  replacement lists as new sources makes nested/recursive expansion fall out
  naturally.
- **`#` / `##`:** stringize operates on the **un-expanded** argument spelling;
  paste concatenates spellings then **re-lexes** the result as one token (gcc
  `paste_tokens` re-lexes via `_cpp_lex_direct`, `macro.cc:1056`; c2mir `do_concat`
  + `T_PLM` placemarkers). Getting this *ordering* right (substitute-then-expand
  vs `#`/`##` on raw spelling) is the subtle part hand-rolled PPs get wrong.
- **Predefined macros:** inject as **`#define` text run through the normal PP**,
  not lexer special-cases. gcc: `c-cppbuiltin.cc:904` defines
  `__GNUC__`/`__GNUG__`/`__cplusplus`/`__STDC_VERSION__`/`__cpp_*` keyed off the
  standard. clang: a synthetic `<built-in>` buffer lexed before the main file.
  c2mir: per-arch `standard_includes[]` string streams — but **no `__GNUC__`,
  `__cplusplus`, `__has_feature`, or `_GLIBCXX_*`**.
- **`__has_include` / `__has_include_next`:** reuse the include-search machinery
  inside the `#if` evaluator (gcc `builtin_has_include`→`_cpp_has_header`,
  `macro.cc:432`).

**madc:** `-E` confirmed the primitives work — `#if 1`, `#if 0/#else`,
`#if __cplusplus >= 201103L`, and `__cplusplus`→`201703` under `--std=c++17`
(undefined→0 in default mode), object/function macros, `#`/`##`. The gaps to
verify/own: predefine the **full** `__GNUC__` / `__cpp_*` / `__STDCPP_*` set keyed
off the `--std=` enum (matches madc's no-hardcoded-standards invariant), and make
`__has_include`/`__has_include_next` run against the real (multiarch-aware) search
path — libstdc++ headers branch heavily on all of these.

---

## 5. Template parsing — the `<` ambiguity, specialization, trailing return

madc derails on `std::less<void>` (the `stl_function.h` blocker for all 5 streams
headers) and on `constexpr auto f() -> decltype(...)`. The canon fixes:

- **`<` / `>` / `>>` disambiguation.**
  - gcc: a parser flag `greater_than_is_operator_p` (`parser.cc:4724`), set
    **false** inside a template-argument list (`:36892`) so the first non-nested
    `>` ends the list; the `>>` case **mutates the token in place to `>`** and
    leaves one `>` unconsumed (`:36934`). madc's token buffer is mutable, so the
    in-place `>>`→`>` trick is directly available.
  - clang: do **name lookup at the point you see `identifier <`**; if it's a
    template, parse the whole `foo<...>` eagerly and **replace those tokens with
    one `annot_template_id` token** spliced back into the stream. A later `<` is
    then unambiguous. (Requires the re-injectable token stream from §1.)
- **Explicit/partial specialization** `template<> struct less<void>`: gcc
  `cp_parser_explicit_specialization` (`parser.cc:22235`) recognizes the empty
  `template <>` by `CPP_LESS` immediately followed by `CPP_GREATER`; the
  `less<void>` part is parsed by the normal template-id machinery
  (`cp_parser_template_id:21135`). **Not** by special-casing the name.
- **Trailing return** `auto f(...) -> decltype(...)`: on `->` (gcc `CPP_DEREF`,
  clang `tok::arrow`) after the parameter clause, parse a type-id as the return
  type (gcc `cp_parser_trailing_type_id:27957`, threaded via `is_trailing_return`;
  clang `ParseTrailingReturnType`). It lives **inside declarator parsing** (on the
  function chunk) — which is another reason to adopt the §2 recursive declarator.

**Recommendation:** the two derailers have clean, well-scoped fixes once madc has
(a) the recursive declarator (§2, gives trailing-return its home) and (b) a
mutable backtrackable token buffer (§1, gives `>>`→`>` and tentative parse). For
`less<void>`: a real template-id parser + empty-`template<>` recognition, plus the
`greater_than_is_operator_p` flag. clang's annotation-token approach is the
gold standard but heavier; gcc's flag + in-place `>>` mutation is the cheaper
first step and fits madc's mutable buffer.

---

## 6. The C++ → C11 boundary (what madc must own before c2mir)

c2mir is strict **C11** (+ GCC extensions + fork primitives `_Complex`,
`__attribute__((cleanup))`, `N_DEFER`, and `class` as a bare struct synonym). It
has **no** `template`/`namespace`/`reference`/`new`/`delete`/`throw`/vtable nodes
(`c2mir_node_code.h`), and **no** C++ feature-test macros. So madc's front end
must fully resolve all C++ (templates instantiated, classes → struct+vtable+fns,
namespaces/overloads mangled, references → pointers, exceptions → setjmp/longjmp,
`new`/`delete` → calloc+ctor/free+dtor) into the C11 node vocabulary **before**
handing the cir_node tree to c2mir. This is consistent with `c11-transpiler.md`
and `lowering-vs-raising.md` and is *unchanged* by this research — it just
confirms the division of labor.

---

## Prioritized action list (derived, not yet scheduled)

1. **Multiarch include path** (small, high value): auto-discover and prepend the
   gcc multiarch C++ dirs to madc's default search path. Directly unblocks
   `_GLIBCXX_*` macro capture. *(Quick win — verify the residual ~53 survivors
   next.)*
2. **Recursive declarator model** (large, highest leverage): replace ad-hoc
   pointer-level counting with one ordered derivation node per `*`/`&`/`[]`/`()`,
   composed by recursion/list-walk. Kills the multi-star / fnptr-decay bug class
   structurally (retiring this session's `max`/peel-all band-aids) and gives
   trailing-return-types a home. Aligns node-for-node with c2mir's
   `check_declarator`.
3. **Mutable backtrackable token buffer** (medium): cheap cursor save/restore +
   in-place token edit. Prerequisite for template `>>`→`>` and tentative parse.
4. **Template-id parser + empty-`template<>`** (medium): unblocks `std::less<void>`
   → all 5 streams headers. Depends on #3.
5. **Predefined-macro completeness** (small): full `__GNUC__`/`__cpp_*`/`__STDCPP_*`
   set keyed off `--std=`; `__has_include_next` against the real search path.

Items 1 and 5 are quick wins; 2 is the structural keystone; 3+4 unblock the
streams. None are scheduled here — this doc is the research basis for choosing
the next target with the user.

---

## Appendix — `madc -E` usage

```bash
bin/madc -E file.mad                       # print preprocessed token stream, no parse
bin/madc --std=c++17 -E probe.mad          # in C++17 mode (sets __cplusplus=201703)
# diff madc's preprocessing against gcc's to localize a divergence:
diff <(bin/madc --std=c++17 -E probe.mad 2>/dev/null) \
     <(g++ -std=c++17 -E header 2>/dev/null | grep -v '^#')
```

Implemented in `src/madc.cpp` as a synonym for the `dump_source` path (madc
preprocesses during `tokenize()`; the post-PP token stream is the preprocessed
TU). Content-only, no `# line` markers, for clean diffing.
