# Feature-Drops Audit — silently dropped / ignored / mis-lowered constructs

**Purpose.** A living ledger of language features that madc currently **eats,
ignores, drops, mis-parses, or mis-lowers** — surfaced incidentally while working
other tracks. Each entry is a future roadmap item. This is NOT the parity worklist
(`docs/parity/root-cause-worklist.md`, the torture-suite gate) nor the
container-cluster handoff — it is the catch-all for "noticed this is wrong, fixed
something else, must not forget."

**How to use.** When you trip over a construct madc gets wrong but it is OUT OF
SCOPE for the current fix, add a row here with a minimal repro instead of silently
moving on. When you later fix one, move it to "Resolved" with the commit. Keep
repros runnable (note the flags). 3-oracle (g++/clang) every claim.

Conventions: repros live in `tmp/` (gitignored) unless promoted to `tests/`.
"DROP" = silently accepted but wrong/no-op. "REJECT" = madc errors on valid code.
"MIS-LOWER" = compiles+runs but wrong behavior.

---

## Open

| # | Construct | Kind | Symptom | First seen | Repro |
|---|-----------|------|---------|-----------|-------|
| 1 | `delete[] p` (array delete) | REJECT (parse) | `[` after `delete` parsed as a lambda capture: `error: Expecting ( after lambda` | 2026-06-16 (vector<X> probe) | `struct W{int*b; ~W(){ delete[] b; }};` |
| 2 | `new int(i)` / scalar `new` of a primitive | REJECT (cir) | `cir error: new without a class type` — `new` only handles class types, not `new T(v)` for builtin T | 2026-06-16 | `int *p = new int(5);` |
| 3 | dead `_S_relocate` / `__relocate_a_1` memmove path | MIS-LOWER (warn) | 2 c2mir warnings at `stl_vector.h:428` ("incompatible argument type for pointer type parameter", "assigning pointer without cast to integer") in the bitwise-relocate path. Never EXECUTED for non-trivially-relocatable types (`_S_use_relocate()` is false), so benign today — but a pointer/int confusion that would bite a trivially-relocatable class | 2026-06-16 (testvector) | `tmp/vecanyX.mad` (warning printed, output correct) |
| 4 | concrete `enable_if<false,T>::type` | DROP (SFINAE) | wrongly ACCEPTED — `::type` resolves via the opaque-member fallback even though the (non-dependent) instantiation has no such member; g++/clang reject. Masks negative SFINAE. Harmless for push_back (overloads split on arg type) | 2026-06-15 (enable-if plan §6b) | see `docs/plans/2026-06-15-enable-if-alias-nontype-bool-fold-plan.md` §6b |
| 5 | global-`::`-scope free function template | REJECT | `template<class T> T idf(T x){return x;}` at global scope then `idf(5)`/`idf<int>(5)` → "use of undeclared identifier"; the SAME inside a `namespace` works | 2026-06-15 (enable-if plan §6) | `tmp/fc.cpp` (global, fails) vs `tmp/ns1.cpp` (namespaced, ok) |

## Resolved (kept for history — move here with the fixing commit)

| Construct | Was | Fixed by | Date |
|-----------|-----|----------|------|
| zero-parameter fn template called with explicit template args (`__check_constructible<V,T>()`) | undefined-import / never instantiated | `18e2212` | 2026-06-16 |
| non-type template arg naming a `const` integral **local** (`__uninitialized_copy<__can_memmove && __assignable>`) | not folded → int/string instantiations collapsed | `e9255b8` | 2026-06-16 |
| class alignment floored at 8 (`struct W{int a; ~W(){}}` sizeof 8 not 4) | hardcoded `maxalign = 8` in compute_layout | `4025d5d` | 2026-06-16 |
| temporary class object materialized once before a (non-compound) loop body | hoisted construct, reused/garbage each iteration | `9630c55` | 2026-06-16 |
