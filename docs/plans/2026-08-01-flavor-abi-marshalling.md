# Flavor-ABI marshalling at the host-symbol boundary (task #69)

**Status: DESIGN — banked 2026-08-01 (session #45), implementation next.**

## The defect

Host-implemented namespace publics (`php::`, `perl::`, `madc::eval_*`, …)
carry `std::string` surfaces. The host binary is compiled against ONE
stdlib flavor (libstdc++). A script compiled with `-stdlib=libc++` produces:

1. **Loud class — mangled-direct misses.** Declaration-only publics
   (`php::trim(std::string&)`, `madc::eval_int_ctx(...)`) mangle with the
   script flavor's ABI namespace (`NSt3__1`); the host exports only the
   `NSt7__cxx11` spellings → undefined MIR imports
   (testphp/testperl/testprefer, all 5 madc-eval tests).
2. **Silent class — layout corruption.** Header-inline wrapper bodies pass
   `std::string*` into extern-C twins (`__php_implode(&result, ...)`).
   The symbol resolves (C linkage), but the host reads the object with
   libstdc++ layout while the script built a libc++ object → garbage /
   SIGSEGV (testlang, testrust).

One root: **a script-flavor string object crosses the host ABI boundary
raw.** Every fix that keeps the object unconverted on the crossing is a
shim somewhere else.

## Alternatives rejected

- **Wrapper bodies over extern-C twins in the script headers** (marshal in
  header-inline C++): violates `cpp-first-api.md` — script-facing publics
  resolve mangled-direct, never through wrapper shims (flattens references
  and overloads; the deleted-shim history).
- **Second host .so compiled with clang++/libc++** exporting `NSt3__1`
  twins: adds a hard clang++/libc++ build dependency to the HOST build,
  a generated shim TU per namespace, and covers only in-repo namespaces —
  a host library loaded with `#load` would still crash. Build-system cost
  without generality.

## The design (compiler-side, mangled-direct preserved)

**Rule: a callee that binds to a HOST-RESOLVED symbol (dlsym/mangled-direct
export — not a script-compiled body) whose signature carries `std::string`
surfaces gets a marshalling thunk whenever script stdlib flavor ≠ host
flavor.** The host flavor is the flavor madc itself was built against
(libstdc++ today; the predicate must read a flavor constant, not assume).

Per string-typed surface in the signature (`string&`, `const string&`,
`string*`), the thunk:

1. allocates a host-flavor string temporary (host string size from the
   flavor model, alloca);
2. constructs it from the script string's bytes via the EXPORTED libstdc++
   ctor `basic_string(const char*, size_type, const allocator&)`
   (`_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EPKcmRKSaIcE`;
   allocator<char> is stateless — pass any live address). Script-side
   `.data()`/`.size()` reads are script-flavor calls madc already
   materializes from the libc++ headers;
3. passes the temp (its address for `&`/`*` params);
4. calls the host symbol;
5. **copy-back** for every non-const `&`/`*` string param: read the host
   temp's `(ptr, len)` — libstdc++ exports no non-inline accessors, so the
   flavor model carries the two layout constants (`_M_p` @0,
   `_M_string_length` @8; ABI-stable, documented as such) — and assign into
   the script string via script-flavor `assign(const char*, size_type)`;
6. **alias-map reference returns**: the php/perl/eval surface always
   returns one of its own reference params (`return s;`). If the returned
   pointer equals temp_i, the thunk returns the SCRIPT arg_i address. A
   non-aliasing `string&`/`string*` return is REFUSED LOUDLY (pre-c2mir
   error naming the callee) until a real case exists;
7. destructs each temp via the exported D1
   (`_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev`).

**By-VALUE `std::string` params/returns are refused loudly in v1** — the
in-repo surface has none; sret + copy-in can be added when a case appears.

`array&`/`madc::value&`, scalars, `const char*` pass through untouched
(flavor-neutral by construction).

## Where

- Detection + thunk emission live at the CIR call-binding layer for
  host-resolved callees (the mangled-direct bind and the dlsym-fallback
  path) — the same place that today mints the script-flavor import. One
  owner for "assemble call to host symbol"; do NOT duplicate per lane
  (dupaudit family ctor_call_assembly is the cautionary precedent, #86).
- The thunk is an ordinary generated C function in the one tree (works for
  JIT, `--emit=c11`, exe/obj). Native link env must add `-lstdc++` beside
  `-lc++` when any marshalling thunk was emitted (`cir_native_link_env`
  is already flavor-aware).

## Gates

- The five madc-eval tests + testphp/testperl/testprefer/testlang/testrust
  under `--stdlib=libc++` are the acceptance set (currently 10 of the 38).
- A dedicated reducer gate (`testflavormarshal`, `.flags -stdlib=libc++`):
  one `php::trim` round-trip + one inout `madc::eval_*_ctx` call, canon
  output equality with the default-flavor run.
- Default-flavor lane must be byte-identical (no thunk emitted when
  flavors match — assert via `--emit=c11` diff on one test).

## Verification oracle

The default-lane (libstdc++) run of each test IS the oracle: same script,
same output, flavor swapped.
