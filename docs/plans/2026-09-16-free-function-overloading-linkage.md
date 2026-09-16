# C++ Symbol Mangling & Overloading per `--std=` — Design

> Scope decision resolved to **(c)** (owner, 2026-09-16): in c++/madc mode ALL user-defined
> C++ symbols mangle Itanium so a madc-compiled `.o`/`.so` is ABI-identical to g++/clang.
> The free-function slice (the darwin blocker) is phase 1 of that; it is NOT the whole feature.
> Retitled from "Free-Function…" accordingly.

**Status:** DRAFT for owner review (2026-09-16). Design only — implementation is a
separate focused session (owner sequencing: draft here, implement fresh).
**Owner:** Claude (design) → fresh session (implementation).
**Motivating bug:** darwin-suite blocker #2 (see
`docs/plans/2026-09-16-darwin-suite-blockers-handoff.md`).
**Rules in force:** #1 (gcc/clang is canon), #2 (deepest layer), #7 (no hard-coding
specifics), `enum-over-strings`, `parse-once`, `cpp-first-api`, `mc11-ir`.

---

## 1. Problem

madc emits **every free function under its bare source identifier in all modes**. The
emit-symbol precedence (`CirBuilder::call_emit_symbol`, `src/cir_builder.cpp:357-364`) is
`emit_symbol → local_emit_name → var_emit_name`, and a plain user global function sets
none of the first two, so it falls through to the bare `v.name`. A comment states the
intent outright (`src/parser.cpp:69672`): *"A PLAIN global function stays on the legacy
path… tracking it would mangle the import."*

Consequences:
- **No free-function overloading.** Two user `void foo(int)` + `void foo(const char*)`
  both emit as `foo` → `MIR fatal error: Repeated item declaration foo`.
- **User definitions collide with C-library prototypes.** Under `--std=madc` the darwin
  umbrella prelude declares POSIX `extern "C" int send(int,void*,size_t,int)` in global
  scope; when a program then defines `void send(madc::channel&, var&)`,
  `Program::parseFunction` reconciles the definition against the prototype **by position**
  and silently mistypes the first parameter (`channel&` → `int`). Every symptom
  downstream (`c.write` misresolving to POSIX `write` via UFCS) follows from that one
  mistyping. See §5.

This violates the owner ruling that madc's behavior must follow `--std=`: in C++/madc
mode gcc/clang **always** mangle a C++-linkage free function, so overloads are distinct
symbols and a user `send` never touches an `extern "C"` `send`.

## 2. The ruling (SETTLED — do not re-litigate)

Owner, 2026-09-16 (memory `feedback_std_determines_semantics`,
`project_free_function_overloading_gap`):

- **`--std=c##`** → C semantics. No overloading. A redefinition with a genuinely different
  signature is a **hard parse-time error** (matches gcc/clang: *"conflicting types for 'f'"*).
- **`--std=c++##` / `--std=madc`** → C++ semantics. **Every user-defined C++ symbol mangles
  Itanium** — free functions, class members, operators, ctors, dtors, namespace functions —
  so overloads coexist as distinct linker symbols selected at the call site by argument type,
  AND a madc-compiled `.o`/`.so` is **ABI-identical to g++/clang**. Default mode is `STD_MADC`.
- **`extern "C"` opts out** of mangling (bare symbol) and therefore cannot overload — the
  only exception, exactly as in C++. `main` is bare too (entry point, never mangled).
- **Rule #1 governs: STRICT mangling, no heuristic, no partial scope.** Two schemes were
  proposed and REJECTED: "mangle-only-on-collision" (diverges from gcc/clang), and "(b) free
  functions only, leave user members on the internal `__oN` scheme" — the latter emits
  `Foo__bar` where g++ emits `_ZN3Foo3barEv`, so a `--std=c++` object would not link against
  real C++: "C++ mode" that isn't ABI-compatible is itself the Rule #1 violation. Every
  non-`extern "C"`, non-`main` user C++ function mangles, unique or not. Blast radius is an
  implementation/migration concern, not a semantics knob.
- **The internal `Class__method__oN` / `__ns__oN` scheme is RETIRED in c++/madc mode.** Itanium
  encodes param types into the symbol, so it subsumes the overload disambiguation `__oN`
  provided; the SAME `_sub` encoders that bind libstdc++ today emit user symbols too — one
  mangling path (Rule #7). `__oN`/internal names may survive only for genuinely madc-internal,
  non-user-facing symbols (compiler-emitted runtime machinery), never for a symbol a g++
  program could name.

This is consistent with `cpp-first-api`: `extern "C"` is *already* the C-host boundary, and
namespace publics *already* "resolve mangled-direct." (c) applies that Itanium line uniformly
to EVERY user C++ symbol, replacing the current split (Itanium for library binding, internal
`__oN` for user-bodied) that made "C++ mode" quietly non-ABI-compatible.

## 3. Current state (verified — `file:line`)

| Concern | Where | State |
|---|---|---|
| Emit-symbol resolution | `cir_builder.cpp:357-364` (`call_emit_symbol`), `4997-5034` (`call_target_*`), call sites `7318`,`8463`,`5504` | Centralized: one precedence chain feeds the MIR import/call name. **Helps.** |
| Itanium encoder (the core) | `src/madc_mangle.cpp:17` (`source_name`), `123` (`itanium_encode_type`: P/R/K + `builtin_code`), `154` (`itanium_encode_params`) | **Load-bearing in production**: the *matching* half binds every `std::` free function (`std::getline`/`stoi`/free operators…) against libstdc++/libc++'s real mangled exports — without it those are undefined-symbol link errors. Comments cite real Rule-#1 symbol-match fixes. **Helps — proven.** |
| Fresh-symbol MINTER | `src/madc_mangle.cpp:163` (`itanium_mangle`), unit test `tests/unit/test_mangle.cpp:219`; input `FuncDef::param_cpp_spellings` (`madc.h:178`, filled `parser.cpp:65915`) | The *minting* half is the dormant part: complete for simple/builtin params, but its naive fallback `source_name("madc::channel")`→`13madc::channel` is **not** strict Itanium for a namespaced type (should be `N4madc7channelE`). Must route through the nested-name-aware path (`itanium_mangle_nested_sub`, `parser.cpp:2904`) the `std::` matcher already uses — one encoder, not a fork. **Helps with a gap to close.** |
| Mangled-direct (bind existing lib symbols) | `parser.cpp:2904-2937`, `namespace_cpp_function_symbol` (only when `declaration_only`) | Matches *external* libstdc++ symbols via the nested-name-aware encoder. The minter (above) must produce byte-identical encodings so a user function and a `std::` function of the same signature mangle the same way. |
| Library (declaration-only) member/ctor/operator symbols | `parser.cpp:45342-45351` → `itanium_mangle_member_sub`/`_ctor_sub`/`_dtor_sub`/`_operator_sub`; comment `parser.cpp:54725` | **Already real Itanium** — this is how `std::string::c_str()` binds libstdc++'s `_ZNKSt7__cxx11…` symbol. Library binding was never on the internal scheme. |
| Internal overload disambiguator (USER-BODIED only) | `unique_overload_symbol` (`parser.cpp:45554`) → `base__oN` | Non-Itanium. Used for USER-bodied member/operator/namespace overloads (self-consistent; no external symbol to match, so they already overload correctly). NOT used for library members (above) or user global free functions (which get nothing → bare). See §4.1 open decision. |
| `extern "C"` linkage | `LinkageSpec`/`current_linkage` (`madc.h:4856`), `TokenEXTERN::parse` (`parser.cpp:51933`), `FuncDef::c_linkage` (`madc.h:609`) | `c_linkage` set in **exactly one place** (`parser.cpp:69826`, namespace branch only). **Fights: must generalize to global scope.** |
| Global overload set | `namespace_fn_overload_sets` keyed `"::name"` (`parser.cpp:69660-69753`) | Machinery exists at global scope; plain user functions excluded by the gate at `69660-69666`. **Helps: widen the gate.** |
| Call-site overload ranking | `resolved_call_funcdef` (`parser.cpp:56085`, parse-time) + `call_target_variable` (`cir_builder.cpp:4802-4996`, CIR-time, authoritative) → `find_namespace_function_overload`; throws "no matching overload"/"ambiguous" | Ranks by argument type, decays arrays/functions. **Helps.** |
| `funcdef_map` | `madc.h:1594,4162` — `map<string, FuncDef*>` | Name-keyed, one FuncDef per bare name. **Fights: an overload set needs >1.** |
| `parseFunction` reconciliation | `parser.cpp:65083-65163` (three branches) + per-param loop `65879-65891` | Blind, unconditional, position-based; the darwin mistyping lives here. No `is_c_mode()` gate. **Fights: make mode+signature aware.** |
| C-mode redefinition error | searched `parseFunction` 64983-66600 | **None exists.** Signature clash silently reconciles. **Fights: add the diagnostic.** |
| C-ABI gate | `scripts/check-c-abi-surface.sh` pipeline `grep -v '^_Z'` | Already excludes mangled symbols. **Neutral: no gate change.** |
| Module/dlsym interop | `dyn_module_member` (`parser.cpp:28042`, lowered `cir_builder.cpp:28891`), dlsym fallback (`cir_builder.cpp:392`) | Bare-name lookups against real library exports — **must stay bare** (already do; they never route through the free-function symbol minter). |
| `--std=` predicates | `is_c_mode` (`madc.h:4981`), `is_cpp_mode` (`4987`), `presents_as_cpp` (`5020`); default `STD_MADC` (`parser.cpp:21793,21835`) | The gate for every mode decision below. |

**Bottom line:** the mangler, the overload sets, the ranking, and the diagnostics all
already exist and already work at global scope for operators/templates/system-headers.
The feature is *wiring + gating*, not new machinery — plus one genuine bug fix
(`parseFunction` reconciliation) and one new diagnostic (C-mode clash).

## 4. Design

### 4.1 When a free function mangles

A free function's emit symbol is its **Itanium mangling** iff **all** hold:
- `presents_as_cpp()` is true (C++ or madc mode); and
- it is **not** `extern "C"` (`FuncDef::c_linkage == false`, once §4.3 generalizes it); and
- it is not `main`; and
- it has a madc body (a declaration-only extern binding keeps the mangled-direct/`emit_symbol`
  path it already has).

Otherwise it stays bare (C mode, `extern "C"`, `main`, and — unchanged — module/dlsym
targets which never reach this path). Both the definition and every call site route
through `call_emit_symbol`, so setting the FuncDef's symbol once is self-consistent across
TUs (both sides mangle identically).

**One encoder (Rule #7 / `no-parallel-implementations`).** The symbol is minted through the
SAME Itanium encoder the `std::` mangled-direct matcher uses (`madc_mangle.cpp` core +
`itanium_mangle_nested_sub`), never a second path — so a user `foo(int)` and `std::` functions
encode consistently, and a user function is byte-identical to what gcc/clang would emit (Rule
#1, and it keeps the door open for the C++-emit transpiler vision). Self-consistency alone
would let a sloppy encoding "work" internally, but Rule #1 requires the real symbol. Two
encoder gaps to CLOSE before minting user symbols (verify against the g++/clang oracle): (a)
namespaced param types need `N…E` nested-name encoding, not the naive length-prefix fallback
(§3); (b) the documented non-type-template-argument gap (`madc_mangle.cpp:40-58`: an integer
NTTP can't be encoded from spelling alone) — **out of scope** here, as free-function overloads
in the motivating cases carry class/builtin params, not NTTPs; note it so the implementer
doesn't trip on it.

**Scope = (c) (resolved).** *Library* (declaration-only) members and free functions are ALREADY
real Itanium (`itanium_mangle_*_sub`, §3). This design brings EVERY *user-bodied* C++ function
onto the SAME Itanium encoders: user members/operators/ctors/dtors move off `Class__method__oN`,
user namespace functions off `__ns__oN`, user global free functions off bare — all → Itanium in
c++/madc mode. Both the definition and every call site route through `call_emit_symbol`
(§3)/the member emit path, so switching the emit symbol at the assignment site keeps def and
calls consistent. Rejected alternative (b) (free functions only) left user members emitting
`Foo__bar` — non-ABI-compatible with g++, a Rule #1 violation (§2).

### 4.2 Overload registration at global scope

Widen the gate at `parser.cpp:69660-69666` so an ordinary user global function (madc body,
C++-linkage, C++/madc mode) is entered into `namespace_fn_overload_sets["::" + name]` just
like a global operator is today. The set already drives call-site ranking
(`call_target_variable`) and the ambiguity/no-match diagnostics — no new resolution code.
Registration assigns each overload its Itanium symbol (§4.1) as `local_emit_name`/`emit_symbol`
via the existing `>= 2` binding step (`69860-69872`), generalized to `>= 1` in C++/madc mode
(a single C++ free function still mangles — strict ruling).

### 4.3 `extern "C"` at global scope

Generalize `FuncDef::c_linkage`: set it whenever `current_linkage == LinkageSpec::C` at the
point a free function is declared/defined (not only in the namespace branch). Then §4.1 reads
one flag uniformly. `extern "C"` functions stay bare, are excluded from
`namespace_fn_overload_sets` (they cannot overload), and a second `extern "C"` declaration of
the same signature reconciles as today. `main` is bare by name test at the same site (the one
legitimate name-keyed special case, mirroring gcc/clang).

### 4.4 `parseFunction` reconciliation — make it mode + signature aware

This is the defect (§5). Replace the blind reconciliation with:
- **Same signature** as an existing prototype (forward-decl → definition): reconcile as today
  (all modes). This is the common, correct case and must keep working (K&R-empty and
  return-type-refresh branches stay).
- **Different signature**, `presents_as_cpp()`: it is a **distinct overload** — mint a fresh
  FuncDef with the definition's *own* parameters (never copy the prototype's), register it in
  the global overload set (§4.2), give it its Itanium symbol. Never merge into the prototype's
  FuncDef.
- **Different signature**, `is_c_mode()`: **hard error** at the definition token —
  *"conflicting types for '<name>'"* (matches gcc/clang; §4.5). No silent reconcile.

"Same signature" = same arity and same parameter DataDefs after decay (reuse the argument-type
comparison the overload ranker already uses; do not hand-roll a new comparison — Rule #7).

### 4.5 C-mode signature-clash diagnostic

Add the `is_c_mode()` hard error in §4.4. Oracle it against gcc/clang exact wording where
practical. This also closes the *silent wrong-answer* class the current reconciliation can
produce even on non-darwin C programs (`fix-what-you-find`).

## 5. Why this fixes the darwin bug (acceptance)

Under `--std=madc`: the umbrella's POSIX `send` is `extern "C"` → bare `send` (§4.3, excluded
from overloading). The user's `void send(madc::channel&, var&)` has a different signature →
§4.4 mints a fresh FuncDef with the user's own params (no mistyping) and an Itanium symbol
`_Z4sendRN4madc7channelER...`. `send(c, m)` ranks the global set and binds the user's send
(the POSIX bare `send` is not in the set). `c` is correctly typed `channel&`, so `c.write`
binds `madc::channel::write` — 2a evaporates because it was only ever a symptom of the
mistyping. Blocker #3 (prelude `extern "C"` wrap) is orthogonal and separately fixes `sqrt`.

Verified expectation vs the canon: `clang++-18 -isysroot <macos-sdk> -std=c++20 -fsyntax-only`
accepts both constructs (rc=0). Reproduce locally without a Mac via
`reference_cross_darwin_repro` (cross-darwin madc on the container).

## 6. Interop & migration (what must stay bare)

- `extern "C"` exports (the C-host libmadc API) and `main` — bare by §4.1/§4.3.
- `dyn_module_member` (module `import`) — bare lookup against the real library export table;
  never routes through the free-function minter. Unchanged.
- Undeclared-function dlsym fallback (`cir_builder.cpp:392`) — targets external C symbols by
  bare name; a user's *own* function always has a FuncDef and mangles, so the fallback set is
  unaffected. Unchanged.
- Forest freeze/restore of the linkage flag (`DF_FUNC_C_LINKAGE`, `madc_cir.cpp:3282`,
  `cir_freeze.cpp:3750`) — must serialize the generalized `c_linkage` so a restored function
  keeps bare/mangled parity. In scope.
- `scripts/check-c-abi-surface.sh` — already excludes `_Z…`; no change, but the migration must
  confirm every symbol that *stays* bare is intentionally `extern "C"`/`main`/module-interop.

## 7. Blast radius & risks

- **EVERY user C++ symbol changes in c++/madc mode** (free + member + operator + ctor + dtor +
  namespace) — the `__oN` scheme is retired (§2). This is the (c) scope's real cost. Mitigated
  by centralized emit (§3, `call_emit_symbol` + the member emit path) — def and calls move
  together — but AOT/`--exe`/`--obj`, the forest pack, headerless, and every cross-TU project
  (SMAUG) exercise the symbols and MUST be in the merge-wave battery. madc-to-madc linking is
  preserved (both sides mangle identically); the NEW capability is madc-to-g++/clang linking.
- **Encoder coverage — lower risk than it first looks.** The `_sub` encoders
  (`itanium_mangle_member_sub`, `nested_sub`, …) already mangle the HARDEST symbols in
  existence — `std::__cxx11::basic_string<char, char_traits<char>, allocator<char>>`, nested
  template instantiations, ABI tags, substitution compression — because `std::` binding fails
  to *link* otherwise (and it doesn't). Arbitrary USER types (a `class Foo`, `send(channel&,
  var&)`) are strictly simpler and are largely already covered. So phase 0 is a VERIFICATION
  harness (`g++ -c` → `nm`, assert byte-equal over a user-shape corpus + the known `_sub`
  cases), not building the mangler — expect small gap-fills (a construct no `std::` binding
  happened to exercise) rather than a rewrite. Any mismatch is still a Rule #1 defect to fix.
  Known documented gap, OUT of scope: integer non-type template args from spelling alone
  (`madc_mangle.cpp:40-58`) — user overloads in the motivating cases carry class/builtin params.
- **The reconciliation change is a heavily-battered path.** Guard behind a `FEATURE_*` macro
  during bring-up (`feature-guards`); keep the same-signature forward-decl→def path byte-for-byte.
- **C-mode error** may surface latent duplicate declarations in existing C tests — triage each
  against gcc (some may be real bugs the silent reconcile hid).
- **`main` + special names** — enumerate vs gcc/clang (only `main` is bare; static/inline still
  mangle). madc-internal compiler-emitted machinery (`__madc_*` runtime helpers) is `extern "C"`
  / internal by construction and stays as-is.

## 8. Test / validation plan

- Reducers in `tests/` with g++ AND clang++ oracle (`fix-what-you-find`):
  - free-function overloading by type (the `foo(int)`/`foo(const char*)` case) — C++/madc pass,
    C mode error;
  - user function vs `extern "C"` same-name prototype (the darwin `send` shape) — resolves to
    the user's, POSIX stays bare;
  - `extern "C"` cannot overload (two `extern "C"` different sigs → error, both modes);
  - `main` stays bare; a call across TUs binds the mangled symbol;
  - C-mode conflicting-types diagnostic wording vs gcc.
- **ABI interop (the (c) acceptance, Rule #1):** a symbol-equality harness — for a corpus of
  free/member/operator/ctor/dtor signatures, assert madc's emitted symbol == `g++ -c` / `nm`
  byte-for-byte; and a link test — a madc-compiled `.o` links against a g++-compiled TU that
  calls its functions, and vice versa. Wire it into `fulltest`.
- Acceptance: darwin-suite JIT back to 0 failures (the 4 madcide tests) via the cross-darwin
  repro after phase 1, then a real darwin-probe round-trip; full (c) validated by the ABI
  harness above.
- Merge-wave battery: full `fulltest` + `--exe` + `--obj` + packed + headerless, c-testsuite,
  wine64, macos build, libcxx, genuine-win, darwin-suite. gcc-torture re-verify (C-mode error
  must not regress class-(a)).

## 9. Implementation phases (for the fresh session's plan)

0. **Oracle harness FIRST.** Build a g++/clang mangling oracle (`g++ -c` a corpus of
   signatures — free/member/operator/ctor/dtor, builtins, pointers/refs/const, namespaces,
   nested classes, templates — `nm` the symbols) and make `itanium_*_sub` reproduce every one
   byte-identical. Close the encoder gaps (namespaced `N…E`, builtin codes, substitutions)
   here, before any emit-symbol is switched. This de-risks every later phase.
1. **Free functions** (unblocks the darwin gate) behind a `FEATURE_*` guard: Itanium emit
   symbol for a c++/madc, non-`extern "C"`, non-`main` free function; generalize where
   `c_linkage` is set; widen the global overload gate (§4.2) so plain user functions register;
   reconciliation mode/signature awareness (§4.4) + C-mode diagnostic (§4.5). Targeted tests:
   overloading by type, the `extern "C"` `send` shape, `extern "C"` can't overload, forward-decl
   → def still reconciles, C-mode conflict errors.
2. **Members / operators / ctors / dtors:** switch user-bodied member emit from
   `Class__method__oN` (`unique_overload_symbol`) to the Itanium `_sub` encoders; **retire
   `__oN` in c++/madc mode** (§2). Highest-volume change — every member call site moves.
   Targeted tests: member overloading, a madc `.o` linking against a g++ TU and vice versa.
3. **Namespace functions:** user-bodied namespace overloads off `__ns__oN` onto Itanium.
4. **Forest serialization** parity for the generalized linkage flag + the new symbols.
5. **Migration sweep + merge-wave battery** (§7, §8); darwin round-trip; add a madc↔g++
   link-interop gate to `fulltest`; remove the feature guard.

## 10. Open decisions for owner

1. **Scope — RESOLVED to (c)** (owner 2026-09-16): every user-defined C++ symbol mangles Itanium
   in c++/madc mode so a madc object is ABI-identical to g++/clang; `__oN` retired. Not reopened.
2. **Sequencing / gate boundary (the one still-open question):** phase 1 (free functions) alone
   closes the darwin-suite blocker, but phases 2–3 (members/operators/namespace) are what make
   c++/madc mode actually g++-ABI-compatible. **Does the master-promotion gate require the full
   (c) feature, or may the darwin gate close after phase 1 with phases 2–3 as fast-follow?**
   The darwin tests don't exercise madc↔g++ linking, so phase 1 makes them pass; but leaving
   phases 2–3 undone means "C++ mode" is still not ABI-compatible. Recommend: land the whole (c)
   feature as one merge wave (it's one coherent "retire `__oN`, one Itanium path" change) rather
   than shipping a half-migrated symbol scheme — but the darwin *lane* can be validated green
   after phase 1 to confirm the fix direction early.
