# Embedded Headers — Reasoning

See `.claude/rules/embedded-headers.md` for the rules themselves.

## Why embed headers instead of reading from disk

Embedding into the binary means `#include <iostream>` works without a
separate install step. A SMAUG port can be distributed as one `madc`
executable plus the `.mad` sources; no system setup required. It also
means the embedded headers are versioned with the compiler — no
mismatch between what the binary expects and what's on disk.

## Why `scripts/gen_embedded_headers.sh` at build time

The script reads every file under `include/madc/` and produces a C++
source file with the contents as string literals, keyed by relative
path. Subdirectory support uses `find` + relative paths so
`sys/wait.h`, `netinet/in.h`, etc. key correctly. Adding a new header
is "drop a file in `include/madc/` and `make`" — no C++ changes.

## Why dlsym fallback for most libc

A header that only defines constants (`EOF`, `SEEK_SET`, etc.) doesn't
need C++-side registration. Function calls without a declared
signature route through `dlsym(RTLD_DEFAULT, name)` at parse time, so
every libc/libm function just works.

This is why `#include <math.h>` doesn't list every function — it only
defines the constants and `#load "libm.so"` so the symbols are
visible to dlsym.

## Why lazy registration exists at all

Some headers need more than constants and dlsym:
- `<iostream>` registers `cout` / `cin` / `cerr` as global variables
  with specific DataDef types.
- `<time.h>` registers `struct tm` with its glibc-matching layout.
- `<stdio.h>` registers `stdin` / `stdout` / `stderr` as pointer
  globals initialised from libc's dlsym values.

These need entries in the parser's symbol tables BEFORE the parser
encounters them. But `tkProgram` doesn't exist yet during lexing, so
eager registration at include time is impossible. `lazy_map` is the
deferred-registration queue the parser drains on demand.

## Why the include-flag + lazy_map split

Two-step design:

1. The include flag (`_include_iostream`, `_include_stdio`) records
   "the user wrote this include in this compilation unit."
2. `_parser_init()` sees the flag and calls `add_iostream_symbols()`,
   which populates `lazy_map` with the symbol-name → metadata entries.

Actual DataDef / Variable creation happens on first use, via
`lazy_resolve` / `lazy_resolve_type`.

This way, an include that's never actually used costs nothing. A real
use triggers only the specific symbol's registration.

## Why `RTLD_GLOBAL` for `#load`

Without `RTLD_GLOBAL`, `dlopen("libm.so")` makes symbols visible only
through the returned handle. `dlsym(RTLD_DEFAULT)` would not see them.
With `RTLD_GLOBAL`, loaded symbols go into the process's global
symbol table, so `sqrt()` (used after `#include <math.h>`) resolves
without any namespace prefix.
