# Class-type exceptions: owned object, type-matched handlers (P1.1e, B202)

Roadmap P1.1e (`docs/plans/cpp-support.md`), reached as BUGS.md B202, a silent
wrong answer, in the dialect-faces arc's section G (silent wrong answers first).

## What is wrong today

- `throw E(5)` with no string form calls `__madc_throw_object(&tmp)`: the
  runtime keeps the ADDRESS of the thrower's temporary, which the unwind
  releases, and no type identity (`src/rt/rt_except.c`).
- The parser gives every class or pointer clause tag 4 so it can never match
  (`TokenTRY::parse`, src/parser.cpp). Since `4b25ffb80` the class throw
  carries `MADC_EXCEPT_CLASS` = 4 as well, so every class handler matches
  every such throw: `catch (const F &)` takes a thrown `E`.
- The handler never binds the object: the catch lowering
  (`src/cir_builder.cpp`, the try/catch arm) binds scalar tags only, and a
  class clause's variable is the handler body's own default-constructed
  `T c;`. By reference and by value both read that object.
- A class with a `c_str()` (std::string, the value carrier) is thrown as its
  text (tag 3), so `catch (const char *)` takes a thrown `std::string`.
- The exception-declaration is read by a hand-rolled `*` / `&` / cv loop in
  `TokenTRY::parse`, not by the declarator owner `Program::parse_declarator`;
  a reference clause is recorded as the class type, by value.

g++ 13 and clang++ 18 on B202's reducer: `by ref 5`, `by value 6`,
`right handler E 7`. madc: `by ref -1`, `by value -1`, `wrong handler F -2`,
exit 0.

## Design

Portable C11 throughout (SJLJ stays the model; `--emit=c11` output needs no
C++ ABI runtime).

### 1. The throw descriptor (CIR builder, per thrown class type)

madc knows the thrown object's complete type at the throw, so every base
offset, virtual bases included, is a compile-time constant. One linkonce
static table per thrown type:

```c
struct __madc_throw_desc {
	void (*dtor)(void *);          /* complete-object dtor, 0 if trivial */
	unsigned long count;
	struct { const char *name; long offset; } match[]; /* the type itself at 0,
	                                  then every UNAMBIGUOUS PUBLIC base */
};
```

`name` is the Itanium type name string madc already spells for `_ZTS`
(`itanium_typeinfo_name_string_cpp`), so a library class (`St9exception`)
and a user class compare by `strcmp` across TUs and images. A pointer throw
(`throw new D`) gets a table of `P`-prefixed names whose offsets adjust the
pointer value. [except.handle]/3: a handler of type cv T / cv T& matches a
thrown E when T is E or an unambiguous public base of E.

### 2. The runtime (src/rt/rt_except.c)

- `void *__madc_exception_alloc(unsigned long size)`: storage for the
  exception object; the lowering constructs the operand into it (the copy
  or move constructor, or the prvalue's constructor directly).
- `void __madc_throw_class(void *obj, const void *desc)`: records the owned
  object and its descriptor, then raises.
- `void *__madc_exception_match(const char *name)`: the adjusted address of
  the matched subobject (or the adjusted pointer value), NULL when the
  in-flight exception is not a class or has no such type.
- A caught stack (Itanium `__cxa_begin_catch` / `__cxa_end_catch`):
  `__madc_exception_begin_catch()` moves the in-flight record (any tag) onto
  it and pushes a cleanup entry that ends the catch if the handler is left by
  a throw; `__madc_exception_clear()` (handler end) pops it, runs the
  descriptor's dtor and frees the object. `throw;` re-raises the TOP caught
  record, so a rethrow after a nested try/catch inside the handler rethrows
  the right exception; a rethrown object is not destroyed by its catch's
  cleanup.
- The four `__madc_throw_*` bodies share one raise tail (set record, unwind
  to the try's mark, longjmp); it becomes one static helper.

### 3. The catch lowering (CIR builder)

A class or pointer clause's guard is
`(__exc_p = __madc_exception_match("<name>")) != 0`; the handler then calls
`__madc_exception_begin_catch()` and binds:

- `catch (cv T &e)`: `e` is a reference bound to `*(T *)__exc_p`;
- `catch (cv T e)`: `e` is copy-constructed from `*(T *)__exc_p` and
  destroyed at the handler's end;
- `catch (cv T *p)`: `p = (T *)__exc_p` (the adjusted pointer value).

Scalar clauses keep their tags.

### 4. The parser (src/parser.cpp — a core-parser change, the owner's call)

The exception-declaration is a parameter declaration ([except.pre]/1): read
it through `Program::parse_declarator` (mode Parameter), so the clause
records its real declared type (reference, pointer, cv), and register the
handler variable with that type. This deletes the hand-rolled declarator
loop (one owner, `check-one-declarator-reader.sh`).

### 5. Text-form throws by `--std=`

Under a C++ `--std=`, a class with a `c_str()` is thrown as a class object
(a thrown `std::string` matches `catch (const std::string &)`, never
`catch (const char *)`). The dialect (`--std=madc`) keeps its text-form throw
of the value carrier unless the owner rules otherwise.

## Tests

- `tests/testcatch_class_bind.mad`: B202's reducer (by reference, by value,
  handler selection among unrelated classes), g++ and clang++ oracles.
- `tests/testcatch_class_base.mad`: a derived throw caught by a public base
  reference, a non-primary base (adjusted address), a virtual base; a
  private or ambiguous base falls through to the next handler.
- `tests/testcatch_class_lifetime.mad`: the thrown object is constructed and
  destroyed exactly once (counted ctor/dtor), a rethrow keeps it, a nested
  try/catch inside the handler, then `throw;` rethrows the outer exception.
- `tests/testcatch_std_exception.mad`: `std::runtime_error` / user
  `std::exception` subclasses caught as `const std::exception &`, `what()`
  through the handler (restores the catch lines held out of
  `testlibvirtualcall`).
- EXE and OBJ passes as well as the JIT; `--emit=c11` output compiled by gcc.

## Thread-safety contract

The in-flight record and the caught stack are per execution context, like
the existing try and cleanup stacks: `_Thread_local` in the host build,
saved and restored by `__madc_except_state_save` / `_restore` at every task
switch (the save buffer grows by the caught-stack head). The descriptors are
immutable static data. No new mutable globals.

## Status

DONE 2026-10-19 (parts 1-5; the owner approved part 4). As built, where it
differs from the design above:

- The match table is per THROW SITE, two block-scope static arrays (names,
  offsets) passed with the count: `__madc_throw_class(obj, dtor, count,
  names, offsets, is_pointer)`. A per-type module table would be missing
  from a loaded forest body; the block-scope arrays travel with it.
- `int __madc_exception_match(const char *name, void **out)`: a null pointer
  thrown at a `B *` handler matches and binds null, which a NULL-means-no
  return could not say.
- Every handler begins the catch (`__madc_exception_begin_catch`) into a
  guard local whose cleanup attribute ends it
  (`__madc_exception_end_catch`) on every normal exit of the handler; a
  throw out of the handler ends it through the runtime cleanup entry. The
  no-match tail is `__madc_exception_resume()`.
- A named class handler's variable is declared from the clause's own
  exception-declaration, `<decl> = *__madc_caught_<n>;`, injected as the
  handler body's first statement; the hidden pointer lives in the enclosing
  block so the guard can write it.
- The exception object's storage is a COMPLETE object: a prvalue operand is
  constructed there through `complete_object_construct_stmts` (virtual bases
  first) — the same fix the `__retbuf` slot needed
  (`tests/testreturn_vbase_prvalue`).
- Implicit move of a thrown local ([class.copy.elision]/3) is applied only
  when no try block encloses the throw in its function; inside a try body
  the operand is copied.
- Out of scope, filed: B205 (a throw through a frame with no try skips that
  frame's destructors), B206 (an unspecified base in a `class` is public, so
  a privately inherited base still matches).
