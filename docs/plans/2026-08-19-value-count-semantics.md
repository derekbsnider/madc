# `.count()` / `.size()` on the madc value carrier — the owner's ruling

**Status:** design settled, not yet implemented (this session's task #1)
**Ruling (owner, 2026-08-18):** for containers/arrays (`madc::array` included)
`.count()` returns the NUMBER OF ELEMENTS; for a string type (a `madc::value` of
string kind, and `std::string`) it returns the LENGTH OF THE STRING; for a
non-string, non-container it should be an ERROR.

## What is wrong today, measured

`tmp/count/c3.mad`, re-measured at live HEAD (both `.count()` and `.size()` give
the same answer, because both bind to the same entry):

| carrier holds        | `.count()` / `.size()` | should be |
|----------------------|------------------------|-----------|
| string `"hello"`     | **0**                  | 5         |
| integer `42`         | **0**                  | error     |
| real `3.5`           | **0**                  | error     |
| boolean `true`       | **0**                  | error     |
| null (`value n;`)    | 0 ✓                    | 0         |
| array of 2           | 2 ✓                    | 2         |

Every wrong answer is a silent `0` — the shape this codebase refuses.

Build the array with `php::array_push`, not `a[0] = 1`: subscript WRITES on a madc
array are not implemented (tests/testarraysubscript.mad's own header scopes task
#91 R0 to subscript *reads*), so a probe that assigns through a subscript leaves
the carrier at kind `null` and measures nothing. That is not a defect found here.

## Root cause

`Program::add_array_methods()` (src/parser.cpp) binds BOTH `count` and `size` on
`ddARRAY` to `emit_symbol = "madarray_size"`, and `madarray_size`
(src/madc_mir_backend.cpp) is

```c
return v->is_array() ? (int64_t)v->as_array().size() : 0;
```

whose own comment says what it is for: **the range-for bound.** "Intentionally NOT
ns_common::value_count: foreach iterates indexed elements only, so an object-kind
ctx must read as length 0 here." That is correct for a loop bound and wrong for a
question the user asked. One function is answering two different questions, and
the loop's answer wins.

## Design

**Two questions, two functions.** `madarray_size` keeps its name, its behaviour
and its comment as the foreach bound. `count`/`size` bind to a NEW entry.

All three pieces below are as built.

1. `ns_common::value_length(const madc::value &, bool *ok)` — the semantic owner
   of the ruling, beside the existing `value_count` (which stays as it is: it is
   php::count / perl::scalar's PHP semantics, array+object only).
   - `array`  -> `as_array().size()`
   - `object` -> `as_object().size()`
   - `string` / `bytes` -> `size()` (the payload byte count IS the length)
   - `null` -> **0**, not an error. `array a;` is kind `null` until a mutator
     vivifies it (madc_mir_backend.cpp says so), so an unfilled carrier is an
     EMPTY container, not a non-container. An error here would break `array a;
     a.count()`.
   - `integer` / `real` / `boolean` / `instance` -> `*ok = false`
2. `madarray_count(void *)` in src/madc_mir_backend.cpp — the thunk. On `!ok` it
   reports through `__madc_throw_cstr` (src/rt/rt_except.c), which is a real madc
   exception: a script `try`/`catch (const char *)` can catch it, and an unhandled
   one prints `Unhandled exception: ...` and aborts. That is what "an error"
   means for a kind that is only known at run time — a compile error is
   impossible, and returning 0 is the silent wrong answer being fixed.
3. `add_array_methods()` binds `count` and `size` to `madarray_count`.

## `std::string` needs no change

`std::string::size()` and `::length()` already return the length — they are real
libstdc++ methods resolved mangled-direct. `std::string` has no `count()` in C++
and madc must not invent one: a method that exists only in madc's dialect on a
real libstdc++ class is exactly the private-dialect divergence the getenv/setenv
note in `populate_builtin_registry()` rules out. The ruling is already satisfied
for `std::string` by the C++ spelling.

## The two surfaces that share the name `size`

The C++ host API `madc::value::size()` (src/madc_value.cpp) returns `_v.size`, the
PAYLOAD BYTE COUNT. That is right for its callers — `rt_dump_value.cpp`'s
`dump_payload` needs exactly those bytes — so it does not change. After this
change the script-visible `.size()` and the C++ `size()` deliberately differ for
array kinds: one answers "how many elements", the other "how many bytes of
payload". Documented here because the two names are the same and the divergence
is now intentional rather than accidental.

## rt_except.h — created here

`__madc_throw_cstr` had no header: its only caller was generated code, whose
externs the CIR builder emits. `madarray_count` is the first HOST caller, and a
local `extern` at the call site is the silent-mismatch trap rt_dump.h documents
— so `src/rt/rt_except.h` now carries the throw-family prototypes, included by
`rt_except.c` (definitions checked) and the backend. Deliberately only the
throw family: the try/cleanup-stack primitives are generated code's own
machinery, and the only correct host interaction is to RAISE.

## Gate

`tests/testvaluecount.mad` — string/null/array/object through both `.count()`
and `.size()` (object kind is script-reachable via `madc::context_set_int`),
the three error kinds each asserted INSIDE a `try`/`catch (const char *)` so
the throw is measured rather than assumed, and range-fors over BOTH an
object-kind carrier (visited=0 while count=2 — the bound and the method answer
different questions) and the array (visited == count).
