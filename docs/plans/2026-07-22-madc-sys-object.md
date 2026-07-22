# madc::sys — the system object (Python `sys` convention)

**Status:** DESIGN AGREED (owner discussion, 2026-07-22). Not yet scheduled
against the open ledger. Follow-on to script mode (task #90, v0.37.0).

## Owner decisions (settled in discussion)

1. **Follow the Python convention** — madc's future is Python-friendly, the
   feature is borrowed from `sys`, and even C++26 borrows from Python.
   Shape: a typed object with dot members (`sys.argv` — Python module
   attributes), NOT a PHP-`$_SERVER` associative array.
2. **`madc::array` for the sequence members** (`argv`, `path`) — and the
   broader principle: madc encourages modern, safe datatypes by default;
   raw system-level access stays available where performance is paramount.
3. **Bare `argc`/`argv` stay** (the script-mode synthesized-main
   parameters) as the raw C door.
4. **Safety over strictness:** the goal is no buffer overflows / crashes,
   not stricter-than-Python immutability. Mutation safety comes from the
   VALUE TYPES (bounds-checked, self-sizing, refcounted cells). Only
   values that are FACTS are immutable: `version`, `platform`,
   `hostname`. `argv` and `path` are mutable, like Python.
5. **`MADC_VERSION` is a preprocessor macro** (compile-time version
   identity, `#if`-testable). The legacy `version` built-in VARIABLE
   retires on the owner's schedule; `sys.version` is the runtime spelling.

## Surface (v1)

| Member | Type | Mutability | Semantics (Python analog) |
|---|---|---|---|
| `sys.argv` | `array` | mutable | `argv[0]` = script path, then args (`sys.argv`; mutable there too) |
| `sys.path` | `array` | mutable | include/`#load` search dirs; mutations affect FUTURE `#load`/eval only — same one-way semantics as Python (`sys.path` never re-does past imports) |
| `sys.platform` | `string` | **immutable** | `"linux"`, `"darwin"` (`sys.platform`) |
| `sys.version` | `string` | **immutable** | madc version, same source as `MADC_VERSION` (`sys.version`) |
| `sys.hostname` | `string` | **immutable** | madc extension (Python: `socket.gethostname()`); populated eagerly at init |

- **`sys.argc`: omitted (owner-confirmed 2026-07-22).** A mutable `argv`
  makes an `argc` member a silent-desync footgun; Python has no
  `sys.argc` (`len(sys.argv)`); the array is self-sizing; bare `argc`
  remains the raw door.
- Future members join by appending (append-only struct discipline):
  `executable` (`/proc/self/exe`), `env`, cwd, pid, …

## Architecture

- **cpp-first / mangled-direct** (per `.claude/rules/cpp-first-api.md`):
  a real host-side C++ object —
  `namespace madc { struct SysInfo { ... }; extern SysInfo sys; }` —
  declared declaration-only in the script-facing embedded header
  (`<ns_madc>` surface), resolved to the host's `_ZN4madc3sysE` symbol.
  No wrappers, no extern-C flattening.
- **Dialect story:** library surface, not a keyword — nothing gated in the
  parser (vision invariants I1–I8 clean). C modes have no namespaces →
  naturally absent. STD_MADC auto-includes it; explicit C++ modes may
  include the header like any library (same status as `php::`).
- **Per-lane population:**
  - JIT: the host populates its own object (it already holds
    `user_argc`/`user_argv` before execution) — populated before ANY
    script code, so dynamic global initializers may read `sys.*`.
  - `--emit=c11` / native (`-c`/`-o`): a TU referencing `sys` emits the
    storage definition plus `__madc_sys_init(argc, argv)` injected at
    main entry BEFORE the `__madc_global_init` call. `platform`/`version`
    may be baked constants at emit time; `argv`/`hostname` are runtime.
  - `-shared` (DT_INIT): everything except `argc/argv` content (no main
    → empty argv), documented.
  - `--project`: one storage definition, riding with the main-owning TU.
- **Script-mode interplay:** unchanged — bare `argc`/`argv` remain
  statement-scoped (v0.37.0 semantics); `sys.argv` works in every
  context (statements, top-level initializers, functions, non-script
  TUs). The starter-classifier corner stops deserving machinery; the
  trivial `<<`/`>>` identifier-continuation widening still lands (bare
  `cout << argv[0];`).

## Immutability enforcement (facts only)

**Revised by R1 recon (2026-07-22, decisions per owner meta-directive):**

- The runtime frozen bit lives at the **VALUE level — `MADC_VF_CONST` in
  `madc_value.flags`** (the 2026-06-12 design's reserved read-only bit),
  NOT in `madc_cell.cell_flags`: array/object kinds carry C++ container
  backing (no cell) and short strings are SSO (no cell), so a cell bit
  cannot guard the actual members. The cell `frozen` slot stays reserved
  for the pool tier.
- **R1 (landed with this slice):** `madc::value::freeze()/is_frozen()`;
  mutation entry points guarded — `array()`, `object()`,
  `instance_data()`, copy-assignment onto a frozen value throw for C++
  hosts (the class's established exception discipline); the script
  runtime's write choke points (`value_array_for_write` /
  `value_object_for_write`) pre-check and reject with the established
  loud-stderr + write-ignored convention, so no exception crosses the
  extern-C boundary into JIT frames. The C-API setter gate
  (`value_accepts_kind`) already rejected CONST. Freeze is slot-local,
  not viral: copies of a frozen value are mutable (Python parity —
  protecting the slot, not the data). Move-assignment onto a frozen
  value is not checked (noexcept); it is host misuse.
- **Fact members are `const char *` struct members in v1** (not
  madc::value, not std::string): zero lifecycle, bakeable at emit time,
  chars naturally immutable; reads work in every string context
  (streams, string ctor/assign via the literal model). Rebinding
  (`sys.platform = ...`) is guarded at compile time by the const member
  qualification — scope the const-member-assignment rejection check if
  the DataDefCONST Phase-1 state doesn't already provide it (probe at
  R2). Python parity note: this is already STRICTER than Python (which
  allows rebinding module attrs); the owner asked for facts to be
  immutable.
- In v1 nothing frozen is script-reachable as an array (`argv`/`path`
  are mutable, facts are not value-backed), so the frozen runtime guard
  is the general embedding primitive; the sys-facts enforcement is the
  compile-time const. `testsysreadonly`'s runtime-error fixture applies
  once a frozen value is script-reachable (embedding tier), not v1.
- **Populate-then-freeze order** in every lane: host/`__madc_sys_init`
  fills values first, then sets frozen.

## Rungs

- **R0 — recon (gates the plan):**
  (a) array-typed struct members end-to-end: `struct S { array a; }` +
  `s.a[1]` read/write through member access — if incomplete, closing it
  is a general language fix, not a sys special (fallback shape
  `madc::sys::argv` namespace members loses the Python dot — avoid);
  (b) the native-lane mechanism for host runtime symbols (the
  `madc_value_*`/string-helper family already works in native ELFs —
  `sys` rides that, never a parallel path);
  (c) mutation entry-point census: confirm array/string writes route
  through a countable set of runtime functions (frozen check = a handful
  of guards, not a scatter).
- **R1 — `MADC_CELL_FROZEN`:** implement the bit + guards + loud error.
- **R2 — the object, JIT lane:** host-side `madc::SysInfo sys`, embedded
  header declaration, population + freeze, auto-include wiring,
  `MADC_VERSION` macro formalization.
- **R3 — native/emit lanes:** storage emission, `__madc_sys_init`
  injection (before `__madc_global_init`), `-shared`/`--project`
  variants.
- **R4 — tests + docs:** `testsysobject` (argv/path/platform/version/
  hostname across JIT + `--exe` + `--emit=c11`), `testsysreadonly`
  (compile-error fixture for const member assignment; runtime-error
  fixture for frozen-cell mutation), `testsysargv` mutation (push onto
  `sys.argv`, iterate — proves mutable-and-safe), script-mode
  interaction test; language docs page; retire-`version`-variable note.

## R0 recon results (2026-07-22)

- **(a) Array struct members were broken at three layers** (reducers
  `tmp/sys_r0_structarray.mad`, `tmp/sys_r0_subscript.mad`):
  1. *Layout:* `member_node` had no array-object arm — `array a;` inside a
     struct emitted `int a;` (4 bytes of a 32-byte madc::value).
  2. *Construction:* the two member-construction loops
     (`append_member_default_constructs` for the implicit default ctor,
     `class_member_construct` for user-ctor prologues) skip non-class
     members, so the member was never placement-new'd
     ("value is instance, not an array" garbage-kind reads).
  3. *Destruction:* `class_member_destruct` / `class_needs_dtor` likewise —
     no `madarray_destruct`, refcounted cells would leak.
  **Fixed as a general language fix** (not a sys special): the existing
  object-member arms gained `is_array_object` siblings — the same shape
  locals already use — plus a shared `array_member_runtime_call` helper.
  Seven touch points in `cir_builder.cpp`, all completing one existing
  mechanism; no parallel path.
  **Decision (documented per owner meta-directive):** extend the
  established runtime-object arms rather than remodel `array` as a
  `DataDefCLASS` — `as_user_class` deliberately excludes dtARRAY
  (rawtype != dtRESERVED), and the full "array is a real header-defined
  class" remodel is the separate legacy-shortcut retirement campaign.
  **Sibling defect found and fixed (same session):** subscript on a madc
  array was wholly unmodeled — `a[i]` lowered to a raw word read of the
  value object's long[] buffer (silent garbage for `long x = a[1]`; a
  bogus in-module ctor-overload import for `string s = a[0]`).
  **Decisions (documented per owner meta-directive):**
  - Element READS are typed **string-first** (the Python/PHP element
    model — `sys.argv`/`sys.path` are string lists; every element kind
    has a string rendering via `__php_array_get_cstr`). Lowering reuses
    the range-for element-fill model: scope-lived string temp +
    `operator=(char*)` fill; `__php_array_get_int` when no string class
    is known (string-less TUs keep numeric reads).
  - Numeric reads under the string typing are a LOUD type error, never a
    silent raw-buffer read; range-for and php:: getters cover mixed
    numeric access. Value-typed element expressions arrive with the
    value-ABI campaign, not here.
  - Subscript WRITES (`a[i] = v`) stay unmodeled in v1 — `php::array_push`
    and the mutator family cover the campaign's mutation story (the
    `testsysargv` test is push-based); a loud diagnostic beats a wrong
    store. Follow-on with the value-ABI.
- **(b) Native lane:** `madarray_*` / `__ns_php_*` host symbols already
  resolve in native ELFs (testphp/testforeach/testlang run un-skipped in
  the green `--exe` lane) via `need_output_extern` + the existing import
  machinery. `sys` rides exactly that; only `__madc_sys_init` storage and
  injection are new (R3).
- **(c) Mutation census:** array/string writes funnel through a countable
  set — `madarray_construct/destruct/size` (madc_mir_backend.cpp), the
  `__ns_php_*` array mutators, `__php_array_get*` readers, and
  basic_string mangled-direct stores. The R1 frozen guard is a handful of
  checks at the madc::value mutation entry points, not a scatter.

## Non-goals (v1)

- `sys.env` / `executable` / cwd (future members, append when needed).
- Runtime `#include` re-resolution from `sys.path` mutations (only
  future `#load`/eval honor them — the Python one-way parallel).
- Retiring the legacy `version` variable (owner's schedule).

## Validation

`make -C src fulltest` + packed arbiter; `bash scripts/run_tests.sh
--exe` (sys storage + init must be native-clean); `--emit=c11` output
compiles under gcc (emit-C oracle) for a sys-using TU.
