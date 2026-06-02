# File Streams as Header-Defined Classes (retire the fstream wrappers) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Model `std::ofstream`/`ifstream`/`fstream` as **header-defined classes** that route every
operation (`open`/`close`/`is_open`/`good`/`eof`/`<<`/`>>`/`getline`) through generic resolution +
the (now-complete) mangler to real libstdc++ symbols — then **delete** `add_fstream_methods`, the
`madc_stream_runtime.cpp` wrappers, and the `dt*STREAM`/`dd*STREAM` builtins. Fixes
`testfstream`/`testloop`.

**Architecture:** Sub-project 1 of the retire-std-hardcoding campaign
(`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`). The mangler is complete
(W1a/b + std-var + fn-ptr), so every stream symbol is generatable. This sub-project changes live
codegen and is the first to need a **non-zero base-subobject offset** (the `std::ios` vbase).

**Tech Stack:** C++11; `include/madc/fstream` (+ iostream bases); `src/parser.cpp` (registration);
`src/cir_builder.cpp/.h` (class member-call + stream-chain routing, base-offset adjust); `madc_mangle`;
g++/`c++filt` as canon; `.mad` integration tests.

---

## Investigation findings (verified 2026-06-02 — read before planning tasks)

1. **madc's class model is single-inheritance, base subobject at offset 0** (`datadef.h:699-722`:
   `DataDefCLASS::base_class` single chain; comment *"the base subobject lives at offset 0"*).
   There is a vtable mechanism (`vtable_slots`/`virtual_methods`/`has_vtable`) but **no non-zero or
   virtual base-offset support**.
2. **Stream subobject offsets (g++ probe, `tmp/voff.cpp`, this libstdc++):**
   `sizeof(ofstream)=512`, `sizeof(ostream)=272`, `sizeof(basic_ios<char>)=264`.
   - `ofstream → ostream` subobject offset = **0** → `<<`, and the ofstream-own `open`/`close`/
     `is_open` (mangled `basic_ofstream::…`), all take `this=&obj` directly. **Work with the
     current offset-0 model.**
   - `ofstream → basic_ios` subobject offset = **248** → `good()`/`eof()` are `basic_ios` methods
     and need `this=&obj+248`. **This is the one piece needing a non-zero base offset.**
   - `ifstream → istream` offset = 0 (same shape; `>>`/`getline`/`open` at 0, `good`/`eof` at +248).
3. **Stream types today are builtin `DataDefCLASS` with hardcoded `sizeof(std::ofstream)`**
   (`datadef.h:765-770`) — those `sizeof(std::…)` refs must leave madc (spec: no real std:: type
   referenced inside the compiler; layout derived from the header, cross-checked in a doctest).
4. **`add_fstream_methods`** (`parser.cpp:5090`) registers `open`/`close`/`good`/`eof`/`is_open`
   bound to the `ifstream_open`/`ofstream_good`/… wrappers (`madc_stream_runtime.cpp`, the whole
   file; + file-stream wrappers in `madc_mir_backend.cpp`). Only consumer of those wrappers.
5. `translate_stream_chain`/`stream_ident_kind` only recognize `cout`/`cin`/`cerr`/`clog` by NAME
   (`cir_builder.cpp:1609`); a local `ofstream` variable is not recognized → `outf << x` falls to
   integer-shift. Must generalize to "any ostream/istream-derived object."

## Required mangled symbols (all generatable now; confirm each vs `c++filt` during impl)

- `ofstream` ctor/dtor: `_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev` / `…D1Ev`
- `open(const char*)`: `_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode`
  (note the defaulted `openmode` arg — pass `std::ios_base::out`=16 for ofstream / `in`=8 for ifstream)
- `close`: `_ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv`; `is_open`: `…8is_openEv`
- `good`/`eof` (basic_ios): `_ZNKSt9basic_iosIcSt11char_traitsIcEE4goodEv` / `…3eofEv` (this=+248)
- `<<`/`>>`/`getline`/`endl`: already covered by the W1 mangler (see test_mangle).
  Generate ofstream/ios symbols by extending `itanium_mangle_member_sub` usage (the `_sub`
  encoder already produces `basic_ofstream`/`basic_ios` member symbols — verify in a doctest first).

---

## Task 1: Mangler doctests for the ofstream/ifstream/basic_ios member symbols ✅ DONE (9582beb)

**Files:** Modify `tests/unit/test_mangle.cpp`

- [x] **Step 1: Confirm each symbol vs the real toolchain** — all symbols demangle via `c++filt`
  AND are exported by `nm -D libstdc++` (open/close/is_open real exports; good/eof weak
  vague-linkage `W` exports → the +248 calls will link).
- [x] **Step 2: Add a doctest** asserting `itanium_mangle_ctor_sub`/`_dtor_sub`/`_member_sub` on
  `"std::basic_ofstream<char,std::char_traits<char>>"`, `"std::basic_ifstream<…>"`, and
  `"std::basic_ios<char,std::char_traits<char>>"` produce those exact symbols. CONFIRMED: the
  `_sub` encoder already handles these shapes with NO mangler change — including
  `std::_Ios_Openmode`→`St13_Ios_Openmode` (a non-template std type) and the plain-std
  (non-`__cxx11`) `basic_ofstream`/`ifstream`/`ios` spellings.
- [x] **Step 3: Build + run** `tmp/test_mangle` — green: 40→44 cases / 134→143 assertions, zero
  regression. No mangler fix was needed (the hypothesis held).

## Task 2: Author the header(s) — declare the real stream classes (layout + inheritance)

**Files:** Create `include/madc/fstream`; extend `include/madc/iostream` with the ios/ostream/istream
base declarations (or an internal `include/madc/bits/ios.h`).

- [ ] **Step 1: Declare the hierarchy with layout-faithful storage.** The header declares
  `basic_ios`/`ostream`/`istream`/`ofstream`/`ifstream`/`fstream` with **bodyless** methods bound to
  mangled libstdc++ symbols, and **opaque storage sized so madc's computed layout matches
  libstdc++** (ostream subobject at 0; basic_ios subobject at +248; total ofstream 512). Express the
  inheritance so `<<`/`open`/`close` resolve at offset 0 and `good`/`eof` resolve to the basic_ios
  base. EXACT member spelling is determined during impl by matching the doctest offsets (Task 4's
  layout cross-check is the oracle). *Design note:* because the basic_ios vbase sits at +248 (not 0),
  a naive `class ofstream : public ios {…}` (base-at-0) is WRONG for good/eof — see Task 4.
- [ ] **Step 2: Wire `#include <fstream>` + auto-include** to register these under `std::` lazily
  (the embedded-header mechanism), and add the `ofstream`/`ifstream`/`fstream`→`<fstream>` entries to
  the auto-include trigger map (the only permitted hardcoded std:: data).

## Task 3: Route offset-0 stream operations through generic resolution + mangler

**Files:** Modify `src/cir_builder.cpp` (class member-call path; `translate_stream_chain`)

- [ ] **Step 1: Failing tests** — `bin/madc tests/testfstream.mad` / `testloop.mad` still error;
  capture the current messages as the baseline.
- [ ] **Step 2: Member calls** `outf.open(f)`/`close()`/`is_open()` resolve via `class_method_call`
  to the mangled `basic_ofstream::…` symbols (this=&obj, offset 0). `open` passes the defaulted
  `_Ios_Openmode` (out=16 / in=8). The string overload `open(const string&)` or the `const char*`
  overload — pick the one matching how the test calls it; `string`→`const char*` via the existing
  `string_cstr` coercion if needed (but prefer the real `open(const string&)` symbol).
- [ ] **Step 3: `<<`/`>>` on a local stream object** — generalize `stream_ident_kind`/
  `translate_stream_chain` from "named cout/cin" to "any object whose type is (derived from)
  ostream/istream", driving the operator symbols from the mangler (offset 0). `getline(inf,line)`
  routes to the mangled `std::getline` (W1b).
- [ ] **Step 4: Build + run** `testfstream`/`testloop` — they should now compile and run EXCEPT for
  `good()`/`eof()` (Task 4). Spot-check `--dump-cir`.

## Task 4: Non-zero base-subobject offset for `basic_ios` methods (good/eof) — the vbase piece

**Files:** Modify `include/datadef.h` (base offset), `src/cir_builder.cpp` (apply offset in the
inherited-method `this`-adjust), `tests/unit/` (layout cross-check doctest)

- [ ] **Step 1: Layout cross-check doctest** — assert madc's header-derived `sizeof(ofstream)`==512,
  `basic_ios` subobject offset==248, etc., by `#include`ing the real `<fstream>` in the TEST and
  comparing to madc's computed layout. (The check lives ONLY in the doctest, never in madc.)
- [ ] **Step 2: Add a base-subobject byte offset to the class model.** Extend `DataDefCLASS` (or the
  base linkage) with the byte offset of a base subobject (0 for ordinary single inheritance; the
  computed 248 for the basic_ios vbase of a stream), **derived from the header-declared layout**, and
  apply it in `class_method_call` when the resolved method belongs to a base whose subobject offset
  is non-zero: emit `sym((char*)&obj + offset, …)`. Keep ordinary classes (offset 0) byte-identical.
- [ ] **Step 3: Build + run** `testfstream`/`testloop` — now fully green (good()/eof() correct).
  `while(inf.good())` reads lines.

## Task 5: Delete the wrapper layer + the stream builtins (the cruft)

**Files:** `src/parser.cpp` (`add_fstream_methods` + the `extern` decls + registrations);
`src/madc_stream_runtime.cpp` (delete file); `src/madc_mir_backend.cpp` (file-stream wrappers);
`include/datadef.h` (`DataDefIFSTREAM/OFSTREAM/FSTREAM` + the `sizeof(std::…)`); `src/Makefile`
(drop `madc_stream_runtime.o`); the `dtIFSTREAM/dtOFSTREAM/dtFSTREAM` enum tags + their parser/
cir_builder branches.

- [ ] **Step 1: Delete** `add_fstream_methods` and its call; the `ifstream_*`/`ofstream_*`/`fstream_*`
  externs; `madc_stream_runtime.cpp` (drop from `src/Makefile`); the file-stream wrappers in
  `madc_mir_backend.cpp`; `ddIFSTREAM/ddOFSTREAM/ddFSTREAM` + `DataDef*` + the `dt*STREAM` enum tags
  and every parser/cir_builder branch on them.
- [ ] **Step 2: Build with `-Wall`** — `-Wunused-function` on the deleted web confirms the cut is
  complete. Fix fallout at the type-system layer (no new special-cases).
- [ ] **Step 3: Grep-gate** `grep -rn "ifstream_open\|ofstream_good\|dtOFSTREAM\|dtIFSTREAM\|dtFSTREAM\|add_fstream_methods\|sizeof(std::ofstream\|sizeof(std::ifstream\|sizeof(std::fstream" src/ include/`
  → **zero**.

## Validation (end state)

- `make -C src test` green (incl. the new mangler + layout doctests); `make -C src fulltest`:
  `testfstream`/`testloop` now PASS → **459** pass (was 457), 4 fail / 55 skip + flaky.
- Full gcc-torture failset-diff = **zero** (pure C, unaffected). SMAUG soak clean (pure C, unaffected).
- `bin/madc --dump-cir tests/testfstream.mad`: stream ops are mangled libstdc++ calls; no wrapper
  names, no `_Z` literals.

## Self-Review

- **Spec coverage:** header-defined stream classes (T2), generic resolution + mangler (T1/T3),
  the vbase offset (T4 — derived from header, doctest-checked, not hardcoded), wrapper/builtin/tag
  deletion (T5). The "no real std:: type inside madc" rule → T5 removes the `sizeof(std::…)`.
- **Open risk (call out, do not hide):** T2's exact layout-faithful member spelling and T4's
  base-offset derivation are the hard parts; both are gated by the layout cross-check doctest (the
  oracle) and the integration tests. If T4's offset can't be cleanly derived from a header
  declaration, STOP and reconsider (do not hardcode 248 in madc) — escalate.
- **Sequencing:** offset-0 ops (T3) land first and fix most of the tests; the vbase (T4) is isolated
  to good/eof; deletion (T5) only after the mangled path covers every op.
