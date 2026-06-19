# Map instantiation strategy — research-driven reset (2026-06-19)

Result of a recon pass (madc engine + clang Sema + GCC cp/ + libstdc++-13 +
literature) on *how* to finally compile+run `std::map<int,int>; m[1]=2`. The
headline: **the remaining work is much smaller and shallower than the
layer-by-layer grind implied — because `map<int,int>` only ever hits the trivial
base cases of the tuple machinery (recursion depth ≤ 1), and the one genuinely
new primitive is a single ~50-line substitution-time builtin (`__integer_pack`).**

## The hypothesis that was WRONG (and the correction)

Hypothesis going in: implement clang's `__make_integer_seq` / `__type_pack_element`
builtins to sidestep recursive template instantiation.

Correction (grep-verified against /usr/include/c++/13):
- libstdc++-13 uses those builtins ONLY under `#if __has_builtin(__make_integer_seq)`
  — the **Clang** path. madc is on the **GCC** path (`__has_builtin` is false), where:
  - `make_index_sequence` / `_Build_index_tuple` use **`__integer_pack(N)`** (GCC builtin).
  - `tuple_element` uses the recursive **`_Nth_type`** (NO `__type_pack_element` anywhere
    in c++/13 — `__type_pack_element` would be dead code here).
- So the live lever is **`__integer_pack`**, and `__type_pack_element`/`__make_integer_seq`
  are not on the critical path for these headers (implement `__type_pack_element`
  later only for forward-compat with newer libstdc++/libc++).

## The decisive finding: `map<int,int>` hits only depth-≤1 base cases

`map<int,int>::operator[]` piecewise-constructs `pair<const int, int>` from
`tuple<const int&>` (the key) and `tuple<>` (empty, for the mapped value). Tracing
every entity (GCC cp/ + libstdc++-13 line refs in the recon), the depths are:

- `_Build_index_tuple<1>::__type` → `_Index_tuple<__integer_pack(1)...>` → `_Index_tuple<0>`;
  `_Build_index_tuple<0>` → `_Index_tuple<>`. **Non-recursive** (one-shot). Needs `__integer_pack`.
- `__integer_pack(N)` called only with **N ∈ {0,1}** → packs `[]` and `[0]`. Non-dependent.
- `tuple_element<0, tuple<const int&>>::type` → `_Nth_type<0, const int&>::type` = `const int&`.
  `_Nth_type` has **explicit specializations for 0/1/2** — I=0 is **depth 0** (the `_Np-3`
  recursion only starts at I≥3, never reached by map).
- `__tuple_element_t<0, tuple<const int&>>` = the alias → that `::type` (depth 0).
- `_Tuple_impl<0, const int&>` = the **terminal single-element spec** (tuple:489) — no
  recursive `_Inherited` base layer. `get<0>`→`__get_helper<0>`→base-convert→`_M_head`→
  `_Head_base<0, const int&>::_M_head_impl`. **Zero tail walks.**
- Indexed ctor `first(forward<_Args1>(get<_Indexes1>(t1))...)` with `_Args1={const int&}`,
  `_Indexes1={0}` (and `_Args2/_Indexes2` empty): the pack expansion is **a single element**
  — no real lockstep multi-element expansion needed; `second(...)` is an **empty** expansion.

So the feared "deep recursive instantiation + N-way parallel pack expansion +
non-type packs" reduces, for `map<int,int>`, to **single-step specialization
matching madc already does for vector, plus one new builtin and 0/1-element packs.**

## Minimal worklist to a RUNNABLE `map<int,int>` (re-scoped, ordered)

1. **W1 — `__integer_pack(N)` builtin.** A substitution-time pack producer, valid
   ONLY as the entire pattern of a pack expansion (`X<__integer_pack(N)...>`). When N
   folds to a concrete `len ≥ 0`, the expansion yields the constant pack `[0..len-1]`
   (size_t). For map, N ∈ {0,1}. Model: GCC `cp/pt.cc:3846-3912` (`expand_integer_pack`
   builds a vec of `size_int(i)`), recognized via `cp/pt.cc:33231` (`declare_integer_pack`),
   dispatched in `tsubst_pack_expansion` (`cp/pt.cc:14191`). In madc: recognize the call
   `__integer_pack(<const-int>)` inside a template-arg pack-expansion (`X<… …>`) and emit
   the integer-constant pack. This is the only entirely-new front-end primitive.

2. **W2 — non-type parameter packs (`size_t... _Indexes`) at 0/1 elements.** The fn-template
   gate currently bails on a non-type pack. Extend the non-type-param support (already
   landed for scalars: `nontype_subst`/`nontype_params`) + the 0/1-element tid-pack
   handling (already landed for TYPE packs: `tidpack_one`/`tidpack_empty_names`) to a
   non-type pack: bind `_Indexes` to a list of integer values, substitute each as a
   `TokenInt`. For map: `_Indexes1={0}`, `_Indexes2={}`. Mirror the type tid-pack code in
   `instantiate_fn_template_binding`.

3. **W3 — parallel pack expansion at 1 element.** `first(forward<_Args1>(get<_Indexes1>(t1))...)`
   expands `_Args1` and `_Indexes1` in lockstep. For map both have length 1 (or 0), so the
   "lockstep" is a single element — reuse the existing single-element `pattern...` drop +
   per-element substitution; no N-way machinery needed yet. (General N-way lockstep =
   later, for tuple<A,B,...>; clang's model is `TreeTransform::TransformExprs`,
   TreeTransform.h:4163 — same-arity rule — for when it's needed.)

4. **W4 — `std::get<0>` body instantiation (the current live wall).** `get<0>(tuple<X>)`
   now deduces + reaches `instantiate_fn_template_binding` but fails in the body. Needs:
   (a) the alias `__tuple_element_t<0, tuple<X>>` → member-type resolution of the
   instantiated `tuple_element<0, tuple<X>>::type` (depth-0 `_Nth_type<0,X>`); (b) the body
   `__get_helper<0>(__t)` → resolve via overload + derived-to-base to `_Tuple_impl<0,X>`
   (terminal spec) → `_M_head`. Pin whether the failure is the alias-return SFINAE
   (parser.cpp ~31084 `is_templateid_ret`) or the body parse (reducer tmp/tupget2.mad,
   MADC_DEBUG_CTORTMPL).

5. **W5 — piecewise→indexed ctor delegation.** With W1–W4, the piecewise ctor body
   `: pair(__first, __second, _Build_index_tuple<1>::__type(), _Build_index_tuple<0>::__type())`
   instantiates (`sizeof...(_Args1)`→1 via the existing tid-pack count; `_Build_index_tuple`
   via W1) and delegates to the indexed ctor (W2 non-type packs + W3 single-element
   expansion). Then `map_insert.mad` → 0 c2mir errors → RUN.

6. **(parallel/independent) multi-VALUE symbol identity** — only needed if a single TU
   instantiates the SAME fn template at two non-type values with identical signatures
   (`get<0>` and `get<1>`). For `map<int,int>` the key-tuple is 1-element so only `get<0>`
   is used — **likely not on the map<int,int> critical path**; defer unless W4/W5 surface it.

## Architectural verdict

madc's incremental, instantiate-real-libstdc++ approach is **sound** — no rewrite
needed. The "days of grinding" reflected scope mis-estimation (reasoning about
general tuple<A,B,C,...> machinery) rather than a wrong architecture: `map<int,int>`
only exercises the depth-0/1 base cases. The remaining path is W1 (one small
builtin) + W2/W3 (extend the just-landed non-type/tid-pack code to non-type packs +
single-element lockstep) + W4 (the shallow `std::get<0>` body) + W5 (wire the
delegation). Each is a bounded, fulltest-gated commit, not a sub-campaign.

Builtins to implement: **`__integer_pack`** (critical path). Optional/forward-compat:
`__type_pack_element`, `__make_integer_seq` (Clang path; dead for libstdc++-13, cheap
if ever wanted). Do NOT gate on them.

## Source references (for the implementer)
- GCC `__integer_pack`: cp/pt.cc:3846-3912 (expand), :14191-14202 (pack-expansion
  dispatch + "must be entire pattern"), :33231-33242 (declare), :4035-4038 (find-packs);
  cp-tree.h:7239.
- Clang builtins (forward-compat model): SemaTemplate.cpp:3749-3832
  (`checkBuiltinTemplateIdType`), SemaLookup.cpp:917-935 (inject on lookup-fail).
- Clang parallel-pack expansion: TreeTransform.h:4163-4238 (`TransformExprs`, same-arity).
- libstdc++-13: bits/utility.h:140-156 (`_Index_tuple`/`_Build_index_tuple`, the
  `#if __has_builtin(__make_integer_seq)` guard), :230-263 (`_Nth_type`), :84
  (`__tuple_element_t`); tuple:258-278/489-515 (`_Tuple_impl`), :1776-1810
  (`tuple_element`/`__get_helper`/`get`), :2248-2269 (pair piecewise + indexed ctors).
- madc engine: instantiate_fn_template_binding + try_instantiate_namespace_fn_template
  (src/parser.cpp), the just-landed non-type (`nontype_subst`) + 0/1-element tid-pack
  (`tidpack_one`/`tidpack_empty_names`) code; instantiate_template_id / alias resolution.
