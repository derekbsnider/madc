# Phase-2 CIR Coverage — Stream I/O Support (Design)

**Date:** 2026-05-29
**Branch:** `feature/cir-node`
**Status:** approved-direction, pre-spec-review

## Goal

Grow `CirBuilder` (`src/cir_builder.cpp`) coverage by handling program output —
the dominant cause of the `c2mir_rejected` bucket (149 of the remaining
`--backend=cir` failures). Two separate, non-conflated output paths:

- **Part A — stdout print builtins** (`puti/putu/putd/putf/printstr`, ~24 tests):
  free functions that write to **stdout** via the `madc_*` runtime.
- **Part B — `cout`/`cerr`/`clog` stream output AND `cin` input** (~74 tests):
  C++ `std::ostream::operator<<` and `std::istream::operator>>`, lowered to
  **direct Itanium-mangled libstdc++ calls** on the real stream objects.
  `cout` → cout, `cerr` → cerr, `cin` → cin.

These are distinct destinations and distinct runtimes — never merged.

Baseline at design time: `--backend=cir` 171/419 passing; `c2mir_rejected` = 149
(dominated by 74× `undeclared identifier cout` + 74× `shift operands should be of
integer type`, which are the SAME tests — CIR currently maps `<<` to `N_LSH`).

## Non-goals (deferred)

- Stream-typed variables (`ofstream`/`ifstream`/`stringstream`) — deferred to a
  later pass (per scope decision). `cin` (the standard istream) IS in scope.
- Stream manipulators beyond `endl` (`hex`/`setw`/`flush`/…) — only `endl` in scope.
- The type-coercion / struct-member / initializer-edge `c2mir_rejected` sub-clusters
  — Phase-3.

## Why direct mangled libstdc++ (not `streamout_*` C wrappers)

`emit_c` lowers streams to hand-written `streamout_int64(void*, long)` shims. CIR
instead emits the **real** mangled `std::ostream::operator<<` symbols. Rationale:
1. The emitted symbol demangles back to real C++ (`_ZNSolsEi` → `std::ostream::
   operator<<(int)`) — serving the CIR-as-IDE-IR faithful-round-trip goal. A
   `streamout_int64` call demangles to nothing meaningful.
2. True C++ ABI; no shim layer to maintain.
3. madc already links libstdc++ and has an Itanium mangler.

## GCC-grounded symbol table (from `g++ -S -O0 -std=c++11` on a cout chain)

Every overload is, at the C ABI level, `ostream* f(ostream* /*=&os*/, value)`
(member and non-member alike — pointer in, pointer out), so chaining is pure
nesting. Symbols (verified, not recited):

| value DataDef     | mangled `operator<<` symbol                                              | value C type   |
|-------------------|---------------------------------------------------------------------------|----------------|
| int               | `_ZNSolsEi`                                                               | `int`          |
| long              | `_ZNSolsEl`                                                               | `long`         |
| unsigned          | `_ZNSolsEj`                                                               | `unsigned`     |
| double            | `_ZNSolsEd`                                                               | `double`       |
| bool              | `_ZNSolsEb`                                                               | `int` (bool)   |
| void* / pointer   | `_ZNSolsEPKv`                                                             | `const void*`  |
| const char*       | `_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc`                 | `const char*`  |
| char              | `_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c`                   | `char`         |
| std::string       | `_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE` | `void*` (string obj ptr) |
| `endl`            | manipulator `_ZNSolsEPFRSoS_E` applied to `&_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_` | fn ptr |

Stream objects: `cout` → `_ZSt4cout`, `cerr` → `_ZSt4cerr`, `clog` → `_ZSt4clog`
(address-taken: the first arg to each `operator<<` is `&_ZSt4cout`).

### istream `cin >>` table (from `g++ -S` on a cin chain)

Symmetric to ostream. Every overload is `istream* f(istream* /*=&cin*/, T* /*=&var*/)`
— the value operand is the **address of the target lvalue** (the var being read
into). Symbols (verified):

| value DataDef   | mangled `operator>>` symbol                                              | operand   |
|-----------------|---------------------------------------------------------------------------|-----------|
| int             | `_ZNSirsERi`                                                              | `&var`    |
| long            | `_ZNSirsERl`                                                              | `&var`    |
| unsigned        | `_ZNSirsERj`                                                              | `&var`    |
| double          | `_ZNSirsERd`                                                              | `&var`    |
| char            | `_ZStrsIcSt11char_traitsIcEERSt13basic_istreamIT_T0_ES6_RS3_`             | `&var`    |
| std::string     | `_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE` | string obj ptr |

cin object: `cin` → `_ZSt3cin` (first arg is `&_ZSt3cin`).

This table is a small **static** mapping of stable libstdc++ ABI symbols — the
emitted names are real and demangle correctly. (`std::string` insertion additionally
needs a constructed string-object pointer; CIR's existing std::string handling
supplies it. If a value is a string-literal `const char*`, use the `const char*`
overload, not the `std::string` one.)

## Lowering algorithm (Part B)

In `CirBuilder::translate_expr`, the binary-operator case currently maps
`tkBSL` → `N_LSH`. Before that mapping, check: is this `<<` part of a chain whose
**leftmost leaf is a stream identifier** (`cout`/`cerr`/`clog`)? If so, lower the
whole chain instead of emitting `N_LSH`:

1. **Detect** the stream root: walk left children of nested `tkBSL` `TokenOperator`s
   to the leftmost leaf; if it is a `TokenVar`/`TokenIdent` whose name is
   `cout`/`cerr`/`clog`, this is a stream chain. (First plan step confirms the exact
   token class for `cout`/`endl`.)
2. **Collect** the chain values left-to-right (mirroring emit_c's `collect_cout_values`).
3. **Emit nested calls:** start with `node1(N_ADDR, id("_ZSt4cout"))` (the stream
   object address); for each value, wrap: `result = node2(N_CALL, id(<symbol>),
   list(result, <value-or-its-translation>))`, choosing `<symbol>` from the table by
   the value's `DataDef` type-class. `endl` → call `_ZNSolsEPFRSoS_E(result,
   &_ZSt4endlIc...)`. Each call's result (an `ostream*`) threads into the next.
4. Return the outermost call node.

**`cin >>` is symmetric:** when a `tkBSR` (`>>`) chain's leftmost leaf is `cin`,
lower the same way but with the istream table and `cin` object. The key difference:
each `operator>>` value operand is the **address of the target lvalue** — emit
`node1(N_ADDR, <translated lvalue var>)` (or pass the var's address) as the second
arg, not the value. So `cin >> i >> d` → `_ZNSirsERd(_ZNSirsERi(&_ZSt3cin, &i), &d)`.

Helpers (new, on `CirBuilder`): `is_stream_ident(TokenBase*)` →
`enum {none, cout, cerr, clog, cin}`; `stream_object_symbol(kind)`;
`ostream_insert_symbol(DataDef*)` and `istream_extract_symbol(DataDef*)` → the table
lookups. Keep these small and named. `<<` chains route through the ostream path,
`>>` chains through the istream path; both share the chain-detect + nested-call
structure.

## Lowering algorithm (Part A)

In `translate_expr`'s `TokenCallFunc` case, when the callee name is a builtin
output fn, emit `N_CALL(id(<runtime>), args)` with the mapped runtime symbol
(`puti`→`madc_puti`, `putu`→`madc_putu`, `putd`→`madc_putd`, `putf`→`madc_putf`,
`printstr`→`madc_printstr`) instead of the bare name. Mirrors `madc_emit_c.cpp:710-720`.
A named helper `builtin_output_runtime(const std::string&)` returns the mapped name
or empty.

## Extern emission

Both parts call symbols that are not user `TokenFunc`s, so the existing
referenced-only extern pass (`translate_module`, ~1289) won't cover them. Track the
output symbols actually referenced during translation and emit their externs in the
module's extern-proto pass:
- Part A: `extern void madc_puti(long);` etc. (signatures per emit_c `:5633`).
- Part B: `extern void *<operator<<-symbol>(void *, <value-c-type>);` for each used
  overload; the stream objects as `extern char _ZSt4cout;` (so `&_ZSt4cout` is a
  valid address); the endl function as `extern void *_ZSt4endlIc...(void *);`
  (address-taken as the manipulator argument).
All are resolved at MIR link via `dlsym(RTLD_DEFAULT)` against the already-loaded
libstdc++ / madc runtime.

## Implementation order

1. **Part A** (stdout builtins) — clean, self-contained, ~24 tests. Do first.
2. **Part B PoC** — `cout << intval` end-to-end: `_ZNSolsEi(&_ZSt4cout, i)` +
   externs; confirm it links against libstdc++ and prints. Validates the mangled
   approach before expansion.
3. **Part B full (output)** — the ostream overload table, chain nesting,
   `cerr`/`clog`, `endl`, `const char*`/`char`/`std::string` non-member overloads.
4. **Part B (input)** — `cin >>`: the istream overload table, `&var` operands,
   chain nesting. Symmetric to step 3, reuses the chain-detect machinery.

## Verification (every step)

- TDD via `cir_run_builder` (runs the snippet through CirBuilder→c2mir→MIR and
  returns/observes output). For output, assert the produced stdout/stream text.
- Demangle the emitted Part-B symbols (`c++filt`) to confirm they map back to the
  intended `std::ostream::operator<<(...)` — the round-trip check.
- `scripts/cir_diff.sh` where a C analogue exists (Part A); Part B has no direct
  `c2m -d` analogue (c2m is a C compiler, no `cout`), so rely on runtime output +
  demangle verification.
- `make -C src fulltest` stays 419/0 (CIR flag-gated; default backend untouched).
- Re-run `tmp/cir_triage.sh` after each part; record histogram movement and any
  newly-surfaced modes (no silent caps).

## Risks / open items

- **Stream-identifier token representation** — `cout`/`endl` survive `prog->parse`
  as an undeclared `TokenVar`/`TokenIdent` (proven: they reach c2mir as identifiers);
  the exact class is confirmed in the first plan step before writing the predicate.
- **`std::string` insertion** needs a real string-object pointer; if CIR's current
  std::string support doesn't yet provide one in output position, defer the
  `std::string`-via-`cout` overload (string LITERALS use the `const char*` overload
  and are unaffected). Document if deferred.
- **libstdc++ symbol stability** — the mangled names are stable Itanium ABI for the
  linked libstdc++; if a future libstdc++ changes them, the table updates. Acceptable
  (madc already targets this toolchain).
- **Part B has no `c2m -d` oracle** (C has no `cout`), unlike Phase-1 — verification
  leans on runtime output + demangle rather than tree-shape matching.

## Success criteria

- `cout << ... << endl` / `cerr << ...` output chains and `cin >> ...` input chains
  compile and behave correctly under `--backend=cir`; emitted symbols demangle to the
  right C++ `operator<<` / `operator>>`.
- `puti/putu/putd/putf/printstr` link and write to stdout under `--backend=cir`.
- `make -C src fulltest` stays 419/0; measured `--backend=cir` pass-count increase
  over the 171 baseline, with remaining `c2mir_rejected` causes re-ranked for Phase-3.
