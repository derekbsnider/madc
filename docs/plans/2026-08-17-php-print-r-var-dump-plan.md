# php::print_r / php::var_dump over ANY madc type — design + slices (2026-08-17)

## Status

**IN PROGRESS — session #100 (2026-08-17), branch
`feature/php-dump-intrinsics-claude`.** Owner-approved in session #99:
mechanism (approach A), return type (`madc::value`), PHP-fidelity rule, pointer
recursion with loop detection, and the iteration-protocol prerequisite
(Option 1).

Landed so far (each its own commit, each green in JIT + `--exe` + `--obj`):

| slice | state |
|---|---|
| S0 — the range-for crash | **DONE.** Type-checked index protocol + one owner for the `operator[]` call. `tests/testforeachkeyed.mad`, `tests/testforeachrefindex.mad` |
| S3a — `print_r` scalars | **DONE.** `tests/testphpprintr.mad`, PHP-oracled |
| S3b — `print_r` aggregates | **DONE.** structs, classes, unions, anonymous unions, bit-fields, fixed arrays. `tests/testphpprintrstruct.mad`, PHP-oracled |
| S4 — `var_dump` | **DONE.** One walk, two renderers; real C type words. `tests/testphpvardump.mad`, PHP-oracled |
| S5a — positional containers | **DONE.** `std::string` as text, `std::vector` / `std::array` as arrays, via the shared type-checked protocol. `tests/testphpseq.mad`, PHP-oracled |
| `print_r($x, true)` — PHP's `$return` | **DONE** (session #101). One function, default second parameter, `madc::value &` return holding the text or `true`. `tests/testphpprintrreturn.mad`, PHP-oracled. See §13. |
| S6 — `madc::value` / `array` | **DONE** (session #102). All nine kinds, arbitrary nesting, both flavors, via ONE runtime walk. `tests/unit/test_dump_value.cpp` (byte-exact) + `tests/testphpdumpvalue.mad`. See §14 — it corrects §12.7. |
| container refusal by name | **DONE** (session #102). `std::map` said `_Rb_tree_color`; it names the container now. `tests/testphpdumprefuse.expect_err`. See §14.6. |
| multidimensional arrays | **DONE** (session #102). The walk carries the dim chain and recurses per dimension. `tests/testphpdumpmultidim.mad`. See §15 — and the c2mir silent-wrong-answer it uncovered. |
| S3c — pointers + `*RECURSION*` | **DONE** (session #103). A generated dumper FUNCTION per (pointee, flavor) + the shared runtime ancestor stack. Rings, self-pointers, mutual rings, shared-acyclic, null, pointer-to-pointer, pointer members. `tests/testphpdumpptr.mad` (+ `.expect_quiet`), PHP-oracled byte-for-byte. See §17 — it supersedes §16.1's rejected attempt. |
| enums | **DONE** (session #103). `DataDefENUM::enumerators` is the one owner; PHP 8.1's enum shape with the REAL backing type. `tests/testphpdumpenum.mad` (+ `.expect_quiet`), PHP-oracled. See §18. |
| S1 — `begin()`/`end()` protocol | NOT STARTED — **resequenced**, see §12.5. Owed regardless of the dumper: `for (auto &kv : m)` does not work today. |
| S5b — associative containers | NOT STARTED — gated on S1. `std::map` / `set` / `list` are still refused BY NAME; this is the last uncovered shape. |

**COVERAGE, not slices** (the standing directive after session #101 released on a
one-slice "done"): a measured shape table for what works and what is refused
lives in the live handoff, and is re-measured — never summarised — before any
release claim.

§12 records what implementation TAUGHT us that the design could not know.

This document is the implementation contract. A cold reader should need nothing
else except the files it names.

## 1. What this is

`php::print_r(x)` and `php::var_dump(x)` for **any** madc type — not just
`madc::value` / `madc::array`. A struct, a `std::vector<int>`, a `char *`, an
`int`, a pointer to a SMAUG `CHAR_DATA`.

**THE GOVERNING RULE (owner, stated twice):** *it should work the way a PHP
developer would expect it to work.* A `std::vector<int>` prints the way PHP
prints an array of ints. A `std::string` prints as its text. The **only**
deliberate divergence is that `var_dump` reports real C/C++/madc types instead
of simulated PHP ones.

Everything below follows from that rule. When a later decision is unclear, the
tiebreak is "what would a PHP dev expect", then "what is the C truth".

## 2. The output contract — captured from PHP 8.3.6, verbatim

`php-cli` is INSTALLED in the container (8.3.6, `sudo apt-get install php-cli`,
session #99). It is **not** in `scripts/provision_container.sh` — add it there in
slice 1 or a container rebuild eats it and the loss reads as green
(`project_container_provisioning`).

These captures are the fixture oracle. Do not retype them from memory.

### print_r — flat array

```
Array
(
    [0] => 1
    [1] => 4
    [2] => 9
)
```

### print_r — nested (the quirks that matter)

```
Array
(
    [0] => 1
    [1] => Array
        (
            [0] => 2
            [1] => 3
        )

    [k] => Array
        (
            [deep] => Array
                (
                    [0] => 4
                )

        )

)
```

Rules extracted: the nested type word sits on the `=> ` line; the nested `(` is
indented **8** past the parent's `(`; entries are `(`-indent + 4; the closing `)`
matches its `(`; and a nested block is followed by a **blank line**.

### print_r — object, with PHP's visibility annotation

```
Foo Object
(
    [prop] => 1
    [s] => hi
    [p:Foo:private] => 2
)
```

### print_r — scalars (no trailing newline; `|` is the probe's separator)

```
42|
hi|
3.5|
1|
|
|
```

`true` → `1`; `false` → empty; `null` → empty.

### var_dump — scalars, array, object

```
int(42)
string(2) "hi"
float(3.5)
bool(true)
NULL
array(2) {
  [0]=>
  int(1)
  [1]=>
  array(2) {
    [0]=>
    int(2)
    ["x"]=>
    int(3)
  }
}
object(Foo)#1 (3) {
  ["prop"]=>
  int(1)
  ["s"]=>
  string(2) "hi"
  ["p":"Foo":private]=>
  int(2)
}
```

Indent is **2** per level; the key line and the value line are separate.

### Recursion markers (both, exactly)

```
Array
(
    [0] => 1
    [self] => Array
 *RECURSION*
)
```

```
array(2) {
  [0]=>
  int(1)
  ["self"]=>
  *RECURSION*
}
```

Note `print_r`'s odd shape: the word `Array` on the entry line, then a line
holding ONE leading space before `*RECURSION*`.

## 3. Surface and boundary

```cpp
namespace php {
    // ONE function, default second parameter — PHP's own signature. See §13;
    // the two-overload spelling this block used to show was WRONG (owner,
    // 2026-08-17) and is what sent §12.3's blocker analysis down a dead end.
    template<class T> madc::value &print_r(const T &v, bool ret = false);
    template<class... Ts> void var_dump(const Ts &...vs);    // PHP's is variadic
}
```

Declared in `include/madc/ns_php` (which already has a real `namespace php { }`
block at line 42), **declaration-only, no definition anywhere**.

- **A template declaration is the only honest spelling.** "Any type" has no
  signature a host implementation could satisfy. It also buys the whole
  existing machinery: name lookup, overload resolution and `--std=` behaviour
  need no new interception hook, and generic madc code composes for free
  (`template<class T> void f(T x) { php::print_r(x); }` lowers at instantiation,
  where `T` is concrete).
- **NOT EXPORTED, structurally** (owner: *"I don't think var_dump() can be
  exported to be used successfully by a C/C++ program outside of madc"*). A
  declared-but-undefined template IS an unresolved symbol for any C++ host that
  calls it. That is the desired behaviour, not a gap to patch.
- **Declare the exception out loud.** These are the FIRST deliberate exception to
  `cpp-first-api.md`'s rule that script-facing namespace publics resolve
  mangled-direct to real host implementations. Every other `php::` public does;
  these cannot, because the compiler *is* the implementation. Record it in
  `docs/rules/cpp-first-api.md` as a named exception in the same commit that
  adds the declarations — an undocumented inconsistency is how the next reader
  "fixes" it wrongly.
- **Return type is `madc::value`, not `std::string`** (owner asked; decided with
  evidence): `<ns_madc>` is explicitly value-first ("the primary API is typed in
  madc::value and const char\*... so madc::-only scripts never pay the `<string>`
  cost"); what PHP returns from `print_r($x, true)` is a *dynamic* string, which
  `value` models and `std::string` does not; and availability is strictly better
  — probe: `madc::value` compiles under `--std=c++17`, while `php::` itself does
  NOT (`<ns_php>` fails there: `ns_php:19:28: error: Failed to find type 'array'`).
  So `value` can never be the limiting factor. No `std::string`-returning twin:
  you cannot overload on return type, and `value.c_str()` already serves the
  C++-flavoured use. YAGNI.
  **AMENDED §13.1:** the return is `madc::value &`, not `madc::value` by value —
  a `value` has no C type to return (it lowers to an opaque `long long[6]` buffer
  whose decay-to-pointer argument passing depends on), so by-value emits `int` and
  silently returns nothing. The reference is madc's own value idiom and needs no
  representation change. The DECISION (value, not std::string) is unchanged.
- **Gating: none new.** `php::` already sits behind
  `registration_policy.enable_php_namespace` (`parser.cpp:23165`), so a host that
  disables the namespace loses these too, correctly and for free.
- **Interception point: the CIR builder, at instantiation** — not the parser,
  where `T` may still be dependent. The parse-once tsubst spine already delivers
  concrete types there (`parse-once.md`).
- **Misuse is a loud error:** `&php::print_r`, passing it as a callback, or
  calling through a function pointer. It has no address because it has no body.
- `php::print_r(x, false)` must PRINT (PHP's default). Honour `ret` as a
  compile-time constant when it is a literal; fall back to a runtime branch
  otherwise.

## 4. Mechanism — approach A, lowered per-type dumper

At instantiation madc knows `T`, so it **generates a dumper specialized to `T`**:
recurse into members, call the container surface where the protocol matches, emit
the text. No runtime type-descriptor table exists at all.

Why not descriptors + a generic runtime walker (rejected): semantic rendering
requires **calling member functions** (`size()`/`operator[]`, or `begin()`/`end()`
after slice 2), and calling needs code, not metadata. A descriptor table would
have to carry function pointers to per-type thunks — approach A with a table
bolted on. It would also put descriptor bytes in every binary that merely
includes the header.

Consequences that make A the right fit here:

- Costs exactly nothing for types nobody dumps.
- Flows through `--emit=c11` by construction: the generated dumper is ordinary
  emitted C.
- Matches how madc already instantiates templates on demand.

## 5. The type walk — a PROJECTION, not a computation

**`DataDefSTRUCT` is already the one aggregate-layout owner** (made so in
v0.79.0: "makes `DataDefSTRUCT` the one aggregate-layout owner and carries its
answer through versioned MC11 records to c2mir and emitted C"). It already holds,
index-parallel, every fact a dumper needs (`include/datadef.h` ~line 644):

| member | what it gives the dumper |
|---|---|
| `members` | name + type per field |
| `member_offsets` | byte offset in the finalized layout |
| `member_counts`, `member_array_flags`, `member_dims` | fixed arrays, multidim |
| `member_bitfields` | storage offset/size, bit offset/width, signedness |
| `member_access` | 0 / `vfPRIVATE` / `vfPROTECTED` → PHP's `[p:Foo:private]` |
| `member_origin` | which base it flattened from (-1 = own) |
| `anonymous_aggregates` | anonymous struct/union grouping already resolved |
| `member_vbase` | virtual-base membership and offset resolution |
| `member_explicit_align`, `pack`, `max_align` | layout truth |

**LAW for this arc: the dumper NEVER computes an offset, size or alignment.** It
reads `DataDefSTRUCT`. Any arithmetic in the dumper is a second layout truth and
therefore a bug (`check-one-aggregate-layout.sh` exists because of exactly this).

Emit a dumper-side field node (name, offset, type, access, bitfield, array
extent, origin). Do **not** try to reuse `SchemaField` (`include/madcdis/schema.h`)
as the descriptor: it is storage-shaped (`nullable`, `persisted`, `key`,
`text_overflow`) and its `resolved_kind()` classifies by substring —
`type_name.find("int")` matches `"Point"`, so a struct named `Point` with an
unset `field_kind` resolves as an integer. Classification here is a **type
predicate on `DataDef`**, per `enum-over-strings.md`, never a name match.

**This walk is the cheapest first customer for L2.1** of
`docs/plans/2026-08-08-flr-random-access-struct-schema-plan.md` ("a `SchemaInfo`
FROM a madc type — record_size = sizeof, field offsets = offsetof, bitfields →
set semantics, enums → enum semantics, nested structs flattened with qualified
names"), which is designed and unbuilt. A dumper needs the same walk with no file
format, no producer/reader ABI assumption and no query IR. Build the walk so a
`SchemaInfo` projection is a later addition over the same owner — do NOT build
that projection now (YAGNI), and do NOT let the dataset binding grow a second
walker.

## 6. Rendering rules

One walk, two renderers. Both take the field list; they differ only in text.

| type | print_r | var_dump |
|---|---|---|
| integer | `42` | `int(42)`, `long(42)`, `unsigned(42)` — the REAL type |
| floating | `3.5` | `double(3.5)` (NOT PHP's `float`) |
| `bool` | `1` / empty | `bool(true)` / `bool(false)` |
| `char` | the character | `char('a')` |
| `char *` / `char[]` | the text | `char *(2) "hi"` — length like PHP's string |
| `std::string` | the text | `std::string(2) "hi"` |
| struct/class | `Point Object` + `( [x] => 1 )` | `struct Point(2) { ["x"]=> int(1) }` — PHP's `#1` handle DROPPED, it means nothing in C |
| array `T[N]` | PHP array shape, indices `[0..N-1]` | `int[3](3) { ... }` |
| enum | the enumerator NAME when the value matches one, else the number | `enum Color(RED)` |
| union | EVERY member's interpretation of the same storage — C offers no active-member truth; say so in the docs | same, with a `union` type word |
| pointer (non-null, complete pointee) | follow: render the pointee | `CHAR_DATA *(0x55f0…) { … }` — address AND contents |
| null pointer | empty (PHP's null) | `NULL` |
| function pointer | the address only, NEVER followed (owner) | same |
| `void *` / incomplete pointee | the address only | same |
| reference | the referent (a reference is an alias) | the referent |
| bitfield | the value | the value; the width belongs in the type word |
| private/protected member | `[p:Foo:private]` | `["p":"Foo":private]` — PHP's exact spellings |

**Recursion:** follow pointers, with a visited-address set. On revisit emit
PHP's own `*RECURSION*` in the exact shapes captured in §2. **No depth cap** —
PHP has none, and the owner's rule is PHP expectations; a runaway dump of a whole
MUD world is the same footgun a PHP dev already lives with. Document it.

**Known inherent hazard, to be documented not defended against:** C can hand you
a garbage pointer, and following it faults. PHP structurally cannot have this
problem. No heuristic distinguishes a valid pointer from an invalid one, so do
NOT add one — a validity guess that lies is worse than a fault.

## 7. The container protocol — and its PREREQUISITE DEFECT

`translate_foreach_loop` (`src/cir_builder.cpp` ~21937) resolves a class's
iteration surface **by name**:

```cpp
std::string szname = "size", opname = "operator[]";
Variable *szmv = ccls->findMethod(szname);
Variable *opmv = ccls->findMethod(opname);
```

Two verified consequences (session #99 probes):

1. **`for (int v : std::map<int,int>)` SIGSEGVs.** It finds
   `map::operator[](const key_type &)` by name and calls it with an integer
   index: `warning -- using integer without cast for pointer type parameter`,
   then `caught SIGSEGV at address (nil)` inside
   `map_int32_t_int32_t_..._operator[]`. **g++ on identical source REJECTS it at
   compile time**: `error: cannot convert 'std::pair<const int, int>' to 'int' in
   initialization`. madc crashes where canon errors.
2. **`for (auto &kv : m)` fails** with `member reference is not a structure or
   union` — the `size()`+`operator[]` protocol cannot express the real element
   type `std::pair<const K,V>`, so associative containers are not range-iterable
   in madc at all.

**Owner decision: Option 1 — extend the protocol.** Add the standard
`begin()`/`end()` range protocol with real element typing, keeping
`size()`+`operator[]` as the fast path for random-access containers. This fixes
the crash properly, makes `for (auto &kv : m)` work (a feature win on its own),
and hands the dumper the element typing that associative rendering needs.

**The dumper's sequence predicate MUST be TYPE-CHECKED, never name-matched** —
`operator[]` genuinely accepts an integral index, `size()` returns an integral,
and the element type is known. Writing it as a by-name `findMethod` inherits the
identical SIGSEGV the first time anyone dumps a map. This is the same by-name
`findMethod` weakness the UFCS work documented as a recovery net.

**Container rendering, once element typing exists:**

- **sequence** → PHP array shape, `[0..n-1]` keys.
- **sequence whose element is a character type** → render as TEXT. This derives
  `std::string` from the sequence rule with no `c_str` name check, and covers
  `std::vector<char>` the same way a PHP dev would expect.
- **associative** → `[key] => value`. Detect by the container exposing a
  `key_type` alias (the signal the standard library itself defines and that
  generic C++ code keys on via SFINAE), reachable through the class
  `type_aliases` map the nested-enum work already uses. RECORDED AS A DEFAULT,
  revisit if it proves too narrow — the alternative considered and rejected was
  keying on a two-member `first`/`second` element, which is a name list.

## 8. Runtime support, AOT, emit-C

The generated dumper is emitted code in the user's TU; the pieces it CALLS are
ordinary runtime:

- the visited-address set for loop detection,
- the output primitives (indent bookkeeping, `string(2) "hi"` formatting, the
  `*RECURSION*` shapes).

These are extern-C `__madc_dump_*` functions — the compiler-machinery exception
in `cpp-first-api.md`, same category as `__madc_scope_set_*`. They MUST be
registered in `scripts/ledger_sources.txt` (the single membership owner), or a
`-static-libmadc` program that dumps will not link on Mach-O, where no dylib
fallback exists. Verify on the `--emit=c11`, `--exe`/`--obj` and headerless lanes,
not only the JIT.

## 9. Execution slices (each vertically complete: code + tests + gate + doc)

**S0 — the range-for crash, its own commit, FIRST.** madc SIGSEGVs where g++
errors. Reducer + g++/clang oracle in `tests/`, `Hypothesis/Layer/Searched/Oracle`
trailers, and it must fail before and pass after. Whether the fix at this stage is
a loud diagnostic or full typing depends on what S1 needs; a crash is not an
acceptable resting state either way.

**S1 — the iteration protocol.** `begin()`/`end()` with real element typing,
`size()`+`operator[]` retained as the random-access fast path, type-checked
selection between them. Gate: `for (auto &kv : m)` over map/set works and matches
g++; the `for (int v : m)` case errors like g++ instead of crashing. Also lands
the `php-cli` entry in `scripts/provision_container.sh`.

**S2 — the type walk.** The `DataDef`/`DataDefSTRUCT` projection: field list with
name/offset/type/access/bitfield/array/origin, anonymous aggregates and
base-flattened members handled, classification by type predicate. Unit tests on
the projection directly (it is pure), including a bitfield, an anonymous union, a
virtual-base member and a `Point`-named struct (the `resolved_kind()` substring
trap).

**S3 — `print_r`, scalars through structs and arrays.** The `<ns_php>`
declarations, the CIR intercept, the runtime output primitives, pointer following
with the visited set. Fixtures oracled against real `php` for every shape in §2.

**S4 — `var_dump`.** Same walk, real type spellings, variadic.

**S5 — containers.** Sequence, text-from-char-sequence, associative.

**S6 — `madc::value` / `madc::array`.** The original motivating case: render the
dynamic value as PHP would render the value it holds.

## 10. Non-goals / stated-not-solved

- No `std::string`-returning `print_r`; `value.c_str()` covers it.
- No depth cap; PHP has none.
- No pointer-validity heuristic; a garbage pointer faults, honestly.
- No `SchemaInfo` projection for the dataset binding in this arc — the walk is
  built to serve it later, and L2.1 must adopt it rather than grow a twin.
- No host-callable export, ever. That is the design, per §3.
- `var_export` / `serialize` are not in scope.

## 11. Evidence log (session #99, all in the container)

| claim | how it was established |
|---|---|
| PHP output shapes in §2 | `php /tmp/or1.php`, `/tmp/or2.php` on php-cli 8.3.6 |
| `php::` unavailable under `--std=c++17` | `bin/madc --std=c++17` on a `php::trim` probe → `ns_php:19:28: Failed to find type 'array'` |
| `madc::value` available under `--std=c++17` | same probe shape, rc=0 |
| range-for over `map<int,int>` SIGSEGVs | `tmp/p_map.mad`, backtrace in `map_..._operator[]` |
| g++ rejects the same source | `g++ -O0 /tmp/pm.cpp` → `cannot convert 'std::pair<const int, int>' to 'int'` |
| `for (auto &kv : m)` fails | `tmp/p_map2.mad` → `member reference is not a structure or union` |
| iteration resolves by name | `src/cir_builder.cpp` ~21980, `findMethod("size")` / `findMethod("operator[]")` |
| `DataDefSTRUCT` holds the layout facts | `include/datadef.h` ~644-700 |
| `namespace php` block is declaration-only for the string family | `include/madc/ns_php:42` |

## 12. Amendments from implementation (session #100)

Everything here was learned by building it. Where it contradicts an earlier
section, THIS section is current.

### 12.1 The walk reads member ACCESSES, not offsets — a stronger law

§5 said the dumper reads `DataDefSTRUCT` and never computes a layout fact. The
implementation does better: it never READS an offset either. Members are emitted
as `obj.member` access nodes (`N_FIELD` / `N_IND`), so c2mir resolves them
against the struct `class_struct_members` emitted, and the dumper inherits — for
free and unbreakably — the bit-field shift/mask rule, anonymous-aggregate
transparency, and base flattening. Any future member-reading code here should
follow the same rule: emit an access, do not address arithmetic.

`member_offsets` is therefore NOT consulted at all. The parts of
`DataDefSTRUCT` the walk does read: `members`, `member_counts`,
`member_array_flags`, `member_access`, `member_origin`, `union_layout`.

### 12.2 A shadowed inherited member is skipped (a named limit)

`class_struct_members` renames a base member hidden by a same-named derived one
to `<name>__flatN`, and that rule has **no reader** — `grep __flat` finds one
site, the emitter. So `obj.<name>` cannot address the hidden member, and the
walk skips it rather than print the wrong storage. Including it requires ONE
owner for "the emitted field name of member i", shared by the emitter and any
reader. That refactor is not part of this arc.

### 12.3 `print_r($x, true)` — ONE function with a default argument

**CORRECTED 2026-08-17 by the owner.** This section previously argued that a
second declaration `value print_r(const T &, bool)` would lose its return type to
the first declaration's `void`. That framing was wrong on its own terms and, more
importantly, solved a problem PHP does not have. PHP's signature is:

```php
function print_r(mixed $value, bool $return = false): string|true
```

**One** function, a **default** second parameter, a **union** return. So:

| PHP | madc |
|---|---|
| `mixed $value` | `template<class T> … (const T &v` |
| `bool $return = false` | `, bool ret = false)` |
| `string\|true` | `value` — madc's mixed type holds either the string or `true` |

`value` is the natural home for `string|true`: a union-typed result is what
`value` is (v0.75.0). No overload set, so nothing to collide.

**The actual blocker, verified in the code:** the placeholder is minted with ZERO
parameters, so a default argument has nowhere to live.
`register_skipped_namespace_template_function` (`src/parser.cpp:50433`) ends at
`pgm.addFunction(parse_id, datatype_vec_t{ret ? ret : &ddINT64}, NULL)`, and
`addFunction`'s second argument is the DATATYPE VECTOR (`include/madc.h:4889`) —
here one element, the return type alone. No parameters ⇒ `FuncDef::param_defaults`
(`include/madc.h:221`) is empty ⇒ the call-site default fill that already works
for free functions (`src/parser.cpp:25199`) and methods (`:18063`) has nothing to
read. The signature is already captured next door by
`capture_free_function_overload` so "the call site can select by arity", so the
fix carries the declared parameters + defaults onto the placeholder. It does not
need new state and it is not an arity mechanism.

**Still open and worth verifying first:** whether a madc function can return a
`value` BY VALUE. Every value-returning entry in `<ns_madc>` is
`value &f(value &out, ...)`, so the by-value path looks unproven rather than
known-good. If it turns out broken, that is an ABI defect to fix — not a reason
to bend PHP's signature into `php::print_r(out, x)`.

### 12.4 `var_dump`'s type word is the CANONICAL type, by construction

There is no "source spelling of a type" owner in madc to reuse: `DataDef::name`
for `int` is not reliably `"int"` (canonicalization maps the simple types by
name, which is why `std::map<int,int>` mangles as `map_int32_t_int32_t_...`).
So `dump_scalar_type_word` maps `DataType` -> spelling in ONE table. The
consequence is honest and worth stating in the docs: var_dump reports the
canonical type, not the typedef the source wrote (`size_t` shows as
`unsigned long`) — exactly what `typeid` does in g++. `long` and `long long`
share `dtINT64` in madc and therefore share the word `long`.

An aggregate is spelled `struct X` / `union X`, never `class X`: madc PROMOTES a
plain struct to `DataDefCLASS` when it earns class-hood, so "class" would be a
claim about the source that the type graph cannot support.

### 12.5 Resequencing: containers before the `begin()`/`end()` protocol (DONE)

The plan ordered S1 (the iteration protocol) second. Implementation shows the
dumper does not need it for the container cases that matter most:
`std::string`, `std::vector` and `std::array` are POSITIONAL sequences, and S0
left behind exactly the type-checked predicate for them
(`class_index_iteration_protocol`). Only ASSOCIATIVE containers (`map`, `set`,
`unordered_*`) need `begin()`/`end()`.

So the order is now: S4 (var_dump, DONE) -> S5a (positional containers,
including `std::string` as a sequence of char, DONE) -> S3c (pointers) -> S1
(begin/end) -> S5b (associative) -> S6 (`madc::value`). S1 is still owed — `for (auto &kv : m)`
does not work today and `for (int v : s)` over a `std::set` errors where g++
runs — it is just no longer the gate for the dump feature.

### 12.6 Pointers (S3c) need a real generated FUNCTION and a runtime stack

The compile-time-expanded walk cannot follow a pointer: `struct Node { Node
*next; }` recurses forever at expansion time. Following pointers therefore
needs

- a GENERATED dumper function per pointee type (so recursion is a runtime call),
  which makes the column a RUNTIME parameter instead of a literal;
- a runtime ancestor STACK, not a visited set: PHP prints the same array twice
  when it appears twice, and says `*RECURSION*` only for a cycle — so the test
  is "is this address currently being printed", push/pop around each aggregate.

Both are new machinery. Nothing landed so far blocks them, and the primitives
already take `col` as an ordinary `int` argument.

### 12.7 `madc::value` (S6) is the ONE type that needs a runtime walker

> ⚠️ **SUPERSEDED by §14.** The first sentence and the runtime-walker conclusion
> hold. The two candidate shapes below, and the instruction to check for an
> exported kind query, were both shaped by a backwards premise — see §14.1.
> Nothing was added to the public C API.

Every other type is walked at compile time because the compiler knows its
shape. A `madc::value` carries its own runtime kind, so its walk must switch on
that kind AT RUNTIME. That makes it the only case that wants a real runtime
function rather than generated code — and it cannot live in `src/rt/rt_dump.c`,
because the ledger's membership rule is strict C11 with no C++ dependency and
the value API is C++. Two candidate shapes, to be decided when the slice starts:
generated code driving the existing extern-C value API
(`madarray_size` / `__php_array_get_*` plus a kind query) in a loop, or a Tier-B
runtime function that a `-static-libmadc` Mach-O program cannot link. The first
keeps the ledger promise; check whether a kind query is exported before choosing.

### 12.8 Runtime home and the ledger

The output primitives live in `src/rt/rt_dump.c`: strict C11, `printf`-based
(NOT `std::cout` — a C++ dependency would disqualify it), registered in
`scripts/ledger_sources.txt` as `all`. That one line is the whole build wiring:
the Makefile derives `RT_OFILES` from the manifest. Verified on the
`--emit=c11` -> `gcc -O0` lane as well as JIT / `--exe` / `--obj`.

### 12.9 What S5a settled about the type word for a container

`var_dump` names a container by `DataDef::canonical_cpp_spelling()`, which is the
only spelling available: the class `name` is a mangled tag
(`vector_int32_t_std__allocator_int32_t_`) no user wrote. It is long and it
carries madc's canonicalization (`int32_t`, not `int`) and the standard library's
ABI namespace (`std::__cxx11::basic_string<...>`, and `std::__1::` under libc++).

Two consequences, both accepted deliberately:

- The FIXTURE cannot pin those lines: `tests/testphpseq.expect` asserts the
  var_dump value lines by their flavor-stable tail (`>(2) "hi"`) and pins the
  print_r blocks — the PHP-fidelity claim — in full. A test that spelled
  `__cxx11` would fail the libc++ lane, which runs the whole suite.
- A shorter alias form was WANTED (owner, 2026-08-17: "I really don't think anyone
  wants to see std::__cxx11::basic_string<...>") and is **DONE** — see §12.10,
  which is now an implementation record, not a proposal. This §12.9 text about
  fixtures asserting flavor-stable tails is SUPERSEDED: the fixture pins whole
  lines, because an alias is flavor-stable.

### 12.10 Getting to `std::string` — invert the datatype map (DONE on `develop`)

**Correction to §12.9's first draft: a type -> alias map DOES exist.**
`Program::namespace_datatype_map` is `namespace -> (name -> TokenDataType *)`, and
`TokenDataType` is `{ std::string str; DataDef &definition; }` — the SPELLING the
source used, bound to the DataDef it names (`include/datatokens.h:11`). `<string>`
says `typedef basic_string<char> string;`, so `std["string"]->definition` IS the
basic_string instantiation.

IMPLEMENTED as `CirBuilder::type_alias_spelling` + `dump_class_type_word` +
`dump_sequence_type_word` + the `dump_type_word` dispatcher (src/cir_dump.cpp).
The rule: **search the datatype maps for an entry
whose `definition` denotes THIS DataDef, and use the shortest qualified spelling
found; fall back to `canonical_cpp_spelling()`, then `struct <name>`.** That is a
type-IDENTITY inversion of the table the source's own `typedef` filled — the
answer is "what did the source call this type", never a pattern match on
`basic_string`. Deterministic tie-break: shortest name, then alphabetical.

Notes for whoever implements it:

- Do the lookup at COMPILE time, once per dumped type; cache per DataDef if the
  scan shows up (the `std` map is a `std::map` enumerated by key).
- `denotes_same_type` / pointer identity: compare through `unqualified()`, and be
  careful that an alias may name a TYPEDEF of the class rather than the class.
- A union must still keep its `union` keyword (§12.9) — the alias, if any, wins
  for the NAME, not for dropping the kind.
- **A SECOND filter is required and was found only by probing:** madc also
  registers the instantiation under its MANGLED tag as a datatype-map key, so the
  first cut returned `std::vector_int32_t_std__allocator_int32_t_` as the "alias" —
  worse than the spelling it replaced. Skip any key equal to the DataDef's own
  `name`.
- `std::vector<int>` has NO alias. It is finished by a second, separable rule: a
  SEQUENCE's word is the template's own name (everything before the first `<`)
  plus its ELEMENT type, which is `operator[]`'s return type — the same one the
  walk dumps. The defaulted allocator and char_traits arguments are dropped
  because they are implementation detail. NOTE what this does NOT do: it never
  parses the canonical argument list, and no per-class template-argument record
  exists to consult (`template_arg_names` lives on the namespace-overload entry,
  not the class). A `std::array`'s extent therefore drops out of the word — the
  count in parentheses carries it.
- Changing the type word changes `tests/testphpseq.expect` (whose var_dump lines
  are currently asserted by their flavor-stable tail precisely BECAUSE the
  spelling was unstable). With `std::string` the fixture can pin the whole line,
  which is strictly better — and flavor-stable, since the ALIAS is the same under
  libstdc++ and libc++.

---

## 13 `print_r($x, true)` — the design, after measuring (session #101)

**Owner go-ahead:** *"alright, let's get print_r completed then."* PHP's signature
is the contract (§12.3): ONE function, default second parameter, union return.

### 13.1 What the probes established, before any design

Four reducers against the live binary (`tmp/r1..r5`), because §12.3's blocker was
recorded from reasoning and had already been wrong once:

| probe | result |
|---|---|
| `value make() { … return v; }` | **compiles, runs, prints NOTHING, exit 0** — emits `int make()`. A SILENT WRONG ANSWER (defect D1 below). |
| struct statement-expression copy-out in C | **works** (`1 4`) — the in-tree MIR fix is real, so a stmt-expr can yield an aggregate. |
| `value &fill(value &out) { out = "filled"; … }` | **rejected** — "assignment of incompatible value" (defect D2 below). |
| `madc::value &` / `*` in a non-madc namespace header | **already in use** (`ns_perl.h`, `ns_js.h`) — the qualified spelling resolves. |

**Why `value` cannot be returned by value, and why that is NOT a missing arm.**
`func_def`'s return-type chain ends at `type_list(ret_dd)` →
`append_decl_type_specs` → `append_type_specs`, which has no `dtARRAY` case and
falls through to `int` — the same fall-through the comment above
`append_var_type_specs` already documents for two storage-class sites, and the
same one that rendered a `DataDefFPTR` as a bare `long`. But adding an arm has
nothing to render: **a `value` has no C type.** It lowers to opaque storage —
`_Alignas(16) long long v[6] __attribute__((cleanup(madarray_destruct)))` plus an
explicit `madarray_construct` — and `is_array_object`'s own comment says argument
passing DEPENDS on that buffer decaying to a pointer. Giving `value` a real
`struct` tag is therefore not a one-line arm — but the cost stated here when this
section was written ("23 `is_array_object` sites plus every decay-dependent
call") is WRONG, and so is the implication that no C type exists. See
`docs/plans/2026-06-12-type-table-value-abi-design.md` §9 for the measured
answer: the 32-byte `madc_value` struct is already declared and `madc::value`
contains it; the real work is copy-on-write at 12 mutation sites in ONE file.

### 13.2 The signature, and why it needs no representation change

```cpp
// include/madc/ns_php
template<class T> madc::value &print_r(const T &v, bool ret = false);
```

`value` holds the captured text when `ret` is true and holds `true` when it is
not — that IS PHP's `string|true`, with **no divergence**. The reference is
madc's established value idiom (all of `<ns_madc>` is `value &f(value &out, …)`),
and a reference return is a supported shape today; only D2's *assignment* is
broken, which generated code does not use.

`madc::value` is also the ONLY correct choice: `std::string` would make
`php::print_r` depend on `<string>`, violating the owner law that madc's own
surface carries no `#include` / PCH / forest dependency.

Also settled: `var_dump` keeps `void`. PHP's var_dump returns void.

### 13.3 Lowering

`lower_dump_call` decides from two facts it already has — whether the call's
result is USED, and what the second argument is:

- **result discarded AND `ret` is a compile-time false** (every call the suite
  makes today) → emit exactly the current code. Pure C11, printf path, no value
  machinery linked. **This is the property worth protecting:** a program that
  never captures still carries nothing.
- **otherwise** → declare a value temp (`array_storage_decl` +
  `array_ctor_call` own the storage, ctor and `cleanup` dtor — no new
  machinery), then:
  - `ret` constant true → walk into a sink, `madarray_assign_cstr(&tmp, buf)`
  - `ret` constant false → walk to stdout, `madarray_assign_bool(&tmp, 1)`
  - `ret` a RUNTIME bool → ONE walk with `sink = ret ? &s : NULL`, then a
    conditional assign. The sink parameter makes this fall out; no duplicated walk.
- the statement expression yields `&tmp`, i.e. the `value &`.

### 13.4 The runtime sink (`src/rt/rt_dump.c`, still strict C11)

Every primitive gains a leading `void *sink` and routes output through ONE
internal writer: `sink == NULL` → stdout, else append to a `{char *buf; size_t
len, cap;}` grown by realloc. One owner for the write, so no primitive can
disagree about where output goes. `open_memstream` is deliberately NOT used — it
is POSIX and the win64 lane has no such function.

### 13.5 The parser change is NOT needed — and would be actively harmful

§12.3 concluded the placeholder must carry its parameters and `param_defaults`.
**Reading the registration through to the end refutes that**, and this is the
third correction in this area, so it is recorded rather than summarised:

> "the 0-param placeholder is **arity-filtered out of the ranking**" … "re-entering
> parseFunction under the placeholder's id would **swallow their parameters** — the
> single-id overload collapse"
> — `register_skipped_namespace_template_function`, `src/parser.cpp`

The zero-parameter placeholder is **load-bearing**. Giving it real parameters
would enter every bodyless namespace template (`std::forward`, `std::addressof`,
`std::declval`, …) into arity ranking it is currently and deliberately excluded
from. That is a change to the whole template call path to serve one intrinsic —
precisely the shape the design rules forbid.

**It is also unnecessary.** Arity is not enforced against the placeholder — which
is exactly why `print_r(x)` works TODAY against a zero-parameter placeholder — so
`print_r(x, true)` parses on the same footing. And the default argument does not
need parser support at all, because **the compiler IS the implementation**:
`lower_dump_call` sees the actual argument list, so "no second argument means
`false`" is the intrinsic's own lowering rule. The declaration's `= false` documents
the contract for the reader; the intercept honours it.

**The reference return needs nothing either.** `returns_reference()` renders a
`T&` return as a pointer, and while `type_list(ddARRAY)` still mis-renders the
POINTEE (D1's fall-through: `int *` rather than a value tag), the ADDRESS is
correct and every value operation is a `void *`-based runtime call
(`madarray_assign_cstr` and friends). That is how `<ns_madc>`'s
`value &eval_unit(value &out, …)` already works. So D1 does not block this slice
— it stays a real defect, just not this one's dependency.

Net: no `src/parser.cpp` change in this slice.

### 13.7 What the implementation changed about §13.3–13.6 (session #101, DONE)

Landed and green: `tests/testphpprintrreturn.mad`, PHP-oracled against
`tmp/or_ret.php`. Five forms — capture, explicit `false`, absent flag, runtime
flag both ways, scalar capture. Corrections the work forced, each found by
running rather than reasoning:

- **The result must be an LVALUE, so the capturing form is HOISTED, not a
  statement expression.** `({ …; &tmp; })` is not an lvalue, and a consumer of a
  `value &` legitimately takes its address — `c = php::print_r(p, true);` failed
  with *"lvalue required as unary & operand"*. The walk is a list of STATEMENTS,
  so for the capturing form it goes to `m_pending_stmts` (the route every object
  temp already takes) and the expression is just the temp's name. The
  statement-expression shape is kept only for the non-capturing form, which needs
  no lvalue.
- **`value` had TWO silent defects in its own declaration path, both fixed here
  because the natural spelling `value s = php::print_r(x, true);` needs them:**
  - a BLOCK-scope `value v = <init>;` got storage + constructor and then
    `continue`d past its initializer (`translate_block`'s statement loop);
  - a FILE-scope one returned from `var_decl`'s array-object arm before the
    dynamic-init queueing, so its initializer never reached
    `__madc_global_init`.
  Both dropped the initializer with no diagnostic: `value a = "hello";` printed
  nothing. **The parser had already resolved the initializer into the registered
  `operator=` call** — an early attempt to select the overload here instead
  produced `madarray_assign_value(&v, *madarray_assign_cstr(&v, "x"))`, which MIR
  rejected. The bug was never a missing selection; it was a dropped statement.
- **A qualified return type on a bodyless namespace template silently became
  `int64`.** `skipped_template_function_return_type`'s backward scan tried each
  token as a standalone identifier against the FLAT `datatype_map`, so `madc` and
  `value` both missed and it fell back to `ddINT64` — the declared
  `madc::value &` return became `long`, and the call site then assigned an ADDRESS
  through the integer path, printing a decimal. Qualified return types now route
  through `resolve_type_token_range`, the canonical resolver the template-id
  branch already used. The `MADC_RETPROBE=<substr>` env probe (already in the
  code) is what localized this in one run.
- **Ordering, worth knowing before filing a bug against it:** a FILE-SCOPE
  declaration with a dynamic initializer runs at main entry, so a top-level
  `value s = php::print_r(p, true);` captures before the surrounding statements
  execute. That is madc's existing model for EVERY type — verified with
  `int copy = seen();` at top level, which behaves identically — not something
  specific to print_r. The test does its work inside functions for that reason.
- **`madarray_assign_*` is never spelled in the new code.** Which runtime entry
  serves which kind is owned by parser.cpp's registered `operator=` table, so the
  dumper reads `FuncDef::emit_symbol` off the registration
  (`class_assign_cstr_operator_def` / new `class_assign_scalar_operator_def`).
  Naming the symbol at a second site is how the two drift.

### 13.6 Two defects this slice UNCOVERED — each its own commit

- **D1 — `value` returned by value silently returns nothing.** Layer chain:
  `func_def` return-type arm → `type_list` → `append_decl_type_specs` →
  `append_type_specs` (no `dtARRAY` case → `int`). The immediate fix is to make
  the fall-through **LOUD** — an error naming the type — because exit 0 with
  empty output is the worst outcome available, and refusing by name is this
  subsystem's established discipline. Reducer: `tmp/r1.mad`.

  ⚠️ **§13.1's "no C type for a value / 23 `is_array_object` sites" estimate was
  WRONG — do not plan from it.** The owner remembered otherwise and was right:
  `madc_value` (`include/madc_api.h:52`) is a real tagged 32-byte C struct, and
  `madc::value` merely CONTAINS it plus two `unique_ptr` members (32+8+8 = the
  emitted `long long[6]`). The corrected, MEASURED recon —
  the two members confined to one file (40 refs, 12 of them mutations, no
  friends), the cell finalizer already wired and called from generated code, and
  copy-on-write as the one genuinely non-mechanical part because the class
  deep-copies today while the cell path shares — is
  `docs/plans/2026-06-12-type-table-value-abi-design.md` §9. Read that, not
  §13.1, before touching this. Owner 2026-08-17: not to be taken on now.
- **D2 — assigning to a `value &` is rejected** ("assignment of incompatible
  value") while assigning to a plain `value` local works. Reducer: `tmp/r3.mad`.
  Not on this slice's path (generated code calls the runtime setters directly),
  but it is a live wrong-rejection of code the value-first API's own shape invites.

## 14 S6 — the `madc::value` kind gamut (session #102, DONE)

> **OWNER, 2026-08-17:** *"the most critical thing for print_r and var_dump to
> support is the full gamut of the madc::value / madc::array values"* … *"and
> those are **easiest** since they are runtime determinable"*

Right on both counts, and the resequencing is the reason: this was scheduled
LAST (behind `begin()`/`end()` and pointers) and is the smallest of the three,
because a value carries its own kind tag. There is no per-type expansion — one
recursive function, the depth as a parameter.

### 14.1 The runtime home — §12.7's open question, answered differently

§12.7 offered two candidates: generated code driving the extern-C value API in a
loop, or "a Tier-B runtime function that a `-static-libmadc` Mach-O program
cannot link", and told the next session to check whether a kind query is
exported before choosing. Both candidates were shaped by a premise that turned
out to be backwards.

**The measured facts.** `include/libmadc/value.h`: the `array` and `object` kinds
are backed by `unique_ptr<vector<value>>` and `unique_ptr<map<string,value>>` —
C++ containers, NOT the 32-byte struct. So a value walk needs the C++ script
runtime no matter where it lives. And `scripts/forest_ledger_gate.sh` **leg 6
already asserts** that a program holding a `madc::value` refuses the ledger path
with a Tier-B message, naming `madarray_construct` / `__php_array_*` as the
reason.

So the C++-ness is not a cost this slice pays — it is a property the value type
already had, gated. The decision follows:

- The walk is **`src/rt_dump_value.cpp`**, ordinary C++, in `src/` and NOT in
  `src/rt/`. `src/rt/` is the ledger lane and the Makefile derives `RT_OFILES`
  from `scripts/ledger_sources.txt`, so a `.cpp` there is not even built.
- Putting it in `rt_dump.c` (what the earlier handoff instructed) would have made
  that strict-C11 file depend on C++, breaking dumping of **ordinary C types** in
  the very lane the membership rule exists to protect. The instruction was wrong
  and the rule is what caught it.
- **No new C accessors were needed.** The earlier plan's step 1 — add count /
  element-at / key-at to `include/madc_api.h` — existed only to let a C11 walker
  reach the containers. A C++ walker uses `as_array()` / `as_object()` directly.
  Nothing was added to the public C API.

### 14.2 One geometry owner — `src/rt/rt_dump.h`

The walk now happens in two places (generated, compile-time columns; runtime,
computed columns), so `8 * depth` needed an owner. `src/rt/rt_dump.h` is it: the
flavor discriminator (`madc_dump_flavor`), `madc_dump_frame_col` /
`madc_dump_entry_col`, and **every primitive's prototype**. `rt_dump.c` includes
it, so the extern-"C" prototypes are compiler-CHECKED against their definitions
— previously an argument-list mismatch between the emitter's `need_dump_extern`
shape and the definition would have linked silently. Same-directory quoted
include, which is the shape the pack-time ledger compile already resolves.

`cir_dump.cpp` converts `CirBuilder::DumpFlavor` to the wire discriminator at
one boundary (`dump_wire_flavor`) and its two column helpers delegate.

### 14.3 var_dump's type word for a value: the KIND

`var_dump` names the real type. For a dynamically typed slot the real type IS the
kind, so it uses `madc::value::kind_name` — the existing single owner of those
spellings — and PHP's `int`/`float`/`bool` read as `integer`/`real`/`boolean`.
Three reasons, in order of weight:

1. No storage word distinguishes `string` from `bytes`, or `array` from
   `object`. `long(42)` would be true of the payload and silent about the slot.
2. It reuses an existing spelling table instead of inventing one.
3. It is honest about dynamism: the reader sees a kind a later assignment can
   change.

`null` keeps PHP's `NULL` — the exception already documented in `rt_dump.c`
("no value" is not a C type). `print_r` diverges from PHP nowhere.

### 14.4 What is NOT byte-identical to PHP, deliberately

- **Object-kind key ORDER.** The backing is a `std::map`, so keys print in KEY
  order; PHP preserves INSERTION order. The oracle file writes its keys in key
  order so the difference stays visible rather than being hidden by a
  reordering.
- **`instance` kind prints a NUMBER, not a type name.** There is no
  type-id → name registry at run time (the segmented typeid table,
  `docs/plans/2026-06-12-type-table-value-abi-design.md` §3). `instance#4242` /
  `instance(24) #4242` reports the identity it HAS; printing `Object` would be
  the quiet guess this arc refuses. When the table lands, the name belongs there.
- **A CAPTURED `bytes` value truncates at an embedded NUL.** The direct-print
  path is binary-exact (`__madc_dump_raw` writes by count), but
  `print_r($x, true)` returns through `madarray_assign_cstr`, which is
  NUL-terminated. Unreachable today — no script constructs a `bytes` value — and
  the fix is a length-carrying assignment, i.e. the value-ABI work.

### 14.5 `*RECURSION*` — implemented, and UNREACHABLE today

PHP marks a CYCLE and only a cycle: a value appearing twice prints twice in full
(oracle `tmp/or_value.php`, `$twice` vs `$cyc`). So the guard is an ANCESTOR
stack, never a visited set.

**A cycle cannot be constructed today.** `madc::value` owns its children through
`unique_ptr` with value semantics, so pushing a value into its own array
deep-COPIES it — the graph is a tree by construction. The guard is therefore
shipped for the refcounted-cell backing on the roadmap (value.h: "their cell
representation arrives with the madcdis pool work"), which is when aliasing can
exist. A recursive printer with no cycle guard is a latent hang in the worst
possible place, so it ships now; it is 12 lines.

**What IS gated** is the FORMAT, which is the part that can silently drift:
`print_r` puts the type word on the entry line and `*RECURSION*` on its own line
indented by exactly **ONE space at every depth** (verified at depths 1 and 2),
with no `(` block and no trailing blank line; `var_dump` puts the bare marker at
the value column. Both primitives are unit-tested against those bytes. The
DETECTION is not gated and cannot be — stated here rather than left implied.

### 14.6 The container refusal was a BUG, and is fixed with the same commit family

`php::print_r(std::map<int,int>)` used to descend four levels into libstdc++'s
red-black tree and refuse at `_Rb_tree_color` — naming a type the user never
wrote and never once saying `std::map`. A map is not a positional sequence (its
`operator[]` takes a KEY, which `class_index_iteration_protocol` already tests
via `key_type`), so `dump_sequence` declined and the member walk took over.

The member walk is right for an ordinary aggregate and wrong for a container:
those members are the library's internals. `container_needs_iterator_walk` in
`cir_dump.cpp` now refuses by the container's own name, using C++'s own
iteration concept — the class advertises `begin()` and `end()` — and nothing
else, so a plain aggregate that happens to expose `size()` is still member-walked.
No begin/end predicate existed (`git grep iteration_protocol|has_begin_end`
found only `class_index_iteration_protocol`). When S1 lands the type-checked
iterator protocol, this predicate's BODY becomes that test and the call site
starts dumping where it now refuses.

Before: `member '_M_t': member '_M_impl': member '_M_header': member '_M_color':
no dumper for type '_Rb_tree_color' yet`.
After: `no dumper for container 'std::map<int32_t,int32_t,...>' yet: its elements
need the begin()/end() protocol`.

Gated by `tests/testphpdumprefuse.expect_err`, which no longer matches if the
diagnostic reverts to naming an internal.

### 14.7 Gates

- `tests/unit/test_dump_value.cpp` — **19 cases / 126 assertions**, BYTE-exact
  against `tmp/or_value2.php`. This is the real gate: a `.expect` fixture asserts
  only that each non-empty line APPEARS, so it cannot see a missing blank line, a
  wrong order, or the trailing space on a null element's `[4] => ` line, and
  print_r's format is made of exactly those. It also reaches the `object`,
  `bytes` and `instance` kinds, which no script can construct.
- `tests/testphpdumpvalue.mad` — end to end through the compiler for the
  script-reachable kinds, including a `value` as a struct MEMBER, where the
  generated walk frames the struct and hands the runtime walk the depth: the
  output has `int(9)` for the C int beside `array(1)` for the value, each walk
  naming what it actually has.
- `tests/testphpdumprefuse.expect_err` — the container refusal, by name.

## 15 Multidimensional arrays (session #102, DONE) — and the c2mir bug they found

### 15.1 The walk carries the dim chain

`int m[2][3]` was refused, and the refusal poisoned the whole dump: one such
member and the entire struct became a compile error. The stated reason was real
— `member_counts` holds the FLATTENED total (6, not 2) and `m[i]` yields a ROW,
so a flat walk reads past the first row — but the dim chain was already recorded
in `member_dims` (struct members) and `Variable::dims` (variables). Nothing was
missing; it was simply not being read.

`dump_any` now takes `const std::vector<carray_dim_t> *dims` in place of the old
`size_t count, bool is_array` pair. **`dims` IS the array-ness**: NULL or empty
means not an array. One parameter instead of two that could disagree — and they
did, which was the bug. `dump_array` gained a `dim_ix` and recurses one level per
dimension, which is exactly how PHP renders one (nested arrays). The access
composes naturally: each level's `eacc` wraps the level above, so the leaf emits
`m[i][j]` and no level knows how deep it is.

The char-array-is-text rule now applies at the LAST dimension only: a row of
`char n[2][8]` is a string, and the level above it is an array of strings —
which is what PHP shows. `var_dump`'s word carries the extents from this
dimension out: `int[2][3]` at the outer level, `int[3]` one level in.

print_r output is byte-identical to php-cli 8.3.6 for both the 2-D and 3-D cases
(diffed programmatically against `tmp/or_multidim.out`, not eyeballed).

### 15.2 The test found a SILENT WRONG ANSWER in c2mir

Writing the struct-member case exposed a defect that had nothing to do with
dumping:

```c
struct S { char n[2][8]; };
struct S s = { { "ada", "bob" } };     /* gcc & clang: [ada][bob] */
```

madc printed `[ada][]`. Exit 0, no diagnostic, wrong data.

**It was c2mir, not madc**, and the bare array was wrong too — `c2m` alone gave
`bare: [ada][]` and `char[2][2][4]` gave `[ab][][ef][]`. madc's parse-time
byte-list expansion (the `c11-transpiler.md` workaround) HID it for a bare
`char n[2][8] = {...}`, because that expansion keys on the DECLARED VARIABLE's
`arr_dims`; a struct member has none, falls through the guard, and reached the
real bug.

Root cause, read off the emitted MIR rather than guessed:

```
	call	memcpy, I_0, fp,      "ada\000", 4
	add	I_2, fp, 4                        <-- must be fp, 8
	call	memcpy, I_1, I_2,     "bob\000", 4
```

`update_init_object_path` descends into an aggregate element until it reaches a
scalar. When a STRING initializes an array sub-object it consumes that
sub-object WHOLE, so the path must not descend into it — the same rule the
function already had for a brace list and for a whole-struct value. Without it
the path was left inside row 0, so the next initializer advanced to `row0[1]`:
offset 1 instead of 8. `rel_offset` then never triggered the gap fill, and "bob"
landed over the middle of row 0.

The fix is the missing third arm beside those two, using c2mir's own
`init_compatible_string_p`. Fixed in `third_party/mir/c2mir/c2mir.c` — in-tree
madc source, maintained like any file (`.claude/rules/build.md`) — and it is an
upstream-worthy bugfix authored here.

Gates: `third_party/mir/c-tests/new/nested-string-array-init.c` (c2mir's own
suite, self-checking, verified against gcc AND clang) and
`tests/teststrarrayinit.mad`. Negative control: the pre-fix `c2m` binary printed
`[ada][]` and `[ab][][ef][]` for those exact shapes.

MIR c-tests after the fix: **1143 tests, 2286 successes, 0 failures.**

### 15.3 Follow-up NOT taken: retiring madc's byte-list expansion

With c2mir correct, madc's parse-time string→byte-list expansion for nested char
array initializers is redundant for the shapes it covers. It is NOT removed
here: it is not a defect, removing it needs its own verification pass over every
shape it serves (including `--emit=c11`), and this commit is a bug fix. Recorded
so it is not rediscovered as a mystery.

## 16 Pointers (S3c) — the design, after a WRONG first attempt (session #102)

### 16.1 What was tried and REJECTED — read this before re-attempting

A compile-time pointer expansion was built, made green for acyclic graphs, and
then **set aside in `git stash@{0}`, not committed.** It followed a pointer by
expanding the pointee inline, guarded by a stack of pointee types on the current
expansion path plus a fan-out budget.

It terminated, and the reasoning was even sound as far as it went: a struct
cannot contain itself BY VALUE (C forbids it), so every unbounded path must
traverse a pointer; the type-path stack refuses a repeat, so depth is bounded by
the number of distinct pointee types. Mutual `A -> B -> A` and longer rings were
caught, not just literal self-reference.

**It is still the wrong design, for the reason the owner gave:**

> *"if you have a linked list of structures with pointers and a loop exists where
> one structure points back to an earlier one in the series, print_r or var_dump
> would loop endlessly without loop detection"*

That is **loop avoidance by refusal, not loop detection.** It terminates only
because it REFUSES `struct Node { Node *next; }` — a linked list, which is the
canonical thing anyone wants to `print_r`. The feature was absent and the check
was hiding that.

Two measured problems on top of the conceptual one:

- **An acyclic DAG bounds the depth but NOT the size.** The stack is a path (it
  must be — see §16.2), so a shared subtree re-expands once per path. A 14-level
  fan-out-2 chain with no cycle at all took **57s and then SIGSEGV**; the 12-level
  one took 6.3s. A budget was added to refuse loudly instead of crashing, which
  is a patch on a design that should not exist.
- ⚠️ **THAT SIGSEGV IS NOT ROOT-CAUSED and is a recorded open defect.** A flat
  hand-written 32,000-statement function compiles fine in 3.3s, so it is NOT a
  per-function size limit — it is specific to the shape the expansion produced.
  Reducer: `tmp/probe/fan14.mad` with `MADC_DUMP_EXPAND_LIMIT` raised (the knob
  exists only in the stash). Whoever lands §16.3 should confirm the generated-
  function design cannot reach it, and if madc/c2mir/MIR can still be crashed
  that way by other means, fix it there.

What CARRIES OVER from the stash: the null rendering (print_r renders a null
pointer as the empty string, keeping only the entry's newline; var_dump prints
NULL at the value column), the `dump_vd_null` builder, `__madc_dump_vd_null` in
rt_dump.c, and `tests/testphpdumpptr.mad` — whose expected output is IDENTICAL
under either design, so it is a real gate on the new one.

### 16.2 The oracle: an ANCESTOR STACK, never a visited set

Captured from php-cli 8.3.6 (`tmp/or_cyc.php`, `cat -A`) because the owner named
`set<void *> visited` as one of the two standard methods and PHP does not behave
that way:

| shape | PHP |
|---|---|
| leaf reachable TWICE, acyclic | printed **twice, in full** — no marker |
| ring `1 -> 2 -> 1` | `N Object` then ` *RECURSION*` |
| self `s -> s` | `N Object` then ` *RECURSION*` |

So a `set<void *>` is WRONG: it would stamp `*RECURSION*` on the second sighting
of a shared node. The structure is a **stack** — push on descend, pop on return —
i.e. "is this address on the path I am currently printing", which is the visited
set narrowed to the current path.

Note also that print_r's marker line is preceded by the OBJECT's own type word
(`N Object`), not `Array`. `__madc_dump_pr_recursion(sink, word)` already takes
the word as a parameter for exactly this.

The owner's second method — a "visited" flag ON each element — is **not available
here**: there is nowhere to put it (these are the user's own structs, e.g. a
SMAUG `CHAR_DATA`), and a dump must not write to the data it reads (const
objects, read-only pages, shared or concurrent access). PHP can do it because it
owns its zvals; madc does not own the pointee.

### 16.3 The design to BUILD

1. **The ancestor stack becomes a shared RUNTIME facility in `src/rt/rt_dump.c`**
   — strict C11, thread-local via the `MADC_RT_TLS` pattern `rt_except.c`
   already uses (the ledger build defines it empty). It must GROW rather than
   cap: a 10,000-node list is legitimate and PHP prints all of it.
   `__madc_dump_anc_push(const void *p)` returns 1 pushed / 0 already-on-path
   (cycle) / -1 could-not-grow, so the generated code branches three ways and an
   allocation failure is never reported as a false `*RECURSION*`.
   **`src/rt_dump_value.cpp` must then USE it** instead of its own local
   `AncestorStack` — one owner for "am I already printing this", not two.
2. **One generated function per (pointee type, flavor)**, memoized:
   `static void __madc_dumpfn_N(void *sink, T *p, int depth, int nested)`.
   Recursion becomes a CALL, which fixes cycles, long lists AND the fan-out
   blowup in one move — a shared pointee is one call per site instead of one
   expansion. Body:
   `if (!p) <null render> else { r = anc_push(p); if (r>0) { <walk *p>; anc_pop(); } else if (r==0) <recursion marker> else <oom marker> }`
3. **Columns stay compile-time constants inside the body.** The geometry is
   LINEAR in depth — `frame_col(d+r) == frame_col(d) + frame_col(r)`, and
   `entry_col(d+r) == frame_col(d) + entry_col(r)` — so the function computes its
   base column ONCE at run time and every column inside is
   `base + <constant>`. This is what avoids threading a runtime column through
   all ~20 builders. Carry the base as a `CirBuilder` member holding the local's
   name, exactly as `m_dump_sink_var` already does, and give the addition ONE
   owner (a `col_expr(fl, depth, entry, origin)` helper that emits either
   `integer(k)` or `N_ADD(base, integer(k))`).
4. **Where the definition goes:** `top_list` in the module assembly
   (`src/cir_builder.cpp` ~26564). Emit the PROTOTYPE at the call site through
   the existing `need_output_extern` so the call is declared before use, queue
   the `N_FUNC_DEF` on a new `m_pending_top_defs`, and drain it into `top_list`
   after bodies translate. There is no existing mid-body top-level queue — the
   lambda/nested-fn hoist happens in the PARSER (real FuncDefs with
   `local_emit_name`), so this queue is new.
5. Inside the body, set `m_dump_sink_var` to the function's `sink` PARAMETER and
   the column base to its base local, then call `dump_any` on the pointee at
   RELATIVE depth 0. `nested` is the parameter, used by the outermost tail and by
   print_r's "a scalar entry owes a newline only inside an aggregate" rule (which
   needs absolute-depth>0, and `nested` is exactly that).
6. Deletes on landing: the type-graph stack, the fan-out budget, and the
   `MADC_DUMP_EXPAND_LIMIT` knob (all stash-only).

### 16.4 Enums — the other outstanding shape, and its one real cost

`DataDefENUM` carries `enum_name` and `underlying` but NOT its enumerators, so
the dumper cannot show a NAME and refuses rather than ship a bare number a later
slice would change. The enumerators exist in three live registrations
(`TokenENUM::parse`: scoped pseudo-namespace / class static members / global
constants) and the forest ALREADY serializes them as a `constvalrec` run on the
`DK_ENUM` record.

The clean shape is `DataDefENUM::enumerators` as the one live owner, populated at
the single point in `TokenENUM::parse` where name and value are both known, and
at the forest restore (which already builds `rt.enumerators`). ⚠️ **The cost is
that `forest_record_enum` (`src/madc_cir.cpp` ~4795) currently reads the
enumerators back out of `prog->namespace_map` keyed by the canonical spelling — a
NAME-keyed reverse lookup.** Making the type the owner means switching that read
too (otherwise there are two answers to one question), which touches the pack
path — so it needs the release / packed / headerless lanes, not just fulltest.

## 17 Pointers (S3c) — BUILT (session #103). Supersedes §16.1's attempt.

§16.3's design landed essentially as written. What follows is what it actually
took, and the two places the design was silent.

### 17.1 The shape that shipped

```c
static void __madc_dumpfn_N(void *sink, T *p, int depth, int nested)
{
        int __dcol = <step> * depth;            /* the base column, ONCE */
        if (p) {
                int r = __madc_dump_anc_push(p, <tag>);
                if (r > 0)       { <walk *p at RELATIVE depth 0>; anc_pop(); }
                else if (r == 0) { <*RECURSION*> }
                else             { <__madc_dump_fail: could not grow> }
        } else { <null render> }
}
```

- **One function per (pointee type, flavor), memoized**, registered in the memo
  BEFORE its body is built — so a body that reaches the same pointee finds a
  CALL to make instead of starting a second expansion that never ends. That is
  the whole cycle fix. On failure the memo entry AND any nested dumper minted
  while building it are rolled back, so a refused dump leaves no dead static
  function behind.
- **`static`, therefore NOT declared through `need_output_extern`** — an extern
  prototype ahead of a static definition is a storage-class conflict. Static is
  also what makes the per-TU name safe: two TUs that each dump a `Node *` get
  their own copy and neither is a duplicate symbol at link.
- **Two pending lists**, `m_pending_top_protos` and `m_pending_top_defs`,
  drained in `translate_module` just before Pass 2. Every prototype ahead of
  every definition, because a mutual `struct A { B *b; } / struct B { A *a; }`
  needs both declared before either is defined.
- The sink is a PARAMETER, so ONE generated function serves a printing dump and
  a capturing `print_r($x, true)` — the memo needs no sink in its key.

### 17.2 The ancestor stack: keyed on (address, TYPE)

`__madc_dump_anc_push(const void *p, unsigned tag)` returns **1** pushed / **0**
already-on-path / **-1** could-not-grow. Three ways, so an allocation failure is
never rendered as a `*RECURSION*` that is not there.

The design did not have the **tag**. It is required:

```c
struct T { int v; int *p; };   T t; t.p = &t.v;
```

`&t` and `&t.v` are the SAME address. Without a type discriminator the walk
pushes `&t` for the `T *` and then sees `&t.v` as already-on-path — a **false
cycle**, i.e. a silent wrong answer, on ordinary C. The tag is the generated
function's own index, which is a bijection with (pointee, flavor) inside the TU;
a whole walk is generated in ONE TU with ONE flavor, so within any path that can
actually be walked it is a bijection with the pointee TYPE.

It GROWS without limit (a 10,000-node list is legitimate and PHP prints all of
it) and it is `MADC_RT_TLS` thread-local, exactly as `rt_except.c` does, with
the ledger build defining that empty.

**`src/rt_dump_value.cpp` moved onto the same stack**, via an RAII frame so an
exception thrown by `as_array()` cannot leak an entry. One owner, not two — and
necessary rather than tidy: the two walks NEST (a value sits in a struct a
pointer reached), so two private stacks would each see half of one path.

### 17.3 Columns: `base + constant`, and the two other runtime facts

The in-line walk is expanded per level, so it knows its absolute depth and every
column is a literal. A generated function is shared by call sites at DIFFERENT
depths, so three things stop being compile-time constants inside it. Each got
ONE owner rather than a parameter threaded through ~20 builders:

| owner | what it decides |
|---|---|
| `dump_col(fl, depth, entry)` | `integer(k)` outside a dumper, `base + k` inside |
| `dump_depth_arg(depth)` | the ABSOLUTE depth, for `__madc_dump_value` (the one primitive that takes a depth, because the runtime value walk computes its own descent) |
| `dump_nested_arg(depth, nested)` | a literal, or the function's `nested` PARAMETER at relative depth 0 |
| `dump_pr_end_entry(fl, depth)` | print_r's end-of-entry newline: nothing, `pr_nl`, or `if (nested) pr_nl` |

This works because the geometry is LINEAR in depth —
`frame_col(a+b) == frame_col(a) + frame_col(b)` — which `rt_dump.h`'s
`madc_dump_frame_col` owns and `dump_col_step` reads.

⚠️ **The bug this shape invites, which bit once:** the context names must stay
SET until every arm is built. Restoring them right after the walk emitted the
var_dump `NULL` at column 0 and dropped print_r's end-of-entry newline entirely,
because the null and cycle arms are part of the same function body and were
being built as if they were top-level.

### 17.4 §16.1's SIGSEGV: no longer reachable through this path

The rejected inline expansion took **57s and then SIGSEGV** on `tmp/probe/fan14.mad`
(an ACYCLIC 14-level fan-out-2 graph). The generated-function design compiles the
same file in **534ms** with correct output, because a shared pointee is one CALL
per site instead of one expansion per path: 15 functions, not 2^14 inline copies.

The crash itself was **never root-caused**, and this does not root-cause it — it
removes the only construct known to reach it (the knob that drove the expansion
past its budget existed only in the stash). A flat hand-written 32,000-statement
function still compiles in 3.3s, so it was never a per-function size limit. If a
future change can produce a similar shape by other means, this is the trail.

### 17.5 Measured, against php-cli 8.3.6

print_r is BYTE-IDENTICAL on every shape (oracles `tmp/or/ptr.php`,
`tmp/or/share.php`, captured with `cat -A`); var_dump matches except madc's
documented real-C-type word.

| shape | result |
|---|---|
| 3-node list, acyclic | **byte-identical**, including the empty `[next] => ` and its newline |
| ring `1 -> 2 -> 3 -> 1` | **byte-identical**, one-space ` *RECURSION*`, no trailing blank |
| self-pointer | **byte-identical** |
| shared leaf, acyclic | **printed twice IN FULL, no marker** — the discriminator that rules out a visited set |
| mutual `A -> B -> A` | `*RECURSION*` with A's own word |
| null pointer | print_r empty, var_dump `NULL` |
| pointer member in a struct | correct at depth |
| `int *`, `int **` | followed through both levels |
| `const char *` member | still TEXT — NOT followed |
| pointer to `std::vector` / `std::string` / `madc::value` | rendered as the pointee |
| 5000-node list, then the same list RINGED | both terminate; the ring differs by exactly the marker |
| `tmp/probe/fan14.mad` | 534ms, correct (was 57s + SIGSEGV) |

### 17.6 The defect the slice uncovered — fixed in its own commit

A `value *` had no real type: `append_type_specs` had no `dtARRAY` arm, so a
value's base type took the `default: int` at every site that spells it through a
DECLARATOR rather than through the storage emitter. `value *p` emitted `int *p` —
a four-byte declaration of a forty-eight-byte object, silent apart from one
c2mir warning because the pointer VALUE was still right.

Fixing the specifier then required `&v` to stop being `N_ADDR` of the storage
array: that yields `long long (*)[6]`, the same address with a different type.
The array NAME already decays correctly, the same way a `T &` parameter's stored
value already IS the address; `&s.v` on a value MEMBER needed the same arm.

This is also the root of **D1** (`value f()` emitting `int f()` and silently
returning nothing). The return path needs declarator work of its own, so D1
stays open. `tests/testvalueptr.mad` carries a `.expect_quiet` fixture, so the
warning cannot return unnoticed.

## 18 Enums — BUILT (session #103). Supersedes §16.4's cost estimate.

### 18.1 The owner

`DataDefENUM::enumerators` — `vector<pair<string,int64_t>>`, DECLARATION order,
duplicates KEPT. Stamped at the ONE point in `TokenENUM::parse` where the name
and the value are both known whichever of the three registration branches ran.

§16.4 was right that no existing structure could answer the question. The three
live stores — a scoped enum's pseudo-namespace, a class-nested enum's
`static_member_*` maps, a plain C enum's global constants — each answer "what is
`Color::GREEN`". None answers "what are `Color`'s enumerators", and the plain-C
registration has no edge back to the tag at all.

`forest_record_enum` now READS the owner instead of re-deriving the run with a
NAME-keyed reverse lookup into `prog->namespace_map`. That also fixes the ORDER
(the map was alphabetical; the tag is in declaration order). The restore
populates the owner, so a bound compile renders what a live one does.

### 18.2 Rendering — PHP 8.1 has real enums, so there is an oracle

Captured from php-cli 8.3.6 (`tmp/or/enum.php`, `cat -A`):

```
Suit Enum:string                 enum(Suit::Hearts)        <- var_dump
(
    [name] => Hearts
    [value] => H
)
```

A C enum IS a backed enum — a name and an integer — so print_r keeps PHP's frame
verbatim and var_dump keeps PHP's one-liner. madc's one divergence is the same
one as everywhere else: the REAL backing type after the colon
(`Color Enum:unsigned int`), which **g++ and clang++ both confirm** is what
`std::underlying_type<Color>::type` is for an all-non-negative unscoped enum
(`int` once an enumerator is negative). The tag uses its CANONICAL spelling, so a
class-nested one reads `enum(Deck::Kind::Tarot)`.

### 18.3 The value -> name lookup

A runtime question, so it is a memoized generated function per TAG — the
machinery §17 just added, reused:

```c
static char *__madc_enumname_N(long long v)
{
        if (v == 0) return "RED";
        if (v == 1) return "GREEN";
        return "";
}
```

A flat if-chain and NOT a switch: duplicate values are legal C
(`enum { A = 1, B = 1 }`) and would be duplicate labels, which c2mir rejects.
The FIRST name wins — the source's order, and what a debugger shows. `char *`
rather than `const char *` because a C string literal IS `char[]`, so this needs
no const plumbing to be warning-free.

**A value naming NO enumerator** is legal C and has no PHP form. The lookup
returns the EMPTY string, which is exactly what print_r renders for a null, so no
call site needs a branch; var_dump reads it as "report the type and the number"
and prints `enum Color(7)`. Neither invents a case that does not exist. An
OPAQUE declaration (`enum E;`) has no table at all and is refused BY ITS OWN
NAME, like every other uncovered type in this file.

---

## 19. Iterator containers — std::map / std::set / std::list (session #104, AS BUILT)

The last uncovered shape. Delivered in commit `f68c4690` (the dumper) and the
range-for's second consumer in the commit after it.

### 19.1 What was actually missing — a RECOGNIZER, not a capability

madc could always iterate these containers in HAND-WRITTEN code:

```c
for (std::map<int,int>::iterator it = m.begin(); it != m.end(); ++it)
        printf("%d=%d\n", it->first, it->second);
```

What did not exist was a GENERATED loop, and the reason was single: madc had
exactly ONE structural recognizer for "is this class iterable" —
`class_index_iteration_protocol` — and it knew only the POSITIONAL shape
(`size()` + integral `operator[]`). Both of its consumers stopped at a
`std::map`: `translate_foreach_class` (built entirely on `size()`+`operator[]`)
and the dumper. So the work was **its twin, plus a loop, plus wiring both
consumers** — never new language support.

### 19.2 `class_iterator_iteration_protocol` — the shared twin

Lives in `src/cir_builder.cpp` beside the positional one, NOT in
`src/cir_dump.cpp`. It is a property of a CLASS and is owed to the range-for as
much as to the dumper; a dumper-private copy is the duplication this repo gates
against. TYPE-CHECKED for the same reason its twin is — naming `begin` proves
nothing, exactly as naming `size` and `operator[]` did not (a by-name match is
what made `for (int v : map)` SIGSEGV):

- `size()` nullary, integral — the loop BOUND (see §19.3);
- `begin()` and `end()` nullary, returning the SAME iterator;
- and then one of two SHAPES:
  - a **class iterator** with a nullary `operator*` and a **prefix**
    `operator++`, whose by-value return is C's native struct return (a
    `__retbuf` iterator is a different call shape and is refused);
  - a **raw pointer iterator** (`int *begin()`), dereferenced and advanced by
    `+= 1` — what a hand-rolled container over a plain array is written with.

It reports its DECLINE reason through `why`, so the refusal quotes the missing
piece instead of describing the walk.

`keyed` is decided STRUCTURALLY: the container names a `mapped_type`. True for
`std::map`, false for `std::set` / `std::list`. Never by class name.

### 19.3 ⚠️ THE LOOP IS COUNTED OFF size() — this is not a shortcut

libstdc++ declares `operator==` and `operator!=` on EVERY one of these iterators
as **friend FREE functions**, not members — `stl_tree.h` 315/320 and 396/401,
`stl_list.h` 318/324 and 408/414. `findMethod` cannot see a free function, so a
member-dispatch `it != c.end()` **cannot be generated at all**. madc does have
free-operator machinery (`Program::free_operator_overloads`,
`CirBuilder::std_free_operator_instantiation`) but every entry point is
TOKEN-driven, and synthesized code has no token.

So:

```c
long n = c.size();  It it = c.begin();
for (long k = 0; k < n; k += 1) { <use *it>; ++it; }
```

It costs nothing: var_dump's head line states the count anyway, so `size()` was
already required. And it is why `size()` belongs to the PREDICATE — a container
with `begin()`/`end()` and no `size()` has no bound, is refused, and the message
says exactly that. That is the ONE remaining refusal, and it is principled:
`tests/testphpdumprefuse.mad` is now built on it.

⚠️ `operator++` needs ARITY selection. Prefix and postfix are both spelled
`operator++` and differ only in the postfix dummy `int`, so `findMethod` picks
either. `class_nullary_call` now selects the nullary overload through
`findMethodOverload(name, {})` — the existing owner of that choice, which
`Program::nullary_operator_call` already used — and takes the symbol of THAT
overload rather than resolving the name a second time. The by-name pick stays as
the fallback, so nothing reachable only through an alias web is lost.

⚠️ In the ADVANCE the reference result is **discarded**, so it must NOT be
dereferenced: `*f(&it);` is a dereference with no effect and every compiler that
looks warns. `class_nullary_call` takes a `discard_value` flag for it.

### 19.4 The KEY — one primitive pair, and the ordinary walk between them

A `std::map`'s element is `std::pair<const K, V>`, and PHP renders a map as
`[key] => value`, never as a pair. So the two halves come from the ELEMENT type's
own `first` / `second` members — which is what `value_type` IS — and nothing in
the walk knows the word "pair".

The key is rendered by the SAME `dump_any` walk as any other value, between two
new primitives (`__madc_dump_pr_key_open` / `_close` and the var_dump twins),
**with the print_r flavor regardless of the caller's**: print_r's scalar form IS
the bare value with no type word and no newline, which is exactly what PHP puts
inside the brackets under BOTH flavors. Depth 0 is what suppresses print_r's
end-of-entry newline. So there is no integral-vs-string key branch at all.

var_dump's one addition is the QUOTES around a text key (`["b"]=>` vs `[3]=>`),
and whether the key is text is a compile-time property of its TYPE —
`dump_type_is_text`, which asks the walk's own text arms (a char pointer, or a
positional sequence of characters).

`rt_dump.c`'s literal key form (`__madc_dump_pr_key`) is rewritten in terms of
the same open/close pair, so the bracket spelling has exactly one owner and a
map's key line cannot drift from a struct member's.

### 19.5 Two word defects this commit fixed, because it would have introduced them

- **`std::__cxx11::list<int>`.** An inline namespace is transparent to qualified
  lookup, so nobody writes it. `strip_inline_namespaces` is driven by
  `Program::inline_namespace_children`, the parser's own record kept for
  [namespace.qual] lookup — so libc++'s `__1` is handled by the same code and no
  spelling is hardcoded. Without it one var_dump line said `std::map<int,int>`
  and the next said `std::__cxx11::list<int>`.
- **A container's word reached through `dump_type_word`** came straight from the
  canonical spelling, so a nested value's head line said
  `std::vector<int32_t,std::allocator<int32_t>>` while the very next line, from
  the sequence walk, said `std::vector<int>`. `dump_type_word` now routes every
  class through `dump_container_type_word`, which asks the two container
  recognizers — the same ones the two WALKS use — so the word and the walk answer
  one question. `dump_template_word` is the one owner of the defaulted-argument
  strip (one informative argument for a sequence, two for a keyed container).

### 19.6 Oracle

php-cli 8.3.6, `tmp/or/map.php` and `tmp/or/iter_pr.php`, captured with `cat -A`.
**All TEN print_r blocks PHP produces for this data are reproduced byte for
byte** — including a nested block's trailing blank line and the empty
container's `Array\n(\n)`. var_dump's key geometry matches too, with this arc's
one documented divergence (the real C++ type in the head word).

Range-for oracle: `tmp/or/feiter.cpp` under `g++ -O0` and `clang++ -O0`, both
agreeing line for line with `tests/testforeachiter.expect`.

### 19.7 Still not covered — say it plainly

- **`for (auto &kv : m)`** — the range-for's element type is never deduced from
  `auto`. A parser deduction gap, independent of the iteration protocol, and it
  fails loudly. `for (std::pair<const int,int> &kv : m)` works.
- **`std::unordered_map`** — does not PARSE yet, for an unrelated reason:
  `hashtable_policy.h:613` uses `__int_traits`, which madc reports as an
  undeclared identifier. Nothing to do with the dump arc.
- **A container with `begin()`/`end()` and no `size()`** — refused, permanently
  and on purpose (§19.3).

### 19.8 A type that contains ITSELF (the fourth #104 commit)

`struct Node { long size(); Node &operator[](long); };` is legal C++ — a
JSON-tree shape — and the walk is EXPANDED per nesting level, so nothing bounded
it: 4 GB, then `tree build failed (std::bad_alloc)`. Pre-existing in
`dump_sequence`; §19.5's `dump_container_type_word` is a SECOND site with the
same shape.

The guard is an ancestor set in the TYPE domain — the compile-time analogue of
§17.2's runtime ancestor stack, and a PATH set for the same reason that one is a
stack: `struct Twice { Pt a; Pt b; }` reaches `Pt` twice without a cycle and must
print it both times.

⚠️ **TWO sets, and they must not be shared.** `dump_any` pushes the type BEFORE
the head-line word is built, so one set would make every container's word fall
back to the canonical spelling. `m_dump_expanding` is the walk's;
`m_dump_word_expanding` is the word's. The word guard is reachable only for a
self-referential TEMPLATE container, because `dump_template_word` needs a `<`.

⚠️ **The POINTER path is exempt and must stay exempt.** `dump_pointer_fn` is
memoized, so a cycle through a pointer emits a CALL and never re-enters
`dump_any` for the same type. Guarding it would refuse the rings §17 exists to
render.

The outcome is not a refusal: the container arm declines and `dump_any`'s existing
fallback prints the type's real MEMBERS — the same rule that keeps a positional
container whose element has no dumper working. Gate:
`tests/testphpdumpselfref.mad`, whose `Twice` case is the negative control.

### 19.9 ⚠️ The guard's own regression, and what it says about the suite

§19.8's type-path set bounded an EXPANSION and carried into a place that is not
one. A generated dumper function's body IS a `dump_any(pointee)` call, but the
recursion THERE is a CALL, already bounded by the memo registered before the body
is built. Carrying the caller's path in refused the canonical case:

```c
struct Link { int v; struct Link *next; };
Link k; php::print_r(k);
// -> php::print_r: member 'next': no dumper for 'Link' yet: it contains ITSELF
```

The top-level expansion of `Link` is still on the path when member `next`
generates `Link`'s dumper and its body expands `Link` again. `dump_pointer_fn`
now swaps the set out for the duration of the body — beside the four context
names (`m_dump_sink_var`, `m_dump_col_base`, `m_dump_fn_depth`,
`m_dump_fn_nested`) it already swaps there for exactly the same reason. The set is
the fifth member of that group; relaxing the guard in `dump_any` instead would
have been the shim, because `dump_any` cannot tell which caller it has.

⚠️ **fulltest was 1084 / 0 with the bug in.** Every pointer test dumps a POINTER
at top level (`php::print_r(&a)`), so the type was never on the path when its
function was built: the BY-VALUE shape had no coverage anywhere in 1084 tests. It
was found by re-measuring the probe set AFTER the release commit was made.
`tests/testphpdumpselfref.mad` now carries the three by-value shapes — a null
self-pointer, a self-pointer at itself, and a two-node ring — so the hole is
closed with a gate.

⚠️ **And the measurement was blind twice before it was right.**
`tmp/patch/coverage.sh` runs `./bin/madc`, which on the NAS is whatever was last
pulled: it printed REFUSED for a shape that had just been fixed, and earlier for
all 21 shapes at once. It now prints a provenance banner — binary, mtime,
`--version`, hostname, HEAD — and says to run on the container.
