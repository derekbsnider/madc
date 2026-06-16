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
| 3 | dead `_S_relocate` / `__relocate_a_1` memmove path | MIS-LOWER (warn) | 2 c2mir warnings at `stl_vector.h:428` ("incompatible argument type for pointer type parameter", "assigning pointer without cast to integer") in the bitwise-relocate path. Never EXECUTED for non-trivially-relocatable types (`_S_use_relocate()` is false), so benign today — but a pointer/int confusion that would bite a trivially-relocatable class | 2026-06-16 (testvector) | `tmp/vecanyX.mad` (warning printed, output correct) |
| 4 | concrete `enable_if<false,T>::type` | DROP (SFINAE) | wrongly ACCEPTED — `::type` resolves via the opaque-member fallback even though the (non-dependent) instantiation has no such member; g++/clang reject. Masks negative SFINAE. Harmless for push_back (overloads split on arg type) | 2026-06-15 (enable-if plan §6b) | see `docs/plans/2026-06-15-enable-if-alias-nontype-bool-fold-plan.md` §6b |
| 5 | global-`::`-scope free function template | REJECT | `template<class T> T idf(T x){return x;}` at global scope then `idf(5)`/`idf<int>(5)` → "use of undeclared identifier"; the SAME inside a `namespace` works | 2026-06-15 (enable-if plan §6) | `tmp/fc.cpp` (global, fails) vs `tmp/ns1.cpp` (namespaced, ok) |
| 6b | `vector<T*>` (any pointer element) — emitted element type | MIS-LOWER / REJECT (c2mir) | With the crash (6, now resolved) gone, `std::vector<int*>` still fails to COMPILE: the pointer element type is not threaded through instantiation/emit. Symptoms on `tmp/vp1.mad`: `&a` → "lvalue required as unary & operand" + "using integer without cast for pointer type parameter" (push_back arg seen as integer, not `int*`); `*v[0]` → "invalid type argument of unary *" (v[0] not a pointer); plus the `stl_vector.h:428` `_S_relocate` pointer/int warnings (row 3). I.e. the `int*` element collapses to an integer through the allocator/`__uninitialized_*`/relocate chain. The `testvectorptr` `rebind<>` error ("Expecting a type argument to rebind<>") is the pointer-to-CLASS sibling. Separate, deeper than the token-lifetime crash — a pointer-element type-threading wall. | 2026-06-16 | `tmp/vp1.mad` (`vector<int*>`); `tests/testvectorptr.mad` (ptr-to-class) |

| 7 | `map<string,string>` / `set<string>` element access — `_M_valptr` | REJECT (parse) | After the `_Tp2` collision fix (Resolved below), `std::map<string,string>`/`std::set<string>` still fail: `error: no member named '_M_valptr'` at the `static_cast<_Link_type>(_M_node)->_M_valptr()` call in `<bits/stl_tree.h>` (~L278/359). `_Link_type` is the typedef `_Rb_tree_node<_Val>*` (the DERIVED node); the cast result appears to resolve to the BASE `_Rb_tree_node_base` (which has no `_M_valptr`), so the node value-accessor method isn't found. `map<string,int>` gets further (fails later at C++20 `contains`), so this is specific to the OBJECT-valued node path. The next shared wall for testmadc_ns + testsubscript. | 2026-06-16 | `tmp/tp2.cpp` (set<string>+map<string,string>); `tmp/map_ss.cpp` |
| 8 | `std::map::put(k,v)` used by tests | INVALID-TEST | `testcontainerdtor.mad:50` calls `dict.put(k,v)` — std::map has NO `put` (g++/clang reject: "has no member named 'put'"); it only worked under the retired shims. Needs a TEST rewrite to `dict[k]=v` / `dict.insert({k,v})`. Same invalid-test class as the memory-noted to_string(out,v)/for_each(2-arg)/std::empty cases. (testmap/testset use C++20 `.contains()` which IS valid — separate: needs `--std=c++20` and a madc `contains` impl.) | 2026-06-16 | `tests/testcontainerdtor.mad` |

| 9 | `new T[n]` / `delete[] p` for a CLASS T with a non-trivial ctor/dtor | DROP (partial) | Scalar/trivial `new[]`/`delete[]` work (row 1/2 resolved). For a class element: `new T[n]` allocates a single object (no per-element ctor loop, no Itanium array element-count COOKIE), and `delete[]` frees only (no per-element dtor — gated off to avoid running one wrong dtor). Needs a cookie-based array-new (store n before the buffer) + ctor/dtor loops. Trivial element types are correct today. | 2026-06-16 | `new W[3]` where `struct W{ W(); ~W(); }` |

## Resolved (kept for history — move here with the fixing commit)

| Construct | Was | Fixed by | Date |
|-----------|-----|----------|------|
| zero-parameter fn template called with explicit template args (`__check_constructible<V,T>()`) | undefined-import / never instantiated | `18e2212` | 2026-06-16 |
| non-type template arg naming a `const` integral **local** (`__uninitialized_copy<__can_memmove && __assignable>`) | not folded → int/string instantiations collapsed | `e9255b8` | 2026-06-16 |
| class alignment floored at 8 (`struct W{int a; ~W(){}}` sizeof 8 not 4) | hardcoded `maxalign = 8` in compute_layout | `4025d5d` | 2026-06-16 |
| temporary class object materialized once before a (non-compound) loop body | hoisted construct, reused/garbage each iteration | `9630c55` | 2026-06-16 |
| `vector<T*>` element access **CRASH** (was row 6): compile-time SIGSEGV in `std::forward` overload partial-ordering reading a freed `ft.decl[0]` | `TokenCppKeyword::clone()` returned `this` (the shared keyword_map prototype), so every `constexpr`/`sizeof`/… aliased one mutable object; a retained fn-template decl borrowed it and a sibling occurrence's delete freed it for all. Fix: leaf-class clone() returns `new TokenCppKeyword(str)` (de-alias); base `TokenKeyword::clone()` keeps `return this` for dispatch subtypes. fulltest 626/7 unchanged. The remaining vector<T*> *type* errors are row 6b. | `e3cc45a` | 2026-06-16 |
| nested `struct _Tp2` "already defined" instantiating `map<string,string>` after `set<string>` (was a fresh open row) | a nested struct in a class TEMPLATE (`__aligned_membuf<_Tp>`'s `struct _Tp2`) was registered under its BARE tag, so two monomorphizations collided. Fix: scope a nested struct tag by its (uniquely-named) enclosing class on bare-name collision. fulltest 626/7 unchanged; advances to row 7 (`_M_valptr`). | `b619c68` | 2026-06-16 |
| `delete[] p` (array delete) — was row 1: `[` after `delete` parsed as a lambda capture ("Expecting ( after lambda") | TokenDELETE::parse now recognizes the empty `[]` and sets is_array; CIR frees only for `delete[]` (no per-element dtor — array element-count cookie is a follow-up). | `31def57` | 2026-06-16 |
| scalar/array `new T` / `new T(v)` / `new T[n]` — was row 2: "new without a class type" (CIR only handled class types) | TokenNEW::parse captures the `[n]` array size; CIR lowers `new T[n]`→`(T*)calloc(n,sizeof(T))`, `new T`/`new T(v)`→`malloc`+optional store. +tests/testnewdelete.mad (627/7). | `31def57` | 2026-06-16 |
