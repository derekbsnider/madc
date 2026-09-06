# Value literals, keyed elements, and the append accessor

The madc carrier (`var` — the dialect spelling; `value`/`array` are the
C++-interop aliases of the same type) takes brace literals, PHP-style
appends, and keyed access with no ceremony.

## Brace literals

```c
var ix = { 7, "eight", 3.5, true };          // positional -> ARRAY kind
var m  = { "a": 1, "b": "two", "c": 2.5 };   // string-keyed -> OBJECT kind
var iv = { 7, "eight", 3: "three" };         // integer keys INDEX (array kind)
var e  = {};                                  // an EMPTY ARRAY, never null
```

- A `key: value` element assigns into the key's vivified slot through the
  same registered `operator=` rows `m[key] = value` binds — the literal
  and the per-key spelling cannot drift.
- String keys vivify the OBJECT kind (a map); integer keys and positional
  elements are the ARRAY kind (a vector). Mixing the two kinds in one
  literal refuses at compile time; a runtime kind clash is a catchable
  script error.
- Keys and values are full expressions: `{ kn: kv, "sum": 2 + 3 }`.
  A `var` key dispatches on its live kind at runtime (see below).

## Literals are expressions

A brace literal works anywhere an expression does — assignment right-hand
sides and call arguments included:

```c
m = { "x": 1 };                    // whole-value replacement
m["sub"] = { "deep": true };       // a literal into a keyed slot
rows.push({ "act": "colon" });     // a braced call argument finds push(value&)
```

An assignment's braced list is target-typed by the assignee
([expr.ass]/9 — this also serves C++ class/aggregate assignees); a braced
call argument searches the callee's overload set for the parameter that
can take a list.

## The append accessor: `rows[] = expr`

PHP's empty `[]` appends a fresh element and assigns into it:

```c
var rows;
rows[] = 42;
rows[] = { "act": "colon", "content": " : vi command line" };
rows[] = r;                        // append an existing value (copies)
m["list"][] = 9;                   // works on any carrier slot chain
```

This replaces the row-building dance (`var r; r["k"] = v; rows.push(r);`)
with one line. `[]` on a non-carrier receiver is a loud compile error.
Note `rows[]` is an *access* — it appends whenever evaluated, so spell it
only as an assignment target.

## Subscripts: keys, indexes, and live-kind dispatch

```c
m["k"]        // a string KEY (object kind) — char* or std::string
a[3]          // an integer INDEX (array kind); negative refuses
m[kn]         // a var index dispatches on kn's LIVE kind:
              //   string kind  -> keys (no .c_str() needed)
              //   integer/bool -> indexes; a real truncates
              //   null/container -> loud (catchable) refusal
```

Every carrier subscript is a SLOT — a live value lvalue: writes land in
place, reads vivify (access creates, the Perl model), and elements carry
the carrier's whole method surface. madc deliberately does not copy PHP's
key coercion: `"8"` is the string key `"8"`, never index 8.

## Known gap

A ternary whose arms involve carriers (`cond ? t : t["k"]`, or one
carrier arm and one scalar arm) does not lower yet — spell it as an
if/else feeding a `var`. (Banked: the conditional will lower through a
value temp assigned per arm.)
