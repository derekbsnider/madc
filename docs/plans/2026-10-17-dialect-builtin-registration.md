# The dialect is builtin — generated registration code, no image

**Date:** 2026-10-17
**Status:** PLAN — replaces Part B (B4–B6) of
`docs/plans/2026-10-16-dialect-per-name-plan.md` and retires its dialect image;
next in the 0.106.0 arc (section I), ahead of section J.
**Decisions:** KG `dialect_builtin_init_code`, `dialect_image_dialect_only`.

## Owner rulings (2026-10-17, verbatim)

- "to have a frozen registry that needs to be unfrozen and loaded adds extra
  overhead and also confusion, no? would it not be better to use a hardcoded
  initialization function for things that are 'builtin'?"
- "anything that the parser does should be able to be done directly in code
  without parsing"
- "the dialect is not supposed to need any headers within dialect code for
  dialect features because they are hard coded"; "for standard c/c++ features
  under the dialect they use the c++17 pack"
- "with --std=madc the dialect has hard coded access to everything that is
  part of the madc:: php:: perl:: rust:: ruby:: js:: etc etc namespaces"
- "I swear this is how it all used to be several months ago" — it was.

## How it got expensive

| when | how the dialect surface reached a program |
|---|---|
| 2026-04-14 → 06-04 | C++ registration code: `Program::add_php_namespace()` (src/ns_php.cpp) — `addFunction` per public, `namespace_map["php"][name]`; wired through `NamespaceRegistry::add_namespace("php", register_php_namespace_spec)` beside `std` / `madc` |
| 06fc1c08a, 06-04 | "retire std hardcoding gate": the registrations were typed with madc's hard-coded `std::string` (`ddSTRING`); they became the `include/madc/ns_*` header files, parsed as text every compile |
| 0.105.0 (Oct) | the fragments frozen as units of the system-header pack, bound whole |
| 0.106.0 Part A | per name, from those frozen units |
| 0.106.0 B1–B3 | a separate dialect image and a second forest |

Each step after June treated the parse cost instead of returning to
registration code. Measured on 603872ac6 (packed release, Ir):
`println("hi")` 11,633,307; `println("{}", php::strtoupper("hi"))`
18,187,829. About 4.7M of the difference is dialect machinery:
`queue_fragment_prerequisites` 3,406,806 (re-scanning the fragments' TEXT
every compile), `forest_restore_side` 1,044,945, map / probe / teardown
~0.4M.

## Design

1. **One source:** `include/madc/ns_*` and `bits/*` stay the only text anyone
   edits. A strict-mode `#include <ns_php>` still parses it; madcide's
   completion and the dialect-faces registry still read it.
2. **Generated registration code:**
   - At build time, madc reads the dialect files once (`--std=madc`) and writes
     C++ into the object tree (`obj/<mode>/dialect_registry.cpp`, beside the
     generated `embedded_headers.cpp`).
   - The C++ holds one builder per entity, constructing what the parser built
     through the same constructors: a function or overload set with its
     parameter types, defaults and emit symbol; a declaration-only template
     (`php::print_r`, `std::format` / `print` / `println`) with its
     `inline_builtin_kind`; a struct; an enum.
   - It also holds one sorted constant table of (namespace, name) → builders.
   - Types are named through their DataDef objects (`ddLONG` …), never sizes,
     so the code is target-independent (LLP64 reads `madc_target_data_model`
     as it does today).
   - It is never committed: the build regenerates it.
3. **Per name, at first use:** a `--std=madc` lookup that misses the
   program's own scopes asks the table. The hooks Part A placed are the call
   sites: `find_namespace_member`, `lazy_resolve(_type)`,
   `resolve_namespaced_type_token`, `namespace_chain_datatype`, the CIR
   fixpoint. The name's builder registers it once per Program. The
   `NamespaceRegistry` gains the dialect namespaces again, as it held them
   before June.
4. **Bodies compiled once, into libmadc:**
   - The `ns_ui_web` / `ns_ui_ws` bodies (and any `bits/*` body) are compiled
     at build time: madc `--emit=c11` → the build's C compiler (the cross
     compiler for a cross target) → linked into libmadc.
   - They are registered as host functions, mangled-direct, exactly like the
     `php::` publics (`cpp-first-api.md`).
   - A body's call into a LAZY module (`madcwebview`) binds through the module
     opener at its first call, never by a link-time reference (the module
     row's contract: the dialect runs on a system without the library).
5. **Run time:**
   - A dialect compile scans no fragment text, maps no image and restores
     nothing.
   - `stderr` / `stdout` / `stdin` are built in, as `println` is (in flight).
   - Standard C/C++ in a dialect program comes from the shared C++17 pack.
   - Strict modes never touch the table.
6. **One implementation per concern — what retires:**
   - the dialect image (`--dialect-image`, the `dialect` role's use);
   - the dialect half of `ForestSide` (B2/B3);
   - Part A's `forest_dialect_*` demand machinery;
   - `queue_fragment_prerequisites` on the dialect path;
   - the 13 fragments in the pack list (v9);
   - the dev build's text parse of the fragments;
   - the stopped B4 type-identity attempt (stash).

   What stays:
   - the ROLE segment (section J's C layer);
   - page-aligned containers;
   - the hooks' call sites, re-pointed;
   - the gates, re-aimed.

## Gated on `--std=madc` (owner 2026-10-17)

"we do need to make sure that the dialect builtin registration is gated on
--std=madc".

- One owner predicate answers "is the dialect surface on" for every
  registration path (the table lookup, the hooks, the NamespaceRegistry's
  dialect entries, the built-in `stderr` / `stdout` / `stdin`). It reads the
  `LanguageStd` enum, never a string. When the `--std=` feature registry
  (vision invariant I4, roadmap P2.11) lands, the predicate becomes its row.
- The existing per-namespace switches (`registration_policy.enable_php_namespace`
  …, the embedding API's options) stay embedder switches UNDER that gate,
  never a way around it.
- A strict compile resolves no dialect name and consults the table zero times
  (a `--show-stats` counter). A user's own `namespace php { … }` there is the
  user's alone.
- Gate `scripts/check-dialect-std-gated.sh`, in `make gates`:
  - over every non-madc standard (each C and C++ standard, gnu variants
    included): `php::strlen` / `madc::…` / `ui::…` are refused and the
    counter reads 0;
  - under `--std=madc` they resolve;
  - negative control: the predicate forced on makes the gate red.
- Owner 2026-10-17: "the entire madc:: namespace is gated to --std=madc".
  Today `add_madc_namespace` registers `madc::value` / `madc::array` in every
  mode ("explicit madc:: qualification is an opt-in even outside the madc
  dialect"): that registration moves under the gate, and its comment goes.
  Strict tests or hosts that name `madc::` today are found by the gate's first
  run and moved to `--std=madc`.

## R1 design (2026-10-17)

Inventory, the decl index of the 13 fragments' freeze (1,367 entries, HEAD
16b1cfea1):

| kind | count |
|---|---|
| functions | ~800: `madc` 190, `php` 177, `ui` 131, `madc_host` 57, `perl` 52, `python` 46, `rust` 46, `ruby` 36, `ui_web` 19, `js` 18, `git` 17, `ui_ws` 5, `markdown` 2 |
| types | ~100 |
| enumerators (`bits/*`) | ~380 |
| structs | 11 |
| namespace-scope variables | 8 |
| templates | 2 (`php::print_r`, `php::var_dump`; `std::format` / `print` / `println` are `add_format_intrinsics`' already) |
| module interfaces the fragments import | `webview.h` 31 functions, `madcgit.h` 16, `madcmark.h` 1 |

Almost every function is C-shaped: `value *`, `array *`, `const char *`,
`int64_t`, `bool`, `double`; `ns_php` has 21 defaults.

Rulings:
- **One registration owner:** the builtin registry, extended.
  - `populate_builtin_registry`'s `add_*_function` / `addFunction` gain what
    the dialect needs: a namespace, parameter names, defaults (literal
    values), a mangled or `extern "C"` emit symbol, and the template-builtin
    kind.
  - Plus `declare_builtin_struct` and `declare_builtin_enum` beside them.
  - Today's hand-written entries (`puts`, `fabs`, the `__atomic_*`
    family, …) and the hand-written `add_madc_namespace` /
    `add_format_intrinsics` / `add_stdio_streams` move onto the same owner:
    one way to declare a builtin.
- **`parseFunction` is not refactored.** The generated calls go through the
  registry's owner. Fidelity is proved by the differential, not by sharing
  code with the parser. That keeps the core parser out of this arc (the
  no-impromptu-parser-change rule).
- **Types** are built from the DataDef singletons and the minting owners
  (`getPointerType` / `getReferenceType` / `getQualifiedType`). A named struct
  or enum is built by its own builder first; the table holds dependencies.
- **The generator:** `madc --std=madc --emit-builtin-registry=<out.cpp>`.
  - It parses the fragments as a strict `#include` does (text — at build
    time only).
  - It walks what the parse registered, keyed by the decl index the
    freeze's tap already records.
  - It writes one builder per name and a sorted (namespace, name) table,
    into `obj/<mode>/dialect_registry.cpp`.
- **Two-stage build:** stage one is madc with an empty table (the dialect
  served as today); it generates; stage two compiles the table, and R3's
  bodies, into libmadc. Cross builds run stage one on the host; the
  generated code names DataDefs, not sizes.
- **Per name:** a `--std=madc` lookup miss binary-searches the table at the
  Part A hook sites; the builder registers the name once per Program.
- **Fidelity gate (the RED test):** for every table entry, the builder's
  registration equals the live parse's, field by field — mangled symbol,
  return and parameter types by spelling, names, defaults, flags, overload
  order.

## Tasks

- **R1 — inventory and the generator for declarations.**
  - Every entity the dialect files define, by kind, with the constructor the
    parser uses for it.
  - The generator for functions and declaration-only templates, plus the
    registration differential: a unit test comparing what the generated code
    registers with what a live parse of the same files registers (name,
    namespace, parameter and return types, defaults, emit symbol, overload
    order). RED first.
- **R2 — structs and enums.** The 9 structs (`ns_madc`, `ns_madc_host`,
  `ns_ui`, `ns_ui_web`, `ns_ui_ws`) and the ~29 enums in `bits/*` (the
  `bits/ui_enums` text stays the engine's and the dialect's one enum text),
  into the same differential.
- **R3 — bodies into libmadc.** The ~33 dialect bodies, compiled at build
  time and registered as host functions; the lazy-module binding; the build
  on Linux, the win64 cross build and macOS.
- **R4 — the switch.**
  - The hooks call the table.
  - The dialect image, the dialect forest side and the demand machinery are
    deleted.
  - Pack list v9 drops the fragments.
  - `--with-forest=none` and the dev build behave identically.
- **R5 — gates and measurement.**
  - The pay-per-use gate: 50 unused functions and one unused enum added to a
    copy of `ns_madc` move a dialect compile by ≤ 0.1%.
  - Parse-cost rows: the dialect rows, plus `php1` (one `php::` call) as a
    new row.
  - The differential over the dialect families.
  - Dev, release and no-pack builds give identical dialect output.

Per task: Tier 1 + Tier 2, the static gate loop, the touched unit binaries,
hot rows measured on the packed release; the batch lane at the end.

## Targets

- `php1` from 18.19M toward ~13.5M Ir.
- The dialect rows fall by the same machinery share.
- C and C++ rows flat.
- The pack loses the fragments' bytes.
- libmadc loses the 4.7 MB image and gains the compiled bodies and the
  registration code (measure both).

## Thread safety

- The table is constant data.
- Builders write only the calling Program's state.
- The bodies are ordinary libmadc functions under their module contracts.
- No new globals.

## Risks

1. Two-stage build: madc generates code that is compiled into libmadc. Stage
   one must build without the generated code; the dev build pays the extra
   step.
2. Fidelity: the generated code must register exactly what the parser does.
   The R1 differential is the gate, not review.
3. Bodies calling lazy modules (`madcwebview`) and the madc runtime from
   inside libmadc: symbol binding on all three platforms.
4. Things the parser does as a side effect of reading the fragments (macro
   state, `_include_*` flags, guards such as `_GLIBCXX_STRING`): the
   inventory must list them.

## Change 1 (ahead of R1) — the stdio streams are built in

- Under `--std=madc`, `stdin` / `stdout` / `stderr` are the compiler's, as
  `println` is: `Program::add_stdio_streams` enters them in every dialect
  Program's `lazy_map`; `lazy_resolve` makes a stand-in
  (`Variable::stdio_stream`) that the CIR lowers to the runtime's
  `__madc_stdio_stream(n)` (src/rt/rt_dump.c). No provider row, no
  `_include_stdio`, no header.
- A program's own `#include <stdio.h>`, before or after the first use,
  declares the one real `stderr`, which adopts the stand-in
  (`declare_object`); `--emit=c11` defines `struct _IO_FILE` once. Strict
  modes are unchanged.
- `--print-dialect-roots` names the 13 `ns_*` fragments only; the dialect
  image went from 51 units / 4,722,144 bytes to 26 / 4,188,888.
- Bind fixes the change exposed: a loaded body's callee takes its producer
  forest's own extern declaration (`Program::forest_extern_node`); the
  stand-in is never a frozen record.
- Tests: `testdialectstdiostreams`, `testdialectimage_ctypes(_after)`,
  `testdialectimage_streams`; leg 3 of `scripts/check-autoinclude-position.sh`.

## R1 execution (2026-10-17)

- The generator: `madc --std=madc --emit-builtin-registry=<out.cpp>`
  (`src/madc_builtin_registry.cpp`). It parses every dialect root as text with
  the pack bind off (`parse_dialect_as_text`), takes the function keys that
  parse registers beyond an empty program, groups them by (namespace, name) in
  overload-set order (`plan_builtin_registry`) and writes one builder per group
  plus the sorted table.
- The owner: `declare_builtin_function` builds the FuncDef and registers it
  through `Program::register_forest_func`, the restored-function registrar
  (namespace member, overload set, storage alias). Its rename waits for R4,
  when the dialect forest path it was named for is deleted.
- Target-neutral output: types are singletons and minting-owner calls; an
  emit symbol is never spelled — `bfEmitIsAlias` sets it from the alias the
  registrar mangles for the target (`int64_t` is `l` on Linux, `x` on macOS
  and Windows).
- Two-stage build (`src/Makefile`): `obj/<mode>/madc-stage1` links the same
  objects with the empty table, generates `obj/<mode>/dialect_registry.cpp`
  (moved only when its text changes), and libmadc compiles it in the empty
  table's place.
- The differential: `tests/unit/test_builtin_registry.cpp` compares
  `builtin_function_line` (types by their builder expression, parameter
  spellings and names, alias, flags, overload index) for every built key —
  479 declarations, identical. A group the generator does not build yet is
  reported with its reason (371 keys at the first green).

- Imports are not builtins (owner 2026-10-17: "Imports are different than
  builtins"). A module row's interface member (`madcgit_*`, `madcmark_*`,
  `webview_*` — every LAZY row's prototype carries `dyn_module_library`) is
  the import's: the import serves it, the builtin table never holds it.
  744a812eb built the 17 `madcgit_*` members; that is withdrawn, with its
  file-scope alias mint. A function the builtin registry registers itself
  (a libc builtin a dialect body calls) is not the dialect's either. 494
  declarations identical; 126 keys not yet built.

Rulings:
- Ruling: R3 counted ~33 bodies; the live parse has 234 bodied dialect
  functions. ~210 are polyglot thin wrappers whose fragment body calls an
  extern "C" `__php_*` export — the inversion `cpp-first-api.md` forbids
  (owner 2026-10-17: the extern "C" functions are the wrappers). They move
  into `src/ns_*.cpp` as the namespace's C++ definitions, declared bodiless
  in the fragments, namespace by namespace, after R1; R3's build-time
  compile stays for `ui_web` / `ui_ws`. Cost if wrong: the wrappers would
  need the R3 compile path instead.
- Ruling: cross and hosted modes link the empty table (the dialect served
  from its text, as before) until R3 builds them from the host's generated
  file; the generated code is target-neutral. Cost if wrong: one Makefile
  arm per mode family.
- Ruling: class members (`madc::channel`, `madc::tool_host`, ...) are R2's
  class registration, not R1's free functions.

## The polyglot publics move into the host (2026-10-17)

Owner 2026-10-17: "the extern "C" stuff was supposed to be the wrappers".
`cpp-first-api.md` already says so: a script header declares its namespace
publics bodiless, resolved mangled-direct to real `namespace X { }`
definitions in libmadc. The polyglot fragments had it inverted — every
public was a one-line body calling an extern "C" `__x_*` export.

Per namespace:
- `src/ns_X.cpp` gains `namespace X { ... }`: each public defined in C++,
  calling the same core its extern "C" export calls (the std::string
  interop overloads included — the host is always C++).
- `include/madc/ns_X` keeps declarations only and drops its extern "C"
  declaration blocks (the dialect no longer names them).
- The extern "C" exports stay, untouched, as C-linkage wrappers over the
  same cores; whether any is deleted is the owner's call.
- `tmp/polyglot_move.py` (scratch) did the mechanical rewrite: 37 perl
  bodies, all through pass-through cores.

Done: `js` (9 lean publics + 5 interop); then `perl` 37, `python` 30,
`ruby` 21, `rust` 33, `php` 93 bodies in one change (`php::time` calls
`::time` directly — its extern "C" export is not a pass-through). The
differential: 511 declarations identical, 157 keys not yet built.

- Ruling: `ruby::delete` (three overloads) keeps its fragment bodies over
  the extern "C" cores — `delete` is a C++ keyword, so host C++ cannot
  define it, and an asm label would spell the Itanium symbol per platform
  and per stdlib flavor by hand. R3's body compile takes it. Cost if wrong:
  three bodies wait for R3.

Next: `git` / `markdown` bodies by hand; `ui_web` / `ui_ws` are R3.

## R2 execution (2026-10-17)

- Enums: `declare_builtin_enum` reproduces `TokenENUM::parse` for a tagged
  namespace-scope C++ enum — the DataDefENUM (base and storage as the close
  left them), one TokenDataType under the bare tag and under the namespace,
  every enumerator a constant in the tag's pseudo-namespace, and an unscoped
  enum's (`ui::level`) also bound in the namespace and the program scope with
  its storage alias (a mangled NAME — target-neutral).
- A function or builder names a dialect type through `builtin_type(pgm, ns,
  name)`: the program's registration, or the table entry's builder the first
  time. Builders are idempotent (a function's builder may register a type
  before the table reaches it). A singleton type (`madc::value` is ddARRAY)
  is every program's.
- The differential compares `builtin_type_line` for every built type: the
  enum's spelling, size, storage, base, flat and namespace registration, and
  each enumerator's value, flags, type and bindings. 526 declarations and
  types identical; 142 keys not yet built (function-pointer typedefs,
  structs, classes and their members, bodies).
- Function pointers have ONE spelling in generated code, an expression:
  `builtin_function_pointer(pgm, RET, { {T, "spelling", const, "typedef"},
  ... }, FLAGS)` — the function type parseFnPtrParams builds, folded by
  getPointerType, a fresh type per use as each declarator makes its own. A
  function pointer a typedef names is that typedef's (`builtin_type`); one no
  typedef names (a callback parameter: `madc::command_add`, `fork_uri`,
  `ui::doc_subscribe`) is spelled in place.
- Function-pointer typedefs: `declare_builtin_typedef` reproduces
  `finish_alias` + `record_typedef` at namespace scope — the TokenDataType
  under the bare alias and under the namespace, the alias among the typedef
  names emitted verbatim, and its file-scope typedef TopDecl, system-origin
  (the emitter's td_system verdict; a dialect header's).
- The differential's lines gained the parameter vectors' lengths (`vecs=`):
  a live declaration records one typedef-name slot per parameter, which the
  R1 builders left empty — now reproduced (`BuiltinParam::typedef_name`,
  NULL where the parse recorded none). Type lines gained the typedef-name
  set and the TopDecls (kind, system verdict, combined body). 540
  declarations and types identical; 128 not yet built.
- R4 carries: a builder that runs mid-parse pushes its TopDecls at that
  point — after the TopDecl of a function whose body triggered it. The
  emitter's ordering of a lazily built typedef/struct ahead of its first use
  is R4's to prove (the forest bind pushed them at the include point).
- Data-only structs: `declare_builtin_struct` reproduces TokenSTRUCT::parse
  for a namespace-scope struct — laid out member by member by the struct's
  OWN `addMember` + `finalize` (the target computes offsets and size; the
  generated code records names, types and spelled aliases, never an
  offset); the bare tag in struct_map and datatype_map, the namespace's
  type, the canonical `ns::Tag`, and its file-scope struct TopDecl
  (system-origin). A struct whose object member promoted it to a class
  (`madc::SysInfo`: `array` members) records that outcome
  (`object_class`), and the builder promotes as the parse does — the class
  takes the struct's state, the tag is repointed, the class layout is
  computed. A member's origin token is a source position a builtin does not
  have (as the forest restore); `definition_file` likewise (read only for
  interactive-session units).
- Namespace objects joined the differential (`builtin_variable_line`): an
  `extern T name;` reference (`madc::sys`) builds through
  `declare_builtin_extern` — no storage, its alias the target's
  `namespace_cpp_variable_symbol(ns, name)` (now the shared owner, declared
  in madc.h), bound in its namespace and the program scope, its dkGlobalVar
  TopDecl with no initializer (the restore's extern-reference precedent).
  `madc::__madc_run_child` — a `static const` whose initializer runs
  `serve_run_child()` before main — is initialization code: R3's.
- 544 declarations, types and objects identical; 128 not yet built: the
  method-bearing classes (`madc::channel`, `tool_host`, `ex_session`,
  `session_claim`) and their 70 members, `php::print_r` / `var_dump`
  (compiler-implemented templates), the bodies (R3), and the ui_web /
  ui_ws aggregates and objects that ride with their bodies.
- The function lines gained the method fields (owner, display name, local
  emit name, method qualifiers, the Variable's flags, the namespaces that
  bind it by key) and two more parameter vectors: a live declaration
  records, per parameter, whether a template parameter spelled it and its
  default — the R1 builders recorded neither (a function TYPE records
  neither; `fill_builtin_params` now knows which it builds).
- Classes (`madc::channel`, `tool_host`, `ex_session`, `session_claim`):
  one builder per class — `declare_builtin_class` (register_class_shell's
  writes under the class's own namespace: struct_map, the flat and the
  namespace type maps, the tag its own scope alias; NULL when the shell
  exists, and the builder stops), its data members with their access, every
  member function through `declare_builtin_method` (parseFunction's
  prototype arm: the FuncDef with the hidden `__this` first, the
  program-scope Variable bound in the class's namespace by key, the Method
  owned by the class) and the LIVE registrar `register_class_method_signature`
  (display name, access, method_map / ctors, the emitted symbol — the
  target's mangling, never spelled), then the live `complete_class_aggregate`
  (layout, noexcept). The registrar's kind is read from the class (a
  constructor is in its ctors; a destructor's display name is `~`).
- A class's member functions are its group's keys (registration order), so
  the differential compares each one's line beside the class line (methods,
  ctors, method_map, scope aliases, layout, flags).
- Skip judgements thread a visiting set: a type met again while it is being
  judged is its own builder's (a class's shell exists before its members
  name it). A closing pass refuses any built group whose generated code
  names a type that got no builder, so a builder never meets a NULL type.
- 619 declarations, types and objects identical; 53 not yet built: the
  bodies (R3), `php::print_r` / `var_dump` (compiler-implemented templates),
  `madc::__madc_run_child` (initialization code, R3) and the ui_web / ui_ws
  aggregates and objects that ride with their bodies.

## R3 execution (2026-10-17)

- `ruby::delete` — `delete` is a C++ keyword, so no host C++ definition can
  carry the public's name. libmadc defines its two lean overloads' Itanium
  symbols as C-linkage names (`_ZN4ruby6deleteERN4madc5valueEPKc`,
  `_ZN4ruby6deleteEPKcS1_`, src/ns_ruby.cpp): one spelling on every target
  madc builds for, because the platform's C-name prefix maps a C name to
  exactly the C++ symbol (Itanium on Linux, darwin and x64 mingw). The
  fragment declares them bodiless; a call resolves mangled-direct like every
  ruby:: public. Ruling: not an asm label — a darwin asm label is the raw
  object symbol (Apple's `_stat$INODE64`), so it would not be target-neutral.
  Cost if wrong: two symbols spelled in the host.
- The guarded `std::string` overload (`#if _GLIBCXX_STRING || _LIBCPP_STRING`)
  keeps its body: its mangling depends on the stdlib flavor, and the guarded
  interop blocks are R4's question (a program that includes `<string>` opts
  into C++ interop; the lean text parse never sees them).
- `madc::__madc_run_child` — the namespace-scope initializer that served a
  Run child of self is gone: `__madc_sys_init` (the injected entry init of
  every program that includes `<ns_madc>`, JIT main wrap and object-mode
  `.init_array` alike) serves it first, before the program's dynamic
  initializers and main (madc_run_child.h's contract). Same population, no
  dialect object: under per-name registration nothing would ever name it.
- Ruling: `ui_ws` (5 bodies, `ui_ws::host`, `ui_ws::__pending`, its
  self-registering ops table) joins `ui_web` in the modules discussion — both
  are ui HOSTS that register themselves by a static initializer, and where a
  host lives (builtin, module) is that discussion's line of division. Cost if
  wrong: ui_ws waits one discussion.
- Ruling: `php::print_r` / `var_dump` (compiler-implemented function
  templates) wait for R4. Their registration is a token pattern
  (fn_template_decl_map holds the declaration's tokens), so a builder must
  lex and parse that one declaration — at namespace scope, which a builder
  run from inside a function body is not. R4 designs the hook's parse
  context; these two ride it. Cost if wrong: two names on the old path one
  task longer.
- Found: `FuncDef::inline_builtin_kind` is a string discriminator compared by
  value across cir_builder / cir_dump / cir_format / parser / cir_freeze —
  an enum-over-strings violation (owner law). Owner 2026-10-17: its own
  session (it touches parser.cpp), right after R4.
- The modules discussion (owner 2026-10-17): the line of division is WHO
  IMPLEMENTS the code. A builtin is what libmadc implements; a module is what a
  library loaded on `import` implements, and it carries its own face.
  `git::` (madcgit), `markdown::` (madcmark) and `ui_web` (madcwebview) are
  their modules' — never builtin-table entries. Owner: `ui_ws` stays a builtin
  ("let's keep it").
- `ui_ws` moved into libmadc: src/ui_ws.cpp holds the host (attach, open,
  close, eval, run) and its ops table; `<ns_ui_ws>` declares the five publics
  bodiless. The host seam (`ui::ui_host_ops`, `register_host`, `post_event`)
  moved verbatim out of src/ns_ui.cpp into include/madcdis/ui_host.h, which
  both include. The engine serves libmadc's own hosts BY NAME
  (`ui_builtin_hosts`, consulted by `ui_host_named`, never by
  `ui_host_serving`): the fragment's comment always said a serve seat opens
  "ws" by name and never by level, and a level is the program's declaration of
  need, served by a host the program registers. `ui_ws::host` and
  `ui_ws::__pending` leave the dialect surface; the slot is `thread_local` in
  libmadc (the host's stated contract: confined to the scheduler thread that
  opened it). B197's shipped instance (`sizeof(host)` taking `ui_web::host`)
  goes with it. New test `testuiws`: a loopback WebSocket pair, `ui::open("ws")`,
  a frame out, a page event back, the close ending the window. Differential:
  625 compared, 42 not built.
- testdialectimage_ctypes / _after lost their subject (no dialect body names
  stderr in the program now: `ui_ws::open` is libmadc's). They still pass; R4
  retires them with the dialect image.

## Modules before R4 (owner 2026-10-17)

Owner chose "modules first": `git::` (madcgit), `markdown::` (madcmark) and
`ui_web` (madcwebview) get their own home before R4 deletes the dialect image,
because the image and the pack freeze them against `ui::` / `madc::` records
the builtin table now owns.

Owner ruling, rejecting a design that kept each face's declarations as a
generated table in libmadc, `available()` beside the module row and the web
host served from the core's module map (verbatim): "This is all wrong and
defeats the point of modules if they need to be statically registered into the
core. Existence should be tested by trying to import. Think more like Python
modules than like C++ headers."

- A module is self-contained: its declarations, its bodies, its hosts,
  commands and tools come from its own library, found by name, registered when
  it is imported. Nothing about a module lives in the core: no row, no
  embedded face or interface header, no auto-include row, no table in
  libmadc.
- Existence is tested by importing: a failed import is the answer. No
  `available()` probes.
- A program that must run without a module (madcide's nexus without
  libgit2) keeps the module's users in their own unit or plugin: that unit's
  import fails when it is loaded, the loader reports a catchable import error,
  and the rest of the program runs (owner 2026-10-17: Python's `try: import x
  / except ImportError`, option (a) — no compile-time `__has_import` test).

### Design

- **D1 — a module describes itself.** Its library exports one C entry
  point named by convention from the module's name (the commands/tools
  columns' `madcdl_sym` precedent, without the row): the module's
  declarations, its commands, its tools, and the ui hosts it serves. The core
  reads only that entry.
- **D2 — declarations are the module's builder table** (CPython's
  `PyMethodDef` shape): the R1 generator runs at the MODULE's build over the
  module's face file and the output compiles into the module. On import madc
  opens the library and registers the face per name from that table, as the
  dialect table does for builtins. Nothing is parsed, nothing is packed.
- **D3 — bodies are the module's C++** (`namespace git { ... }` in
  modules/madcgit/), called through the open module.
- **D4 — hosts register when the module loads**: madcwebview's own initializer
  calls `ui::register_host("web", ui::WEB, ...)`, so a program that imports it
  and opens `ui::WEB` gets the web window. The core has no web knowledge.
- **D5 — a failed import** (no library, no entry point, or an entry from an
  incompatible madc) is a compile error naming the module and the loader's
  reason; a unit or plugin loader turns it into a catchable error (D-a above).
  An --exe or --obj records the module as a dependency, and a missing module
  at program start is the same import error.
- **D6 — what leaves the core:** the madcgit / madcmark / madcwebview rows,
  their embedded interface headers and faces (`ns_git`, `ns_markdown`,
  `ns_ui_web`, `madcgit.h`, `madcmark.h`, `webview.h`), the `git` /
  `markdown` / `WEB` auto-include rows, `madc::module_available`, the LAZY
  flag ("compiles without the library"), the faces' pack entries. The slot
  binding STAYS: `dyn_module_library` / `dyn_module_member` →
  `__madc_dl_member` is how a call reaches a module's code in the JIT, --exe
  and --obj; a module's builders stamp it (D5's required dependency replaces
  the optional `?` one).
- **D7 — C++20 named modules and madc modules are one thing in two forms**
  (owner 2026-10-17: "we want to support both C++20 style modules and madc
  modules"). A madc module is madc's BUILT form of a C++20 named module: its
  exported declarations as a builder table, plus its code, in one library.
  The SOURCE form is an interface unit (`export module git;`). ONE `import`
  reader and ONE resolver: `import git;` searches the module path for `git`
  and finds either a built module (opened, registered from its table) or an
  interface unit (compiled in-process into the same registration — no cache
  file, owner law). `madc --emit=module git.cppm -o git.so` builds the binary
  form explicitly. The module kind is data the resolver finds, never a second
  import path.
- **D8 — names are C++20 module names** (dotted, `a.b`), independent of
  namespaces. madc's own modules follow a convention: module `git` exports
  `namespace git`. So the program writes `import git;` (not `import madcgit;`)
  and the three modules are renamed `git`, `markdown` and a web module.
- **D9 — gated by `--std=`.** Named-module import is a feature of c++20 and
  later and of madc, through the `--std=` feature registry — never a
  hard-coded standard test. An earlier standard refuses `import NAME;`.
- **D10 — `import c;` / `import m;`** would collide with C++20 module names:
  they become modules madc SHIPS (Python's `import math`) over the platform's
  C runtime images — no core rows.
- **D11 — `--import=NAME` on the command line** (owner 2026-10-17: "a CLI
  argument way to import as well so the program can test import before
  starting"), repeatable, a row in the one option table
  (`madc::cli::parse`, src/madc_cli.cpp). With a source it imports NAME
  before the program, as if the source began with `import NAME;`. With no
  source it only tries the import: exit 0, or the import error and a
  DISTINCT exit status, so a launcher tests before starting (`python -c
  "import git"` is the precedent).
- Where madc finds a module: madc's module directory
  (`<prefix>/lib/madc/modules/<name>.so` / `.dylib` / `.dll`, apart from
  system libraries so a short name cannot collide with a system `libgit`), the
  program's own directory, and `--module-path=DIR` entries.

### Slices (modules before R4)

- **M1 — the module ABI, the resolver, the generator's module mode.**
  `include/madc_module.h` (the description a module exports, an ABI number);
  the resolver over the module path; `import NAME;` opens a built module,
  checks the ABI and registers its declarations per name (each function
  stamped as the module's member); a failed import is the import error. The
  generator's module mode: the base is the dialect, the live parse adds the
  module's face, the delta is the module's table (R1's `parse_dialect_as_text`
  generalized).
- **M2 — `markdown`** (madcmark: 2 bodies) — the first module through M1;
  testmarkdown, testlazymoduledirect, the commonmark lane.
- **M3 — `git`** (madcgit: 16 wrappers; its commands/tools ride the
  description).
- **M4 — the web module** (madcwebview: 19 bodies, the host on load, the GUI
  flag in the description); the GUI lane.
- **M5 — `--import=NAME`** and the distinct exit status.
- **M6 — the deletions** (D6) once nothing uses them.
- Then R4. After R4, on the C++20 needle: the source form (`export module`,
  `--emit=module`), `import c;` / `import m;` as shipped modules, D9's
  feature fact.
- madcide follows in its own repository: its imports change (`import git;`,
  the web module's import, no `available()`).

### M1 execution (2026-10-17)

- `include/madc_module.h`: `MadcModuleDescription` (ABI 1, name, flags,
  declarations, commands, tools), the entry point `madc_module_<name>` (each
  '.' as '_'), `MADC_MODULE_EXPORT`. `src/madc_modules.cpp` (the one spelling
  owner): `madc_module_file_spelling` (`<name>.<dso>`, no lib prefix),
  `madc_module_dir` (`<lib>/madc/modules`), `madc_module_find_file`.
- `import NAME;` (no alias): `Program::module_search_path()` — the program's own
  directory, then `--module-path` entries — then madc's directory. Found: the
  import opens the module, reads and checks its description, records it.
  Neither a module nor a row: "import: no module named 'NAME'".
- Ruling: the declarations register in `_parser_init` (`register_imported_modules`),
  not inside the directive — the import runs at LEX time and the parser's init
  resets the maps (the embedded-headers rule: never register during
  tokenization). Cost if wrong: none observed; the import's errors still
  surface at the directive.
- Ruling: a module registers WHOLE at its import, in table order (Python runs a
  module's init whole; its tables are small); `builtin_type` finds a type a
  builder names in the imported tables. Pay-per-name is the dialect's (R4).
- `declare_builtin_function` stamps each function as the module's member: the
  library is the path the import resolved, the member the declaration's
  external symbol (`storage_alias_name` — Itanium; `emit_symbol` when set).
  Ruling: the PATH — the JIT and a same-machine --exe / --obj are exact; an
  artifact moved to another machine resolving its modules by name there is
  M5's, with D5's required dependency (today the module is an optional entry
  of the object's module list: a missing one fails at the first call).
- The generator's module mode: `--emit-module-table=<out.inc> <face>` —
  `parse_module_as_text` (base: the dialect; live: the dialect and the face),
  `emit_builders` shared with the dialect table, every group must build. A
  face is `<name>.madi` (declarations only).
- `src/madc_modules.mk`: the module rule (face → table → library), the Linux
  host arm. Test module `tests/modules/greet` (a scoped enum, a text return,
  the value carrier) → `obj/test-modules/greet.so`; tests `testmodule_import`
  (JIT, EXE, OBJ) and `testmodule_missing`. Ruling: darwin / win64 domain
  skips until M2 brings those arms (the first shipping module).

### M2 execution (2026-10-17)

- madcmark becomes the module `markdown`, all of it under `src/modules/markdown/`:
  `markdown.madi` (the face: `markdown::parse` and `#include "markdown_enums"`),
  `markdown.cpp` (the body, then the generated table and the entry point
  `madc_module_markdown`), `markdown.h`, `markdown_enums`. The C API
  (`madcmark_*`, `include/madc/madcmark.h`) and the core's face
  (`include/madc/ns_markdown`, `include/madc/bits/markdown_enums`,
  `include/madcdis/markdown.h`) are gone, along with the auto-include row, the
  preferred-order entry, the module-map row and the pack-list entry.
- `src/markdown.mk` (was `madcmark.mk`): the table from the face
  (`madc_module_table_rules`), then `lib/madc/modules/markdown.so` (Linux),
  `lib/madc/modules/markdown.dll` (Windows, linked against libmadc's import
  lib), `lib/madc/modules/<arch>-macos/markdown.dylib` plus a flat copy
  (macOS). The tables are target-neutral, in `obj/modules/<name>/`, written by
  the host's madc in cross and hosted modes.
- Packages: `/usr/lib/madc/modules/markdown.so` (deb/rpm; `%files` carries
  `/usr/lib/madc`), `lib/madc/modules/` in the tarball, the macOS bundle and the
  Windows zip; `check-module-exports.sh` reads the module-directory images;
  `notices.tsv` names the new carriers.
- Programs say `import markdown;`: testmarkdown, the commonmark lane's
  driver, madcide (its own repository). `markdown::available()` is gone: a
  missing module is the import error. testfragmentobjects drops markdown.
- Ruling: testlazymoduledirect is retired. Its subject (a LAZY row's direct
  call) has no row left to test; M6 deletes the flag. Cost if wrong: git and
  the web module keep the LAZY path until M3/M4, still covered by testgit*
  and the GUI lane.
- Carry (R3's): cross and hosted builds still link the empty dialect table;
  the markdown table itself is host-generated and needs nothing more.
- Validation: Tier 1 (13 JIT, 10 EXE, 10 OBJ), Tier 2's seven lanes green
  (commonmark 664/670, the baseline), test_markdown / test_builtin_registry /
  test_cir_freeze / test_modules, static gates 0 red. The win64 and darwin
  arms are built and run at the seam.
