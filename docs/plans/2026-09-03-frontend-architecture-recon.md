# Recon — how gcc and clang structure multiple input languages and C++ variants

Date: 2026-09-03. Owner ask: before decomposing parser.cpp, look at how the
two canon compilers organize (a) several input languages over one middle
end and (b) the C/C++ standard variants, so the split serves the polyglot
transpiler vision (docs/plans/madc-vision-and-invariants.md, I1–I8) instead
of freezing today's shape. gcc facts read from the local tree
(/workspace/gcc, 2026-09 trunk); clang facts from its tree layout as of
LLVM 18–20 (`clang/include/clang/Basic/*.def`, `clang/lib/{Lex,Parse,Sema,
AST,CodeGen}`).

## 1. Languages — two answers, one lesson

**gcc: one front end per language, one hook interface.** Each language is
a directory (`gcc/c`, `gcc/cp`, `gcc/fortran`, `gcc/go`, `gcc/rust`, `gcc/d`,
`gcc/ada`, `gcc/m2`, `gcc/objc`, `gcc/objcp`, `gcc/jit`, `gcc/lto`) that
fills in `struct lang_hooks` (`gcc/langhooks.h`: ~117 function pointers in
sub-structs `lang_hooks_for_types` / `_for_decls` / `_for_tree_dump` /
`_for_lto`; `langhooks-def.h` supplies the defaults). The middle end never
names a language; it calls hooks (`parse_file`, `type_for_size`,
`pushdecl`, `getdecls`, ...). The driver picks the front end by file
suffix or `-x`. The C FAMILY shares `gcc/c-family/` (options, pragmas,
attributes, builtins, format checking, PCH — 23 files) and libcpp (the
preprocessor and lexer), while C and C++ keep separate grammars
(`gcc/c/c-parser.cc` 31k lines, `gcc/cp/parser.cc` 58k).

**clang: one front end for the whole C family, separate front ends for
everything else.** One `Parser` and one `Sema` serve C, C++, Objective-C,
OpenCL, CUDA and HIP, gated everywhere by `LangOptions`. Swift, Rust,
Flang (Fortran) and the MLIR-based front ends are NOT clang plug-ins: they
are separate front ends that meet LLVM at the IR. Flang's own pipeline
(parse tree -> semantics -> FIR, an MLIR dialect -> LLVM IR) is a
progressive-lowering IR: high-level while it needs to be, lowered when it
must be — the same posture as MC11-IR (`.claude/rules/mc11-ir.md`).

**Lesson.** Neither compiler ever handles a dialect of one language with a
second parser: variants of C/C++ are FLAGS over one grammar. Languages
that are not C-shaped are separate front ends behind an interface, meeting
the shared semantic/IR layer. That is exactly the vision doc's split:
madc's C/C++/dialect reader is one front end; TypeScript, Python, Ruby are
new front ends over the same core and the same `cir_node`.

## 2. Standard variants — tables, not code paths

**libcpp `lang_defaults`** (`libcpp/init.cc:119`): one ROW per language
level (`CLK_GNUC89 ... CLK_GNUC2Y, CLK_STDC89 ... CLK_STDC2Y, CLK_GNUCXX98
... CLK_CXX26, CLK_ASM`), one COLUMN per feature (c99, cplusplus,
extended_numbers, extended_identifiers, c11_identifiers, xid_identifiers,
std, digraphs, uliterals, rliterals, user_literals, binary_constants,
va_opt, scope, dfp_constants, ...). `cpp_set_lang` copies the row into
`cpp_options`. Adding a standard = adding a row.

**gcc reserved words** (`gcc/c-family/c-common.cc:395`
`c_common_reswords[]`): every keyword carries a D_ mask
(`c-common.h:438`: D_CONLY, D_CXXONLY, D_C99, D_C23, D_CXX11, D_CXX20,
D_CXX26, D_EXT, D_EXT89, D_EXT11, D_ASM, D_OBJC, D_CXX_CONCEPTS,
D_CXX_COROUTINES, D_CXX_MODULES, D_CXX_CHAR8_T, D_TRANSMEM). The lexer
consults the mask against the active dialect; the parser never spells a
keyword's availability. `enum cxx_dialect` (`c-common.h:736`: cxx98=cxx03,
cxx0x=cxx11, cxx14, cxx17, cxx20, cxx23, cxx26) is an ORDERED enum and the
C++ grammar gates by version floor — 153 `cxx_dialect` tests in
`cp/parser.cc`, 34 `flag_isoc*` tests in `c/c-parser.cc`. `c-opts.cc`
`set_std_cxxNN()` sets the enum plus the libcpp row; `c-cppbuiltin.cc`
derives the `__cpp_*` feature macros from the same enum.

**clang tables**: `Basic/LangStandards.def` — one `LANGSTANDARD(id, name,
lang, desc, features)` line per standard with a feature bitmask
(LineComment, C99, C11, C17, C23, CPlusPlus, CPlusPlus11..26, Digraphs,
GNUMode, HexFloat, OpenCL); `Basic/LangOptions.def` — every knob a
`LANGOPT(Name, bits, default, desc)`; `Basic/TokenKinds.def` — every
keyword a `KEYWORD(name, KEYALL | KEYC99 | KEYC23 | KEYCXX | KEYCXX11 |
KEYCXX20 | KEYGNU | KEYMS | KEYOPENCL | ...)` and `IdentifierTable.cpp
getKeywordStatus()` turns (flags, LangOptions) into
enabled / disabled / extension / future — the "future keyword" state is
how clang warns that an identifier will become a keyword in a later
standard.

**Lesson.** Both compilers keep the standard as DATA in three tables —
per-standard feature row, per-keyword availability mask, per-feature
macro — and one ordered enum for version floors. madc already has the
ordered enum (`Program::LanguageStd`, madc.h:4709, `is_c_mode()` /
`is_cpp_mode()` / `cpp_keyword_active(min_std)` as version floors,
`default_cpp_std` as the presented bar, `gnu_dialect` as the strictness
modifier) and the keyword gate. What it does not yet have as ONE table is
the per-standard feature row (today: predicates on the enum scattered
where each feature is parsed) and the per-keyword mask as data rather than
per-registration `cpp_keyword_active(STD_CPPnn)` calls. Both are the
`lang_defaults` / `TokenKinds.def` shape and belong to the decomposition's
front-end layer.

## 3. Parser / Sema — the seam that makes the files

**clang** separates recognizing syntax from meaning: `Parser` (in
`lib/Parse`, ~15 files: ParseDecl, ParseDeclCXX, ParseExpr, ParseExprCXX,
ParseStmt, ParseTemplate, ParseTentative, ParseInit, ParseObjc, ParseOpenMP,
ParsePragma, ParseCXXInlineMethods, ParseStmtAsm) knows tokens and grammar
and calls `Sema::ActOn*`; `Sema` (in `lib/Sema`, ~60 files: SemaDecl,
SemaDeclCXX, SemaExpr, SemaExprCXX, SemaTemplate, SemaTemplateDeduction,
SemaTemplateInstantiate, SemaTemplateInstantiateDecl, SemaOverload,
SemaLookup, SemaType, SemaInit, SemaCast, SemaAccess, SemaExceptionSpec,
SemaLambda, SemaConcept, SemaCoroutine, SemaModule, ...) owns types,
lookup, overload resolution, template instantiation and builds the AST;
`CodeGen` consumes the AST. Parser never builds a type; Sema never reads a
token.

**gcc/cp** keeps grammar and semantics in one front end but in files by
CONCERN: parser.cc (grammar), pt.cc (templates, 34k), call.cc (overload
resolution and conversions, 15k), decl.cc / decl2.cc (declarations),
name-lookup.cc, class.cc (layout, vtables), typeck.cc / typeck2.cc (type
checking), semantics.cc, init.cc, method.cc (implicit members),
constexpr.cc, lambda.cc, mangle.cc, rtti.cc, except.cc, cvt.cc, search.cc
(base lookup), module.cc, coroutines.cc, contracts.cc, friend.cc, error.cc.

**Lesson for parser.cpp (66k lines = clang's Parse + Sema + gcc's whole
cp/ minus codegen).** The file map to borrow is gcc/cp's concern list
crossed with clang's Parser/Sema boundary:

| madc today (parser.cpp)                                   | concern file               | clang / gcc analogue            |
|-----------------------------------------------------------|----------------------------|---------------------------------|
| parseStatement / parseDeclaration / parseFunction / Token*::parse | parser_cxx_*.cpp (front end) | Parse*.cpp / cp/parser.cc |
| instantiate_template_use, tsubst spine, pending completion, fn templates | sema_template.cpp | SemaTemplate*.cpp / cp/pt.cc |
| overload ranking, score_arg_to_param, free-operator lanes, conversions | sema_overload.cpp | SemaOverload.cpp / cp/call.cc |
| resolve_named_datadef, namespaces, using, ADL, class-scope aliases | sema_lookup.cpp | SemaLookup.cpp / cp/name-lookup.cc |
| the builtin-spelling table, proven_scalar_identity, canonical spellings, typedef_alias_spelling | sema_type.cpp | SemaType.cpp / cp/typeck.cc |
| class layout, vtables, ctors/dtors, has_user_ctor, bases                | sema_class.cpp             | cp/class.cc + method.cc         |
| mangling                                                  | mangle (exists: madc_mangle.cpp) | cp/mangle.cc               |
| forest thaw hooks, lazy registration, registries          | persistence (exists: cir_freeze.cpp, pch.cpp) | c-pch.cc / clang modules |

The seam rule: files right of the front-end row contain no token reads;
front-end files contain no type formation. A front end for another
language adds `parser_<lang>_*.cpp` and a `TokenSource`; it never adds a
`sema_*` file.

## 4. What this fixes in the plan

- The vision's I-invariants gain a FILE boundary the gates can check
  (a `sema_*` file that includes the lexer, or a `parser_*` file that
  forms a type, is a violation a grep finds).
- `LanguageStd` grows toward `lang_defaults`: one row per standard, the
  scattered `language_std >= STD_CPPnn` predicates become column reads;
  the keyword registry becomes the `TokenKinds.def` mask table. New
  language front ends select their grammar through the same enum
  (gcc's `-x`).
- `Program` sheds roles in the order the seams are clean: TokenSource
  first (the lexer already has one consumer), then the services (lookup,
  overload, template, type, class), each a behaviour-preserving move with
  the battery as the oracle, exactly gcc/cp's per-concern files.
- Duplicates surface as the split proceeds: a helper needed by two
  concern files must be declared once in the internal header — the
  textual-clone half `jscpd` reports, the semantic half `/dupaudit` does.

## Sources

- /workspace/gcc: gcc/langhooks.h, gcc/langhooks-def.h, libcpp/init.cc,
  libcpp/include/cpplib.h (enum c_lang), gcc/c-family/c-common.{h,cc},
  gcc/c-family/c-opts.cc, gcc/cp/*.cc (sizes via `wc -l`).
- clang: clang/include/clang/Basic/{LangStandards.def, LangOptions.def,
  TokenKinds.def}, clang/lib/Basic/IdentifierTable.cpp (getKeywordStatus),
  clang/lib/{Parse,Sema} file lists.
