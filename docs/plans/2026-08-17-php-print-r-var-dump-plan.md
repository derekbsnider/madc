# php::print_r / php::var_dump over ANY madc type — design + slices (2026-08-17)

## Status

**DESIGNED, NOT STARTED.** Owner-approved in session #99: mechanism (approach
A), return type (`madc::value`), PHP-fidelity rule, pointer recursion with loop
detection, and the iteration-protocol prerequisite (Option 1). No code written.

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
    template<class T> void print_r(const T &v);
    template<class T> value print_r(const T &v, bool ret);   // ret==true -> the text
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
