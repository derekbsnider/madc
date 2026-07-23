# madc Header Partition — Implementation Handoff

## Objective

Make madc compile against **unmodified** system libc (glibc) and libstdc++ headers by
supplying *only* the small set of headers that are the compiler's responsibility, and
consuming everything else stock from the system. Do **not** handroll the C or C++
standard library. The deliverable is a madc-owned freestanding header directory plus the
predefined-macro and `__builtin_*` surface that lets the real system headers parse
through madc as if it were the GCC they were written for.

## The boundary rule (load-bearing)

A header is **madc's responsibility** if and only if getting its contents correct
requires a fact only the code generator knows (exact target type of `size_t`, layout of
`va_list`, numeric limits, ISA intrinsics). Everything else — anything expressible in
portable source — belongs to glibc/libstdc++ and is consumed unmodified.

Do **not** derive the set from the C++ standard's "freestanding vs hosted" clause. That
is a partition of *library facilities*, not of *physical header files*, and it does not
match (e.g. `<type_traits>`, `<new>`, `<initializer_list>` are standard-freestanding yet
ship from libstdc++, not the compiler). The authority for *what files madc must supply*
is the toolchain's own include directory, discovered below — not the standard's list.

## Step 1 — Discover the authoritative set (do not hardcode)

Pin the GCC version madc impersonates (the system GCC is the natural choice). Enumerate
its compiler-owned directory programmatically so the set stays correct across GCC
upgrades:

```sh
GCC=${GCC:-gcc}
OWN=$("$GCC" -print-file-name=include)         # compiler-owned header dir
FIXED=$("$GCC" -print-file-name=include-fixed) # fixincludes (usually near-empty)
echo "Compiler-owned dir: $OWN"
ls "$OWN" | sort                                # THE definitive list
```

Treat the contents of `$OWN` as the canonical "madc must provide an equivalent" set for
the pinned version. Record the GCC version and a checksum of this listing in the repo so
drift is detectable on upgrade.

## Step 2 — Classify into three buckets

Every file in `$OWN` falls into one of three buckets. Verify each by inspecting whether
it contains `#include_next`:

1. **Pure compiler headers** (no system twin; self-contained). Examples: `stddef.h`,
   `stdarg.h`, `stdbool.h`, `stdalign.h`, `stdnoreturn.h`, `iso646.h`, and the entire
   intrinsic family (`immintrin.h`, `x86intrin.h`, `xmmintrin.h` … `avx512*intrin.h`).
   → **madc supplies these fully.** Their bodies resolve to madc builtins.

2. **Layering shims** (compiler-known piece, then `#include_next` falls through to the
   system version). Examples: `stdint.h` (essentially just `# include_next <stdint.h>`),
   `limits.h` (does its part, includes same-dir `syslimits.h`, then
   `#include_next <limits.h>` to glibc), `float.h`.
   → **madc supplies a thin shim that chains to the system header via `#include_next`.**

3. **Everything not in `$OWN`** — all of glibc (`stdio.h`, `stdlib.h`, `string.h`,
   `unistd.h`, `sys/*`) and all of libstdc++ (`<vector>`, `<string>`, `<type_traits>`,
   `<tuple>`, the `<cXXX>` wrappers).
   → **Consumed unmodified. madc never ships these.**

To partition automatically: a file in `$OWN` containing `#include_next` is bucket 2;
otherwise bucket 1. Intrinsic headers (`*intrin.h`, `*mmintrin.h`, `immintrin.h`,
`x86*intrin.h`) are bucket 1.

## Step 3 — Implement the madc freestanding directory

- Create a madc-owned include dir (inspect the repo for the existing convention first;
  if none, propose e.g. `include/madc-freestanding/`). Discover how madc currently
  injects system include paths before adding to it.
- Implement bucket-1 cores so the compiler-known facts map to madc builtins:
  `stddef.h` → madc's target types for `size_t`/`ptrdiff_t`/`wchar_t`/`max_align_t`,
  `NULL`, `offsetof` → `__builtin_offsetof`; `stdarg.h` → `va_list`/`va_start`/`va_arg`/
  `va_end`/`va_copy` mapped to madc's `__builtin_va_*` and `__builtin_va_list`.
- Implement bucket-2 shims to `#include_next` the system header. Verify madc's
  preprocessor implements `#include_next` correctly (it must continue searching the
  include path *after* the directory the current header was found in — this is the
  mechanism the whole approach depends on).
- For intrinsics, implement only the ISA extensions madc actually targets now; stub or
  omit the rest. This family is the only part that grows over time (≈once per new ISA
  extension), and only if madc chooses to support that extension.

## Step 4 — GCC impersonation (so the system headers take the right `#if` paths)

The system headers reconfigure themselves based on who they think the compiler is.
madc must predefine the macros real GCC predefines for the target ABI. Discover the
ground-truth values to match:

```sh
gcc -dM -E -x c /dev/null | sort        # all predefined macros for C
g++ -dM -E -x c++ /dev/null | sort      # all predefined macros for C++ (adds __cplusplus etc.)
```

At minimum madc must define, matching the pinned GCC and target:

- Identity: `__GNUC__`, `__GNUC_MINOR__`, `__GNUC_PATCHLEVEL__` (and `__cplusplus` to the
  correct standard value in C++ mode).
- ABI/type facts the headers branch on: `__SIZEOF_*__`, `__*_TYPE__` (e.g.
  `__SIZE_TYPE__`, `__PTRDIFF_TYPE__`, `__WCHAR_TYPE__`), `__CHAR_BIT__`, endianness,
  the `__*_MAX__` family.
- Feature-test plumbing the modern headers gate on (`__GNUC_STDC_INLINE__`, the
  `__has_*` operators — `__has_builtin`, `__has_include`, `__has_attribute` — and the
  `__cpp_*` feature-test macros relevant to the supported standard).

Diff madc's `-dM` output against real GCC's and close the gaps that the target headers
actually consult. Do not blindly define all of them; define what the headers branch on.

## Step 5 — `<type_traits>` is a builtins workstream, not a parser workstream

libstdc++'s `<type_traits>` and the container/utility headers call compiler intrinsics
that are **not expressible in C++ source**; they are oracles madc must implement in the
semantic layer:

```sh
# enumerate the intrinsics the installed libstdc++ actually calls:
g++ -E -x c++ - <<'EOF' >/dev/null 2>&1; \
  grep -rhoE '__(is|has|underlying|builtin)_[a-z_]+' \
  $(g++ -print-file-name=../../../../include/c++)/* 2>/dev/null | sort -u
#include <type_traits>
#include <tuple>
#include <memory>
EOF
```

Expect the load-bearing set to include `__is_base_of`, `__is_trivially_constructible`,
`__is_standard_layout`, `__underlying_type`, `__is_aggregate`,
`__has_virtual_destructor`, plus instantiation-shortcut builtins like
`__type_pack_element` and `__integer_pack`. Each is a semantic-layer implementation, and
collectively they are real two-phase-lookup / template-instantiation work that sits
**upstream** of madc's common AST — not lexer work. Track them as a checklist; getting
`<tuple>`/`<utility>`/`<type_traits>` through is the actual conformance milestone here.

## Step 6 — Include search order

madc's `<...>` search path must place the madc-owned freestanding dir **first**, then
the system dirs, mirroring GCC's order. Reference (from GCC 13 on Ubuntu 24.04):

```
/usr/lib/gcc/x86_64-linux-gnu/13/include   <- compiler-owned (madc's dir takes this slot)
/usr/local/include
/usr/include/x86_64-linux-gnu
/usr/include
```

This ordering is what makes `#include_next` in the bucket-2 shims land on the system
header, and what lets madc's `stddef.h` win over any other on the path.

## Acceptance tests (freeze as regression oracle)

1. **C smoke:** preprocess+parse a TU including `<stdio.h>`, `<stdlib.h>`, `<string.h>`,
   `<stdarg.h>`, `<stddef.h>`, `<limits.h>` — must produce a correct AST with no
   madc-owned-header fallthrough errors.
2. **C++ smoke:** same for `<type_traits>`, `<utility>`, `<tuple>`, `<vector>`,
   `<string>`, `<memory>`. This is the real bar.
3. **Macro parity:** `madc -dM` vs `gcc -dM` diff contains no macro the target headers
   branch on.
4. **Provenance intact:** confirm lowered nodes still carry source-token / pre-erasure
   source-type annotations through these headers (madc's existing provenance invariant).
5. Snapshot pass/fail of (1)+(2) as a hard regression set **before** any later
   lexer/parser rewrite. The current contextual-disambiguation ugliness is the spec for
   which warts are load-bearing; a pass-rate drop after a rewrite identifies essential
   coupling that was sanded off.

## Non-goals / pitfalls

- **Do NOT** handroll or vendor glibc or libstdc++. The library is bucket 3, stock,
  untouched. That is the maintenance nightmare this design specifically avoids.
- **Do NOT** add GCC's own `$OWN` dir to madc's search path — its `stddef.h`,
  `stdarg.h`, intrinsic headers assume GCC builtins madc may not match. madc supplies
  its own equivalents.
- **Do NOT** use the standard's freestanding/hosted clause to decide what to ship; use
  the `-print-file-name=include` listing.
- **Do NOT** treat header failures as parser bugs by default — a large fraction are
  missing predefined macros (Step 4) or missing `__is_*`/`__has_*` builtins (Step 5),
  which are orthogonal to lexing.

## First inspection tasks for the repo (do these before writing code)

1. Find madc's predefined-macro mechanism and list what it currently defines.
2. Find how madc registers system include search paths and confirm `#include_next`
   semantics are implemented (search continues *after* the current file's dir).
3. Find the existing `__builtin_*` surface (esp. `__builtin_va_*`, `__builtin_offsetof`)
   and the `__has_*` operators.
4. Locate madc's integration/torture test harness to wire in the acceptance tests above.
