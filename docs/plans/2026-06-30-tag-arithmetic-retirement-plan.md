# Tag-arithmetic retirement — plan

**Status:** Phase 0 + Cluster A done; gate extended to two metrics. Burndown live;
**baselines raw-tag=3** (was 25), **consumer=17** (new; the offset-reading surface the
rt*-only count missed). Incremental consumer migration confirmed viable (see
"Investigation 2026-06-30" below) — migrate ahead of the small atomic core flip.
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

## Investigation 2026-06-30 — base `is_pointer()` is tag-aware; incremental migration IS viable

Settled the recorded first-action question (is base `DataDef::is_pointer()` tag-aware
or structural → incremental-vs-atomic). Answer: **tag-aware at the base, structurally
overridden on `DataDefPTR`.** Two populations coexist:
- **(a) structural objects** — `DataDefPTR`/`DataDefREF` override `is_pointer()`→true
  but their ctor ALSO sets `_type = rtPtr(base.type())` (the `+10000` offset). e.g.
  `ddCHARptr = DataDefPTR(ddCHAR)`.
- **(b) plain tagged** — a plain `DataDef` whose pointer-ness comes ONLY from the
  base tag-aware `is_pointer()` (`_type>=10000 && <20000`). e.g. `DataDefLPSTR`
  (datadef.h:998, `rtPtr(dtCHAR)`).

Decisive consequence: the **predicates** (`is_pointer()`/`is_reference()`/`rawtype()`)
already answer correctly for BOTH populations today. So consumer migration is
**behavior-preserving and can run AHEAD of the core flip**, in small gated commits:
- `type() == dtCHARptr` → `is_pointer() && rawtype()==dtCHAR` — invariant across the
  flip (`DataDefPTR::rawtype()` returns `dtCHAR` before and after, since the offset it
  strips today simply won't be there to strip after).
- `reftype() == rtReference` → `is_reference()` — strictly a **bug-fix**: a
  `DataDefREF` reports `reftype()==rtPointer` today (its ctor chains `DataDefPTR`/
  `rtPtr`), the "three encodings" disagreement; `is_reference()` is the correct SoT.

Only the core edit is atomic, and it is small/self-contained in datadef.h: drop
`rtPtr` from the `DataDefPTR` ctor + `DataDefLPSTR`, add structural `reftype()`/
`setRef()` overrides on `DataDefPTR`(→rtPointer)/`DataDefREF`(→rtReference), delete the
enum ranges + macros, rewrite `same_representation`'s tail (Cluster B) — `-Wunused`
confirms. Refined sequencing: **migrate the ~30 consumers to predicate form first
(lower-risk, each behavior-preserving), then the atomic core flip last.**

**Gate extended (`2f64ded1`).** `tag_arith_burndown.sh` now tracks TWO ratcheted
metrics (baseline file = first two number-only lines): line 1 = raw-tag sites (3),
line 2 = consumer surface (17 — `dt*ptr/dt*ref` named-constant uses + `reftype()`
band-reads). The consumer count is invisible to the rt*-only metric and surfaced **3
missed Cluster-A signature literals** (`madc_program.cpp:416/417`, `parser.cpp:11838`
used the named constant `dtCHARptr` inside `datatype_vec_t{}` rather than
`rtPtr(dtCHAR)`). **Migrated `fd179ced`:** those 3 → `ptr_of(ddCHAR)` (consumer
17→14). Safe because a signature literal is a pure type-IDENTITY declaration —
`ptr_of(ddCHAR)` resolves via `getPointerType(ddCHAR)` to the SAME `ddCHARptr` the
tag produced, exactly. All gates green, torture byte-identical.

### CORRECTION — the equality consumers are NOT a trivial mechanical rewrite

The optimistic `type()==dtCHARptr` → `is_pointer() && rawtype()==dtCHAR` rewrite is
**not equivalent**, because the tag can't nest. `char**` is
`DataDefPTR(DataDefPTR(char))`, whose ctor computes `rtPtr(dtCHARptr)` =
`dtCHARptr + 10000` — overflowing into the *reference* band (20000+). So `rawtype()`
strips 20000 → `dtCHAR`, and `is_pointer()` (the `DataDefPTR` override) returns true,
so the rewrite would **newly match `char**`**, which the exact `type()==dtCHARptr`
(precisely `10000+x`) does not. The faithful structural form needs pointee inspection
(`is_pointer() && base_type->rawtype()==dtCHAR && !base_type->is_pointer()`), but the
plain-tagged population (b) DataDefs (e.g. `DataDefLPSTR`) carry **no `base_type`** —
their pointer-ness is the bare tag. So the equality consumers genuinely cannot be
migrated faithfully in isolation: their exact semantics depend on the tag's
single-level corner behavior, which only becomes well-defined once the structural
graph is the sole representation. **Confirms the plan:** the `type()==dt*ptr/ref`
equality consumers + the `reftype()` band-reads move WITH the core flip (alongside
Cluster B), NOT ahead of it. `reftype()==rtReference` → `is_reference()` IS a safe
standalone bug-fix where the site means "is this a reference" (not "what band"), but
the two such sites in `same_representation` (parser 9054/9056) are Cluster B and stay
coupled. Remaining consumer surface (14) is therefore the core-session worklist.

## Core-removal endgame — investigation 2026-06-30 (DE-RISKED, smaller than feared)

Deep read of the actual code shrank the endgame and ordered it into safe gated steps:
- **`setRef()` is DEAD** — zero live callers (one commented-out line at
  parser.cpp:37004). No live in-place tag mutation to chase.
- **Plain-tag (non-`DataDefPTR`) populations are just two vestigial globals:**
  `DataDefLPSTR` (a `char*` named "LPSTR", ~5 use sites) and `ddVOIDref` (one `void&`
  for the `MADC_TYPEID_VOID_REF` typeid slot, one reader). Everything else pointer/
  reference is already a structural `DataDefPTR`/`REF` object.
- **`reftype()` readers are all benign:** `same_representation` (parser 9054/9056,
  guarded by `!ap` so a `DataDefREF` — which IS-A `DataDefPTR` — never reaches it;
  caught by the `ap && bp` base_type recursion at 9041), `as_user_class` (cir_builder
  2527/2537 — a `DataDefREF` fails the `btClass` gate at 2526 first, and with
  `setRef` dead no class is ref-tagged in place, so these are dead-defensive), and one
  logging line (16105, cosmetic). So `reftype()` is barely load-bearing.
- **`same_representation` is built around exact tag corners** (the comment at
  9043-9051 spells out the T** = 20000+x reuse and the bare-ref-tag-on-plain-DataDef
  case) — it is rewritten WITH the tag drop, last.
- **Subtlety — `is_numeric()`/`is_integer()` read `_type` directly** (`< dtRESERVED`/
  `< dtFLOAT`), so a plain tagged `char*` (`ddLPSTR`, `_type`=10003) reports
  `is_numeric()==false` TODAY; converting it to `DataDefPTR` flips that to true (a
  defensible consistency fix, but a behavior CHANGE — gate it, don't call it
  identical). `type()==dtCHARptr` consumers likewise can't move to
  `is_pointer() && rawtype()==dtCHAR` faithfully (matches `char**`); the faithful form
  needs `base_type` pointee inspection, which is why `ddLPSTR` must become structural
  first.

### Ordered, gated step plan (each build+fulltest+torture-byte-identical)
1. **Structural `reftype()`/`rawtype()` overrides on `DataDefPTR`/`REF`** — DONE this
   session. Verified behavior-identical at all readers; mirrors `DataDefCONST`. The
   structural objects stop reading the `_type` band for ref/raw classification.
2. **Eliminate `ddLPSTR`'s plain-tag `char*`** — make it a `DataDefPTR(ddCHAR)` (needs
   moving its class def after `DataDefPTR` + an out-of-line ctor since `ddCHAR`'s
   instance is declared later). Accept/validate the `is_numeric` flip. Then every
   `char*` has a `base_type`.
3. **Migrate `type()==dtCHARptr` consumers** → `is_pointer()` + `base_type` pointee
   check (behavior-identical with the tag still present; lowers the consumer metric).
4. **`ddVOIDref` typeid slot + `dtARRAYref` (cir 4277) + the dead reftype guards** —
   convert/retire structurally.
5. **Drop the tag:** remove `rtPtr`/`rtRef` from the `DataDefPTR`/`REF`/`LPSTR` ctors,
   rewrite `same_representation`'s tag tail (Cluster B), delete the `dt*ptr`/`dt*ref`
   enum ranges + `rt*` macros; `-Wall -Wunused` confirms the cut; flip the gate to a
   finish-line check.

## TURNKEY FLIP RECIPE — ✅ EXECUTED @ 69e5d2b3 (2026-06-30)

The flip landed exactly as recipe'd, plus two pointer-guards the recipe's
blast-radius note implied but did not enumerate: a pointer's `type()` is now its
pointee scalar, so (1) the shim classifier's `void*` return path was guarded with
`!is_pointer()` so it no longer aliases `case dtVOID` (R_VOID), and (2)
`native_type_from_datadef` now bails pointers to `integer` BEFORE the scalar
switch (else `void*`→dtVOID and `bool*`→dtBOOL would mis-classify). Unit tests
that encoded the tag (base+10000/+20000, rt* inverses, the char**-tag pin, the
`is_cstr()==type()==dtCHARptr` cross-checks) were removed; the structural
assertions remain. Gate metrics 5/3 → 0/0 (now a finish-line check). Build
0-warn, fulltest 673/0/0/16, torture byte-identical to the 51-name baseline.

### Recipe as designed (for reference)


After core steps 1-3 + the flip groundwork (consumer metric at 5, all reftype()
readers; raw-tag at 3, all the `same_representation` tail), the atomic flip is a
single gated commit. Ordered edits:

1. **Convert `ddVOIDref` to a structural reference.** `DataDefVOIDref`
   (datadef.h:974) is the LAST plain-tag global (a `void&`, plain `DataDef` with
   `_type=dtVOIDref`, is_reference()==false). Make it derive from `DataDefREF`:
   `class DataDefVOIDref : public DataDefREF { public: DataDefVOIDref(); };` with an
   out-of-line ctor in parser.cpp (after `ddVOID`, before `ddVOIDref` at 8584):
   `DataDefVOIDref::DataDefVOIDref() : DataDefREF(ddVOID) { name = "void&"; }`. Now
   is_reference()==true, reftype()==rtReference (the step-1 override), and the
   `same_representation` ref-strip at 8979 handles it. NOTE: this changes its
   type() (dtVOIDref→dtVOIDptr while the tag still exists, then →dtVOID after the
   offset drop) and is_reference() (false→true) — check the MADC_TYPEID_VOID_REF
   slot test in test_datadef.cpp (it pins slot→global identity, not type(), so it
   should be fine, but confirm).

2. **Rewrite `same_representation`'s tag tail** (parser.cpp 9043-9063) — replace
   the whole `DataType atag/btag … return atag==btag` block (incl. the now-dead
   `!ap && reftype()==rtReference` fallback, dead once ddVOIDref is a real
   reference) with the structural form:
   ```cpp
   // Both are btSimple and NOT both DataDefPTR (a pointer pair was handled
   // above); references already stripped. Pointer-ness must agree — a lone
   // pointer differs from a scalar — then the scalar DataType tags match exactly.
   if ( a->is_pointer() != b->is_pointer() )
       return false;
   return a->type() == b->type();
   ```
   This is correct BOTH before and after the offset drop (verified: char*-vs-char
   rejected by the is_pointer guard; int-vs-int by type()==; ptr-pairs already
   handled at 9041; void&-vs-void by the ref-strip once ddVOIDref is a reference).
   Removes all 3 raw-tag sites + the 2 Cluster-B reftype readers.

3. **Clean the dead reftype guards** (cir_builder 2527/2537, `as_user_class`): with
   setRef dead and no class ever ref-tagged in place, a class DataDef never has
   reftype()==rtReference — the guard is dead-defensive. Either delete it or
   leave it (it compiles fine post-flip since reftype() still exists structurally);
   the logging read at 16109 is cosmetic and can keep `(int)reftype()`.

4. **Drop the offset from the ctors.** `DataDefPTR` ctor (datadef.h:1006):
   `rtPtr(base.type())` → `base.type()`. `DataDefREF` inherits (its reftype()
   override already returns rtReference structurally; rawtype() inherited). After
   this, char* and char& both have type()==dtCHAR, distinguished only by
   is_reference()/reftype() — the structural objects are now the SOLE truth.

5. **Delete the encoding.** Remove the `dt*ptr` (datadef.h:70-77) and `dt*ref`
   (80-87) enum ranges, the `rt{Ptr,Ref,DePtr,DeRef}` macros (119-122), and make
   the base `is_pointer()`/`rawtype()`/`reftype()`/`setRef()` bodies structural
   (is_pointer→false at base, rawtype→(DataType)_type, reftype→rtValue, setRef→
   gone since dead). `-Wall -Wunused` confirms nothing else referenced them.

6. **Flip the gate** to a finish-line check (counts must be 0; assert stays 0).

WATCH-OUTS: (a) confirm NO plain-tag pointer DataDef survives besides DataDefPTR
(grep `rtPtr(`/`dt*ptr` construction outside DataDefPTR) — ddLPSTR is done; (b)
base is_pointer() (`_type>=10000`) goes always-false after the drop, correct since
only DataDefPTR marks pointers; (c) run the `===` (spaceship/strict-eq) suite +
torture; the failset MUST stay byte-identical to the 51-name baseline.

## Guardrails

- Structural-first: a derivation question is `is_pointer()`/`is_reference()`/
  `base_type` / `derived_type_id`, never literal `± 10000/20000`.
- Never let the count rise (the gate enforces it). A new feature that needs a
  derived type uses `getPointerType`/`getReferenceType`/`derived_type_id`.
- The `_type` offset and the object graph currently can DISAGREE for `DataDefREF`
  (tag says pointer). Until the final phase, prefer the structural method on any
  site you touch; do not "fix" the tag — retire the reliance on it.
