# Free-Function Linkage & Overloading per `--std=` — Design

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
- **`--std=c++##` / `--std=madc`** → C++ semantics. Free functions **mangle** (Itanium),
  so overloads coexist as distinct linker symbols and are selected at the call site by
  argument type. Default mode is `STD_MADC`.
- **`extern "C"` opts out** of mangling (bare symbol) and therefore cannot overload — the
  only exception, exactly as in C++. `main` is bare too (entry point, never mangled).
- **Rule #1 governs: STRICT mangling, no heuristic.** A "mangle-only-on-collision" scheme
  was proposed and REJECTED — it diverges from what gcc/clang actually do. Every
  non-`extern "C"`, non-`main` free function mangles in C++/madc mode, unique or not.
  Blast radius is an implementation/migration concern, not a semantics knob.

This is consistent with `cpp-first-api`: `extern "C"` is *already* defined as the C-host
boundary, and namespace publics *already* "resolve mangled-direct." Strict free-function
mangling applies that existing line uniformly instead of the current
everything-is-secretly-`extern "C"` behavior.

## 3. Current state (verified — `file:line`)

| Concern | Where | State |
|---|---|---|
| Emit-symbol resolution | `cir_builder.cpp:357-364` (`call_emit_symbol`), `4997-5034` (`call_target_*`), call sites `7318`,`8463`,`5504` | Centralized: one precedence chain feeds the MIR import/call name. **Helps.** |
| Itanium encoder (the core) | `src/madc_mangle.cpp:17` (`source_name`), `123` (`itanium_encode_type`: P/R/K + `builtin_code`), `154` (`itanium_encode_params`) | **Load-bearing in production**: the *matching* half binds every `std::` free function (`std::getline`/`stoi`/free operators…) against libstdc++/libc++'s real mangled exports — without it those are undefined-symbol link errors. Comments cite real Rule-#1 symbol-match fixes. **Helps — proven.** |
| Fresh-symbol MINTER | `src/madc_mangle.cpp:163` (`itanium_mangle`), unit test `tests/unit/test_mangle.cpp:219`; input `FuncDef::param_cpp_spellings` (`madc.h:178`, filled `parser.cpp:65915`) | The *minting* half is the dormant part: complete for simple/builtin params, but its naive fallback `source_name("madc::channel")`→`13madc::channel` is **not** strict Itanium for a namespaced type (should be `N4madc7channelE`). Must route through the nested-name-aware path (`itanium_mangle_nested_sub`, `parser.cpp:2904`) the `std::` matcher already uses — one encoder, not a fork. **Helps with a gap to close.** |
| Mangled-direct (bind existing lib symbols) | `parser.cpp:2904-2937`, `namespace_cpp_function_symbol` (only when `declaration_only`) | Matches *external* libstdc++ symbols via the nested-name-aware encoder. The minter (above) must produce byte-identical encodings so a user function and a `std::` function of the same signature mangle the same way. |
| Internal overload disambiguator | `unique_overload_symbol` (`parser.cpp:45554`) → `base__oN` | Non-Itanium. Used today for member/operator/namespace bodied overloads. See §4 open decision. |
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

**Open decision (flag for owner):** class-member and namespace bodied overloads currently
use the internal `__oN` scheme, not Itanium. This design mangles **free functions** (global
scope, incl. global operators) with Itanium and leaves members on `__oN`. Recommend keeping
members as-is for this feature (their calls are internally consistent and they are not the
ruling's target); unifying members onto Itanium is a clean follow-up, not a blocker. If you
want members unified now, it enlarges scope.

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

- **Every user free-function symbol changes in C++/madc mode** (strict ruling). Mitigated by
  centralized emit (§3) — both def and calls move together — but AOT/`--exe`/`--obj`, the
  forest pack, and any cross-TU project (SMAUG) must be in the merge-wave battery.
- **The reconciliation change is the highest-risk edit** (heavily-battered path). Guard behind
  a `FEATURE_*` macro during bring-up (`feature-guards`); keep the same-signature path
  byte-for-byte.
- **C-mode error** could surface latent duplicate declarations in existing C tests — triage
  each against gcc (some may be real bugs the silent reconcile hid).
- **`main` and other special names** — enumerate against gcc/clang (only `main` is special for
  linkage; static/inline still mangle).

## 8. Test / validation plan

- Reducers in `tests/` with g++ AND clang++ oracle (`fix-what-you-find`):
  - free-function overloading by type (the `foo(int)`/`foo(const char*)` case) — C++/madc pass,
    C mode error;
  - user function vs `extern "C"` same-name prototype (the darwin `send` shape) — resolves to
    the user's, POSIX stays bare;
  - `extern "C"` cannot overload (two `extern "C"` different sigs → error, both modes);
  - `main` stays bare; a call across TUs binds the mangled symbol;
  - C-mode conflicting-types diagnostic wording vs gcc.
- Acceptance: darwin-suite JIT back to 0 failures (the 4 madcide tests) via the cross-darwin
  repro, then a real darwin-probe round-trip.
- Merge-wave battery: full `fulltest` + `--exe` + `--obj` + packed + headerless, c-testsuite,
  wine64, macos build, libcxx, genuine-win, darwin-suite. gcc-torture re-verify (C-mode error
  must not regress class-(a)).

## 9. Implementation phases (for the fresh session's plan)

1. **Wire the mangler + `c_linkage` generalization** behind a feature guard: set the Itanium
   emit symbol for a C++/madc, non-`extern "C"`, non-`main` free function via the SAME encoder
   the `std::` matcher uses (§4.1); FIRST close the encoder gaps (namespaced `N…E`; verify
   builtin codes) against a g++/clang oracle table (`g++ -c` a set of signatures, `nm` the
   symbols, assert byte-equal). Generalize where `c_linkage` is set. Gate every decision on
   `presents_as_cpp()`/`is_c_mode()`. Targeted tests: single free function mangles to the exact
   g++ symbol; `extern "C"`/`main` stay bare.
2. **Widen the global overload gate** (§4.2) so plain user functions register; confirm
   call-site ranking + ambiguity/no-match diagnostics fire at global scope. Targeted tests:
   two-overload resolution.
3. **Reconciliation mode/signature awareness** (§4.4) + **C-mode diagnostic** (§4.5). Targeted
   tests: forward-decl→def still reconciles; different-signature → overload (C++/madc) / error
   (C).
4. **Forest serialization** parity for the generalized linkage flag.
5. **Migration sweep + merge-wave battery** (§7, §8); darwin round-trip; remove the feature
   guard.

## 10. Open decisions for owner

1. **Member/namespace unification (§4.1):** mangle free functions Itanium now, leave class
   members on `__oN` (recommended), or unify members onto Itanium in the same feature (larger)?
2. **Strictness confirmation:** the ruling is strict (every non-`extern "C"` free function
   mangles, unique or not). Confirmed — flagged only because it drives the whole blast radius.
   A unique free function that some external tool expects by bare name must be declared
   `extern "C"` (the correct fix).
