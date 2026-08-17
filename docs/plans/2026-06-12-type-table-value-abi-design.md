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
**§6 phase 2 (value ABI) IMPLEMENTED** on `feature/value-abi-claude`
(2026-06-12, plan `docs/superpowers/plans/2026-06-12-value-abi-phase2.md`):
the 32-byte struct + `MADC_VF_*` flags in `madc_api.h` (old `madc_value_kind`
constants now alias typeid slots — TEXT/BYTES/OBJECT took slots 31-33),
`madc_value_cell.h` refcounted-cell runtime (saturating + permanent tier),
helpers/bridges rewritten with SSO + gradual-typing enforcement
(LOCKED/COERCE/NULLABLE/CONST; contract survives clear; typed null = size 0),
new `madc_value_copy`/`madc_value_text`. Refinement vs §3 as written: SSO
threshold is **15 bytes + NUL** so inline text is always a valid C string;
cell text is NUL-terminated too. fulltest 577/0/0/18, zero warnings, gates
green. Next: eval package C consumes this ABI.

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
[1, 0x100)            primitives — pinned compile-time constants (enum),
                      ABI-stable; 255 usable, a primitive id fits in a byte
[0x100, 0x01000000)   system segment — types from the embedded system forest,
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

---

## 9. Recon, 2026-08-17 — what it would take to make the SCRIPT `value` BE the
## 32-byte struct (measured, NOT scheduled)

Owner: *"I don't believe it's necessary to take on now, but you should make note
of these details."* So this section is a record, not a plan. It exists because
the same question was answered WRONGLY once already (see below), and the next
person to ask deserves the measurement rather than the guess.

### 9.1 The question, and why it comes up

`php::print_r($x, true)` returns `madc::value &` rather than `madc::value`
because a value **cannot** be returned by value: `func_def`'s return-type chain
ends at `type_list(ret_dd)` → `append_type_specs`, which has no `dtARRAY` case
and falls through to `int`. So `value f() { … return v; }` compiles, runs, prints
nothing and exits 0 — a silent wrong answer (recorded as D1 in
`2026-08-17-php-print-r-var-dump-plan.md` §13.6).

**The earlier claim that there is "no C type for a value" was WRONG, and so was
the cost estimate attached to it ("a representation arc, 23 `is_array_object`
sites").** The owner remembered correctly: the type exists and is declared.

### 9.2 What actually exists today

- **`madc_value` — `include/madc_api.h:52`** — is a real, tagged, 32-byte
  `__attribute__((aligned(16)))` C struct: `type_id`, `flags`, `size`, and a
  16-byte payload union (integer / real / `text_value` / `data_ptr` /
  `inline_text[16]` SSO / `wide_value[2]`). It is returnable by value from C
  today.
- **`madc::value` — `include/libmadc/value.h`** — the C++ class the SCRIPT
  `value` type maps to (`DataDefARRAY` is sized `sizeof(madc::value)`)
  **contains** it:

  ```cpp
  madc_value                                    _v;       // 32
  std::unique_ptr<std::vector<value>>           _array;   //  8
  std::unique_ptr<std::map<std::string, value>> _object;  //  8   -> 48
  ```

  48 bytes is the `long long[6]` the CIR builder emits. The class has a
  user-declared copy ctor, copy-assign and destructor, which is WHY it is
  lowered as opaque storage plus `madarray_construct` / `madarray_destruct`
  (`new(ptr) madc::value` / `->~value()`) instead of a named struct. The reason
  was never a missing type; it was the two owning C++ members.
- **The cell is designed AND wired.** `madc_cell` (`include/madc_value_cell.h`)
  carries `refcount`, `cell_flags`, and a **payload finalizer**
  (`void (*destroy)(void *payload)`) documented for exactly this use: *"A
  typed-instance cell carries the instance type's own destructor here."*
  `madc_value_make_instance()` allocates through `madc_cell_alloc_dtor` and
  parks the payload in `data_ptr` with `MADC_VF_HEAP` — and **generated code
  already calls it** (`src/cir_builder.cpp:25789` declares the extern,
  `:25797` emits the call). `madc_value_copy` already retains/releases with the
  aliasing order handled.

### 9.3 Measured scope

- **`_array` / `_object` are confined to ONE file.** They are `private`, the
  class declares **no friends**, and all 40 references live in
  `src/madc_value.cpp`. Nothing outside it depends on them being direct members.
- Of those 40: **12 are mutations** (the sites that would need a copy-on-write
  clone check) and 28 are reads.
- The `is_array_object` sites in `cir_builder.cpp` are about the LOWERING and
  would mostly be RETIRED by this change, not edited — the opposite of the
  earlier estimate.

### 9.4 The part that is not mechanical — copy semantics

`madc::value`'s copy constructor **deep-copies**:

```cpp
if (other._array)  _array.reset(new std::vector<value>(*other._array));
if (other._object) _object.reset(new std::map<std::string, value>(*other._object));
```

while the cell path gives **shared refcount** semantics. Moving the containers
behind `data_ptr` therefore converts array/object copies from independent to
shared — a change that passes a suite and breaks a program. Preserving observable
behaviour needs **copy-on-write**: retain on copy, clone before mutation when
`refcount > 1`, at those 12 sites.

That is the right end state for this language (PHP arrays ARE CoW
value-semantics, and the cell's non-atomic saturating refcount with
`MADC_CELL_PERMANENT` for the literal tier was built for it), but it is a
SEMANTIC decision with its own oracled tests, not an implementation detail to
fold into something else.

### 9.5 What lands if it is ever done

`madc::value` becomes exactly `madc_value` (32 bytes, no C++ members) →
`value` gets a real emitted C struct type → **D1 disappears** (returnable by
value, so `php::print_r` could drop the reference and return `madc::value`
outright, matching PHP's `string|true` even more directly) → the opaque-buffer
lowering and its `is_array_object` special-casing retire → array/object copies
become CoW.
