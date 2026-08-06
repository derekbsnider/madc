# Headers: Real, Embedded, and Auto-Included

madc's `#include` reaches three kinds of headers:

```c
#include <iostream>    // the REAL installed system header
#include <ns_php>      // a madc embedded header (baked into the binary)
#include "myfile.mad"  // project file, filesystem relative to the includer
```

## Real system headers are the default

`<iostream>`, `<string>`, `<vector>`, `<math.h>`, `<stdio.h>` and the
rest of the C/C++ library are the **real installed headers** — madc
parses them and resolves their symbols against the real libc and the
active C++ standard library (libstdc++, or libc++ under
`-stdlib=libc++`). There are no madc stand-in copies of these; the old
embedded shim twins were retired.

In the packed release binary, common system headers load from the
frozen forest (pre-parsed state) instead of being re-parsed — same
semantics, faster.

## The embedded set

`include/madc/` bakes a small set of headers into the binary — no
external files needed at runtime:

- **Freestanding C headers:** `stddef.h`, `stdint.h`, `stdarg.h`,
  `stdbool.h`, `float.h`, `limits.h`
- **The namespace headers:** `<ns_php>`, `<ns_perl>`, `<ns_python>`,
  `<ns_ruby>`, `<ns_js>`, `<ns_rust>`, `<ns_madc>` — declaration-only
  surfaces for the [multi-language utility namespaces](overview.md#namespaces),
  each self-contained (they include `<string>` themselves)

A real header found earlier in the include search path outranks the
embedded copy of the same name, and `--no-embedded-headers` disables the
embedded set entirely.

## Auto-include (madc dialect)

In the default dialect, using a well-known identifier pulls its header
automatically, in the official include order — `string`, `cout`/`cin`/
`endl`, the containers, the C limit macros, and the namespace heads
(`php::…` pulls `<ns_php>`):

```c
string s = "zero-ceremony";   // <string> auto-included
cout << s << endl;            // <iostream> auto-included
php::trim(s);                 // <ns_php> auto-included
```

Standards modes (`--std=c*` / `--std=c++*`) do no auto-inclusion —
explicit includes are required, as the standards demand. Embedding
hosts' security policies are honored: a namespace the host disabled is
never auto-served.

## dlsym Fallback

Functions need no explicit registration: an unresolved call tries
`dlsym(RTLD_DEFAULT, name)`, so anything in the process's loaded
libraries (libc always; anything brought in by `-l` or `#load`) is
callable. The call's convention is built from the actual argument
types; string arguments auto-coerce to `const char*`. Embedded headers
declare real return types where it matters (`strcmp` returns `int`,
pointer-returning functions are typed) — see
`.claude/rules/embedded-headers.md` for the contributor-facing rules
and the lazy-registration procedure.

## Build System

Embedded headers live in `include/madc/`; at build time
`scripts/gen_embedded_headers.sh` converts them into
`src/embedded_headers.cpp`. The Makefile regenerates it automatically
when any file in `include/madc/` changes.
