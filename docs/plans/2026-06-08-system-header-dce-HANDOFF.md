# HANDOFF — System-header origin + reachability DCE for library-class method bodies

**Read this FIRST on resume/post-compaction.** Self-contained cold-start brief for
the next increment of the header-partition campaign. Assume you remember nothing.

Run `bash scripts/resume.sh` first (live git/branch/build truth), then read this.

---

## 0. TL;DR

Branch **`feature/header-partition-claude`**, HEAD **`86d868d`**, working tree clean,
**local only** (9 commits ahead, unpushed). Gates green: **fulltest 542/4**
(known reds: testdefer/testfstream/testlargesizeofquery/testloop), **gcc.c-torture
1566/31/57/1** (run ALONE), all **21 MI/RTTI/vdtor/virtual** tests pass.

The vtable-ownership milestone is **DONE and runtime-validated** (commits below).
The NEXT task (this handoff): **stop madc from emitting DEAD inline method bodies of
library (std::) classes.** Root cause: madc's Pass 2 emits the body of *every*
parsed function, ungated — so consuming a real header drags in its whole inline-
method web (mostly dead), and dead code was never lowered correctly. The fix has
two principled, no-shim tiers (both = what g++ actually does), detailed in §6.

This is a **C++-real-header-path-only** change. `is_externally_defined()` and the
new system-header predicate only fire for real `--no-embedded-headers` C++ builds,
so fulltest/torture/MI (no library classes) are **structurally unaffected** — that
is the safety property to preserve.

---

## 1. The campaign & where this sits

**Header-partition campaign** (full plan: `~/.claude/plans/clever-scribbling-dove.md`;
governing doc `madc-header-partition-handoff.md`; memory `project_header_partition`):
madc ships only compiler-freestanding + its own `ns_*` headers, and **consumes the
REAL glibc/libstdc++ headers** (eventually as one pre-LEXED compressed embedded
package). The keystone is making real C++ headers parse, lower, and RUN.

This handoff is the R5 (real-header sema-completeness) sub-track: getting a real
`<iostream>` program (`tests/testcout.mad`, run with
`--std=c++17 --no-embedded-headers`) to compile and run end-to-end. Two structural
walls already fell this session (repeated-declaration; vtable ownership). The
remaining wall is dead library inline-method bodies.

**Principle that governs everything here (do not violate):**
> madc emits *definitions* only for entities it *defines*. Anything defined in
> libstdc++ (vtables, typeinfo, out-of-line virtuals, explicitly-instantiated
> members) is referenced by its real Itanium symbol, never re-synthesized.
> All discriminators are DATA-DRIVEN — never a `namespace == "std"` or class-name
> test (project Rule #7).

---

## 2. The session arc (5 commits since the prior handoff `8e38571`) — understand these

The codebase state you inherit was built by these, in order. Each is gated green
(fulltest known-reds-only + torture baseline). All are on the real-header path
except where noted.

### `4fa746e` (prior session) — std:: free-fn mangling `_ZSt…`
Background: `std::terminate()` etc. were mangled `_ZNSt9terminateEv` (nested-name)
instead of the canonical `_ZSt9terminatev` (Itanium `St` unscoped-name
abbreviation), so dlsym missed symbols already in-process. Fixed in
`madc_mangle.cpp` `mangle_nested_function` (single-`std`-qualifier → `_ZSt<name>…`).
Real `<type_traits>` then runs. **This is the precedent**: the `St` abbreviation
machinery (and `encode_type`/`itanium_encode_type_sub`, `std_abbrev`) is the single
source of mangled std:: symbols — reuse it, never hardcode a literal.

### `b9d667f` — Cause A: unique struct tag for colliding typedef aliases
`std::string` and `std::pmr::string` both register the bare alias `string` backing
DIFFERENT structs → two `typedef X string;` at module scope → c2mir "repeated
declaration". Fix (`cir_builder.cpp`): a chokepoint helper `typedef_emit_name(alias,
dd)` — an alias backing >1 distinct struct tag emits the underlying struct's
already-unique tag instead of the bare alias; applied at all 5 type-spec emission
sites; `struct_behind` lifted to a static member. "Store bare, emit unique-when-
colliding." Non-colliding aliases unchanged.

### `e9dbef1` — Cause B: shared virtual base laid out once (diamond-with-data)
A transitive virtual base reached via two paths (`ios_base`/`basic_ios` via both
`basic_istream` and `basic_ostream`) had its members flattened once PER PATH and
resolved against the DIRECT (non-virtual) base offset → DUPLICATED in the struct
AND mis-located. Root cause: `member_origin` is only an int index into direct
`bases[]`, can't express a TRANSITIVE virtual base. Fix:
- new `member_vbase` map (member index → its virtual base) — `datadef.h:276`,
  forward-decl `datadef.h:242`; mirrors the `member_explicit_align` map pattern.
- flatten in TWO passes (`parser.cpp` ~`16991` `flatten_member` lambda, Pass A
  `~17008`, Pass B `~17027`): Pass A copies each non-virtual direct base's
  NON-vbase members per path; Pass B walks the virtual-base CLOSURE
  (`collect_vbases`) and copies each unique vbase's members ONCE, tagged.
- `apply_member_layout` (`parser.cpp` ~`6160`) resolves a `member_vbase` member
  against `vbase_offset[V]` first.
Validated diamond-with-data + deep nested-vbase byte-for-byte vs g++. Permanent
test `tests/test_mi_vbase_data.mad` (g++-matched). The blind spot it closed:
`test_mi_vbase_ctor`'s diamond had an EMPTY vbase; `test_mi_layout`'s vbase was
single-path.

### `addfee3` — vtable ownership, half 1: data-driven predicate + suppression
madc was synthesizing a PARALLEL vtable+typeinfo for every std:: library
polymorphic class under WRONG un-namespaced symbols (`_ZTI11logic_error` ≠
libstdc++'s real `_ZTISt11logic_error`), referencing bodyless virtual slots →
the "undeclared `__what`/`__do_*`" wall + a divergent typeinfo that would silently
break cross-boundary catch/dynamic_cast.
- **`DataDefCLASS::is_externally_defined()`** — `parser.cpp:6301`, decl
  `datadef.h:710`. Purely data-driven: `has_vtable && !canonical_cpp_spelling.empty()
  && every VTABLE SLOT (virtual method / virtual dtor) is a bodyless external &&
  whole base chain external`. **Keyed on VIRTUAL methods only** — inline NON-virtual
  helpers (`ctype::toupper` forwarding to external `do_toupper`) carry bodies but
  aren't slots, so they must not disqualify (the trap that made a naive "no method
  has a body" predicate suppress only 1/43). `has_madc_body(v)` =
  `!declaration_only && emit_symbol.empty() && !pure_virtual && !defaulted_or_deleted`.
- cir_builder Pass 1.5 (`~8497`, the single vtable/typeinfo/thunk driver) skips
  external classes; defensive early-returns in `class_typeinfo_def` (`~2475`) and
  `class_vtable_def` (`~2600`). Suppressing the vtable initializer also drops the
  now-unneeded virtual-method prototypes (`referenced_funcs`).
Real <iostream>: emitted std:: vtables 43→19, errors 37→24. Cannot misfire on user
classes (global → no spelling) or user-derived-from-std:: (overrides have bodies).

### `86d868d` — vtable ownership, half 2: reference the REAL `_ZTVSt…`/`_ZTISt…`
With definitions suppressed, the consumers that still need them — vptr-install in
madc-emitted inline ctors, `dynamic_cast`, `typeid` — now reference the real
libstdc++ symbols.
- mangler (`madc_mangle.cpp:210/215`, decl `madc_mangle.h`):
  `itanium_vtable_sym_cpp(spelling)` = `"_ZTV" + itanium_encode_type_sub(spelling)`;
  `itanium_typeinfo_sym_cpp` likewise with `_ZTI`. Built from the St-aware encoder:
  `"std::bad_alloc"` → `_ZTVSt9bad_alloc`; `std::_V2::error_category` →
  `_ZTVNSt3_V214error_categoryE` (nested). For an un-namespaced class the encoding
  equals `source_name`, so user classes are byte-for-byte unchanged.
- cir_builder helpers: `class_vtable_symbol(cdd)` (`2435`),
  `class_typeinfo_symbol(cdd)` (`2442`) — madc symbol for a class madc defines, real
  symbol for an external one. `data_extern_decl(sym)` (`2450`) emits
  `extern void *SYM[];` (deduped via `m_rtti_data_externs`).
- Pass 1.5 (`~8497`) emits `extern void *_ZTVSt…[];`/`_ZTISt…[];` for each external
  class. All vptr-install sites (`class_ctor_call` ~`3995`, `new` ~`5319`, ctor
  prologue ~`7911`) and RTTI sites (`dynamic_cast` ~`5056/5057`, `typeid`
  ~`5093/5112`) route through the helpers.
- **PROVEN end-to-end vs g++** — `tests/test_extern_polymorphic.mad`
  (`.flags = --std=c++17 --no-embedded-headers`): `std::bad_alloc e; e.what()` →
  `std::bad_alloc` (virtual dispatch through the REAL `_ZTVSt9bad_alloc`);
  `typeid(e).name()` → `St9bad_alloc` (real `_ZTISt9bad_alloc`). Construction +
  virtual dispatch + RTTI are runtime-correct, not just symbol-correct.

---

## 3. The CURRENT PROBLEM — dead library inline-method bodies

Real `<iostream>` is down to **21 c2mir check errors** (from 37). Run to reproduce:
```bash
bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad 2>&1 | \
  grep -iE "error|undeclared" | grep -vi warning | \
  sed -E 's/:[0-9]+:[0-9]+:/:L:C:/' | sort | uniq -c | sort -rn
```
The full deduped set (HEAD `86d868d`):
- `undeclared identifier current` ×128 — `basic_string`/`string` (a mis-lowered
  local `current` in a basic_string member body).
- `undeclared identifier __madc_objtmp_NNN` ×~25 — `basic_string`/`string`
  (synthetic object-temp names referenced out of their scope in emitted bodies).
- `_M_sbuf` ×10 — `basic_ios` (private member access in an inline method).
- `_M_resource` ×8 — pmr `string`.
- `ctype_*__do_*` / `num_put__do_put` / `num_get__do_get` /
  `basic_streambuf__xsputn`/`__xsgetn` / `__ctype_abstract_base__do_scan_*` —
  inline facet/streambuf methods making DIRECT calls to bodyless virtuals.
- system_error ×8 — `invalid types of comparison operands`, `too few arguments`,
  `lvalue required as unary &`, `incompatible argument type for arithmetic`.

**These are ALL dead.** `cout << "literal" << -x << endl;` uses `const char*` and
`int` — it never constructs/uses `std::string`, `ctype`, `num_put`, etc. madc emits
their bodies anyway because **Pass 2 emits the body of every parsed function,
ungated** (`cir_builder.cpp:8293` `for (TokenFunc *tf : funcs) { func_def(tf); }`).
Being dead, the bodies were never exercised/lowered correctly → the errors. This is
a DEAD-CODE problem, not a lowering problem; fixing the individual lowerings would
be effort spent on code that must never be emitted.

Confirmed via recon: the dead vtable/ctor cluster (the `*__vtable` refs) was
resolved by `86d868d`; what remains is the inline-method web. A polymorphic-only
reachability worklist was prototyped this session and **reverted** because it
helped only 22→21 — the bulk is **non-polymorphic** `basic_string`, which
`is_externally_defined()` (requires a vtable) does not cover.

---

## 4. HOW g++ HANDLES THIS (research — informs the design)

Two complementary mechanisms; verified empirically this session:

**(1) ODR-use-gated emission of inline functions (weak/COMDAT).**
g++ emits an inline function/method **only when it is ODR-used in the TU**, as a
weak (COMDAT) symbol the linker dedups across TUs. Verified:
```
struct S { int used(){return 1;} int unused(){return 2;} };
int main(){ S s; return s.used(); }
```
`g++ -O0 -c` → `nm -C` shows `W S::used()` and **no `S::unused()` at all**. Unused
inline functions are simply never emitted. **madc lacks this** (emits all parsed
bodies) — that is precisely the bug. Tier 1 below is "implement g++'s ODR-use
model" = reachability DCE.

**(2) `extern template` → bind to the library's explicit instantiation.**
For specializations libstdc++ explicitly instantiates (notably
`std::__cxx11::basic_string<char>` and `<wchar_t>`), the header declares
`extern template class basic_string<char>;` (in `bits/basic_string.tcc`, guarded by
`_GLIBCXX_EXTERN_TEMPLATE`). This tells the compiler: **do NOT instantiate/emit these
member bodies even when used** — bind to libstdc++.so's out-of-line **weak** copies.
Verified those symbols exist:
```
nm -DC libstdc++.so.6 | grep 'basic_string<char.*>::\(substr\|append\|c_str\)'
→ W std::__cxx11::basic_string<...>::c_str() const, ::substr(...), ::append(...), …
```
So for USED `std::string` methods, the *correct* model is mangled-direct binding to
these symbols — exactly the strategy that already makes `cout<<`, `what()`,
`typeid` work. This is Tier 2 below.

Net: g++ never emits dead inline bodies (Tier 1), and for explicitly-instantiated
library specializations it never emits the bodies at all — it calls the .so
(Tier 2). madc should do both. Neither is a shim; both are what a conforming
toolchain does.

---

## 5. THE PLAN (a) — two tiers, in order

### Tier 1 (the immediate ask): reachability DCE for library-class function bodies

**Goal:** emit a library function/method body only if it is reachable from the
program's roots (the user's own code). Dead library inline methods are dropped →
the 21 errors (all dead) disappear, the same way g++ never emits them.

**Two pieces:**

**T1.a — a data-driven "from a system/real header" signal per declaration.**
`is_externally_defined()` requires a vtable, so it misses `basic_string` (non-
polymorphic, fully inline). Need a broader, still data-driven discriminator: "this
class/function was parsed from a system header (`<...>` / a system include dir),
not from user source." Two viable, no-hardcode approaches:
  - **Preferred — reuse data already present:** every `TokenBase` carries `file`
    (from `TokenBase::_parse_file`, `tokens.h:86/89`). The class's defining token
    (and each method's TokenFunc) has a `file`. Classify it as system by testing
    the path against the existing system-include dir list
    **`madc_sys_include_paths[]`** (`extern` in `lexer.cpp:1559/1625`, generated by
    `scripts/gen_sys_includes.sh` → `src/sys_include_paths.cpp`). Add a helper
    `Program::is_system_header_path(const char *file)` (prefix-match against
    `madc_sys_include_paths`, like the include resolver already does). No new lexer
    state, no per-class flag needed if you classify on demand from the token file.
  - Alternative — set a `bool DataDefCLASS::from_system_header` (and a per-TokenFunc
    equivalent) at parse time in `TokenCLASS::parse` (ddc created at
    `parser.cpp:16883/16899`) by classifying `TokenBase::_parse_file` then. Cache
    it. More plumbing; only do this if on-demand classification proves too slow or
    the file is unavailable at an emission site.
  Whichever: it MUST be data-driven (path-based), never `namespace=="std"`.

**T1.b — reachability worklist in Pass 2 body emission** (`cir_builder.cpp` around
the `func_def` loop at `8293`). The structure that already works for PROTOTYPES
(translate all bodies → `referenced_funcs` complete → emit only referenced protos
at `8301`) is the template. For BODIES:
  1. Partition `funcs` into **roots** (functions NOT from a system header — the
     user's `.mad` code, `main`, free functions, thin wrappers) and **library
     functions** (from a system header).
  2. Translate roots first → seeds `referenced_funcs` (populated as `N_CALL` nodes
     build) with what user code calls.
  3. **Worklist to fixpoint:** translate a library function iff its emit symbol (or
     unmangled `tf->var.name`) is in `referenced_funcs`; translating it may pull in
     more library functions (transitive). Repeat until no growth.
  4. `func_def_nodes` = translated roots + translated reachable library functions.
  The emit symbol of a method: `func_emit_name(tf->var, dynamic_cast<FuncDef*>(
  tf->var.type))`. The owner class: `tf->method ? tf->method->owner_class : NULL`
  (see `func_def` at `7861`). Check both the mangled symbol and `tf->var.name`
  against `referenced_funcs` (mirror the proto pass at `8307-8308`).

  **The prototyped worklist (reverted; broaden the predicate from
  `is_externally_defined()` to the T1.a system-header test):**
  ```cpp
  std::vector<node_t> func_def_nodes;
  std::map<std::string, TokenFunc *> lib_funcs;   // emit-symbol -> library fn
  std::vector<TokenFunc *> roots;
  for (TokenFunc *tf : funcs) {
      FuncDef *tfd = dynamic_cast<FuncDef *>(tf->var.type);
      bool from_lib = tfd && tf->origin_is_system_header();   // <-- T1.a predicate
      if (from_lib) lib_funcs[func_emit_name(tf->var, tfd)] = tf;
      else          roots.push_back(tf);
  }
  for (TokenFunc *tf : roots) { node_t fd = func_def(tf); if (fd) func_def_nodes.push_back(fd); }
  std::set<std::string> lib_emitted;
  for (bool grew = true; grew; ) {
      grew = false;
      for (auto &kv : lib_funcs) {
          if (lib_emitted.count(kv.first)) continue;
          TokenFunc *tf = kv.second;
          if (!referenced_funcs.count(kv.first) && !referenced_funcs.count(tf->var.name)) continue;
          lib_emitted.insert(kv.first);
          node_t fd = func_def(tf);
          if (fd) func_def_nodes.push_back(fd);
          grew = true;
      }
  }
  ```
  CAUTION: `func_def` has side effects beyond `referenced_funcs` (e.g.
  `m_global_ctor_stmts`, `m_user_func_names`). `collect_global_ctors` runs before
  the loop (`~8287`) and `main` is a root, so global-ctor assembly for `main` is
  fine. Verify `m_user_func_names` (set just before, cleared after at `~8297`) is
  still set across the worklist (move the clear to after the worklist).

**Tier-1 risk & safety:** gating only fires for system-header functions, so a
non-real-header build (every test in fulltest except `test_extern_polymorphic`) has
zero library functions → every function is a root → behaviour byte-for-byte
unchanged. KEEP that property (it's why fulltest/torture can't regress). The one
real hazard: a library function reached only via a **function pointer / vtable
slot** (not a direct `N_CALL`) would be missed by `referenced_funcs`. Vtable slots
of external classes are already suppressed (libstdc++ owns them), so the live cases
are mangled-direct or in `referenced_funcs`. If a fn-ptr-only library function
surfaces, it appears as an honest undefined symbol — not silent-wrong — and is the
signal to also seed the worklist from address-taken refs.

### Tier 2 (the complete std:: answer): honor `extern template`

For an explicitly-instantiated specialization (`extern template class
basic_string<char>;`), do NOT emit the member bodies even when used — bind member
CALLS to the libstdc++ out-of-line weak symbols (mangled-direct), exactly like the
non-inline-method path already does (`emit_symbol`). This is what makes
`cout << std::string("x")` correct, and it shrinks emitted C dramatically.

Sketch:
- Parse `extern template class X<args>;` (a declaration form madc likely drops
  today — verify in `parser.cpp` template handling). Record the specialization as
  "externally instantiated."
- For a method CALL on such a specialization, set/derive the mangled `emit_symbol`
  (via the existing member-mangler `itanium_mangle_member_sub` / the `St` family)
  and DON'T emit the body — same mechanism as bodyless methods. libstdc++'s weak
  symbols resolve it at MIR link (R2 auto-load already links libstdc++).
- Validate: a real-`<string>` program (`std::string s("hi"); puts(s.c_str());`)
  compiles, links, runs, matches g++.

Tier 2 is the deeper, complete model; Tier 1 alone clears the *current* (all-dead)
errors and is the immediate ask. Do Tier 1 first, validate `<iostream>` compiles &
runs, commit; then Tier 2 for USED std::string/streams. The two compose: Tier 1
drops dead inline methods; Tier 2 avoids emitting *used* explicitly-instantiated
ones (binds to the .so instead).

---

## 6. KEY ANCHORS (live line numbers @ `86d868d`)

- Predicate: `DataDefCLASS::is_externally_defined()` — `src/parser.cpp:6301`; decl
  `include/datadef.h:710`. `has_madc_body` lambda is inside it — reuse its shape.
- Pass 2 body loop (where Tier-1 worklist goes): `src/cir_builder.cpp:8293`.
- Proto pass (the reachability template): `src/cir_builder.cpp:8301`, dual
  symbol/name check at `8307-8308`. `m_user_func_names` set `~8290`/cleared `~8297`.
- `func_def` + owner-class derivation: `src/cir_builder.cpp:7665`, `ocls` at `7861`.
- `func_emit_name`: `src/cir_builder.cpp:151` (returns `fd->nested_emit_name` if set,
  else `var_emit_name`).
- System include path list: `madc_sys_include_paths[]` extern in
  `src/lexer.cpp:1559/1625`; defined in generated `src/sys_include_paths.cpp`
  (`scripts/gen_sys_includes.sh`). Include resolver prefix-matching: `lexer.cpp`
  `resolve_include_path` `~1530`, `resolve_include_next_path` `~1620`.
- Token source file: `TokenBase::file` / `TokenBase::_parse_file` — `tokens.h:86/89`;
  set from `_cur_token->file` at `madc.h:1513`.
- Class creation in parse: `src/parser.cpp:16883` (`ddc=`), `16899` (new), `16824`
  (fwd). `TokenBase::_parse_file` is current at these points.
- Real-symbol helpers (Tier 2 binding): `itanium_mangle_member_sub`,
  `itanium_mangle_ctor_sub`, `itanium_encode_type_sub` — `madc_mangle.h:72-95`,
  impls in `madc_mangle.cpp`.
- External-class vtable/typeinfo helpers (already wired): `class_vtable_symbol`
  `cir_builder.cpp:2435`, `class_typeinfo_symbol` `2442`, `data_extern_decl` `2450`.

---

## 7. METHOD (mandatory — same as what worked this session)

- **Hybrid:** read-only Explore subagents (opus) for RECON / localizing; do the
  FIXES yourself (correctness-critical). Do NOT delegate edits to one-shot agents.
- Per change: reduce → compare gcc/clang/`c2m FILE -ei/-eg` (gcc-pass & clang-pass &
  c2m-fail = c2mir bug; else madc bug) → DEEPEST-layer fix → rebuild → re-probe →
  fulltest (known reds only) → torture failset **run ALONE** (1566/31/57/1) → commit.
- Validate by RUNNING (or `--dump-cir`), NOT `--emit=c11`-as-truth: `--emit=c11`
  parses but SKIPS c2mir checking, so "repeated declaration"/"unknown type"/
  "undeclared identifier" only appear on the RUN path.
- Real-header probes need `--std=c++17 --no-embedded-headers` (default STD_MADC does
  not define `__cplusplus` → real headers fail to parse).
- Reducers in `tmp/` (gitignored). Cap every run `( ulimit -t 120; timeout 180 … )`,
  ONE heavy job at a time. **NAS mtime trap:** `touch src/<f>.cpp` before `make`;
  clean-rebuild if results look impossible.
- For the g++ ODR/weak model, the reference experiments are in §4 — re-run them if
  you need to confirm behaviour.

## 8. EXACT COMMANDS

```bash
cd /workspace/madc
git rev-parse --short HEAD                                   # 86d868d
make -C src 2>&1 | grep -iE 'error:|warning:'                # clean build
make -C src fulltest 2>&1 | grep -E 'passed,|FAIL:'          # 542/4 (known reds)
python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc | tail -1  # 1566/31/57/1 ALONE
# the target program + error breakdown:
bin/madc --std=c++17 --no-embedded-headers tests/testcout.mad 2>&1 | grep -iE 'error|undeclared' | grep -vi warning | sed -E 's/:[0-9]+:[0-9]+:/:L:C:/' | sort | uniq -c | sort -rn
# vtable-ownership regression proof (must stay PASS):
bin/madc --std=c++17 --no-embedded-headers tests/test_extern_polymorphic.mad   # what=std::bad_alloc / name=St9bad_alloc
# the 21 MI/RTTI/vdtor/virtual tests (must stay PASS) — see resume.sh list.
# g++ ODR-use reference (§4):
printf 'struct S{int u(){return 1;}int n(){return 2;}};int main(){S s;return s.u();}\n' > tmp/odr.cpp
g++ -O0 -c tmp/odr.cpp -o tmp/odr.o && nm -C tmp/odr.o | grep 'S::'   # only S::u() (weak), no S::n()
nm -DC /usr/lib/x86_64-linux-gnu/libstdc++.so.6 | grep 'basic_string<char.*>::c_str'  # the extern-template weak symbol
```

## 9. ACCEPTANCE / VERIFICATION

- **Tier 1 done when:** real `<iostream>` (`testcout.mad`) compiles AND runs,
  producing g++-matching output; the 21 dead-method errors are gone; fulltest
  542/4 (known reds), torture 1566/31/57/1, all 21 MI/RTTI tests + 
  `test_extern_polymorphic` still pass. Add a permanent test (a real-`<iostream>`
  `cout << ... << endl` program with a `.flags` of `--std=c++17 --no-embedded-headers`
  and a g++-matched `.expect`) — it becomes the headline real-header regression.
- **Tier 2 done when:** a real-`<string>` program (`std::string s("hi");
  puts(s.c_str());`) compiles, links (libstdc++ weak symbols), runs, matches g++ —
  with NO emitted basic_string member bodies (verify via `--emit=c11`: the calls are
  mangled `_ZNSt7__cxx1112basic_string…` references, not `basic_string…__c_str`
  bodies). Permanent test with `.flags`/`.expect`.
- After both: revisit retiring the hand-tooled `include/madc/{string,iostream,…}`
  shims (campaign milestone M) — the real headers now carry their weight.

## 10. OPEN ITEMS

- **Unpushed:** `feature/header-partition-claude` is 9 commits ahead, local only.
  Also `develop` (123 ahead — the earlier merge) and MadSMAUG `develop` are local.
  All PENDING the user's push decision — do not push without asking.
- `tmp/` has scratch reducers (`diamond.mad`, `ctor_rtti.*`, `io_*.c`, `odr*.cpp`) —
  gitignored, ignore/clean as needed.
- The four known fulltest reds (testdefer/testfstream/testlargesizeofquery/testloop)
  predate this work; testfstream/testloop/testdefer are slated to go green when the
  real-header `<fstream>` retirement lands (campaign M). Do not "fix" them here.

## 11. WHY THIS IS NOT A SHIM (the user's standing constraint)

The user has repeatedly required: no shims, no hardcoding, fix at the deepest layer,
especially "at the finish line." This plan honors that:
- The predicate is data-driven (path-based system-header test + bodyless-virtual
  aggregation), never a `namespace=="std"`/name check (Rule #7).
- Tier 1 IS g++'s real model (emit inline functions only when ODR-used). We're
  implementing standard compiler behaviour, not papering over symptoms.
- Tier 2 IS the C++ `extern template` contract (bind to the library's explicit
  instantiation). Same mangled-direct mechanism already proven for cout<</what/typeid.
- We do NOT emit a guard/"not-yet-supported" placeholder on a live path: an
  unreachable-but-needed function surfaces as an honest undefined symbol, never a
  silently-wrong result. Dead code is simply not emitted (correct), not stubbed.

See `[[project_header_partition]]`, `[[project_cpp_mangled_direct]]`,
`[[feedback_correct_over_shortcuts]]`, `[[feedback_dont_cling_to_legacy]]`.
