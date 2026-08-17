# C++-First API, C Shim Last — Reasoning

See `.claude/rules/cpp-first-api.md` for the rules themselves.

## Why this needs an explicit rule

The `libmadc` work is heading toward two host classes:

- native C++ hosts that want a clean embedding API
- foreign runtimes like Node.js that need a stable C boundary

Those two needs are compatible, but only if the implementation order is
correct. If the project designs the library around the C ABI first, the
internal model gets flattened too early:

- ownership becomes handle-shaped before the right object boundaries exist
- diagnostics collapse into string-return conventions
- IO and policy get pushed into global/process state
- every future improvement has to preserve an ABI-shaped mistake

madc is already a C++ codebase with object lifetimes, stream abstractions,
RAII-friendly state, and parser/compiler ownership that are naturally
expressed as classes. The clean design path is:

1. shape the real API as C++ classes
2. make the CLI use that same C++ surface
3. exercise it in tests and internal integration
4. only then add the C shim as a narrow forwarding layer

## What "C++ first" means in practice

The primary interfaces for new embedding work should be things like:

- `MadcEngine`
- `Program`
- diagnostics and IO controller objects
- factory / lifecycle methods like `create_program()`, `load_file()`, `compile()`

Those types own the real behavior and the real invariants. The future C API
should not duplicate logic; it should only wrap those objects with opaque
handles and narrow forwarding functions.

## Why the C shim should come later

A C shim is still necessary for ABI stability and FFI consumers, but it is
not the right place to discover the design. The shim is easiest to write
after the C++ model answers:

- who owns engine state
- how programs are created and configured
- how diagnostics are collected
- how output routing is configured
- what compile / execute lifecycle the host actually needs

When those answers are clear in C++, the C layer becomes mechanical. Before
that point, the C layer is guesswork with long-term compatibility cost.

## What this rule does not mean

This rule does not reject a C shim. It only fixes the order:

- C++ interface is primary
- C shim is secondary
- both can coexist
- the C shim must remain thin

That ordering keeps `libmadc` clean for native hosts while still leaving a
stable bridge for Node and other non-C++ environments.

## Script-facing namespace surfaces resolve mangled-direct (2026-06-10)

The same ordering applies to the embedded namespace headers the *scripts*
consume (`<ns_madc>`, `<ns_php>`, …). The first `<ns_madc>` landing declared
extern-C `__madc_eval_*_runtime` prototypes in the script header and wrote
`madc::` wrapper bodies over them. The user corrected this twice: the
extern-C exports exist exclusively so a **C programmer** linking
libmadc.a/.so has a callable surface — they were never meant to be the path
a *script's* `madc::eval_int(...)` call resolves through.

Routing scripts through the C ABI loses exactly what the C++-first rule
protects: reference parameters flatten to pointers, overload sets collapse
into name suffixes, and every call pays a wrapper body that exists only to
undo the flattening. The correct shape (the php:: precedent, 2026-06-04):

- the script header declares `madc::eval_int_ctx(const char *, array &)`
  declaration-only — no body, no extern-C twin in the header
- declaration-only namespace functions mangle to their Itanium symbols by
  default, and `cir_import_resolver` finds them via dlsym/-rdynamic
- the real implementation is an ordinary `namespace madc { … }` C++
  function in the host binary — the single real implementation
- the extern-C export, where wanted, is a thin shim over that C++ function,
  declared in the C API surface for C hosts — not in the script header

The exception is deliberate: symbols the CIR builder itself emits as
lowering machinery (`__madc_scope_set_*`, `__madc_vla_free`) are not
user-resolved functions; they stay extern-C like any other runtime support
symbol, so the builder can name them without a mangler round-trip.

## The second exception: a compiler-implemented namespace public

`php::print_r` (and `php::var_dump` behind it) is neither of the two shapes
above. It is declared in `<ns_php>` as

```cpp
template<class T> void print_r(const T &v);
```

with **no definition anywhere** — not in the header, not in `ns_php.cpp`, not
behind an extern-C twin. The *compiler* is its implementation: at the call site
madc knows the concrete `T`, so `src/cir_dump.cpp` generates a dumper for that
type and the runtime carries only output primitives (`src/rt/rt_dump.c`).

Three consequences, all wanted:

- **"Any type" has no signature a host could satisfy.** No `namespace php { }`
  C++ function could be written that renders an arbitrary aggregate, because
  the member walk needs the *type*, which is gone by the time a compiled
  function runs. A declaration-only template is the honest spelling.
- **Non-export is structural, not a gap.** A declared-but-undefined template
  IS an unresolved symbol for any C++ host that calls it, which is exactly the
  contract the owner asked for ("I don't think var_dump() can be exported to be
  used successfully by a C/C++ program outside of madc", 2026-08-17). There is
  nothing to patch here, and a future reader must not "fix" it by adding a body.
- **Recognition is a TAG, never a name.** The parser stamps
  `FuncDef::inline_builtin_kind` when it registers the declaration
  (`madc_namespace_template_builtin_kind` — the one place the spelling appears),
  and the CIR builder dispatches on that tag exactly as it does for
  `std::addressof` / `std::forward` / `__destroy`. The tag rides the forest
  (`cir_arena` `builtin_kind_id`), so a packed `<ns_php>` keeps it — a name
  comparison in the builder would also have worked, and would have been a
  string compare in the wrong layer that a user's own `php::print_r` could
  hijack.

Full design: `docs/plans/2026-08-17-php-print-r-var-dump-plan.md`.

Design spec where the script-header rule itself was settled:
`docs/superpowers/specs/2026-06-10-eval-leftovers-design.md`.
