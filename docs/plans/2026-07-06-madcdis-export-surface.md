# madcdis Export Surface — make the substrate reusable through libmadc and the madc language

**Date:** 2026-07-06 · **Status:** design (implementation not started) · **Owner-directed.**

Sibling of `docs/plans/madcdis-plan.md` (the umbrella substrate plan) and
`docs/plans/2026-06-12-type-table-value-abi-design.md` (the value/type-id ABI).
This doc is narrower: it is about **exposing what `madcdis` already is** — a
general, position-independent serialization/data substrate — as reusable
functionality for (1) C++ hosts embedding `libmadc` and (2) madc scripts, and
about the near-term **helper-sharing refactor** that both DRYs the forest
save/load code and produces the first cleanly-exportable `madc::dis` primitive.

## Principle (owner, 2026-07-06)

> Export as much of `madcdis` as is coherent. Anything clever and useful to the
> **development of madc** is likely useful in reusable form to **users of madc**.

This is not a new direction — `madcdis-plan.md` Principle 1 / "Name and Boundary"
already state the substrate "ships as part of core `libmadc`" and is "available to
every Mad-C program without an opt-in dependency." This doc operationalizes that
for the pieces that already exist, and sets the discipline for new ones.

Consequence for how we write code from now on: a reusable serialization/data
helper is authored as a **public `madc::dis` class from the start** (clean
interface, no `DataDef`/parser knowledge), not as a file-static internal in
`madc_cir.cpp` / `cir_freeze.cpp`. `madc::dis` stays **DataDef-agnostic** so it
remains a general substrate; the madc-specific glue (typeid ⇄ `DataDef*`) lives on
the madc side.

## What ships today (the concrete export candidates)

Verified present in `include/madcdis/` (currently internal to `libmadc`):

| Primitive | Header | What it is | Reusable-for-users value |
|-----------|--------|-----------|--------------------------|
| `intern_table` / `frozen_intern_table` | `intern_table.h` | index-linked string/identifier interner, three zero-fixup blocks | byte-level dedup; a symbol table any script can use |
| `arena` (`TokenArena` HAS-A) | `arena.h` | bump allocator, pointer-stable, drop-all | bulk-lifetime allocation |
| `id_table<T>` | `id_table.h` | segmented stable `uint32`-id ⇄ object registry (the type table's spine) | stable-id handles that survive save/load |
| `value_pool` | `value_pool.h` | deduping `uint32` handles over wide-literal limbs | compact value storage |
| `snapshot_writer` / `snapshot_reader` | `snapshot.h` | the container: header / 16-aligned compressed segment frames / directory / footer; standalone file **or** appended to a binary | a portable, versioned, compressed serialization container |

Already public: the 32-byte `madc::value` (`include/libmadc/value.h`) — the ABI
of `2026-06-12-type-table-value-abi-design.md`; its type_tag is the `id_table`
typeid. So one leg of the substrate (values) is already script/host-facing; the
rest (interning, id tables, snapshot container, arenas) is not yet.

Still planned (do NOT export yet — they don't exist): the `Pool`/`PoolPtr`/`shm://`
subsystem, `array`/`map`/`set`/`tuple`, `DataSet`, query IR/planner (madcdis-plan
V1–V7). This doc covers exporting the **built** primitives + the new record helper;
the big substrate lands on its own track and exports as each piece becomes real.

## Export mechanism (governed by `.claude/rules/cpp-first-api.md`)

C++ first, C shim last, script binding via embedded header:

1. **C++ public API (the single real implementation).** Promote the `madcdis`
   headers from internal to a public tier of `libmadc` (a `libmadc/dis.h`
   umbrella that re-exports the stable subset, or move the headers to a public
   include path). Keep ownership/lifetime/diagnostics modeled in the C++ objects.
2. **Thin `extern "C"` shim** — the C-host API only, thin wrappers over the C++
   layer (never a script-side resolution path).
3. **madc-language embedded header** (`include/madc/…`, script-facing): declare the
   `namespace madc { namespace dis { … } }` publics as **declaration-only C++**,
   resolved **mangled-direct** (Itanium symbols) to the real implementations in
   the host — the `<ns_madc>`/`<ns_php>` discipline. No wrapper bodies over the
   extern-C exports (that flattens references/overloads).

## Step 1 (near-term, the helper-sharing refactor) — the entry point

**✅ LANDED (2026-07-06).** `include/madcdis/pod_record.h` — the first public
`madc::dis` serialization primitive: `pod_words<T>()` (record stride in uint32
words), `pod_append(buf, rec) -> off` (append a POD record's words, return its
start word offset), `pod_read(buf, off, out) -> bool` (bounds-checked read at a
word offset). Header-only templates, DataDef-agnostic, `static_assert` that the
record is a whole number of uint32 words. The forest save side
(`cir_forest_serialize_members` / `_bases` / `_append_methods`, madc_cir.cpp) and
load side (the member / base / method reads in `CirFrozenForest::materialize_types`,
cir_freeze.cpp) now go through it — the hand-rolled copy loop + offset/bounds
arithmetic is gone. The madc-side `type_id → DataDef*` swizzle is extracted as
`forest_swizzle_type` (a file-static in cir_freeze.cpp, DataDef-aware, NOT a `dis`
export), replacing the ~6 inline `primitive-or-by_id` lookups (pass-1b operand,
member type, base type, method return, method param, typedef underlying).
Round-trip unit test: `tests/unit/test_pod_record.cpp`. Byte-identity held across
every gate (see below) — the wire format is unchanged, so #13 builds on the shared
primitive. `_record_derived` was noted here as a save site but records into the
typed `type_records` vector (bulk-`memcpy`'d whole), not the flattened uint32
payload, so it needs no per-record codec.

The forest save/load repeats two **general, DataDef-agnostic** patterns that are
the natural first `madc::dis` serialization primitive:

- **Save:** "append a fixed-stride POD record's words to a `uint32` payload" —
  copied in `cir_forest_serialize_members` / `_bases` / `_append_methods` (records
  *and* param runs) / `_record_derived`.
- **Load:** "read a fixed-stride POD record at word-offset N, bounds-checked" —
  copied ~5× in `CirFrozenForest::materialize_types`.

**Extract → `madc::dis` typed POD (de)serialization over a `uint32` buffer**
(e.g. `dis::pod_append<T>(buf, rec) -> off` and `dis::pod_read<T>(buf, off, out)
-> bool`, or a small `dis::u32_record_view`). Requirements:
- Byte-identical output: same word order / stride as today (the gates below prove it).
- No `DataDef` knowledge — pure buffer + POD.
- Public/exportable shape from the start (this is the first `madc::dis` utility to
  flow through the export mechanism above).

Separately, extract the **madc-side** `type_id → DataDef*` swizzle
(`primitive-via-madc_type_from_id else by_id`, repeated ~5× in `materialize_types`)
into one madc helper. This stays on the madc side (it is DataDef-aware) — it DRYs
the per-kind code but is NOT a `dis` export.

## Phasing

1. **Step 1 (above):** extract the `dis` POD-record primitive + the madc-side
   swizzle helper; refactor the forest save/load onto them. Gated (see below).
   *This is the concrete "share the per-kind code" work.*
2. **Export the built primitives** via the mechanism above: `intern_table`,
   `id_table`, `value_pool`, `snapshot`, the new record primitive → libmadc C++
   public API → C shim → madc embedded header. Each with a round-trip test.
3. **madc-language ergonomics:** a script can intern strings, build an id table,
   write/read a snapshot container — the "save/load state" primitives users asked
   for, now first-class.
4. **Later:** the madcdis-plan V1+ substrate (pools, collections, datasets, query)
   exports piece-by-piece as each is built.

## Gates (the refactor's safety net — must stay byte-identical)

The Step-1 refactor changes only *how* the same bytes are written/read, so:
- `test_cir_freeze` (byte-level record assertions) unchanged.
- `forest_bind_gate` all cases: `MADC_DUMP_MIR` byte-identical to live.
- gcc-torture 50-name failset byte-identical, 0 timeouts (compile path untouched).
- `make -C src fulltest` green, no new warnings.

If any byte-level gate moves, the extraction changed the format — revert and re-do.

## Non-goals for this doc

- Not re-litigating "keep the objects, table the ids" — DataDef stays polymorphic
  and heap-resident (see `2026-06-12-type-table-value-abi-design.md` lines 37–40).
- Not building the V1–V7 substrate here; that is the madcdis-plan's own track.
