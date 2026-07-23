# Forest B3 — arena record-layout design (step 1)

**Date:** 2026-07-06
**Status:** DESIGN — step 1 of the B3 track (see `2026-07-06-forest-arena-native-scoping.md`)
**Decision:** B3 chosen by owner 2026-07-06. This doc is the crux: get the record schema
right and the ~548-site conversion is mechanical.

---

## 0. The core move (one paragraph)

`DataDef`/`FuncDef`/`Variable` keep their virtual **read** interface unchanged (so the
~1,130 read sites — `dynamic_cast`, `is_pointer()`, `rawtype()`, field reads — do not
change). Their **data** moves out of scattered heap fields (`std::string`, `std::vector`,
`std::map`, raw `DataDef*`) into a **flat POD record in one contiguous arena**, addressed by
**index**, with identifiers as **intern-pool offsets** and variable-length collections as
**`(begin,count)` slices** into side-payload arenas. The live object becomes a **thin handle**
wrapping `(arena*, record_index)`; its methods read/write the record. Then **SAVE = dump the
arena** (memcpy of three vectors + the intern pool) and **LOAD = mmap + wrap handles** — no
field-by-field copy in either direction, and a new field is added to the record and dumped
for free. The hand-written per-category freeze/restore (~1,540 LOC, §3d of the scoping doc)
is deleted.

## 1. We are not inventing the arena — we already have it

The serialization format already IS an arena of POD records over a `uint32` buffer:

- `include/madcdis/pod_record.h` — `pod_append`/`pod_read`/`pod_words`: append a
  trivially-copyable, whole-`uint32`-words record to a `std::vector<uint32_t>`; read it back
  at a word offset. Wire format == the struct's in-memory layout, unchanged.
- `cir_freeze.h` already defines `cir_forest_type_record` (the per-type record, indexed by
  typeid), and `cir_forest_type_member` / `_base` / `_method` / `_anon` packed into the
  `SNAP_KIND_CIR_TYPE_PAYLOAD` stream via `pod_append` — with `member_begin/count`,
  `base_begin/count`, `method_begin/count` **already the `(begin,count)` slice pattern**.
- Identifiers already intern: `madc::dis::intern_table` (name ids), and `Variable::name_sid`
  already carries an intern id in the live object today.

So the type graph is **already** expressed as `(flat POD records + payload slices + intern
ids)`. What makes it a *serialization mirror* rather than *storage* today is `materialize_types`
(313 LOC): on load it **allocates fresh `DataDef` objects and copies every field back out of
the records**, and `cir_forest_fill_type_records` (275 LOC) does the reverse copy on save.
B3 deletes both copy directions by making the record the storage the live object reads through.

**Consequence:** the record *schema* is ~80% designed already. B3's schema work is (a) make it
**complete** (every semantic field, not the hand-picked subset — the v6→v16 gaps), (b) add the
**FuncDef** and **free-function** record kinds (none today), (c) flatten the **three hard-tier
containers**, and (d) decide the **handle read/write contract**.

## 2. Arena layout (the container)

Reuse the existing `madc::dis` snapshot container + segment framing (kept per scoping §3d).
The DataDef arena is these segments, all `mmap`-restorable verbatim:

| Segment | Holds | Addressing |
|---|---|---|
| `SEG_DEFS` | `defrec[]` — one fixed-stride POD record per `DataDef`/`FuncDef`/`Variable` | **index** = record slot; this is the type-id space |
| `SEG_PAYLOAD` | `uint32[]` — all variable-length runs (members, bases, methods, params, slice contents, flattened maps) packed by `pod_append` | `(begin,count)` word slices from `defrec` fields |
| `SEG_INTERN_*` | the existing `intern_table` (bytes/entries/buckets) — every identifier/string | intern id (already used) |
| `SEG_DIR` | unit directory + include-DAG edges + per-unit `(defrec begin,count)` + PP events | header-name → its slice of `SEG_DEFS` |

Key properties:
- **Index, not pointer.** Every cross-reference (`base_type`, `member->second`, a base class,
  a method's return type) is a `uint32` **defrec index**. Position-independent → `mmap` needs
  **no relocation**. (This is what `type_id` / the connector table already approximate; B3
  makes indices the *only* cross-ref form.)
- **Declaration order (RC1, for free).** `SEG_DEFS` is appended **in parse declaration order**
  — the arena is built as the parser declares things, not enumerated afterward from a sorted
  `std::map`. The per-unit directory slice `(begin,count)` is therefore already an ordered
  run; binding a header replays its slice in order. This dissolves RC1 (no `struct_map`
  alphabetical / dependency-fixpoint enumeration).
- **Free functions (RC2, for free).** A free function is just a `defrec` of kind `FUNC` in the
  same ordered stream (it already lives in `tkProgram->variables` in true order — that becomes
  the append source). No separate category, no special path.

## 3. Per-record schema

One tagged POD record type, `defrec`, with a `kind` discriminant (replaces the vtable for
storage purposes; the live handle still dispatches virtually — see §5). Common header:

```
struct defrec {
  uint32 kind;          // DK_STRUCT / DK_CLASS / DK_PTR / DK_REF / DK_CONST / DK_ENUM /
                        // DK_PRIM / DK_FUNC / DK_VAR / DK_SIMD / DK_FPTR / DK_MEMBERPTR / ...
  uint32 name_id;       // intern id (DataDef::name)
  uint32 canon_id;      // intern id (canonical_cpp_spelling); 0 = none
  uint32 size;          // DataDef::size
  uint32 flags;         // kind-independent bits (is_anonymous, union_layout, is_complete, ...)
  uint32 ns_id;         // defining namespace intern id (v10); 0 = global
  // ---- kind-specific tail (fixed-width union-by-kind; unused fields zero) ----
  ...
};
```

Easy tier (scoping §3a) — near-trivial tails:
- `DK_PRIM`/`DK_VOID`/`DK_BOOL`/…: header only (name+size+DataType already imply everything).
- `DK_PTR`/`DK_REF`/`DK_CONST`: `ref0` = pointee/referee/unqualified **defrec index**.
- `DK_ENUM`: header (enum_name == name).
- `DK_SIMD`: `ref0` = element index, `vector_bytes`, `lane_count`.
- `DK_FPTR`: `ref0` = target `FuncDef` index, `ptr_syntax`.
- `DK_MEMBERPTR`: `owner_idx`, `member_type_idx`.
- `DK_CARRAY`: `elem_idx`, `count`, `count_expr` (node ref — see §6).

Hard tier — `DK_STRUCT` / `DK_CLASS` tail. Reuse the existing member/base slice model and
**complete** it (add every DataDefSTRUCT/CLASS field from the anatomy, not the subset):
- Layout scalars: `pack`, `max_align`, `tag_explicit_align`, `nvsize`, `own_block_off`,
  `class_align`, bitfield-unit state, and the bool flags (union_layout, is_complete,
  reverse_scalar_storage, has_anon_aggregate, has_vptr_slot, has_vtable, from_system_header,
  is_extern_template_instantiated, has_user_ctor/dtor, is_dependent_placeholder, …).
- `members_begin/count` → run of `memberrec` in `SEG_PAYLOAD` (see §3.1).
- `bases_begin/count` → run of `baserec`.
- `methods_begin/count`, `ctors_begin/count`, `staticconst_begin/count` → runs of
  method/var records (each a defrec index or an inline method-rec — reuse `cir_forest_type_method`).
- `anon_begin/count` → run of `anonrec` (already `cir_forest_type_anon`).
- `vbase_begin/count`, `vgroup_begin/count` → the two hard flattenings (§4).
- The name-keyed maps (`method_map`, `type_aliases`, `static_member_types`,
  `static_member_const_values`, `member_default_inits`, `virtual_methods`) → each a run of
  `(name_id, value)` pairs; the map is rebuilt on the handle lazily or the handle does a
  linear scan (these are cold — lookup happens at parse/resolve, not in hot codegen).

`DK_FUNC` tail (`FuncDef`) — the one class with **no builder funnel**, ~18 containers:
- `ret_idx` (returns), scalar/bool flags (is_varargs, is_void_params, declaration_only,
  defaulted_or_deleted, pure_virtual, is_const_method, has_forest_body + forest_body_unit/idx,
  template deduce fields, …), the string ids (emit_symbol, local_emit_name, method_display_name,
  namespace_name, …).
- `params_begin/count` → run of `paramrec { type_idx; flags(const,pack,is_type); cpp_spelling_id;
  typedef_id; default_expr_node }`.
- `return_types_begin/count`, `template_param_*_begin/count`, `captures_begin/count`
  (`caprec { name_id; type_idx }`), `ctor_inits_begin/count`
  (`ctorinitrec { name_id; args_begin/count → node refs }`).
- `tsubst_type_arg_packs` (vector<vector<idx>>) → a run of `(begin,count)` slices, each into a
  run of indices (nesting via one indirection — same trick as vgroup slots).

`DK_VAR` tail (`Variable`, already vtable-free, easiest): `type_idx`, `name_id`, `name_sid`
(already an id), scalar flags/count, `storage_alias_id`, `typedef_id`, `dims_begin/count`,
`vla_size_expr`/`param_vla_side_effect_expr` (node refs). `data`/`aot_*` are runtime — §7.

### 3.1 `memberrec` (already ~`cir_forest_type_member`, completed)
```
struct memberrec {
  uint32 name_id;        // memberpair_t.first
  uint32 type_idx;       // memberpair_t.second  (defrec index)
  uint32 typedef_id;     // memberpair_t.typedef_name
  uint32 offset;         // member_offsets[i]
  uint32 count;          // member_counts[i]
  uint32 flags;          // array_flag, access(2b), origin-is-own, is_flexible
  uint32 origin;         // member_origin[i]  (base index, or -1)
  // bitfield (BitFieldInfo): is_bitfield, storage_offset, storage_size, bit_offset,
  //                          bit_width, is_unsigned, reverse_storage
  uint32 bf_flags; uint32 bf_storage_off; uint32 bf_storage_sz; uint32 bf_bit_off; uint32 bf_bit_width;
  uint32 explicit_align; // member_explicit_align[i] (0 = natural)
  uint32 vbase_idx;      // member_vbase[i] as a defrec index (0xffffffff = none)
  uint32 dims_begin;     // member_dims[i] -> run of carray_dim in payload
  uint32 dims_count;
  uint32 default_init_node; // member_default_inits[name] node ref (0 = none)
  uint32 count_expr_node;   // member_count_exprs[i] node ref (0 = none)
};
```
The four index-keyed maps on DataDefSTRUCT (`member_vbase`, `member_explicit_align`,
`member_default_inits`, plus per-member `member_count_exprs`) collapse into fields on the
member record — no separate map serialization. (Fixes the "map iteration order" latent issue too.)

## 4. The three hard-tier flattenings (the crux design)

### 4a. `DataDefCLASS::vbase_offset` — `std::map<DataDefCLASS*, size_t>` (pointer-KEYED)
The awkward one: the key is a pointer. Flatten to a **sorted run of `(class_idx, offset)`
pairs** referenced by `vbase_begin/count`:
```
struct vbaserec { uint32 class_idx; uint32 offset; };
```
Lookup on the handle = linear scan (vbase counts are tiny — 0–few) or binary search if sorted
by `class_idx`. **Written from exactly one method (`compute_layout`, 3 call sites)** — the
conversion is a single builder body.

### 4b. `DataDefCLASS::vtable_groups` — `vector<{owner, this_offset, vector<string> slots, addr_point}>`
Nested container. Two-level slice:
```
struct vgrouprec { uint32 owner_idx; uint32 this_offset; uint32 slots_begin; uint32 slots_count; uint32 addr_point; };
// slots_begin/count -> run of uint32 name_ids in payload
```
`vgroup_begin/count` on the class points to the `vgrouprec` run; each group's `slots_begin/count`
points to a run of interned slot-name ids. **Written from one method (`build_vtable_groups`,
2 call sites).** `vtable_slots` (flat `vector<string>`) is likewise a `(begin,count)` id run.

### 4c. `FuncDef` containers — no builder funnel, ~18 vectors
Each becomes a `(begin,count)` slice (§3 `DK_FUNC` tail). The nested ones
(`tsubst_type_arg_packs = vector<vector<DataDef*>>`, `ctor_initializers` with a nested arg
vector) use the same two-level slice trick as 4b. Because there's no builder method, the
~64 external `parameters` mutations (in the 6 named functions) write through a **new thin
builder API** added in step 2 (`FuncDefBuilder::add_param(...)` etc.) — this is the one place
B3 adds a funnel that doesn't exist today.

## 5. The handle contract (read preserved, write funneled)

The live `DataDef`/subclass keeps its class identity and virtual methods, but its **only data
member becomes `(arena*, uint32 idx)`** (or the handle IS a view constructed over the arena).
Two paths:

- **Read (unchanged for callers).** `bool DataDefPTR::is_pointer() const { return true; }` stays.
  `DataDef::rawtype()`, `size`, etc. read the `defrec` (`arena->defs[idx].size`). Field reads
  like `s->members[i]` become `arena->member(idx, i)` returning a lightweight member view —
  **but** to avoid touching the ~1,130 read sites, the handle exposes the **same accessor
  surface** (`members` behaves like an indexable range; `->size` is a member or an accessor).
  DECISION POINT: `size`/`name` are currently *public fields* read directly at many sites.
  Options: (i) keep them as real fields on the handle, written-through to the record on set
  (cheap, preserves `x->size` read syntax); (ii) convert to accessors (touches read sites —
  avoid). **Choose (i) for the hot public scalars** (`size`, `name`, `type_id`) — the handle
  caches them, so reads are unchanged and only writes go through the record.
- **Write (funneled).** All mutation goes through the builder methods (§ scoping 3e: ~13
  methods + the new FuncDef builder) which append to / update the record + payload. The ~137
  `new DataDefX(...)` sites become `arena.alloc<DK_X>(...)` returning a handle. The ~411
  scattered field/container writes (75% in 6 functions) are converted to builder calls or
  write-through setters.

This is the line that keeps B3 at ~548 sites instead of ~1,200: **reads keep their syntax,
only construction+mutation moves.**

## 6. Node references (TokenBase*) and runtime-only fields

- **Node refs** (`member_count_exprs`, `member_default_inits`, `runtime_size_expr`,
  `param_defaults`, `vla_size_expr`, ctor-init args): these point into the **CIR node tree**,
  which is *already* frozen generically (`SNAP_KIND_CIR_RECORDS`, one record per node). So a
  node ref is a **node-record index** — the same connector mechanism already used. No new work
  beyond storing the index.
- **Runtime-only fields NOT stored** (rebuilt live, never serialized — scoping §3a):
  `DataDefCLASS::vtable` (`void**`, built at compile time), `extern_ctor`/`extern_dtor`/`_dtor_ptr`
  (host C-fn pointers; `register_extern_ctor_dtor` is dead code — confirm & drop),
  `Variable::data` (interpreter backing store), `decl_file` (raw C-string into token storage).
  These are execution state, not the static type graph — they default to null/empty on load
  and are populated by the same runtime paths that populate them after a live parse.

## 7. Migration sequence (each step feature-guarded, byte-identity as the gate)

Guard: `FEATURE_FOREST_ARENA` (a `#ifdef`, per feature-guards rule) so the arena path and the
current live-object path coexist until switchover; `develop` stays green throughout.

1. **(this doc) Record schema.** Finalize `defrec` + payload record structs; land the header
   (extend `cir_freeze.h`'s existing structs to complete + add DK_FUNC/DK_VAR/vbase/vgroup).
2. **Arena + handle layer** behind the guard. Build the arena, the thin-handle wrappers, and
   route the ~13 builder methods + the new `FuncDefBuilder` + the ~137 allocation sites through
   `arena.alloc`. Live objects still fully functional (handle over an in-process arena).
3. **Convert the ~411 scattered writes** (concentrated in `TokenSTRUCT::parse`,
   `TokenCLASS::parse`, `parseFunction`, `TokenTEMPLATE::parse`,
   `register_skipped_class_template_function`, `cir_builder` clone/symbol-binding). Mechanical;
   gate after each cluster.
4. **Switch freeze/restore** to dump/`mmap` the arena; **delete** `materialize_types`,
   `cir_forest_fill_type_records`, `forest_restore_decls`'s per-category body,
   `flush_forest_pending_globals`'s per-category body, and the 3 delete-segments. Build
   `SEG_DEFS` in declaration order (RC1) and include free functions (RC2).
5. **Gate throughout:** `make -C src fulltest` + `scripts/forest_bind_gate.sh` +
   `bin/test_cir_freeze` (rewrite ~16 cases against the arena) + torture byte-identity. The
   whole-`<string>`-TU `MADC_DUMP_MIR` becomes byte-identical to live **by construction**
   (closes #23); the strbind gate then asserts whole-TU identity.

## 8. Open questions / risks (resolve before/within step 2)

- **Handle representation:** is `DataDef` a value-view constructed on demand, or a persistent
  object owning `(arena,idx)`? Persistent objects (one per record, created at
  alloc/load) preserve identity comparisons (`a == b` pointer-equality is used widely) — likely
  required. Then `arena.alloc` returns a stable handle pointer; load creates one handle per
  record in a parallel vector. (Cheaper than it sounds: handles are tiny + uniform.)
- **Identity/pointer-equality** sites (`if (a == b)` on `DataDef*`, `this == target`): must map
  to index-equality. Count/verify these during step 2 (a subset of the read surface).
- **Public-field reads** (`->size`, `->name`, `->members[i]`): confirm the write-through-cache
  approach (§5) covers them without touching read sites; enumerate the exceptions.
- **`FuncDef` builder** is net-new API surface — design it in step 2 alongside the ~64
  `parameters` mutation conversions.
- **Const-view vs mutation during codegen:** verify codegen only *reads* DataDefs (scoping §3b
  said the runtime/library layer is 0-coupled; confirm `cir_builder` codegen paths don't mutate
  type objects mid-emit).

---

*Grounded in: `include/madcdis/pod_record.h`, `include/datadef.h` (DataDef/STRUCT/CLASS +
element structs), `include/cir_freeze.h` (existing type records), and the five scoping sweeps
(2026-07-06). Field-level completeness is finalized in step 1's header; step 2 exercises it.*
