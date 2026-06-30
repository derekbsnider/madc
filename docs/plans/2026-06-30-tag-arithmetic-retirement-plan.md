# Tag-arithmetic retirement — plan

**Status:** Phase 0 + Cluster A done. Burndown gate live; **baseline 3** (was 25).
Cluster A (the 18 builtin-signature `rtPtr(dtX)` sites) migrated to `ptr_of(ddX)` —
structural `DataDef` + `RefType`, resolved through `getPointerType`, proven
representation-identical to the `dtXptr` tag (both `resolve_data_type` paths
converge on the same `ddXptr` global). Remaining 3 = Cluster B (the
`same_representation` comparator), which is coupled to the final core-removal phase
(it compares tag-only types) and is NOT independent — see the corrected analysis below.
**Origin:** `docs/plans/2026-06-12-type-table-value-abi-design.md` §1 (motivation),
§6.4 ("Tag-arithmetic retirement: migrate `rawtype()`/tag-range consumers onto
derived-type entries. Big blast radius; needs its own gate (count drops, like
retire-std-hardcoding)."). Prerequisites (now landed): the typeid table
(`id_table`, `type_id_for`/`type_from_id`) and the id-addressable derived-type
API (`Program::derived_type_id`).

## What we are retiring

`DataType` (include/datadef.h) encodes pointer/reference **derivation** as numeric
ranges on the tag:

```
dtINT    = 11           base
dtINTptr = 10011        int*  = base + 10000   (rtPtr macro)
dtINTref = 20011        int&  = base + 20000   (rtRef macro)
```

`rawtype()` strips the offset to recover the base; `reftype()` reads the band to
classify ptr/ref/value; `setRef()` adds/subtracts the offset. This is a **bit
trick**, and it is a problem:

- **Cannot nest.** One level only — there is no `int**`/`int*&`/array-of-N/fn-sig
  tag. The 2026-06 double-pointer/reference tag-range collision bug came from this.
- **Fixed 255-slot bands**, never-renumberable (PCH/typeid ABI), permanently cramped.
- **Parallel identity scheme.** First-class-references already moved reference-ness
  into a real `DataDefREF` object (and retired `vfREFERENCE`), but the `dt*ref`
  band still exists — so a `DataDefREF` can carry a `_type` tag of +10000 (pointer)
  while `is_reference()` is true. Two disagreeing sources of truth ("three
  encodings"). This is exactly the no-parallel-implementations trap.

The replacement already exists and nests cleanly:
- **Structure:** `DataDefPTR(base)` / `DataDefREF(base)` / `DataDefCONST(base)` with
  `base_type` traversal and `is_pointer()`/`is_reference()`/`is_const()`. `int**`
  is `DataDefPTR(DataDefPTR(int))`.
- **Identity:** the typeid table + `Program::derived_type_id(DerivedKind, operand_id)`
  is the id-addressable, serializable "pointer-to(id)" — the structural replacement
  for `X + 10000`.

## Scope (measured 2026-06-30)

The scary number — ~54 `rawtype()` call sites — is a **red herring**: those are
method calls that STAY; they get reimplemented structurally (traverse `base_type`)
in the final phase, not migrated per-site. The actual external worklist is the
**raw tag arithmetic**: 25 sites, ALL in `src/parser.cpp`:

- **Cluster A — builtin-signature registration (~18 sites, ~11773–12073). DONE.**
  `rtPtr(DataType::dtCHAR)` etc. built `datatype_vec_t` signatures for builtin/host
  functions. `datatype_vec_t` is `vector<typespec_t>`, and `typespec_t` already had
  a structural `DataDef* + RefType` form with a `ptr_of(DataDef&)` helper — so each
  `rtPtr(dtX)` became `ptr_of(ddX)`. `resolve_data_type` resolves `ptr_of` via
  `getPointerType(spec.dd)`, the SAME `ddXptr` the tag path resolves to → exact, no
  behavior change (torture byte-identical). The 2 host-callback locals widened from
  `DataType` to `typespec_t`.
- **Cluster B — `same_representation` (3 sites: `rtDeRef` 9055/9057, raw `>= 20000`
  13127). DEFERRED to the core phase.** CORRECTED ANALYSIS: this is NOT an
  independent early consumer. By the time control reaches the tag tail (9052+),
  references-as-instances are stripped and `DataDefPTR` pairs handled structurally;
  what remains are **plain `DataDef`s whose only representation IS the tag** (no
  `base_type` to recurse). So the comparison is inherently tag-based **until those
  types stop carrying tags** — i.e. until the core encoding is removed. Rewrite
  `same_representation`'s tail AS PART OF the final phase, not before.

**Core (NOT counted; removed LAST):** the `dt*ptr=10000`/`dt*ref=20000` enum ranges,
the `rt{Ptr,Ref,DePtr,DeRef}` macros, and the offset math in the
`rawtype()`/`reftype()`/`setRef()`/`is_pointer()` accessor bodies + the two
`rtPtr` uses inside datadef.h (`DataDefLPSTR`, the `DataDefPTR` ctor). These ARE
the encoding; they go in the final phase once the external count is 0.

## Method — burndown gate (retire-std-hardcoding playbook)

- `scripts/tag_arith_burndown.sh` counts the external worklist; `--check` ratchets
  against `docs/parity/tag-arith-baseline.txt` (fails if the count RISES). Wired into
  `make -C src fulltest`.
- Each migration commit lowers the baseline in the SAME commit, stays
  fulltest-green / census-0 / **torture byte-identical** to the 51-name baseline.
- Phases (each its own gated commit):
  1. **Cluster A — DONE** (baseline 25 → 3). `rtPtr(dtX)` → `ptr_of(ddX)` across the
     builtin/host signatures; representation-identical via `getPointerType`.
  2. **Final / core phase** (baseline 3 → 0 and beyond). Reimplement the
     `DataDefPTR`/`DataDefREF` ctors + base accessors structurally (drop the `_type`
     offset so `is_pointer`/`reftype` are purely structural and tag-only pointer/ref
     types no longer exist), rewrite `same_representation`'s tag tail (Cluster B) to
     compare structurally, delete the `dt*ptr`/`dt*ref` enum ranges + `rt*` macros,
     and let `-Wall -Wunused` confirm the cut. Flip the gate to a finish-line check.
     This is one coupled phase because Cluster B only becomes well-defined once the
     tag-only types are gone — sequence carefully, lean on torture + the `===` suite.

## Endgame surface (measured 2026-06-30) — the burndown UNDER-counts it

The `--check` gate counts only `rt*` construction + literal `10000/20000`. It does
NOT count consumers that read the offset through `type()`/`reftype()`, which ALSO
break the moment a `DataDefPTR`'s `_type` stops carrying the +10000 offset. Before
the core flips, extend the gate to count these, then migrate them:
- **~13 `dt*ptr`/`dt*ref` named-constant uses** in src (mostly `dtCHARptr`) — code
  that names the encoding directly (`type() == dtCHARptr`, signature literals).
- **~7 `reftype()` consumer sites** + **~6 `type() == dt*ptr/ref` comparisons** —
  classification that must become `is_pointer()`/`is_reference()`/structural.
So the realistic distance-to-core is ~30 consumer sites + the 3 Cluster B + the
core edit (ctors/accessors/enum/macros) — not 3. The "baseline 3" reflects only the
explicit `rt*` arithmetic; treat the core flip as its own session and re-measure
with the extended gate first.

## Guardrails

- Structural-first: a derivation question is `is_pointer()`/`is_reference()`/
  `base_type` / `derived_type_id`, never literal `± 10000/20000`.
- Never let the count rise (the gate enforces it). A new feature that needs a
  derived type uses `getPointerType`/`getReferenceType`/`derived_type_id`.
- The `_type` offset and the object graph currently can DISAGREE for `DataDefREF`
  (tag says pointer). Until the final phase, prefer the structural method on any
  site you touch; do not "fix" the tag — retire the reliance on it.
