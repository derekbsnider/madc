# Tag-arithmetic retirement — plan

**Status:** Phase 0 landed (this commit). Burndown gate live; baseline 25.
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

- **Cluster A — builtin-signature registration (~18 sites, ~11773–12073).**
  `rtPtr(DataType::dtCHAR)` etc. build `datatype_vec_t` (`vector<DataType>`)
  signatures for builtin/host functions. The registry is itself tag-based; these
  migrate when the registry's parameter types become structural (DataDef*-based,
  e.g. `ddCHARptr`/`getPointerType`) instead of `DataType` tags. This is the
  biggest single sub-task.
- **Cluster B — strict-equality / strip (3 sites).** `rtDeRef` at 9055/9057 and the
  raw `>= 20000` compare at 13127, in `same_representation`-style tag logic →
  use `is_reference()`/`reftype()`/`rawtype()` (the methods) instead of literal math.

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
- Phases (each its own gated commit, smallest-safe-cluster first):
  1. **Cluster B** (3 sites) — pure method substitution, low risk; warms up the gate.
  2. **Cluster A** — make the builtin/host registry parameter types structural
     (DataDef*), so `rtPtr(dtX)` → the canonical pointer DataDef. May split across
     several commits (core/process/dlfcn/host-callback groups).
  3. **Final** — external count 0 → reimplement the `DataDefPTR`/`DataDefREF` ctors
     and base accessors structurally (drop the `_type` offset), delete the enum
     ranges + `rt*` macros, let `-Wall -Wunused` confirm the cut; flip the gate to a
     finish-line check.

## Guardrails

- Structural-first: a derivation question is `is_pointer()`/`is_reference()`/
  `base_type` / `derived_type_id`, never literal `± 10000/20000`.
- Never let the count rise (the gate enforces it). A new feature that needs a
  derived type uses `getPointerType`/`getReferenceType`/`derived_type_id`.
- The `_type` offset and the object graph currently can DISAGREE for `DataDefREF`
  (tag says pointer). Until the final phase, prefer the structural method on any
  site you touch; do not "fix" the tag — retire the reliance on it.
