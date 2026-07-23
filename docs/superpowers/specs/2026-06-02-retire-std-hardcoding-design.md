# Retire ALL std:: Hardcoding — Design Spec

> **Status:** design / brainstorming. Finishes the direction of
> `2026-05-31-stdtypes-as-real-classes.md` (which migrated string + vector/map/set but left
> streams, conversions, and a residual wrapper/hardcoded-literal layer behind). **Rewritten
> 2026-06-02** to remove all per-type framing: this is ONE generic mechanism, not a set of
> type-specific migrations.

## Guiding principle (the invariant)

**madc hardcodes ONLY the C/C++ primitive basis; every other type is COMPOSED from it.** This is
the whole point of C/C++: the language hardcodes a tiny set of fundamentals — `void`, `char`,
`short`/`int`/`long`/`long long`, `float`/`double`/`long double`, `_Bool`, and the *composition
mechanisms* (pointer, array, struct, union, enum, function) — and **everything else is built from
those**. `std::string`, every stream, every container, and every user type is therefore an
**ordinary composed type (a `DataDefCLASS`/`DataDefSTRUCT` built by parsing its declaration)** —
NOT a privileged builtin. There must be **no builtin DataDef instance and no `DataType` enum tag
for any non-primitive type**. (madc already did this for `dtSTRING` in P2.14; streams never got it.)

**madc is a C++ front-end against the real libstdc++ — exactly like g++/clang++ + the linker.**
A symbol declared in a `#include` header *exists*; a use site is resolved at compile time to that
declaration, the symbol is **mangled from the declaration**, and the linker resolves it against
libstdc++. There is **no per-type code** — no "string handling," no "stream handling." String,
streams, containers, and any future or user C++ type all flow through the **same generic path**.

**Why this is the only durable anti-drift design:** any type-specific code OR type-specific
type-tag is a thing that can rot. Removing them entirely — types are composed data, mechanism is
generic — leaves nothing to drift. Type-specific cruft is both a drift risk *and completely
unnecessary*, because the primitive basis + composition already expresses every type.

**The ONLY hardcoded data in the entire system is the auto-include trigger map** (symbol → header
to include, e.g. `cout`→`<iostream>`). That is a lookup table by nature — config, not logic.
Everything else is either **generic mechanism (code)** or **a type declaration (header data)**.

Concretely, the end state has:
- **No hardcoded symbol literals** — zero `_ZSt…` strings; every symbol is mangler-generated.
- **No extern-C wrapper shims** (`string_*`, `streamout_*`, `streamin_*`, `*stream_open/good/…`,
  `sstream_*`, `__std_stoi`, `__std_to_string`).
- **No builtin DataDef instances** (`ddSTRING`, `dd*STREAM`) and **no `dt*STREAM` enum tags**.
- **No per-type lowering** (`SK_COUT`, `ostream_insert_symbol`, `translate_ostream_chain`,
  `add_string_methods`, `add_fstream_methods`, the `STR_*` statics, every string/stream branch).

*Hardcoded helpers/types/symbols are drift. The only durable defense is to have no per-type code
left to drift — types become header data, resolved by generic machinery.*

## Scope note: SMAUG and gcc-torture are BOTH pure C — neither exercises this work

SMAUG is **C89/C** and **gcc.c-torture** is **C** — neither instantiates any std:: C++ class, so
neither *exercises* these changes. The **functional gate is the C++-specific integration tests**
(`testfstream`, `testloop`, `testcin`, the string/container tests). gcc-torture and the SMAUG soak
are **regression safety nets only** — this work must leave them byte-identical, and the zero-diff
is the proof of isolation.

---

## The model: declaration (header) + generic resolution + mangling + ABI

### Types live in headers (data), distinguished only by declaration-vs-definition

| Form in the `std::` header | Generic treatment | Examples |
|----------------------------|-------------------|----------|
| **bodyless** method/operator/ctor/dtor/global | external symbol: mangle from the declaration, call it, linker resolves against libstdc++ | `string`, `ostream`, `istream`, `ofstream`, `ifstream`, `fstream`, `stringstream`, `cout`/`cin`/`cerr`, `getline`, `stoi`, `to_string` |
| **with a body** | madc compiles the body (template instantiation, as today) | `vector`, `map`, `set` |

This is just C++ separate compilation. No "libstdc++-backed type" special case — bodyless = a
declaration whose definition lives elsewhere (libstdc++), exactly like any `extern` function.

### A use site is ordinary overload resolution + mangling

`cout << x`, `outf.open(f)`, `s.length()`, `a + b`, `getline(inf, line)` are all resolved the same
way: look up the declared candidates, pick the overload matching the argument types, **mangle the
chosen declaration's signature**, emit the call. `a << b << c` is just left-associative operator
calls. There is no stream-specific or string-specific path.

### ABI is derived from the declaration, generically

- **Object size/layout — derived from the parsed declaration.** The header declares the real data
  members; madc computes `sizeof`/alignment/member-offsets with the **same generic struct-layout it
  runs on any struct** (e.g. `string { char *_M_p; unsigned long _M_len; char _M_buf[16]; }` →
  32, matching libstdc++). No `sizeof()` probe, no macro, no literal byte count — the declaration
  *is* the layout, exactly as g++ derives it from the real header. **madc itself must contain NO
  reference to a real std:: type** (no `#include <string>`, no `sizeof(std::string)` — kill the
  existing `string_obj_words()`); that would be std:: knowledge hardcoded into the compiler.
  *Correctness guard lives in a DOCTEST ONLY:* a unit test `#include`s the real libstdc++ headers and
  asserts `madc's header-derived sizeof/layout == sizeof(real std::type)`, catching an ABI change at
  test time — never inside madc.
- **`std::ios` virtual-base `this`-adjustment:** the header declares the real inheritance
  (`ios ← ostream ← ofstream`, …) with the real base members; generic base-subobject-offset logic
  (already in the class model) computes the offset **from the layout madc just built** when calling
  an inherited method. Not stream code, not a probe.
- **By-value `sret` returns** (`operator+`→`string`, `to_string`, `getline`→`istream&`): emitted
  through the existing generic `__retbuf`/sret machinery, driven by the declared return type.

---

## Generic-capability work-items (the ONLY code to write)

These are type-agnostic; completing them makes every std:: type work with zero type-specific code.

### W1 — Complete the mangler (single source of every symbol)

> **ROOT CAUSE (2026-06-02, `tmp/mangle_probe.cpp` vs `c++filt`):** `madc_mangle` mishandles the
> **complete-specialization** abbreviations `So`/`Si`/`Sd`/`Ss` — it treats them as class-template
> names and appends template args, e.g. `ostream::operator<<(double)` → `_ZNSoIcSt11char_traitsIcEElsEd`
> instead of the real `_ZNSolsEd`. Per the Itanium ABI, `So` *already denotes*
> `std::basic_ostream<char,std::char_traits<char>>` (emit `So`, stop); only `Sa`/`Sb`/`St` take
> further components. `std::string` escaped the bug only because C++11 `__cxx11::basic_string`
> isn't spelled `Ss`. **This bug is why the stream symbols were hardcoded** — the author hit the
> wrong output and pasted the `c++filt` literal rather than fixing the mangler (a shortcut → days
> of drift).

- Fix the `So`/`Si`/`Sd`/`Ss` complete-specialization abbreviation handling.
- Add a public helper for **non-member std template operators**
  (`std::operator<<<char_traits<char>>(basic_ostream<char>&, const char*)`).
- **Round-trip unit test:** every symbol madc generates is fed through `c++filt` (or matched
  against `nm -D libstdc++`) and must demangle to the intended declaration. This is the permanent
  guard against silent mangling drift.

### W2 — Non-member operator overload resolution

Member-operator resolution already exists (user classes via `class_operator_call`). Verify/extend
generic resolution to also consider **non-member `operator` functions declared in a namespace**
(so `os << v` can bind `std::operator<<(ostream&, const char*)` when no member overload matches).
This is the one resolution gap streams expose; it is generic, not stream-specific.

### W3 — ABI-from-declaration plumbing

- Generic base-subobject-offset application for an inherited method call (vbase), offset **computed
  from the declared inheritance/layout** (not a probe).
- Confirm the generic sret/`__retbuf` path handles libstdc++ by-value struct returns through MIR.

### W4 — `extern` globals declared in headers

`cout`/`cin`/`cerr` resolve as ordinary `extern` globals of the declared stream type; their symbol
is mangled from the declaration. Removes `make_hidden_std_global(... rdbuf())` and `SK_*`.

### W5 — Auto-include map entries (the sole permitted hardcoding)

Add the std:: symbol→header triggers (`cout`/`cin`/`cerr`/`endl`→`<iostream>`,
`string`→`<string>`, `ofstream`/`ifstream`/`fstream`→`<fstream>`, `stringstream`→`<sstream>`,
`getline`/`stoi`/`to_string`→appropriate header) to the existing auto-include table. This is the
only hardcoded association in the design.

---

## Headers to author (type knowledge = data)

- `include/madc/string` — declare `std::string` (bodyless surface; the size macro).
- `include/madc/iostream` (+ the `ios`/`ostream`/`istream` base declarations) — `cout`/`cin`/`cerr`
  globals, `operator<<`/`>>`, manipulators, the inheritance chain.
- `include/madc/fstream` — `ofstream`/`ifstream`/`fstream` deriving from the iostream bases.
- `include/madc/sstream` — `stringstream`.
- Conversions (`stoi`/`stol`/`stof`/`stod`/`to_string`) declared in their headers.

Sizes, member offsets, and base-subobject offsets are **computed by madc from the declared members**
(generic struct layout) — no probe macros, no literals, and **no reference to a real std:: type
inside madc**. Each header's layout accuracy is cross-checked **in a doctest** (which `#include`s the
real libstdc++ header and compares) — never inside the compiler.

---

## Deletion list (everything per-type — removed, not migrated)

`SK_COUT`/`SK_CERR`/`SK_CIN`; the `_ZSt…` literals (`cir_builder.cpp:1628-1725`);
`ostream_insert_symbol`; `translate_ostream_chain`; the `STR_*` statics; all `string_*`,
`streamout_*`, `streamin_*`, `ifstream_*`/`ofstream_*`/`fstream_*`, `sstream_*`, `__std_*`
wrappers (`madc_mir_backend.cpp`, `madc_stream_runtime.cpp`); `add_string_methods`,
`add_fstream_methods`; `ddSTRING`/`DataDefSTRING`, `dd*STREAM`/`DataDef*STREAM`; `dt*STREAM` enum
tags and their parser/cir_builder branches; `make_hidden_std_global`.

---

## Sequencing (lowest blast radius first — but each is the SAME generic work)

The work-items above are built/proven where the blast radius is smallest, then everything else
falls out for free because there is no per-type code:

1. **W1 mangler fix + round-trip test** (foundation; proven against `c++filt`, no behavior change).
2. **W2/W3/W4 on file streams** — isolated, fixes `testfstream`/`testloop`, exercises the hardest
   ABI (vbase) and non-member-operator resolution. Delete the file-stream + ostream per-type code
   as the generic path covers it.
3. **iostream ops + cin** (`testcin`) — same generic path; delete `streamout_*`/`streamin_*`,
   `ostream_insert_symbol`, `translate_ostream_chain`, `SK_*`, the literals.
4. **string** — the generic path already covers it; delete the residual `string_*` wrappers +
   `ddSTRING` + `STR_*`. Highest test count, so last; migrate test-by-test keeping green.
5. **stringstream + conversions + final grep-gate.**

Each step keeps the C++ integration suite green; torture/SMAUG stay byte-identical.

---

## Validation (end state)

- `make -C src fulltest` — C++ integration tests are the functional gate; never drop the count;
  every per-type path is deleted only once the generic path covers it (no red branch tip).
- **Doctests (development-time only, never inside madc):** (a) mangler round-trip — every generated
  symbol demangles correctly via `c++filt`; (b) layout cross-check — madc's header-derived
  `sizeof`/offsets for each std:: type equal the real libstdc++ `sizeof` (the test `#include`s the
  real headers; madc does not).
- Full gcc-torture failset-diff — **zero** (proof of isolation). SMAUG soak clean (safety net).
- **Grep-gate:** `grep -rn "_ZSt\|dt.*STREAM\|dd.*STREAM\|ddSTRING\|streamout_\|streamin_\|string_concat\|string_obj_words\|sizeof(std::\|__std_\|SK_COUT\|ostream_insert_symbol" src/ include/`
  returns **zero** (outside the mangler and the auto-include map).
- `--emit=c11` of a string/stream program shows the class struct + mangled libstdc++ calls only.
- New `.claude/rules/` rule: std:: symbols are mangler-generated, never hardcoded literals; the
  only hardcoded std:: data is the auto-include trigger map.

## Risks & mitigations

- **vbase `this`-adjustment (highest):** proven on file streams (step 2) before string; if the
  probe+offset approach can't match g++, stop and re-design.
- **Non-member operator resolution (W2):** if generic resolution can't yet pick a namespace-scope
  `operator<<`, that capability is the real deliverable — build it generically, never re-add a
  stream-specific symbol picker.
- **libstdc++ ABI/version sensitivity:** sizes/offsets/symbols are probed/generated, not literal —
  they track the host libstdc++ (the same dependency g++ has).
- **Exceptions from libstdc++:** out of scope (tests don't enable stream exceptions).
