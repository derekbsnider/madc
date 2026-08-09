# FLR random access & the struct-schema arc (2026-08-08)

## Status

- **S1 + S2 SHIPPED 2026-08-08** (`feature/flr-random-access-claude`):
  SeekableDataChannel with positioned transfers; FLR lazy open, streaming
  cursor, O(1) `record_index` locator lookup, positional update; the
  capability-truth gate (`tests/unit/test_driver_capability_truth.cpp`,
  running inside `fulltest`'s unit stage). Two refinements over the
  original sketch, both strengthening it:
  - The seekable mixin carries `read_at`/`write_at` (pread/pwrite shape)
    besides `seek`/`size` — a shared channel with only seek+read is a
    positional race the moment a cursor and a point lookup coexist.
  - The read-counting shim required a real injection seam, so it landed
    as a first-class feature: `ChannelBackedDataDriver::open_on_channel`
    opens a record driver over ANY seekable channel (an embedding host's
    memory image, or the gate's counting channel). Proof machinery and
    the records-over-memory story are the same code.
- **S3 is next**: locator-as-column pushdown + the VLR offset sidecar
  (VLR deliberately does NOT claim `locator_lookup` until then — the
  gate's claims table pins that).

## Motivation

The v0.70.0 review established that the record-file drivers never got
the access shape their format promises: FLR materializes the ENTIRE
file at `open()`, implements no locator access at all, and VLR's
locator lookup is a linear search over materialized rows — while the
`RecordLocator` API and the plan's "point lookup (by record locator)"
capability line have existed all along. A fixed-length-record file is
the one format where random access is free arithmetic
(`offset = index * record_size`) — no index needed. That gap
(interface declared, implementation eager) is also the exact failure
mode this plan is structured to prevent from recurring.

## Design requirements

| Requirement | What it means here |
|---|---|
| The record's position is a first-class schema concept | `RecordLocator::kind::record_index` + a schema-declared locator column FLR/VLR expose on every row — not a side API |
| Point access and iteration are ONE protocol | Seek-based `get_record_by_locator` (one seek + one read) and the streaming cursor share the same channel/driver surface; a seekable cursor is `seek_to(index)` then `next()` |
| Constant-memory access at every entry point | `open()` reads no records; scans stream; point lookups touch one record |
| A data file may carry a header region | `SchemaInfo` region metadata: `data_offset`, `max_records`, validated at open and honored in every offset computation |
| One queryable namespace over heterogeneous per-table backends | The declarative data catalog (Level 2): table -> DataSource URI bindings over the existing scheme registries |
| Native binary files are queryable in place | C-struct-as-schema (Level 2): the producer's own header IS the schema; no import step, no DDL |
| In-place record update | Positional single-record write (seek + write) on FLR; no whole-file rewrite |
| Atomic whole-file rewrite where position is not stable | Stream-to-`.new`-then-rename for DSV/VLR compaction (already the shape; keep and gate the rename atomicity) |
| Fixed-width set/enum/time columns | Fall out of C-struct-as-schema for free (C bitfields + enums); no bespoke type grammar |

What the substrate already provides: the Query IR, SchemaInfo, the
scheme→factory registries, capability-probed streaming seams,
SIGPIPE/cloexec-disciplined channels, the tombstone/compaction
machinery, a real test/gate culture — and a C/C++ compiler front end
that computes exact struct layouts. The plan spends those assets.

## LEVEL 1 — true random access on the v0.70.0 seams

**L1.1 — `SeekableDataChannel` extension mixin** (pattern:
`DatagramDataChannel`): `seek(uint64_t)`, `size(uint64_t &)`, and the
positioned transfers `read_at`/`write_at` (pread/pwrite) that carry
their own offset and never disturb the sequential position — cursors
and point lookups can share one channel without a positional race.
Implemented by the file channel (fstat/lseek/pread/pwrite) and by
`MemoryDataChannel`; the dormant `ChannelCapabilities.seek` flag
finally means something and is set truthfully per fd (`S_ISREG`, never
for `O_APPEND`). Sockets/FIFOs/processes never claim it.

**L1.2 — FLR reborn lazy.** `open()` = open the channel + stat +
size-multiple validation + load the tombstone BITMAP (small) — zero
records read. All record access flows through:
- `scan_stream()` — sequential cursor, one record per `next()`,
  tombstones skipped; constant memory.
- `get_record_by_locator()` — `record_index` kind: one seek to
  `data_offset + index*record_size`, one read, one decode. O(1).
  `byte_offset` kind accepted when it is record-aligned.
- Positional `update_record`/`insert_record` — seek + write of ONE
  record; append = seek to end. `erase`/`restore` keep the tombstone
  sidecar semantics unchanged.
- The legacy `scan_records` vector API remains (delegates to the
  cursor), per the ABI-compatibility rule.

**L1.3 — the locator is a schema-visible column.** A dataset over FLR
can expose the record index as a queryable pseudo-field, so
`where id = 12345` becomes a point lookup the planner can push down
instead of a scan. Pushdown eligibility rides the existing
`can_stream_query`/capability surface.

**L1.4 — VLR stops pretending.** VLR's offset table becomes a
persisted sidecar (built on first full scan, invalidated by mtime/size
change); `get_record_by_locator(byte_offset)` becomes seek + read of
the self-delimiting record. Compaction keeps the write-`.new`-then-
rename atomic shape.

**L1.5 — region metadata.** `SchemaInfo` gains `data_offset` and
`max_records`; FLR validates
`(size - data_offset) % record_size == 0` and respects both in every
offset computation. A header region maintained beside appended records
(running counts/aggregates) becomes expressible, feeding the
derivation-relations track (5A.11) later.

**L1.6 — the capability-truth gate**
(`tests/unit/test_driver_capability_truth.cpp`, running inside
`fulltest`'s unit stage). The gate cross-examines advertised
capabilities against behavior: `locator_lookup` (a stronger claim than
`point_lookup`) must serve a locator hit as positioned access WITHOUT
reading the whole file — measured through a read-counting channel
injected via `ChannelBackedDataDriver` — and a claim with no injectable
seam behind it fails as unprovable; a streaming driver must serve the
first row without draining. A `sizeof(DriverCapabilities)` ratchet
fails the build when a capability field is added without a truth leg,
and the core schemes' exact claims are pinned in a table so any claim
change must visit the gate. **No capability may be advertised that the
gate does not exercise.** This is the structural fix for
interface-without-implementation: a hollow claim is a build failure,
not a review finding.

## LEVEL 2 — the leap that is uniquely madc's

**L2.1 — C-struct-as-schema (zero-DDL binding).** Every external tool
in this space makes users redeclare a binary format's fields by hand
(name, width, offset) — a copy of the truth that drifts. madc IS a C
compiler: a struct declaration already carries exact sizes, offsets,
bitfield packing, and enum values. So the producer's OWN header
becomes the schema:

```c
#include "telemetry.h"           // the format owner's real header
auto log = madc::dataset::bind<struct sensor_rec>("flr:///var/log/sensors.db");
for ( const sensor_rec &r : log.where(field(&sensor_rec::station) == 7) )
    ...
```

Mechanics: a `SchemaInfo` FROM a madc type — record_size = sizeof,
field offsets = offsetof, bitfields → set semantics, enums → enum
semantics, nested structs flattened with qualified names. This is
"parse the real headers" applied to data: no DDL, no drift from the
format owner's truth. SMAUG's binary on-disk structures are the first
honest in-repo customers. Constraints stated up front: same-ABI
assumption (producer and reader share arch/endianness/packing) is
DOCUMENTED and checked where possible (magic/size probes); pack
pragmas and cross-endian views are follow-ons, not silently wrong.

**L2.2 — the data catalog.** Binding a NAME to a typed source lands as
a first-class madc object, not a bespoke config grammar:

```c
madc::catalog game;
game.bind<area_rec>("areas", "flr://" + dir + "/areas.db");
game.bind("names",   "dsv://" + dir + "/names.db", names_schema);
game.bind("notes",   "qdbm://" + dir + "/notes.qdb", kv_schema);
```

One catalog = one queryable namespace over heterogeneous backends,
feeding directly into the roadmapped capability-aware planner (5A.9)
when cross-table queries arrive. A declarative text form can be
layered later; the API is the owner.

**L2.3 — typed cursors as language surfaces.** The first real Track 5C
customer: `bind<T>` datasets are range-`for` iterable (streaming
cursor under the iterator), `where`/`select` build Query IR lazily,
and the row type is the USER'S struct — no `value` boxing on the hot
path for FLR (decode straight into `T` when the schema came from `T`).

**L2.4 — maintained summary regions.** With L1.5's region metadata, a
header summary region (counts, high-water marks, running aggregates)
can be maintained transactionally with appends — generalized later by
derivation relations (5A.11, keyframe aggregation) rather than
special-cased per format.

## Execution slices (each vertically complete: code + tests + gate + doc)

| Slice | Content | Ships with |
|---|---|---|
| S1 ✅ | SeekableDataChannel + file/memory impls + seek capability truth | channel contract tests (regular file / FIFO / append / memory legs) |
| S2 ✅ | FLR lazy open + streaming cursor + locator point lookup + positional update + `open_on_channel` seam | locator dataset tests + the capability-truth gate (read-counting O(1) proof, claims table, sizeof ratchet) |
| S3 | Locator-as-column pushdown + VLR offset sidecar | query tests + gate leg |
| S4 | C-struct-as-schema (`SchemaInfo` from madc type; `bind<T>`) | reducer suite vs g++-computed offsets (the oracle IS offsetof) |
| S5 | Catalog object + range-for typed cursors | integration tests; a SMAUG binary structure as the real-data fixture |

Rules binding every slice: no interface lands without a working
implementation AND a gate that fails on a hollow claim (L1.6); the
legacy vector APIs stay ABI-stable; every capability flag is truthful;
`fulltest` + the battery gate each merge per the standing cadence.

## Non-goals / stated-not-solved

- Cross-endian / cross-ABI file views (documented same-ABI assumption;
  probes where the format provides them).
- Concurrent multi-writer safety (single-writer + advisory locking is
  a later slice; mmap/shm pool work is 5A.8's).
- Secondary indexes (keyed lookup beyond position stays on the keyed
  backends; index sidecars are a follow-on).
- The SQL/GQL textual surface (5A.10/5C.7) — the catalog and Query IR
  are its substrate, not its grammar.
