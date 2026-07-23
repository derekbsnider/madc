# CODEX HANDOFF — Kind 3 (dependent member body): instantiate `allocator_traits::destroy`'s nested member call so it HITS

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-28.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `fef9b3a0` · gates GREEN (flag-on + flag-off 670/0/0/18; both drift gates GREEN).
**Strategic plan:** `docs/plans/2026-06-27-two-tree-end-state-and-reparse-deprecation.md` (parse-once, generic, g++ model — 4 KINDS, no per-shape/per-name catalog).
**Prior rounds (LANDED & VERIFIED — build on, do NOT redo):** `c588b182` (re-resolve copied dependent calls + materialize retained free-operator templates + requeue ODR-used bodies), `fef9b3a0` (Pass 3: scalar-returning free operator on class operands → `_M_realloc_insert` HITS).

---

## ⚡ START HERE — read this whole file, then execute §TASK. The gap is pinned to ONE bail site. Do NOT re-derive; do NOT widen scope.

### `/goal` (the contract — falsifiable end-state)
`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats tests/testvector.mad` exits 0 with **`std::allocator_traits::destroy<_Up>` GONE from the `[why:]` fallback list** (it tsubst-HITS), vector engagement **≥13 hit / ≤2 fallback** (up from 12/3), AND the gate holds: flag-off `make -C src fulltest` = **670/0/0/18** + `scripts/check-no-std-hardcoding.sh` GREEN, flag-on `run_tests.sh` = **670/0/0/18**, torture byte-identical. No callee/member-NAME hardcoding (Rule #7).

### THE MISSION (one sentence)
Make `std::allocator_traits<std::allocator<T>>::destroy<_Up>` tsubst-**HIT** by making its **nested dependent member call** (`__a.destroy(__p)`) actually instantiate an emittable body — Kind 3 (dependent member body) of the parse-once spine — instead of bailing to a re-parse fallback.

---

## THE EXACT GAP (evidence — do NOT re-investigate)

`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats tests/testvector.mad` fallback profile (HEAD `fef9b3a0`):
```
12 hit / 3 fallback
  2x std::allocator_traits::destroy<_Up>        [why: tsubst: unresolved dependent member body]   <-- YOUR TARGET
  1x std::__cxx11::basic_string::_M_construct    [why: template-id '<' in body]                     (OUT OF SCOPE — next round)
```

**The bail site (THE fix site):** `cir_builder.cpp:1050`, inside `resolve_copied_dependent_call` — the **member-call branch** (`~1003`–`1055`). This is the SAME function that already handles Kind 1 (the free-function branch below it, `~1058`+, landed in `1d69ee40`). The member-call branch:
1. Resolves the dependent member call → `recv_class->findMethodOverload(mname, mat)` → `winner` (`~1012`). **This succeeds** (the winner is found).
2. If the winner is a member template + `declaration_only`, calls `m_prog->instantiate_member_fn_template_for_call(&synth)` (`~1033`).
3. Checks body availability (`~1036`–`1048`): deferred-lazy body? external symbol? a non-decl-only entry in `pending_funcs`?
4. **Still not available → bail `"tsubst: unresolved dependent member body"` (`1050`–`1053`).**

So step 2's `instantiate_member_fn_template_for_call` is **NOT leaving an emittable body** for this nested member-template call. That is the bug.

**The body chain (CONFIRMED — madc default `__cplusplus == 201703`, i.e. C++17):**
`allocator_traits<allocator<T>>::destroy<_Up>(allocator_type& __a, _Up* __p)` body (`/usr/include/c++/13/bits/alloc_traits.h:554`) is — under `#if __cplusplus <= 201703L` —
```cpp
__a.destroy(__p);          // a DEPENDENT MEMBER CALL on allocator_type& __a
```
which resolves to `std::allocator<T>::destroy<_Up>(_Up* __p)` whose body is the **pseudo-destructor call** `__p->~_Up();`. So the inner thing to instantiate is `allocator<T>::destroy<_Up>` (a member template), and its body is a pseudo-destructor on a (often trivial) concrete `_Up`.

MIR undefined-import dump (prior-round evidence) named the symbol: `allocator_traits_std__allocator_int32_t___destroy__mti` (and a `string` variant) — i.e. `_Up = int` and `_Up = basic_string`.

---

## YOUR TOOLS (all landed — use them)
- **MIR undefined-import dump** — `cir_dump_undefined_imports` (`madc_cir.cpp:103`, auto-fires on link failure): names every missing symbol UNTRUNCATED. After you get `destroy` to stop bailing, the dump tells you whether the NESTED `allocator::destroy` (or the pseudo-destructor) is still missing.
- **THE MODEL is in the same function.** `resolve_copied_dependent_call`'s **free-function branch** (`cir_builder.cpp:~1058`+, the Kind-1 win `1d69ee40`) already does: re-resolve on concrete args → instantiate-on-miss → post-resolve body-availability check → return winner on success / bail on miss. The member-call branch (`~1003`–`1055`) is the SAME shape but its instantiate-on-miss (`instantiate_member_fn_template_for_call`) doesn't produce a body here. Make the member branch as complete as the free branch.
- **`instantiate_member_fn_template_for_call`** — `parser.cpp:33889` (called at `cir_builder.cpp:1033`, and at `parser.cpp:10060/10223/13614`). This is the instantiator that must leave a `pending_funcs` entry with `!declaration_only` for the resolved member template. Trace why it declines/leaves-decl-only for `allocator::destroy<_Up>` (likely: it doesn't recurse into the nested `__a.destroy(__p)` call, OR the pseudo-destructor `__p->~_Up()` body isn't materialized for a trivial `_Up`).
- **Round-1/2 foundation** (completeness check `bail_restore` `~13680`, ODR-use recording `cir_collect_call_callees` `~15966`): once `destroy`'s body becomes emittable, the completeness check stops bailing the outer body and it becomes a HIT. No edit needed there.

---

## THE TASK — one KIND, gated commit

**Slice — make the member-call branch instantiate the nested dependent member body.**
1. In `resolve_copied_dependent_call`'s member-call branch (`cir_builder.cpp:~1019`–`1054`): after `instantiate_member_fn_template_for_call(&synth)`, the resolved `allocator::destroy<_Up>` (the body `__a.destroy(__p)` calls) must itself be instantiated with an emittable body — recurse the instantiate-on-miss into the nested dependent member call exactly as the free-function branch does for nested free calls.
2. If the residual gap is the **pseudo-destructor `__p->~_Up()`** for a concrete `_Up`: for a trivial `_Up` (e.g. `int`) it lowers to a no-op; for a non-trivial `_Up` (e.g. `basic_string`) it lowers to the real dtor call. Make the member-template body materialize that lowering — do NOT special-case the type by name; key on `is-trivially-destructible` / the existing dtor-lowering machinery.
3. Reuse the existing instantiation path (`instantiate_member_fn_template_for_call` / the Kind-1 free path) — do NOT add a parallel member-instantiation path (Rule #7 / no-parallel-impl). If `instantiate_member_fn_template_for_call` needs to recurse or emit a body it currently skips, fix it THERE.

**DONE for this slice:** `std::allocator_traits::destroy<_Up>` is GONE from testvector's `[why:]` (HITS), vector ≥13 hit, all 4 container tests exit 0. **OUT OF SCOPE:** `basic_string::_M_construct` (`[why: template-id '<' in body]` — a different KIND, next round). One KIND, one gated commit.

---

## GATE (every commit — correctness only, NEVER perf-gate)
1. `make -j4 -C src` clean, no new warnings.
2. flag-off `make -C src fulltest` → **670/0/0/18** + drift gates green (flag-off byte-identical by the `getenv` gate — production unchanged).
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → **670/0/0/18**.
4. `bash scripts/check-no-std-hardcoding.sh` → GREEN (key on KIND — dependent member call needing instantiation, trivial-vs-nontrivial dtor — never on `"destroy"`/`"allocator_traits"`/mangled literals).
5. `--show-stats` engagement UP on testvector (≥13 hit), never down; `allocator_traits::destroy` gone from `[why:]`.
6. gcc.c-torture byte-identical (flag-on-only change → by construction; spot-check).

---

## SETTLED — DO NOT RE-LITIGATE
- **Parse-once, generic (g++) is the direction.** Fix by KIND (dependent member body), never by callee/member name. A fix that greps for `destroy` or `allocator_traits` is WRONG (Rule #7).
- The bail site is `cir_builder.cpp:1050`; the fix is making the member-call branch's instantiate-on-miss as complete as the free-function branch (same function, just below). Do NOT remove or fork the round-1/2 machinery, the completeness check, the construction/operator guards, or the env-gate.
- The env-gate (`MADC_XTEST_DEP_PARSE`) keeps flag-off byte-identical — every change is production-safe by construction.
- Scope is ONE KIND. Do NOT also chase `basic_string::_M_construct` (template-id-in-body — separate KIND). One gated commit, one KIND.

## FILE:LINE MAP (HEAD fef9b3a0 — verify, they drift)
- **FIX SITE:** `resolve_copied_dependent_call` member-call branch `cir_builder.cpp:~1003`–`1055`; the bail at `1050`. The MODEL is the free-function branch in the same function `~1058`+ (Kind-1, `1d69ee40`).
- member-template instantiator: `Program::instantiate_member_fn_template_for_call` `parser.cpp:33889` (called `cir_builder.cpp:1033`).
- completeness / ODR-use (no edit needed): `bail_restore` `~13680`, `cir_collect_call_callees` `~15966`.
- MIR dump: `cir_dump_undefined_imports` `madc_cir.cpp:103`.
- real body to study: `/usr/include/c++/13/bits/alloc_traits.h:551`–`562` (`destroy<_Up>` → `__a.destroy(__p)`), and `std::allocator<T>::destroy` (`bits/allocator.h` / `ext/new_allocator.h`).

## HANDBACK
When `allocator_traits::destroy` hits (or you wall): one paragraph — engagement before/after (testvector hit/fallback), gate results, and if walled, the dump output + which instantiation step (member-template instantiate / nested call / pseudo-destructor lowering) declined. Claude verifies and queues the last vector KIND (`basic_string::_M_construct`).
