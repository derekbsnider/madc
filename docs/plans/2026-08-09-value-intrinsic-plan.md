# madc::value as a first-class script intrinsic (+ `var`) — Slice V1

**Owner decision (2026-08-09):** `madc::value` is an intrinsic madc data type
and must be usable in scripts without any `#include` — spelled `value`,
`madc::value`, or declared with `var`. `array` is NOT renamed: a
`madc::array` IS a `madc::value` whose kind is array (the host class
models exactly this — one tagged carrier, `kind::array` backed by
`std::vector<value>`). All spellings resolve to the SAME DataDef
(no parallel type, no second implementation).

**Dialect gate (owner):** the new spellings are active only under
`--std=madc` (the default). Strict `--std=cNN` / `--std=c++NN` modes
must keep `value`, `var`, and `array` as ordinary identifiers.

**Acceptance shape** — the owner's `testsort.mad` (15 lines, hits every gap):

```c
madc::channel sorter("exec://sort");
...
sorter.write("pear\napple\nmango\n");
sorter.close_write();
value line;
while ( sorter.readline(line) )
    printf("sorted: %s\n", line);
sorter.close();
```

## Confirmed state at branch point (develop v0.74.0 @19fc704e)

Probed in the container at HEAD:

1. `value line;` → `use of undeclared identifier 'value'`. No spelling was
   ever registered — `git log --all -S 'madc_ns["value"]'` is empty. The
   script surface stayed at the MadValue-era `array` alias when the
   value-ABI track renamed the host class (ddARRAY is already
   `DDClass("array", sizeof(madc::value))` with canonical spelling
   `"madc::value"` — the compiler internally knew; the surface didn't).
2. `array line; sorter.readline(line)` → `cannot bind a non-const lvalue
   reference parameter to a converted temporary` (correct v0.74.0 refusal;
   `channel` has only `readline(std::string&)`).
3. `array a; a = "hello";` → `assignment of incompatible value` (c2mir
   check error — ddARRAY has no `operator=` methods, so the assignment
   reaches c2mir as a raw struct=pointer N_ASSIGN).
4. `printf("%s", <value>)` untested behind (3); needs a varargs coercion
   rule for the runtime-tagged carrier.
5. `var` — completely absent (no keyword, no token, no spelling).
6. Working today and must not regress: `array b; b.count()`,
   `madc::array a;` (namespace Variable `__madc_array` path), all
   php::/perl:: array consumers.

## Design

### D1 — spellings, not new types

`value`, `var`, and `array` are three spellings of the one intrinsic
DataDef (ddARRAY, canonical spelling `madc::value`). `var` is NOT a
keyword — it is a gated type-name spelling; `var x = expr;` is an
ordinary declaration whose type is the dynamic carrier. This deliberately
contrasts with `auto` (compile-time inference, type fixed at initializer):
`var` slots stay runtime-retaggable.

### D2 — the gate lives in instance-aware lookup, NOT the static table

`Program::resolve_builtin_type_spelling` is the STATIC identity/
canonicalization table (template binding keys, argument-spelling
canonicalization). Dialect spellings must NOT live there:

- It has no `language_std` access (static — cannot gate).
- Bare-name identity mapping collides with user identifiers in other
  standards. **Latent bug found during recon:** `array` IS in the static
  table today, so a scalar `typedef long array;` under `--std=c++17`
  would canonicalize its template-binding identity to ddARRAY
  (`canonical_template_binding_dd` maps btSimple alias dds through the
  table by name). Removing `array` from the table fixes this by
  construction; a regression test pins it.

**As landed** (two architectures were tried; the second is the one that
survived the suites — record of WHY, so nobody retries the first):

- **`value`/`var` are NOT lexer datatype tokens.** The first attempt
  registered tkVALUE/tkVAR prototypes in the lexer's datatype_map
  (beside tkARRAY, STD_MADC-gated like the `wchar_t` fundamentals).
  It made `value v;` work — and broke 15 suite tests + libstdc++
  header parses, because a datatype token hijacks EVERY identifier
  position: `static constexpr _Tp value` members, `make(U value)`
  params in template recipes, `(value - base)` cast disambiguation.
  Each is its own parser arm; the whack-a-mole does not converge.
- **They ride the typedef lane instead**: plain ttIdentifier at lex
  time, resolved as types only where user typedefs resolve —
  `resolve_declared_type_token`'s tail gains a dialect arm
  (`Program::madc_dialect_type_spelling`, instance, STD_MADC-gated)
  placed AFTER every user-type lookup, **carrying the shadow guards
  itself**: `!findVariable(name) && !current_method_class_has_member(name)`.
  The guards live in the resolver because the statement lane's
  speculative declaration probes call it with no shadow context of
  their own; this is also C++'s actual name-hiding ([basic.scope.hiding],
  [class.mfct] — a variable or member named `value` wins over the type
  spelling exactly as g++ would read it). The statement arm then needs
  NO dedicated code: the existing speculative probe declares `value v;`
  through the resolver.
- **`array` keeps its historical lexer datatype token**, now gated to
  STD_MADC (dialect purity; strict modes get `int array;` back as
  plain C). Its contextual-identifier machinery gained the
  member-shadow half (`current_method_class_has_member` consulted by
  the parseStatement ttDataType arm + the expression Redo-to-ident
  arm) and the three TokenCLASS::parse member-NAME positions accept
  contextual tokens — pre-existing array-member gaps, fixed en route.
- **`Program::madc_dialect_type_spelling(name)`** is also consulted by
  `resolve_named_datadef`, the argdep unresolvable-name probe, and the
  `typedef_alias_matches_datadef` name-identity check (where the static
  table previously answered for `array`).
- **pch.cpp spelling→dd deserialize table** gains `value`/`var` →
  ddARRAY beside `array` — restore is a codec, not a language gate
  (unconditional; namespace-prototype clones spelled "value" can be
  serialized). NOTE: this table is a THIRD copy of the builtin-spelling
  mapping (with resolve_builtin_type_spelling and the lexer block) — a
  pre-existing DupFamily candidate; record it at the next /dupaudit.
- Qualified + in-namespace spellings: `madc_ns["value"]` Variable
  (beside `madc_ns["array"]`) for the expression lane, plus
  `namespace_datatype_map["madc"]["array"/"value"]` TokenDataType
  prototypes — the latter is what resolves `value &` parameters inside
  the embedded `<ns_madc>` header's `namespace madc { class channel {...} }`
  (namespace_chain_datatype) and `madc::value` in type positions.
  Unconditional like `madc::array` — explicit qualification is an
  opt-in even in C++ modes. No `madc::var` (var is a declaration
  spelling, not a namespace member).

**Builder-side (D3/D4/D5 as landed):**

- ddARRAY is deliberately OUTSIDE the user-class universe
  (`as_user_class` requires dtRESERVED — array objects lower to
  aligned long[] buffers, not class instances). Three admissions teach
  the existing machinery about it without widening as_user_class:
  `class_operator_call` admits a ddARRAY lvalue as operator LHS (the
  native operator= family resolves through select_operator_overload
  like any parsed operator); `is_class_object_value` admits
  ddARRAY-typed vars/members (routes value args through
  object_arg_addr's named-variable arm — the LHS was previously
  MATERIALIZED INTO AN UNCONSTRUCTED TEMP — and through
  object_cstr_arg in char*/varargs positions); `object_cstr_arg` /
  `thrown_object_has_cstr` fall back to &ddARRAY so the native c_str
  (madarray_cstr) serves the text view.
- Native registrations on ddARRAY (add_array_methods): five
  `operator=` overloads (cstr/int64/double/bool/const array&) bound to
  madarray_assign_* runtime entries, plus `c_str` → madarray_cstr.
  Return type and the copy-assign param are a REAL DataDefREF passed
  as a plain typespec_t (ref_of() collapses to the base in
  addFunction's resolve_data_type; a by-value ddARRAY return would
  wrongly take the sret path — returns_reference() gets the N_DEREF
  lowering matching the runtime's returned receiver pointer).
- madarray_cstr renders non-string kinds through the ONE value→text
  owner (ns_common::value_to_string) into a thread-local 8-slot ring
  (several value args in one printf call keep distinct buffers);
  string kind returns the value's own payload.

### D3 — scalar assignment surface: native operator= on ddARRAY

Follow the `madarray_size` pattern exactly (`add_array_methods`:
`addFunction(..., isMethod=true)` + `declaration_only` +
`emit_symbol` + `method_display_name`). Register on ddARRAY:

- `operator=(const char *)`  → emit_symbol `madarray_assign_cstr`
- `operator=(long)`          → `madarray_assign_int`
- `operator=(double)`        → `madarray_assign_real`
- `operator=(bool)`          → `madarray_assign_bool`
- `operator=(array&)`        → `madarray_assign_value` (copy-assign;
  also gives struct-member copy paths a real deep-copy — today
  `class_copy_assign_members` falls back to bit-copy for array members)

Runtime entries: extern-C in `madc_mir_backend.cpp` beside
`madarray_construct/destruct/size`, implemented over the real
`madc::value` (compiler-machinery boundary — extern-C is correct here
per cpp-first-api.md's exception; these are never user-resolved names).
The existing parse-time operator-overload resolution and the builder's
`class_assign_operator_def` / `class_assign_cstr_operator_def` helpers
match `method_display_name == "operator="` — no new resolution machinery.
`value v = "x";` (init form) must lower like `string s = "x"` does
(default-ctor + assign or converting-ctor — whichever the existing
class-init path picks once operator= exists; verify, don't invent).

### D4 — channel integration

Host methods on `madc::channel` (src/madc_channel_object.cpp), declared
in the embedded `<ns_madc>` header, resolved mangled-direct like the
existing `readline(std::string&)`:

- `bool readline(value &out)` — line lands as string kind
- `bool readall(value &out)` — whole payload as string kind
- `bool write(value &text)` — writes the text payload

No stdlib-flavor marshalling needed (madc::value is host-owned; its ABI
does not vary with -stdlib=). Mangling: parameter encodes via the
DataDef canonical spelling (`madc::value` → `N4madc5valueE` /
substituted `NS_5valueE` inside the madc namespace) — verify the emitted
symbol against `nm` on the host binary during implementation; fix the
encoder, not the header, if they diverge.

### D5 — varargs coercion (printf)

A `value` argument in a variadic call cannot be runtime-dispatched by
format (C varargs ABI). Rule: a value passed to a variadic C function
passes `madarray_cstr(void*)` — `const char *` of its TEXT:

- string kind → the payload pointer (value-owned, stable);
- other scalar kinds → rendered text in a small thread-local ring of
  rotating buffers (inet_ntoa model; N slots so several value args in
  one call don't alias);
- array/object kinds → a diagnostic-friendly rendering (`[array:N]`),
  never a crash.

`%s` therefore always works (polyglot default: ints render as "42");
`%d` with a value is the user's format mismatch, same class of UB as C.
Implementation point: the SAME site where `std::string` args coerce in
variadic calls today — extend that site for ddARRAY, do not add a
parallel path. (Search first: the concept is "class-typed argument
coercion in variadic call lowering".)

### D6 — what stays untouched

`array` spelling and semantics, `madc::array`, php::/perl:: consumers,
vivification model (`value v;` and `array a;` both start kind-null and
vivify on container use — same type, same behavior; scalar assignment
sets the kind directly), subscript string-first element typing, emit-C
handling of dtARRAY (all spellings are one DataDef — emission already
canonical).

## Work plan

- **V1a — spellings + gate.** madc_dialect_type_spelling(); remove
  `array` from the static table; wire resolve_named_datadef + the argdep
  probe site; madc_ns["value"]. Tests: bare `value`/`var`/`array` decls
  + `madc::value` + `.count()`; `--std=c++17` negative (`.flags` +
  `.expect_err`) for bare value/var/array; shadow regressions
  (`int value = 5;` under madc dialect; `typedef long array;` under
  c++17 template-arg identity).
- **V1b — scalar surface.** madarray_assign_* runtime entries +
  operator= registrations; init-form check. Test: assignments of all
  scalar kinds + copy-assign, re-tagging (int over string), count()
  after scalar (kind change, not append).
- **V1c — varargs.** madarray_cstr + coercion at the existing site.
  Test: printf %s with string/int/real-kind values.
- **V1d — channel overloads.** Host methods + header decls + symbol
  verify. Test: value-spelled exec://sort round-trip.
- **V1e — acceptance.** tests/testvaluesort.mad = the owner's testsort
  shape verbatim (script mode, no includes, `value line;`). Batch gate:
  fulltest + libcxxjit; session end: EXE/OBJ/release/packed.

## Traps / notes for the next session

- `resolve_builtin_type_spelling` is called from identity formers —
  NEVER put dialect spellings back in it. The `typedef long array;`
  c++17 identity bug is the proof; the regression test is the gate.
- parser.cpp:15197 `resolve_class_static_member_const_value(scope,
  "value", ...)` is trait `::value` STATIC-MEMBER lookup — qualified,
  unrelated to the bare type spelling; do not "unify".
- The owner's root `testsort.mad`/`test.mad` are owner probe files:
  read/run only, never modify/commit; the acceptance test is a COPY
  under tests/ with fixtures.
- exec://sort tests: mirror testexecchannel's fixture set (check its
  exe_skip/obj_skip status before assuming JIT-only).
