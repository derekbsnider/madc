# Type Table (typeid) + 32-byte `madc_value` ABI — design

**Status:** DESIGN AGREED (2026-06-12, user-signed direction in session dialog).
Implementation phased; the identity layer + value ABI are near-term (eval
package C builds on them), the compiler-internal migration is a later campaign.
**§6 phase 1 (identity layer) IMPLEMENTED** on `feature/type-table-claude`
(2026-06-12, plan `docs/superpowers/plans/2026-06-12-type-table-identity-layer.md`):
`include/madc_typeid.h` ABI slots, `DataDef::type_id` + `madc_primitive_for_slot`
/ `madc_stamp_primitive_type_ids` (stamped from `add_datatypes`), and
`Program::type_id_for` / `type_from_id`; doctest-pinned; fulltest 577/0/0/18,
zero warnings, both check gates green. Next: §6 phase 2 (the 32-byte value ABI).

Related docs this builds on / feeds:
- `docs/plans/2026-06-09-frontend-representation-refactor.md` — P0 (value pool),
  P3 (uid side-arrays), P4 (AST-forest serialization). This design is the
  *type-pool sibling of P0* and the prerequisite that turns P3's `datadef`
  side-array and P4's type references into plain integers.
- `docs/plans/2026-06-09-embedded-header-forest-design.md` — the prior-art
  verdict "Copy Clang: ID/offset cross-refs, type-ID low-bits-for-builtins".
  This design promotes that from *serialization format detail* to *live
  representation* (per the refactor doc's own mindset #2: the representation
  IS the PCH design).
- `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md` — eval package C
  (`register_function`, `get/set_global`, string call marshalling) is the
  **first consumer** of the new value ABI.
- **Tokens (refactor P1) — the same move, opposite storage answer.** P1's flat
  token records are this design's twin for the token spine: tokens are
  millions, uniform, transient → kill the objects, keep records; DataDefs are
  thousands, polymorphic, long-lived → keep the objects, table the ids.
  **Binding requirement:** the P1 token record must name `type_id` (this
  table) and the P0 value-pool handle as its reference fields from the start,
  so P1 is not built against pointers and re-done. The token-kind enum already
  lives by the same append-only/ABI-pinned discipline the primitive slots
  adopt (PCH serializes kind ids — the `tk3NotEq`-at-enum-tail rule).
- `docs/plans/madcdis-plan.md` (Track 5A) — **reconciled 2026-06-12.** The
  madcdis plan predates this design and specifies its own value system
  (Principle 5, Class Model, V1: "`madc::value` with inline scalars and heap
  pointers" — an 8-byte tag + 60-bit handle, with a heap `value_header`
  carrying `type_tag`/refcount/hash/flags). Precedence and division of labor:
  - The **public `madc::value` is THIS design** (32-byte typeid struct; the
    A0-landed class becomes its RAII wrapper). madcdis's 8-byte tagged handle
    is the *internal pool value-handle* (dense storage tier) — it marshals
    to/from `madc_value` at the substrate boundary and must not claim the
    public name.
  - **`value_header.type_tag` := the uint32 typeid from this table** (ONE
    table). This is an enabler for madcdis, not a constraint: its
    position-independent `mem://`/`shm://` pools cannot store `DataDef*`
    pointers; the segmented stable-integer ids are exactly what pool-resident
    type refs need.
  - **Adopted back from madcdis into §3's cell header:** saturating refcounts
    with a permanent tier (literals/schema skip counting — the SMAUG hashstr
    discipline), and reserved `cell_flags` bits for interned / frozen /
    hash-present — so a malloc'd cell today and a pool-resident cell later
    share one header shape.
  - **SSO/interning interplay:** strings ≤16 bytes inline and are never
    interned (no sharing benefit); interning is the long-string and
    `madc::Symbol` discipline, a madcdis pool feature (V2).
  - madcdis's sequencing is unchanged (Track 5 starts only after Track 1.3
    parity); see the UPDATE block in `madcdis-plan.md`.

## 1. Context — why

Type identity in madc is fragmented across four mechanisms:

1. **Pointer identity** — `DataDef*` raw pointers everywhere.
2. **Name-keyed maps** — `Program::datadef_map` / `struct_map`
   (`include/madc.h:1134-1135`).
3. **Tag arithmetic** — pointer/reference derivation via tag ranges
   (the 20000+x offsets, `rawtype()` stripping). This is the machinery behind
   the 2026-06 double-pointer/reference tag-range collision bug and the
   "references have three encodings" mess the strict-equality track had to
   work around (`operand_value_datadef`).
4. **Predicates** — `DataDef::same_representation()` (parser.cpp ~6339),
   written for `===` because none of the above answers "same type?" directly.

Meanwhile three upcoming consumers all need a *stable, serializable, compact*
type reference:

- **Forest/PCH serialization** (refactor P4): a pointer-graph cannot be
  mmap'd; type references must be integers with a stable immutable segment.
- **eval package C marshalling**: `register_function` / `get/set_global` need
  a dynamic value that can carry *any* madc type — the current closed
  kind-enums (`madc_value_kind` in `include/madc_api.h:21`, `madc::value::kind`
  in `include/libmadc/value.h`) categorically cannot hold a user struct.
- **The polyglot vision** (I1–I8): loosely-typed frontends (PHP/Perl/Python/JS)
  need a value whose type is *data, bound late, reassignable* — with optional
  gradual-typing discipline.

One table answers all four fragments and all three consumers.

## 2. The type table

A single id→type table; **uint32 typeid** is the canonical type identity.

```
id 0                  invalid / "no type" sentinel
[1, 100)              primitives — pinned compile-time constants (enum), ABI-stable
[100, 0x01000000)     system segment — types from the embedded system forest,
                      frozen at forest build time, identical across runs
[0x01000000, 2^32)    project segment — user typedefs/structs/classes/enums,
                      per-Program, deterministic registration order
```

- **Storage:** layered view, not one allocation. Primitives = a static const
  array (process-wide). System segment = owned by the (future) embedded forest,
  shared read-only. Project segment = `std::vector<DataDef*>` on `Program`.
  Lookup by id dispatches on segment boundaries; all three resolve to
  `DataDef*`.
- **`vector<DataDef*>`, NOT `vector<DataDef>`** — DataDef is polymorphic
  (DataDefCLASS, DataDefENUM, …) and growth must not invalidate.
- **Stamping:** every DataDef gains a `uint32_t type_id` field assigned at
  registration through ONE chokepoint. `Program::add_datatypes()`
  (`include/madc.h:1569`) pins the primitive slots; user-type registration
  paths (typedef/struct/class/enum) append to the project segment.
- **Fixed segment bases** (project base = `0x01000000`) decouple the segments:
  a regenerated system forest never renumbers project ids, and a primitive id
  is meaningful with no table at all.
- **Derived types are table entries, not bit tricks.** pointer-to(id),
  reference-to(id), array-of(id, n), fn(sig) are entries created on demand,
  memoized by `(kind, operand-id)` in a small map. This is the structural
  replacement for the 20000+x tag arithmetic — but see Sequencing: the
  *consumers* of tag arithmetic migrate in a later campaign, not here.
- **Scope & lifecycle:** the compiler side is per-Program (the project driver
  creates a fresh Program per TU — each TU has its own project segment over the
  shared immutable prefix). Host-held `madc_value`s with project-segment
  typeids are valid only while their program is loaded; primitive/system ids
  are always safe. The embedding API resolves ids through the program handle.
- **AOT constraint (write it down now):** the project segment is part of the
  saved program image — `save_object`/`save_executable` (package C) must
  serialize the table so ids survive a save/load round trip.

## 3. The value ABI — 32-byte `madc_value`

```c
struct madc_value {              /* _Alignas(16), 32 bytes */
    uint32_t type_id;            /* index into the type table */
    uint32_t flags;              /* see §4 */
    uint64_t size;               /* per-kind, see table below */
    union {                      /* 16-byte aligned payload */
        int64_t  integer_value;  /* bool folds in; typeid distinguishes */
        double   real_value;
        char    *text_value;     /* refcounted heap cell when not inline */
        void    *data_ptr;       /* array buffer / object / oversize struct cell */
        char     inline_text[16];/* SSO; length in size; MADC_VF_INLINE set */
        uint64_t wide_value[2];  /* __int128 / _BitInt / long double /
                                    _Complex double / v128 — portable spelling */
    };
};
```

**Why 32 and not 16:**
- Every madc primitive inlines — madc is a C dialect whose primitive set
  genuinely includes 16-byte types: `__int128` (P0 makes wide ints
  correctness-critical), `long double`, native `_Complex double` (fork), and
  `v128` vectors (the fork's ≤16-byte SIMD). A 16-byte struct would heap-box
  exactly the primitives currently being invested in.
- Small-string optimization: strings ≤16 bytes inline with zero allocation —
  the dominant case in loose-typed workloads.
- Flag space = ABI headroom (this becomes public ABI with package C; growing
  the struct later is an ABI break, reserving now is free).
- A 16-byte-aligned payload (single aligned SIMD move for v128) forces the
  struct to 32 bytes anyway; "24 bytes" doesn't exist after padding.
- The density argument for 16 (4 vs 2 per cache line) is real but within
  industry norms (PHP 7 hash buckets are 32 B/element); bulk numeric data uses
  real typed C arrays on the static side, not arrays-of-dynamic-values. The
  SysV ≤16-byte register-passing advantage is moot: the API passes
  `madc_value*` everywhere (`include/madc_api.h`).

**`size` semantics per kind (never ambiguous):**

| typeid kind            | `size` holds                        |
|------------------------|-------------------------------------|
| null / numeric prims   | `sizeof(type)` (uniformity)         |
| string / bytes         | byte length (SSO or cell)           |
| array                  | element count                       |
| dynamic object (map)   | field count                         |
| struct/class instance  | instance byte size                  |

**Marshalling rule (uniform, mechanical):** payload inlines if the type's size
≤ 16 bytes, else `data_ptr` points at a cell; the DataDef behind `type_id`
supplies everything else (layout, members, dtor). User structs ride through
`register_function`/`get/set_global` with **zero per-type code** — this is the
rule-#7-conforming design (no hardcoding specifics into general machinery).

**Heap cells & ownership:** pointer payloads reference a refcounted cell:
`{ uint32_t refcount; uint32_t cell_flags; payload bytes… }`. Copy = retain,
`madc_value_clear` = release. Refcounts are non-atomic (script execution is
single-threaded per engine; fork isolation gives CoW pages) and **saturating
with a permanent tier** (a cell at the saturation count is never decremented
or freed — literals and schema metadata pin there; the SMAUG hashstr
discipline, adopted from `madcdis-plan.md` Principle 5). `cell_flags`
reserves bits for `permanent` / `interned` / `frozen` / `hash-present` so
this header is the malloc'd form of madcdis's pool `value_header` — one
header shape across both tiers, not a fork. The refcount lives in the cell,
NOT the value struct — struct copies are independent and inline counts would
desync.

**Array / object representation:** an array is a value whose typeid is an
array entry, `data_ptr` → contiguous `madc_value` buffer, `size` = count.
Heterogeneous PHP-style arrays come free (each element carries its own
typeid) — this is what MadArray's tagged union hand-rolled, unified.

**Existing types:** the new struct replaces the flat 40-byte `madc_value`
in `include/madc_api.h:30` (pre-package-C, so the ABI change lands before
external consumers). The `madc_value_kind` enum is subsumed by the primitive
typeid slots — one vocabulary. The C++ `madc::value`
(`include/libmadc/value.h`, 8-kind deep-copy class) is **phase-2**: it becomes
a thin RAII wrapper over the struct, but the freshly-landed A0 surface is NOT
broken in the same change — it converts at the boundary initially.

## 4. Flags — storage, gradual typing, nullability

`flags` is a uint32; bits stay BORING (speculative bit-packing is how the
20000+x mess was born). Initial assignments, all else reserved-zero:

```
MADC_VF_HEAP        bit 0  payload is a refcounted cell (vs inline)
MADC_VF_INLINE_TEXT bit 1  payload is SSO inline_text
MADC_VF_TYPE_LOCKED bit 2  re-tag is an error (strict gradual typing)
MADC_VF_TYPE_COERCE bit 3  assignments convert to current type_id (soft hint)
MADC_VF_NULLABLE    bit 4  null assignment allowed even when LOCKED/COERCE
MADC_VF_CONST       bit 5  value is read-only
```

**Re-tagging semantics** (the loose-typing rule, user-decided 2026-06-12):

- **No type flags** — plain `madc::value`: unrestricted re-tag; assignment
  adopts the source's typeid (PHP/JS-style).
- **COERCE** ("type hint"): assignment converts the incoming value to the
  current `type_id` where a conversion exists, else error (Perl-ish "this
  variable is numeric").
- **LOCKED**: mismatched-domain assignment is an error (PHP `strict_types` /
  Python type-hint discipline). Lock/unlock is runtime-flippable — it's a bit.
- **NULLABLE** modifies both: a LOCKED/COERCE value may still hold/accept
  null (`?int` shape); without it, null assignment errors too.

No extra storage is needed for hints: a hinted/locked variable is just
`type_id` set at declaration + the flag. The compiler (for declared script
variables) and the host API (for engine-side values) share this one mechanism.

## 5. Language surface — `typeid` is wanted (user-confirmed)

- The dynamic-value surface first: a `<ns_madc>` declaration-only function
  (e.g. `madc::type_of(value) -> uint32`, mangled-direct like the rest of
  `<ns_madc>`), plus lock/unlock/hint helpers.
- A STD_MADC `typeid(expr)` builtin over *static* types can follow. **Gating
  caution:** C++ already has conforming `typeid` returning `std::type_info`
  (RTTI landed with the MI track), so any dialect spelling must be
  STD_MADC-gated via the LanguageStd registry exactly like `===` was — and
  under `--std=c++*` conforming RTTI behavior is untouched. Whether
  `std::type_info` is internally *backed* by table ids someday is an open
  question, not a commitment.

## 6. Sequencing

1. **Identity layer** (small, near-term): primitive-slot enum in a shared
   header (ABI constants), `DataDef::type_id` stamped at the registration
   chokepoints, project-segment vector on `Program`, id↔`DataDef*` lookup,
   derived-type memo API. Doctests pin slot assignments. Compiler internals
   keep using `DataDef*` — the table is the identity authority, conversion at
   boundaries.
2. **Value ABI**: the 32-byte struct + cell runtime + SSO + marshalling rule
   in `madc_api.h` / libmadc; `madc::value` converts at the boundary.
3. **eval package C** builds on 1+2 (the queue item — register_function,
   get/set_global, string marshalling, AOT save/load incl. table
   serialization). The deferred script-side `array`/`madc::value` whole-value
   ops (strict-equality spec out-of-scope item) land here too: assignment
   re-tags per §4, `===` over dynamic values = typeid compare + payload
   compare.
4. **Later, separate campaigns** (do NOT fold into 1–3):
   - cir_node `datadef` side-array holds ids not pointers (refactor P3), and
     P4 forest type-refs serialize as ids + table segments.
   - Tag-arithmetic retirement: migrate `rawtype()`/tag-range consumers onto
     derived-type entries. Big blast radius; needs its own gate (count drops,
     like retire-std-hardcoding).
   - `madc::value` class reworked as wrapper over the struct.
   - NaN-boxed dense storage inside madcdis (5A.5) if ever — internal only.

## 7. Guardrails

- **ONE table.** The dynamic-value runtime and the compiler must share it from
  day one. Test: `register_function` marshalling a user struct resolves its
  layout through the *same* entry the compiler compiled it with.
- **ONE struct.** No "compact 16-byte internal twin" later — that's the
  parallel-implementation trap.
- **Flags stay boring.** Discriminators + the four semantic bits above;
  everything else reserved-zero until a concrete need exists.
- **Don't break the A0 `madc::value` surface** in the same change that lands
  the struct.
- Primitive slot numbers, segment bases, struct layout, and flag bits are
  **ABI once package C ships** — pin them in doctests like the manglings.

## 8. Open questions

- Exact primitive-slot enumeration (mirror the dd* builtin set; include the
  16-byte primitives; reserve room for `_BitInt(N)` families — possibly as
  derived entries keyed by width rather than slots).
- Cell allocation strategy (plain malloc first; arena/interning is madcdis
  territory later).
- Whether `std::type_info` gets backed by table ids (RTTI unification).
- Host-visible table introspection API surface (id → name/size/kind queries)
  — likely wanted for package C's host callbacks; size it there.
