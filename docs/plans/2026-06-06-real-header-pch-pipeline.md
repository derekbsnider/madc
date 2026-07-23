# Real system-header PCH pipeline — build-time pre-parse + compress + `#embed`

Status: **planned (design agreed)**. Supersedes hand-curating std:: headers
(incl. the parked "fstream inc 5/6"). Owner: open.

## Goal

`#include <iostream>` (and the rest of the std:: / libc surface) must come from
the **actual system headers**, pre-parsed and compressed **at madc build time**,
embedded into the binary, and loaded on demand — **not** hand-tooled stubs we
curate ourselves. The current `include/madc/{iostream,string,vector,fstream,…}`
stubs are a stopgap and get retired.

Principle: **headers come from the originals.** The only legitimate hand-written
headers are madc's *own* `ns_*` polyglot headers (below), which have no system
equivalent.

### Scope & phasing (decided 2026-06-06)

- **This pipeline targets the std:: C++ headers first** (`iostream`, `string`,
  `vector`, `map`, `set`, `fstream`, `sstream`, `algorithm`, `typeinfo`) — the set
  we most want from originals and which parses relatively cleanly.
- **libc/POSIX headers stay curated for now** (small, stable, deliberately tuned;
  real glibc is parse-gnarly). A **later phase** migrates them to real headers —
  the special-handling they'd need is catalogued under "Header categories" §2.
- **`ns_*` madc-own headers stay embedded permanently** (no system original).

## Current state (facts, 2026-06-06)

- The **parser was hardened** for real-header syntax (`e7b06f3`: class-scope
  aliases, nested/template aliases, explicit specializations, method-result
  receiver chains, base-qualified `this`, arity-aware member lookup).
- But madc **cannot ingest the real `<iostream>` closure** yet, for two reasons:
  1. **Missing C++ include paths.** `src/lexer.cpp` `sys_paths[]` is hardcoded
     C-only (`// TODO: should come from ./configure`); the C++ paths
     (`/usr/include/c++/13`, `/usr/include/x86_64-linux-gnu/c++/13`, `…/backward`,
     gcc-internal) are absent, so the first transitive `bits/*` include fails.
  2. **Macro / include-guard state isn't preserved** across the PCH boundary
     (`real_system_pch_guard_macro_state`) — the linchpin.
- The PCH infra **already exists**: `src/pch.cpp` serializes post-lexer **token
  streams**, **zstd (preferred) / zlib (fallback)** compressed → `.madh`, with a
  compiler-hash staleness guard, `--emit-pch`, and lexer load integration. It
  serializes **tokens only — no preprocessor state**.
- Curated embed today: `scripts/gen_embedded_headers.sh` → `src/embedded_headers.cpp`
  (112 KB of `include/madc/*` as C++ string literals).
- **Toolchain:** `clang-19` installed (Ubuntu 24.04); `#embed` verified working
  (C `-std=c23` clean; C++ `-std=c++26` as a clang extension). `gcc-15` NOT needed
  (build-compiler `#embed` ≠ parsed-header version; libstdc++ stays 13).

## Header categories in `include/madc/`

1. **std:: stubs** (`iostream`, `string`, `vector`, `map`, `set`, `fstream`,
   `sstream`, `algorithm`, `typeinfo`) → **replace** with real system-header PCH.
2. **libc/POSIX stubs** (`stdio.h`, `string.h`, `sys/*`, …) → **STAY curated for
   now** (a later phase migrates them; see "Scope & phasing"). They're small,
   stable, and deliberately tuned, and real glibc headers are gnarly to parse
   (`/usr/include/stdio.h` ≈177, `string.h` ≈91 `__THROW`/`__nonnull`/
   `__attribute__`/`_FORTIFY_SOURCE`/inline tokens). Special handling that a raw
   real-header swap would lose (the migration checklist for later):
   - **(A) signed-`int` returns** — `string.h` `strcmp/strncmp/strcasecmp/`
     `strncasecmp/memcmp` (dlsym fallback returns `long` → the SMAUG
     `bsearch_skill_exact`/combat bug). `ctype.h` classifiers
     (`isalpha/toupper/…`) are currently *undeclared* (dlsym→`long`) — same latent
     bug; **declaring them `int` is a worthwhile near-term fix, independent of the
     pipeline.**
   - **(B) `#load`** — `math.h`→libm, `crypt.h`→libcrypt, `assert.h`→libc,
     `dlfcn.h` first-class. Real headers have no `#load`, so symbols wouldn't
     resolve.
   - **(C) struct layouts + lazy registration** — `time.h` (`struct tm`,`time_t`),
     `sys/stat.h` (`struct stat`), `dirent.h`, `pwd.h`/`grp.h`, `netdb.h`,
     `sys/time.h`; globals `stdin/stdout/stderr` (`LAZY_STDIO`); types
     `size_t/pid_t/time_t`.
   - **(D) deliberate simplifications** — `stdio.h` `FILE`→`void`, `EOF`/`NULL`/
     `size_t` macros, printf via dlsym; `stddef.h`/`stdint.h` typedefs.
3. **madc-own `ns_*`** (`ns_php`/`ns_perl`/`ns_python`/`ns_ruby`/`ns_rust`/`ns_js`,
   the no-extension headers `.mad` code does `#include <ns_php>`) → **STAY
   embedded.** No system original exists; they declare `extern "C"` bindings to the
   `__php_*`/etc. runtime symbols and **reference `std::string`**, so they sit
   *on top of* the real `<string>` PCH. (`ns_*.h` are the internal C++ build
   headers, separate.)

## Architecture (agreed)

**Per-file `.pchz`** (granularity decided — dedup payoff measured: a 6-header TU is
**210 unique files** vs **716 summed per-header**, ~71% saved; `<iostream>` alone =
185 files). A `.pchz` is a **structure-preserving ordered stream of entries**, not
a flat token blob:

- **token-run** — a span of already-lexed/expanded body tokens
- **include-edge** — "load `<bits/foo.h>`'s `.pchz`", gated by `included_files`
- **macro-op** — `#define`/`#undef`/`push_macro`/`pop_macro`, replayed in order

On `#include <X>`: walk X's entries in order — splice token-runs, recurse into
include-edges (skip if already in `included_files`), replay macro-ops. This
reconstructs the exact preprocessor effect, **dedups shared `bits/*` via the
existing `included_files` guard**, and accumulates the macro table for later code.
Body tokens are stored pre-expanded (no re-expansion); macro-ops stay as ops so
**macro state survives the boundary**.

**Packaging:** one **embedded archive** = `[index: path → offset/len][zstd blob₁]…`,
a **single `#embed`** (clang-19) — or a generated `static const unsigned char[]`
fallback for non-`#embed` toolchains. On `#include <X>`: index lookup → inflate
**only that one blob** on demand (lazy).

## Macro-state mechanism (the linchpin)

madc's entire preprocessor state is four serializable structures
(`include/madc.h:1102-1118`): `define_map` (`map<string,string>`), `macro_map`
(`map<string,MacroDef{params,variadic,variadic_param,body}>`), `included_files`
(`map<string,bool>` — the guard set), `_macro_save_stack` (push/pop). `ifdef_stack`
/`ifdef_done_stack` are transient (balanced to empty at a clean header boundary —
asserted, not serialized).

- **Capture:** snapshot macro state → lex the file → snapshot again → store the
  **net delta** (adds/redefines + explicit `#undef` removals) as macro-ops in
  source order, interleaved with token-runs and include-edges.
- **Load:** replay macro-ops into live `define_map`/`macro_map`, mark
  `included_files`, splice token-runs.
- **Validity fingerprint:** a PCH is valid only if use-context == capture-context
  (gcc/clang's PCH rule). Each archive carries a fingerprint = hash{predefined
  macros + `--std=` + compiler-hash}. Mismatch (different std, adversarial user
  `#define` before the include) → **fall back to live parse.**

## Capture driver + paths

- `./configure` runs `g++ -std=c++17 -E -v` (and/or clang) to detect the real
  include search list → feeds madc's `sys_paths` (retires the TODO). Needed both
  for capture and for the live fallback.
- New madc mode (`--emit-pch-bundle` or similar) + a `make` target. Seeded from a
  **seed set**, it follows `#include`s to discover the full closure and emits one
  `.pchz` per physical file + the packed archive. Runs at **build time, per host**
  — the binary carries that host's real libstdc++ (build-from-source model,
  confirmed acceptable).
- **Seed set** = the std:: / libc headers + **madc-own `ns_*`**, auto-derived by
  scanning `#include <...>` across `tests/` + `MadSMAUG/`. Both system headers
  (from system paths) and madc-own headers (from `include/madc/`) are captured into
  the same bundle; `ns_*` capture pulls in their `<string>` dependency via the
  normal include-edge mechanism.

## `--std=`

Capture for **one canonical std first — C++17 (`__cplusplus=201703L`)** (complete,
stable, matches the std:: surface). Fingerprint → live fallback for other std.
Multi-std bundles are a later add.

## Include resolution + retirement

`#include <X>` → (1) `included_files` guard → (2) embedded PCH index → (3) live
real header (now findable) → (4) curated embedded header (transition) → (5) error.
Retire curated std:: / libc stubs **incrementally, per header, test-gated** (don't
delete curated `<iostream>` until `cout`/`cin` tests pass via the PCH path).
**Keep `ns_*` and any deliberately-tuned libc headers embedded.**

## Sequencing (front-load the risk)

1. **`./configure` C++ paths** → wire into the lexer. Small; unblocks everything.
2. **Minimal end-to-end proof:** capture → archive → `#embed` → load **one real
   header** through the new entry model (build up from a leaf like `<cstddef>` to
   `<iostream>`), with a test passing via the PCH path and curated fallback intact.
   Validates macro-ops + guards + include-edges on real `bits/*` **cheaply**, before
   building the rest.
3. **Widen** to the full seed closure (system + `ns_*`); tune compression/size.
4. **Flip** resolution to prefer the PCH index; **retire** curated std:: stubs one
   at a time, test-gated.
5. The 3 `ofstream` reds + curated `<string>`/`<vector>`/etc. fall out of step 4 —
   fixed by real `<fstream>`/`<string>` parsing, no hand-tooling.

If the macro-state model is flawed, step 2 surfaces it on one header — not after
the whole pipeline is built.

## Open / risk

- Macro-state correctness on a real `bits/*` closure (proven at step 2).
- Bundle size (~210 files, pre-lex+zstd → a few MB embedded; acceptable).
- `#embed` needs the build at `-std=c++2b`/`c++26` *or* a small C TU *or* the
  byte-array fallback — decide at step 3 (not blocking).
- libc tuned-header policy (which to replace vs keep) — decide during step 4.
- Multi-`--std=` and non-x86_64/non-gcc-13 hosts — later.

## Relation to other tracks

- Supersedes hand-written fstream inc 5/6 (`Gap{ofstream_needs_fstream_header_inc5_6}`).
- Builds on `e7b06f3` real-header parser hardening + existing `pch.cpp` infra.
- Unblocks retiring `include/madc/*` std:: stubs (the retire-std-hardcoding
  campaign's end state, done from *real* headers rather than curated ones).
