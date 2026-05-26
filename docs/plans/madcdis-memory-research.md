# madcdis Memory Management Research

## Purpose

This document grounds the `madcdis` substrate design in production
references and historical lineage. It is the research foundation for
the architectural choices in `madcdis-plan.md`.

The core thesis: a typed in-memory data substrate is best built as a
unified system that treats **structural redundancy** as a first-class
concern. Pool allocation, reference counting, interning, object
deduplication, copy-on-write, column encoding, and derivation-based
aggregation are all instances of the same architectural principle —
*store the structure, not the redundancy* — applied at different
scales.

## Design lineage

### SMAUG (1990s-present) — the load-bearing reference design

SMAUG (Simulated Medieval Adventure Multi-User Game), a MUD codebase
created by Derek Snider, is the longest-running production proof of
the unified-memory-management approach. Its discipline:

**String hashing (predates SMAUG, inherited from Merc).** A global
hash table holds canonical instances of every distinct string in the
game world. Identical strings share storage automatically. Room
descriptions, mob names, item titles — all deduplicated.

**Reference counting (added by Derek Snider).** A link-count field on
each interned string allows safe reclamation when the last reference
drops. The "permanent" tier (link count saturated at 65535) covers
strings that should outlive everything. The pattern is documented in
`hashstr.c` and has run in production for 30+ years.

**Object deduplication with multiplicity counts (also Derek Snider's
addition).** Identical objects in a collection collapse to a single
canonical instance plus a multiplicity count. A player carrying 100
identical healing potions stores one potion record with `count=100`,
not 100 separate records. This is a generalization of string interning
to arbitrary structured types.

**Pool-allocated game world.** Rooms, mobs, objects, exits, resets,
shops, specials — all allocated from dedicated pools per area, with
bulk reload-and-drop semantics tied to the area reset cycle. Per-area
lifetime management without per-object cleanup.

**Proto-mob and proto-object patterns.** Canonical "prototype" records
define the template for each entity type; live instances are cheap
copies. This is the entity-component-system pattern that game
engineering rediscovered in the 2010s, shipped by SMAUG in 1990.

These techniques compose. A potion's name is an interned string with
a refcount. The potion record itself is an instance of a prototype
with possible multiplicity-count deduplication when stacked. The
prototype is in a pool that reloads on area reset. Multiple layers,
one coherent system, decades of production validation.

### Lucene / Elasticsearch — the column-encoded validation

Lucene's doc-values format and Elasticsearch's LogsDB/TSDS work
demonstrate the same principles at petabyte scale for log workloads.

**Per-column codec selection.** Lucene picks an encoding strategy per
field based on the data's characteristics. The cascade for numerics:

1. If all values differ, store raw
2. If values fit in 8 bits, use a simple encoding table
3. If values share a greatest common divisor, store base plus offsets
4. Otherwise, use bit-packed delta encoding

This is automatic encoding selection based on observed value
distribution. Same principle as interning (find the redundancy and
exploit it), generalized to numeric structure.

**Dictionary encoding for keyword fields.** Repeated string values
in a column are stored once in a dictionary; the column holds integer
IDs. This is hashstr applied per-column rather than globally — same
mechanism, different scope.

**Index sorting unlocks order-dependent codecs.** Sorting documents
by `(host, timestamp)` enables run-length encoding on the host field
and delta-of-delta encoding on timestamps. Sort order becomes a
compression strategy, not just a query optimization.

**Synthetic _source.** Recent Elasticsearch versions skip storing the
original JSON document and reconstruct it on demand from the encoded
columns. The encoded form is canonical; the original is implied.

**Time-tiered storage with codec changes per tier.** Hot tier uses
fast codec (LZ4); warm tier reindexes with best_compression
(DEFLATE). Different storage characteristics, different cost/speed
tradeoffs, same underlying columnar layout.

The combination of these techniques is why Elasticsearch can ingest
terabytes of logs per day and query them interactively — small
storage *and* fast queries simultaneously.

### Columnar database family (Vector, Kinetica, ClickHouse, SingleStore)

The compression techniques are uniform across modern columnar systems:

- **Dictionary encoding** — categorical fields with bounded value
  domains
- **Run-length encoding** — sorted columns or columns with long
  identical runs
- **Frame-of-reference plus bit-packing** — bounded numeric ranges
  (timestamps, prices, IDs)
- **Delta encoding** — smoothly-varying numeric sequences
- **Prefix compression** — sorted strings with shared prefixes
- **GCD compression** — numerics with a common divisor (timestamps
  with second/minute granularity)

Results: 5-30x storage reduction on typical analytical workloads,
30x+ on low-cardinality columns, all with *faster* query execution
because operations run on encoded form without materialization.

The cache and bandwidth effects are causal: smaller data fits in
faster cache tiers, modern CPUs are bandwidth-bound, encoded
operations are SIMD-friendly. Smaller storage *causes* faster
execution, it doesn't trade against it.

### Modern arena allocator research

Ryan Fleury's arena writing, the BPF arena work in the Linux kernel,
and the shared_arena ecosystem in Rust have crystallized the modern
discipline:

- Arena/bump-pointer allocation is the default; per-object free is
  the exception
- Most data has bulk lifetimes that map naturally to arena scopes
- Page-level reference counting enables reclamation without
  per-object overhead
- Position-independent pointers within arenas enable cross-process
  sharing, serialization, and compaction
- Composition is the discipline: arenas, slabs, refcounting,
  interning combine as needed per workload

### Reference counting at language-runtime scale

CPython, PHP zvals, Swift, Objective-C ARC, and Rust's `Rc<T>` all
demonstrate refcounting as the standard answer for shared immutable
data in a language runtime. The pattern is universal:

- Construction increments
- Destruction decrements
- Zero count releases
- Cycles need separate handling (weak references or cycle detection)

Performance is well-understood: a few cycles per increment/decrement,
predictable cost, no pauses. The cycle limitation is the only real
weakness, mitigated by weak references in most practical cases.

### Copy-on-write at the page level

Linux fork() pattern: pages marked read-only and refcounted; on
write, allocate a new page and update mappings. The kernel handles
the bookkeeping; applications get instantaneous snapshot semantics.

Redis uses this for BGSAVE: fork() the process, child sees a frozen
snapshot via COW, writes the snapshot to disk while the parent
continues serving requests. Production-proven for in-memory databases.

LMDB uses mmap'd files with COW within the database file itself:
readers see consistent snapshots without locking, writers append to
a copy. The disk file format is the canonical format; memory is just
the cache.

### Event sourcing with snapshots

The pattern of "append-only event log plus periodic state snapshots,
reconstruction starts from nearest snapshot" appears in many forms:

- Database WAL plus checkpoints
- Event sourcing with aggregate snapshots
- Time-series databases with downsampling (Prometheus, InfluxDB)
- Git's pack files (loose objects periodically packed against bases)
- Video codecs with keyframes and inter-frames

The unifying insight: any state at time T is reconstructible from
"nearest snapshot before T plus events from snapshot to T." Old
events can be safely discarded once their information is captured in
snapshots. Storage stays bounded; query capability for historical
periods is preserved at coarser granularity.

## The unifying architectural principle

All of these techniques are answers to one question:

**The data has structural redundancy. How do we represent the
structure efficiently rather than the redundancy?**

They differ in what kind of redundancy they exploit:

| Technique | Kind of redundancy |
|---|---|
| String interning | Same byte sequence appears N places |
| Object dedup (SMAUG potions) | Same object state appears N times in collection |
| Reference counting | Same resource owned by N holders |
| Dictionary encoding | Same value appears N times in column |
| Run-length encoding | Same value appears N consecutive times |
| Frame-of-reference | Values cluster within a bounded range |
| Delta encoding | Adjacent values are close |
| Keyframe aggregation | Many events between checkpoints |
| Copy-on-write | Many readers, few writers |
| Pool allocation | Many allocations share a lifetime |

All produce the same kind of win: less storage, faster operations on
the storage, queryability without full materialization. They compose
because they operate at different scales — pool allocation manages
bulk lifetimes, interning eliminates byte-level redundancy, column
encoding compresses field-level repetition, keyframes summarize
time-window aggregations.

A substrate that takes this principle seriously and provides the full
toolkit as composable primitives is qualitatively more powerful than
one that picks a few techniques. The goal for `madcdis` is the full
toolkit, integrated coherently, with the user picking which
combinations apply to which data.

## Layered architecture

The substrate has a layered structure where higher layers depend on
lower ones, and each layer addresses redundancy at its appropriate
scale.

### Layer 0: Memory pools

Pools are regions of memory with allocation discipline. Multiple
kinds:

- **Arena pool** — bump-pointer allocation, O(1) drop
- **Slab pool** — fixed-size cells, O(1) alloc/free per cell
- **Size-class pool** — multiple slabs, routing by size
- **Intern pool** — backs the intern tables (specialized slab)

Pools are characterized by:

- Backing memory (heap, shared memory, mmap'd file)
- Growth policy (fixed, growable)
- Lifetime mode (manual drop, page-refcounted reclamation)

All pointers within pools are position-independent (base plus offset).
This enables cross-process sharing in `shm://` pools, snapshot
serialization, and compaction.

### Layer 1: Value representation

Every `madc::value` is internally a small structure:

- **Inline scalars** — small ints, bools, nil, small enums stored
  directly in the value bits (NaN-boxing style, ~64 bits total)
- **Heap pointers** — pool ID plus offset into the pool, for anything
  too large to inline

The inline/heap distinction is invisible to user code. Operations on
values dispatch on the type tag; inline values skip the heap layer
entirely.

Heap-allocated values have a header containing type tag, refcount,
hash (if interned), flags (permanent, interned, frozen). The payload
follows the header.

### Layer 2: Reference counting

Every heap-allocated value has a refcount. Construction increments;
destruction decrements; zero count releases the slot back to the
pool.

Refcount discipline:

- **Saturating** — counts have a maximum (e.g., 16 or 32 bits);
  reaching the max marks the value as permanent (no further counting)
- **Atomic for shared pools** — `shm://` pools use atomic
  inc/dec; process-local pools use non-atomic for speed
- **Permanent tier** — explicitly marked values never count; useful
  for literals, schema metadata, type tags

The SMAUG hashstr pattern is the reference design here, generalized
from strings to all heap values.

### Layer 3: Interning and deduplication

Per-type intern tables provide structural dedup. Construction of an
interned-type value:

1. Compute hash of content
2. Look up in the intern table
3. If found, increment that slot's refcount and return its handle
4. If not found, allocate, store, register in table, return handle

Interning is opt-in per type with sensible defaults:

- **Strings** — interned by default (the hashstr discipline)
- **Symbols** — interned by definition (always canonical)
- **Small immutable structs** — interned by annotation
- **Arrays** — not interned by default (typically unique)
- **Large composites** — not interned by default

Beyond per-value interning, collections support **multiplicity-based
deduplication**: a `madc::array` can be configured to collapse
identical adjacent elements into `(value, count)` pairs. This is the
SMAUG potion-stack pattern, generalized.

The relationship between these:

- **Interning** removes byte-level/structural redundancy across the
  program
- **Multiplicity** removes redundancy within a single collection

Both compose. An array of 100 identical interned potion values uses:
one interned potion (one allocation, refcount=100 from the array)
plus one multiplicity record (`(potion_handle, 100)`). A massive
saving versus 100 separate potion records.

### Layer 4: Collection types

`madc::array`, `madc::map`, `madc::set`, `madc::value` (as composite)
are built on layers 0-3. Their elements are value handles; identical
elements automatically share storage via interning.

Optional per-collection compression:

- **Multiplicity collapse** (consecutive identical elements)
- **Dictionary encoding** (column-scale interning for arrays of
  values)
- **Run-length encoding** (sorted arrays with runs)
- **Frame-of-reference plus bit-packing** (numeric arrays with
  bounded ranges)
- **Delta encoding** (smoothly-varying numeric arrays)

These match the columnar database encoding catalog. They activate
opt-in based on the collection's declared usage pattern (append-mostly
log, sorted index, random-access record).

### Layer 5: Datasets and relations

`DataSet<T>` builds on collections. Two storage modes:

- **Row-oriented** — records stored as packed structs; fastest random
  access and updates
- **Column-encoded** — fields stored in separate columns with
  per-column encoding; fastest scans and aggregations, smaller
  footprint

Both modes participate in the value system underneath. Row-oriented
datasets benefit from interning of repeated field values
automatically. Column-encoded datasets explicitly compress with the
encoding catalog.

`Relation<A, B>` defines the relationships between datasets:

- **Key match** — foreign-key style
- **Positional** — by index
- **Offset** — by pool offset (legacy locator support)
- **Graph edge** — for graph queries
- **Derivation** — A is derived from a time/range of B

The derivation kind is new and load-bearing for the keyframe pattern.

### Layer 6: Snapshot and persistence

A pool's contents can be serialized to disk and restored. Because
everything inside is position-independent and self-describing, this
is largely a memcpy plus header.

Snapshot modes:

- **Direct snapshot** — write pool bytes to a stream
- **COW snapshot via fork()** — child process sees frozen pool,
  writes snapshot, exits; parent continues serving (Redis BGSAVE
  pattern)
- **mmap'd persistence** — pool is mmap'd from a file; OS page cache
  handles hot/cold pages transparently

Restore is the reverse: read pool bytes back, validate header,
register in the pool registry, attach the address-space mapping.

### Layer 7: Derivation and tiering

`DerivationRelation<Source, Derived>` captures the keyframe pattern.

A derivation declares:

- Source dataset (raw event stream)
- Derived dataset (keyframe summaries)
- Period (how often keyframes are generated)
- Derive function (Source range to Derived summary)
- Coverage (which source range each keyframe captures)

The system:

1. Generates keyframes periodically by applying the derive function
   to recent source data
2. Tracks which source rows are captured by which keyframes
3. Rewrites queries to use the appropriate tier based on time range
   and required granularity
4. Prunes source rows whose information is fully captured in
   keyframes, according to retention policies

This is event sourcing with automatic checkpoint management,
integrated with the substrate's storage and query layers.

### Layer 8: Federation

The federated planner spans:

- Multiple `mem://` pools
- Multiple `shm://` pools
- mmap'd disk-backed pools
- External drivers via `madcdat` (relational DBs, graph DBs, etc.)
- Derivation tiers (hot raw data plus warm keyframes plus cold
  archives)

Queries cross layers transparently. The same `Cursor<T>` API works
whether the data is in a process-local pool, a shared-memory segment,
an mmap'd file, or a remote database — capability-aware pushdown
handles the differences.

## What's specifically borrowed and from where

| From | Adopted technique |
|---|---|
| SMAUG hashstr | String interning plus refcount plus permanent tier |
| SMAUG object dedup | Multiplicity counts for collection elements |
| SMAUG proto-mob/object | Prototype plus instance pattern for typed records |
| SMAUG area pools | Bulk-lifetime pool reload semantics |
| Lucene doc-values | Per-column codec selection, sorted-index unlocks RLE |
| Elasticsearch synthetic _source | Encoded form is canonical, original implied |
| Vector / X100 | Vectorized operators on encoded data |
| Kinetica | Tiered storage with placement decisions |
| ClickHouse / SingleStore | Operations on dictionary-encoded data |
| BPF arena (Linux) | Page-level refcounting for arena reclamation |
| Redis BGSAVE | fork() plus COW for consistent snapshots |
| LMDB | mmap'd file as canonical storage format |
| Modern arena research | Arenas as default, per-object free as exception |
| CPython / Swift / Objective-C | Refcounting as language runtime discipline |
| Event sourcing literature | Keyframe-based reconstruction from event logs |
| Prometheus / InfluxDB | Downsampling and retention tiers |

The composition of these into one coherent system is what's
distinctive. Each is well-understood individually; integrated as
layers of one substrate, they reinforce each other.

## What's deliberately not included

Worth being explicit about what `madcdis` is *not* trying to be:

- **A general-purpose tracing garbage collector.** Refcounting plus
  weak references plus pool drop handle the cases. Tracing GC is
  reserved for opt-in pool policies if a workload genuinely needs
  it.
- **A vectorized analytical execution engine** (like Vector or
  Kinetica). The encoding catalog enables vectorized execution if a
  user wants to write it, but `madcdis` itself is row-oriented at
  the cursor level. SIMD kernels can be added as optimization later.
- **A distributed database.** Single-machine, multi-process via
  `shm://`, plus external federation through `madcdat`. Cross-machine
  distribution is out of scope.
- **A full-text search engine** (like Elasticsearch). Inverted
  indexes for tokenized text and BM25 ranking are not in scope.
  Structured queries on encoded log data are.
- **A schema migration tool.** Schema evolution support exists at
  the encoding level (versioned encoders); managing schema changes
  in long-lived persistent stores is application territory.

These exclusions keep the substrate focused on its actual value: a
unified memory-management system for typed structured data, with
exploitation of structural redundancy as the architectural backbone.

## Performance expectations

Honest expectations for the various layers:

**Pool allocation:** 5-10ns per allocation in the common case (slab
or bump pointer). Faster than the system allocator by 3-10x. Drop is
O(1) regardless of pool size.

**Refcounting:** 1-3ns per inc/dec for non-atomic counts (process-
local pools). 10-30ns for atomic counts (`shm://` pools).

**Interning:** Hash lookup is ~30-50ns for a hit. New insertion is
~100ns plus allocation. For hot-path interning of common strings,
results in dramatic memory savings with negligible time cost.

**Multiplicity dedup:** Equality check plus count increment is ~20ns
for adjacent-element collapse.

**Column encoding (dictionary):** 30x+ compression on
low-cardinality columns; filter operations 10-20x faster than
row-oriented equivalent because they operate on packed integer IDs.

**Snapshot:** Direct memcpy speed (~10-30 GB/s on modern hardware).
COW fork() snapshot is essentially free for the running process; the
child does the I/O work.

**Derivation:** Background work, doesn't affect query latency.
Keyframe-aware query rewriting may be 100-1000x faster than scanning
raw events for historical periods.

These are the targets. Actual measurements will validate or refute
them, and the design accommodates incremental optimization without
architectural changes.

## Open questions

- What's the right inline-value size for `madc::value`? 64 bits with
  NaN-boxing fits most numerics, small strings (up to ~7 bytes
  inline), and pointers. Larger inlines (128 bits) help more cases
  but double the value-passing cost.
- How aggressive should default interning be? Strings should
  certainly be interned by default; what about small numeric arrays,
  enum-like values, time stamps?
- When does multiplicity dedup activate? Always, on opt-in, or
  automatically when the collection sees enough duplicates?
- What's the right interaction between refcounting and cross-pool
  references? A value in pool A referenced by a value in pool B
  needs careful handling.
- How does the snapshot format handle schema evolution? Pool headers
  need versioning; encoders need backward compatibility hooks.
- What's the right concurrency model for `shm://` pool writes? V1
  probably restricts to single-writer/multi-reader; later phases
  could allow concurrent writers with appropriate locking.

These are real design questions that need answers as implementation
proceeds. The architecture accommodates multiple answers; the
specific choices come from concrete workload requirements.

## The bigger picture

`madcdis` as designed here is not "yet another in-memory data
library." It's a coherent substrate that takes the techniques scattered
across hashstr, Lucene, Elasticsearch, Redis, LMDB, ClickHouse, and
the modern arena research, and integrates them into one system at
the language level.

The user writes Mad-C code with typed structs and typed
relationships. The substrate handles:

- Efficient typed allocation from appropriate pools
- Automatic structural deduplication where applicable
- Reference counting with appropriate lifetimes
- Column encoding when datasets opt in
- Tiered storage from hot pool memory to cold disk archives
- Federated querying across all tiers and external sources
- Snapshot, persistence, and cross-process sharing
- Query rewriting against derivation tiers

The user doesn't think about most of this most of the time. They
write `DataSet<Message> chat("shm://chat")` and `chat.append(msg)`
and `sql::SELECT * FROM chat WHERE user = alice`. The substrate
applies the right techniques at the right scales automatically, with
explicit controls available when the user wants to tune behavior.

That's the architectural ambition: a substrate that makes the proven
techniques of decades of systems programming available as a coherent
default, while keeping every layer accessible for users who want to
work at finer granularity.
