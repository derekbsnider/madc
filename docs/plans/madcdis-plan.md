# madcdis: Core Data Substrate Plan

## Status

Active design track. `madcdis` is the dependency-free data substrate
and streaming framework that ships as part of core `libmadc`. It
supersedes earlier framings in which the entire data subsystem was
optional or limited to typed in-memory storage.

> **UPDATE 2026-08-07** - The streaming/process-flow milestone is complete.
> `madcdis` now owns the optional cursor extensions, typed sources/sinks/flows,
> raw channels, process endpoints, and standard dependency-free transports and
> record formats. `madcdat` owns only implementations that require external
> libraries. See `2026-08-07-data-channel-streaming-process-flow-plan.md`.

> **UPDATE 2026-06-12** — reconciled with
> `docs/plans/2026-06-12-type-table-value-abi-design.md` (DESIGN AGREED),
> which post-dates this plan. Sequencing is unchanged (Track 5 still starts
> only after Track 1.3 parity), but four points re-base when V1 begins:
> - **The public `madc::value` is the 32-byte typeid struct** of that design
>   (today: the A0-unified class in `include/libmadc/value.h`, which becomes
>   its RAII wrapper). The 8-byte tag + 60-bit handle described in Principle 5
>   / Class Model / V1 is the **internal pool value-handle** (the dense
>   storage tier) — rename it accordingly (e.g. `madc::pool_handle`); it
>   marshals to/from `madc_value` at the substrate boundary and must not
>   claim the public name.
> - **`value_header.type_tag` is the uint32 typeid** from the one segmented
>   type table — no second type vocabulary. This is an enabler:
>   position-independent `mem://`/`shm://` pools cannot hold `DataDef*`
>   pointers; the table's stable-integer segments are what pool-resident type
>   refs require.
> - **The cell header is shared, not parallel:** the value ABI's malloc'd
>   cell `{refcount, cell_flags, payload}` already adopts this plan's
>   saturating-refcount + permanent tier and reserves flag bits for
>   interned/frozen/hash-present, so `value_header` is its pool-resident
>   superset — one header shape across both tiers.
> - **Interning starts above the SSO line:** strings ≤16 bytes live inline in
>   the 32-byte value and are never interned; interning (V2) is the
>   long-string and `madc::Symbol` discipline.

This plan is paired with:

- `madcdis-memory-research.md` — the design lineage and technical
  research grounding the architecture
- `madcdat-plan.md` — the optional external driver bundle that
  depends on `madcdis`

## Name and Boundary

`madcdis` ("madc data internal substrate") is the core/internal half
of the data system. It lives inside `libmadc` and is available to
every Mad-C program without an opt-in dependency.

`madcdat` is the optional companion library that adds external-library
storage and service implementations on top of `madcdis`. It depends on
`madcdis`; `madcdis` does not depend on it.

Headers split accordingly:

- core interfaces under `include/madcdis/`
- compatibility forwarding headers under `include/madcdat/` and
  `include/libmadc/`
- dependency-specific implementation headers under `include/madcdat/`
- `madc::DataSource` stays in `include/libmadc/` because it is a
  general external-conduit abstraction broader than storage

## Intent

`madcdis` is a unified memory-management substrate for typed
structured data. Its architectural backbone is the exploitation of
**structural redundancy** at every scale:

- Byte-level (string interning)
- Object-level (multiplicity dedup)
- Field-level (column encoding)
- Lifetime-level (pool allocation, refcounting, COW)
- Time-window-level (keyframe derivation)

These techniques are integrated as composable primitives, not
bolt-on features. The user picks combinations based on workload; the
substrate handles the mechanics.

Every Mad-C program gets the substrate, typed flows, process endpoints,
memory/file/FIFO/TCP/UDP/UDS channels, and DSV/FLR/VLR drivers by default.
Implementations backed by external client libraries plug in via `madcdat`.

## Non-Goals

- No required external-library storage or service drivers (those live in
  `madcdat`). Dependency-free standard drivers and transports are core.
- No general-purpose tracing garbage collector. Refcounting plus
  weak references plus pool drop handle the cases.
- No distributed/cross-machine clustering. Single-machine,
  multi-process via `shm://`.
- No full-text search engine. Structured queries on encoded data,
  not BM25/tokenized search.
- No compiler magic. The substrate is runtime; compiler integration
  (native query syntax) is separate and consumes this runtime.

## Core Principles

### 1. The substrate is core, not optional

Pools, values, refcounting, interning, datasets, relations, query IR,
planner, typed flows, raw channels, processes, and dependency-free
drivers ship as part of `libmadc`. A Mad-C program can declare
`DataSet<Room> rooms("mem://world")`, stream DSV, or connect raw sockets
without linking any optional library.

### 2. Structural redundancy is exploited at every layer

The substrate's design backbone:

- **Pools** for bulk-lifetime memory management
- **Refcounting** for shared resource lifetime
- **Interning** for byte-level and object-level dedup
- **Multiplicity** for collection-level dedup
- **Column encoding** for field-level compression
- **Derivation/keyframes** for time-window aggregation
- **Copy-on-write** for snapshot semantics

These layers compose. A value can be interned (saving bytes), held
in a pool (managing lifetime), referenced in a multiplicity-deduped
array (saving collection space), inside a column-encoded dataset
(saving field space), with periodic keyframes (saving time-series
space), persisted via COW snapshot (saving I/O cost). The user picks
the layers that apply.

### 3. Memory pools are the allocation primitive

All `madcdis`-managed memory comes from pools. Pools are typed,
sized, scoped, and droppable:

- bump-pointer allocation with O(1) wholesale drop
- optional per-type slab allocation for known-size records
- optional refcounted objects within a pool
- optional page-level refcounting for arena-with-reclamation
- position-independent addressing for cross-process sharing
- snapshot/restore for persistence handoff

Pool drop is the canonical bulk-cleanup operation. Per-object free
is supported but discouraged for pool-allocated data; the pool's
lifetime is the unit of memory management.

### 4. `mem://` and `shm://` are pool-backed drivers

Both schemes implement the same `DataDriver` interface and share a
common pool implementation. The difference is the backing memory:

- `mem://` allocates from the process heap
- `shm://` allocates from a POSIX shared-memory segment

Both support the full local execution capability set. The `shm://`
driver additionally supports cross-process attach, position-
independent data structures, and COW-based snapshots via fork().

Pool format is stable enough to be opened by multiple processes
simultaneously when backed by `shm://`. Schema versioning is part
of the pool header; mismatched attach attempts fail cleanly.

### 5. Values are refcounted and interned by default

Every `madc::value` is internally a tagged structure:

- **Inline scalars** (small ints, bools, nil, small enums) stored
  directly in the value bits, no allocation
- **Heap pointers** for anything larger, into pool-allocated slots
  with refcount, hash, and flags

Heap values participate in:

- **Reference counting** with saturating counts and permanent tier
- **Interning** for opt-in types (strings by default; user types by
  annotation)
- **Position-independent addressing** within pools

This is the SMAUG hashstr discipline generalized to all value types.
Strings are interned automatically; identical strings share storage
across the entire program. Refcounts manage lifetime; permanent
strings (literals, schema metadata) are saturated and skip counting.

### 6. Collections support multiplicity dedup

`madc::array`, `madc::map`, `madc::set` hold value handles. Identical
elements automatically share storage via interning.

Beyond per-value interning, collections support **multiplicity-based
deduplication**: an array can collapse identical adjacent (or
identical anywhere) elements into `(value, count)` pairs. This is
the SMAUG potion-stack pattern, generalized.

A player carrying 100 healing potions stores:

- One interned potion record (one allocation in the global pool)
- One multiplicity entry in the inventory array: `(potion, 100)`
- Total: ~24 bytes of overhead versus 100 × sizeof(potion)

Multiplicity dedup is opt-in per collection. Many collections won't
benefit (typically unique data); others benefit dramatically
(inventories, vote tallies, log buckets).

### 7. Datasets have two storage modes

`DataSet<T>` supports:

- **Row-oriented** (default) — records stored as packed structs;
  fastest random access, fastest single-record updates. Used for
  transactional data, point-access workloads, SMAUG-style game
  worlds.
- **Column-encoded** (opt-in) — fields stored in separate columns
  with per-column encoding; fastest scans/filters/aggregations,
  much smaller footprint. Used for logs, events, time series,
  append-mostly workloads.

The encoding catalog for column-encoded datasets:

- Dictionary encoding (categorical fields)
- Run-length encoding (sorted columns with runs)
- Frame-of-reference plus bit-packing (bounded numeric ranges)
- Delta encoding (smoothly-varying numerics)
- Prefix compression (sorted strings with shared prefixes)
- GCD compression (numerics with common divisors)

Encoding selection can be automatic (the system picks based on
observed data) or user-specified per column.

### 8. Derivation relations enable bounded retention

`DerivationRelation<Source, Derived>` captures the keyframe pattern.
Periodically, an aggregation function reduces a range of source
records into a single derived record. Old source records whose
information is captured in derived records can be safely pruned.

Use cases:

- Transaction logs with daily balance keyframes
- Authentication logs with hourly success/failure aggregates
- Time-series sensor data with minute/hour/day rollups
- Game combat events with combat-round summaries

The federated planner rewrites queries to use the appropriate tier
based on time range and required granularity. Hot raw data answers
recent queries; warm keyframes answer historical queries; cold
archives answer compliance queries.

### 9. GQL is the canonical query language

The Query IR is shaped to faithfully represent GQL semantics
(ISO/IEC 39075). GQL is the only standardized query language that
natively expresses graph patterns, tabular operations, and the
composition between them.

SQL and Cypher are supported as sub-grammars that lower to the same
Query IR. They exist for compatibility with users' existing mental
models. The builder API is the third front-end, producing the same
IR programmatically.

### 10. Native query syntax is namespaced

Query languages activate via namespace prefix on the expression:

```c
auto adults = sql::SELECT name, age FROM users WHERE age >= 18;
auto net   = cypher::MATCH (u:User)-[:FRIEND*1..3]->(f) RETURN f;
auto path  = gql::MATCH (a)-[*]->(b) WHERE a.id = 1 RETURN b;
```

This avoids reserving keywords in the main Mad-C namespace. Compiler
integration (lexer, sub-grammars, lowering passes) is part of the
Mad-C compiler frontend; this plan describes the runtime that
consumes the lowered IR.

### 11. Types stay local values

`DataSet<T>` materializes values of `T` from storage. Instances of
`T` are ordinary Mad-C values in memory, no proxy semantics, no
hidden I/O on field access.

Updates go through explicit dataset operations; the language does
not pretend that mutation of an in-hand value automatically
persists. This rules out the ActiveRecord/proxy-object pattern by
design.

### 12. The planner is core, capability-aware, and federation-ready

The federated planner is part of `madcdis` from V1, even when the
only drivers it sees are `mem://` and `shm://`. Federation across
in-memory pools is the V1 baseline; adding external drivers via
`madcdat` extends the planner's source set without redesign.

Driver capabilities are advertised through `DriverCapabilities`; the
planner pushes down what each driver can honor and handles the rest
locally.

## Class Model

### Pool subsystem

```
madc::Pool
    backing: mem | shm | mmap_file
    growth: fixed | growable
    lifetime: manual | refcounted | page_refcounted | traced
    
    allocate(size, align) -> PoolPtr<>
    deallocate(PoolPtr<>)
    drop() -> all allocations gone, O(1)
    snapshot(stream) -> serialize entire pool
    restore(stream) -> reload pool from snapshot
    attach(name) -> for shm:// only
    fork_snapshot() -> COW snapshot via fork()
    
madc::PoolPtr<T>
    position-independent pointer (pool_id + offset)
    
madc::TypedPool<T>
    slab-allocated pool for fixed-size T
    
madc::ArenaPool
    bump-pointer pool, drop-all semantics
    
madc::SizeClassPool
    multi-size general allocator
    
madc::PageRefcountedPool
    bump alloc with page-level refcount reclamation
```

### Value system

```
madc::value
    inline scalar: tag + 60 bits of payload
    heap pointer: tag + 60-bit PoolPtr
    
    construction: routes through intern check for interned types
    destruction: decrements refcount, releases at zero
    equality: pointer compare for interned, content compare otherwise
    
madc::value_header (heap values)
    type_tag
    refcount (saturating)
    hash (if interned)
    flags: permanent, interned, frozen
    
madc::InternedPool<T>
    intern table for type T
    intern(value) -> handle (existing or new)
    permanent(value) -> handle marked never-freed
    
madc::Symbol
    always-interned identifier type
    used for field names, type names, edge labels
    
madc::RefCounted<T>
    explicit refcount wrapper (when not using full value system)
```

### Collection types

```
madc::array<T>
    sequence of value handles
    optional dedup mode: multiplicity collapse, RLE, dictionary
    
madc::map<K, V>
    associative container, keys typically interned
    
madc::set<T>
    deduplicated set, typically using interned elements
    
madc::tuple<...>
    fixed-arity heterogeneous collection
```

### Data layer

```
madc::SchemaInfo, SchemaField
madc::MappingSpec<T>
madc::DataMapper<T>
madc::MapperBuilder<T>
madc::FormatAdapter<T>

madc::DataSet<T>
    storage_mode: row_oriented | column_encoded
    backing: PoolPtr or column chunks
    indexes: registered relation indexes
    
    get(key) -> T
    insert(T)
    update(key, T)
    erase(key)
    scan() -> Cursor<T>
    query(Query) -> Cursor<T>
    
madc::Cursor<T>
    forward-iterating result handle
    
madc::Relation<A, B>
    kind: key_match | positional | offset | graph_edge | derivation
    
    resolve(key, B&) -> bool
    query_related(from_query, to_query) -> Cursor<B>
    
madc::DerivationRelation<Source, Derived>
    extends Relation
    period: time interval
    derive_fn: range<Source> -> Derived
    coverage: Derived -> source range
    
    advance() -> produces new keyframe
    prune(retention) -> deletes captured source rows
    
madc::Cardinality
    multiplicity tracking for deduped collections
```

### Column encoding catalog

```
madc::ColumnEncoding (abstract)
    encode(values) -> encoded buffer
    decode_at(buffer, index) -> value
    filter(buffer, predicate) -> matching indices
    aggregate(buffer, op) -> aggregate value
    
madc::DictionaryEncoding
madc::RunLengthEncoding
madc::FrameOfReferenceEncoding
madc::DeltaEncoding
madc::PrefixCompressionEncoding
madc::GCDEncoding
madc::BitPackedEncoding

madc::EncodingSelector
    auto-select encoding based on observed data
```

### Driver interface (abstract)

```
madc::DataDriver
    virtual interface for storage backends
    implemented by dependency-free drivers in madcdis
    implemented by external-library drivers in madcdat
    
madc::DataDriverRegistry
    registers drivers by scheme
    
madc::DriverCapabilities
    advertises what operations a driver supports
    
madc::RecordLocator
    opaque pointer-to-record handle
```

### In-memory drivers (ship with madcdis)

```
madc::MemPoolDriver
    implements DataDriver
    backing: madc::Pool with mem:// allocation
    supports: read, write, scan, point/range lookup, all pushdowns,
              graph_match, edge_expand, path_search
    
madc::ShmPoolDriver
    implements DataDriver
    backing: madc::Pool with shm:// allocation
    supports: same as MemPoolDriver plus cross-process attach,
              COW snapshots via fork()
```

### Query IR and planner

```
madc::Query
    GQL-shaped IR
    flat predicates
    graph patterns
    path expressions
    tabular operations (group by, having, aggregations)
    composition between graph and tabular results
    
madc::QueryBuilder
    programmatic query construction
    
madc::QueryPlanner
    plan(Query, datasets) -> Plan
    execute(Plan) -> result cursor
    
madc::QueryPlan
    capability-annotated stages
    pushdown decisions
    join strategy
    derivation-aware rewriting
    
madc::parse_sql(string) -> Query
madc::parse_cypher(string) -> Query
madc::parse_gql(string) -> Query
    runtime parsers for dynamic queries
    (compile-time native syntax is a compiler feature)
```

## Storage and persistence

### Snapshot format

A pool's bytes are the canonical serialization format. Pool header:

- Magic number and version
- Pool kind (mem/shm/mmap)
- Allocation strategy (arena/slab/sizeclass/refcounted)
- Type metadata table (registered types and their layouts)
- Intern table (if any)
- Encoding catalog version (for column-encoded data)
- Payload (the pool's actual data, position-independent)

Restore validates the header, registers types, attaches the intern
table, maps the payload into memory. Forward-compatible header
extensions allow newer versions to load older snapshots.

### Persistence modes

- **Direct snapshot** — write pool bytes to a stream synchronously
- **COW fork snapshot** — fork(), child writes snapshot, exits;
  parent continues serving (Redis BGSAVE pattern)
- **mmap'd backing** — pool is mmap'd from a file from the start;
  OS page cache handles hot/cold pages; msync() flushes
- **Incremental snapshot** — periodic full snapshot plus change log
  of intervening updates

### Tiered storage

Datasets can span tiers:

- Hot: `mem://` or `shm://` (in-memory pool)
- Warm: mmap'd disk file (still queryable, slower than RAM)
- Cold: madcdat external driver (compressed disk file, S3, etc.)

Federation planner queries across tiers transparently. Tier
migration is policy-driven (age-based, access-frequency-based, or
explicit).

## Derivation and retention

Derivation relations enable bounded-retention storage of unbounded
data streams.

### Definition

```c
DataSet<Transaction> log("shm://transactions");
DataSet<DailyBalance> balances("shm://balances");

DerivationRelation derivation(log, balances)
    .period(1.day)
    .derive_fn([](range<Transaction> day) -> DailyBalance {
        return DailyBalance{
            .date = day.front().date,
            .closing_balance = compute_balance(day),
            .transaction_count = day.size(),
        };
    });

derivation.set_retention(90.days);  // keep raw txns for 90 days
```

### Behavior

1. Every period, a background worker invokes `derive_fn` on the
   recent source range and inserts the result into the derived
   dataset.
2. The derivation tracks coverage (which source records each derived
   record summarizes).
3. The retention policy prunes source records older than the
   retention window, provided they're fully covered by derived
   records.
4. The planner rewrites queries to use derived records when the
   query range falls within their coverage and granularity is
   acceptable.

### Query rewriting

A query for "Alice's balance on 2024-03-15" might:

1. Look up nearest balance keyframe before that date
2. If keyframe is exactly on that date, return it
3. Otherwise, apply transactions from keyframe to target date

This handles 90 days of detail and 10 years of summary in a single
query interface, with O(transactions per period) cost instead of
O(transactions all time).

## Delivery Phases

### V1 — Pool substrate and basic value system

Land:

- `madc::Pool` with mem and shm backing, multiple allocation modes
- `madc::PoolPtr<T>`, position-independent pointers
- `MemPoolDriver`, `ShmPoolDriver`
- `madc::value` with inline scalars and heap pointers
- Basic refcounting (no automatic interning yet)
- `DataSet<T>`, `Cursor<T>`, `Relation<A,B>` migrated from current
  `madcdat/` to `madcdis/`
- `SchemaInfo`, `MappingSpec<T>`, `DataMapper<T>` migrated
- `DataDriver` interface and registry migrated
- `Query` IR (current flat form retained, GQL extensions pending)
- `QueryBuilder` (current code, retained)
- Programmatic API only (no native query syntax yet)
- Snapshot/restore for `mem://` pools

Outcome: every Mad-C program can construct typed in-memory and
shared-memory datasets, relate them, scan them, query them via the
builder API, and persist them via snapshot/restore.

### V2 — Automatic interning and refcounting

Land:

- String interning by default in `madc::value`
- `madc::Symbol` interned identifier type
- `InternedPool<T>` for user-marked types
- Saturating refcounts with permanent tier
- Atomic refcounts for `shm://` pools
- Page-refcounted pool variant
- COW fork snapshots for `shm://`

Outcome: hashstr-equivalent string deduplication available
program-wide; refcounted heap values; production-grade `shm://`
snapshots.

### V3 — Multiplicity dedup and query IR redesign

Land:

- `madc::array` multiplicity collapse mode
- Extended Query IR supporting graph patterns, path expressions,
  tabular operations, and composition
- `QueryPlanner` with capability-aware pushdown
- In-memory federated planning across `mem://` and `shm://`
- Builder API extensions for graph patterns and paths

Outcome: SMAUG-style object dedup is available; builder API
expresses full GQL semantics; planner federates across in-memory
pools.

### V4 — Native query syntax

Land:

- Gecko grammars for SQL, Cypher, GQL sub-languages (compiler work,
  but lands alongside madcdis updates)
- Compiler lexer and parser recognize `sql::`, `cypher::`, `gql::`
  namespaced sub-grammars
- Lowering passes from sub-grammar ASTs to Query IR
- Symbol resolution and type checking in query expressions
- `using namespace sql;` etc. for bare-keyword activation
- Runtime `madc::parse_sql/cypher/gql` entry points for dynamic
  query strings

Outcome: Mad-C programs use native query syntax that compiles to the
same IR as the builder API, with compile-time type checking against
bound schemas.

### V5 — Column-encoded storage

Land:

- Column-encoded `DataSet<T>` layout (opt-in per dataset)
- Encoding catalog: dictionary, RLE, FOR, delta, bit-pack, GCD,
  prefix-compression
- Encoding selection (manual or auto-detect)
- Encoding-aware query operators (filter, aggregate, scan on
  encoded form)
- Persistence preserves encoded layout (no re-encoding on save)
- mmap support for disk-backed encoded datasets

Outcome: log and event workloads get 5-30x storage reduction with
faster query performance. The Elasticsearch/Lucene capabilities
become available in-process.

### V6 — Derivation and tiered storage

Land:

- `DerivationRelation<Source, Derived>`
- Background derivation worker (keyframe generation)
- Retention policy execution (pruning of captured source rows)
- Derivation-aware query rewriting in the planner
- Time-partitioned dataset abstraction
- Federation across hot/warm/cold tiers

Outcome: bounded-retention storage of unbounded streams becomes
first-class. Logs, audit trails, time series, event streams all use
the same pattern with automatic management.

### V7+ — Refinements

Land progressively:

- Optional tracing GC for pools with cyclic data
- Pool compaction for long-running fragmented pools
- Cross-pool refcount migration
- Advanced encoding strategies
- Vectorized SIMD kernels for encoded operations
- Sharded write-mostly `shm://` pools (multi-writer concurrency)
- Schema evolution support for persistent stores

These are real engineering work but don't change the architectural
shape. They sharpen what V6 already delivers.

## Migration from current state

The interface ownership migration is complete:

1. **Canonical core headers**: schema, mapper, query, relation, dataset,
   driver, source adapter, cursor, sink, flow, channel, format flow, and
   process interfaces live under `include/madcdis/`.
2. **Compatibility shims**: older `madcdat/` and `libmadc/` paths forward to
   canonical headers where compatibility requires them.
3. **Core implementations**: DSV/FLR/VLR and memory/file/FIFO/TCP/UDP/UDS
   support build independently of `madcdat`.
4. **Optional implementations**: BDB, GDBM, QDBM, SQLite, and future
   client-library providers remain in `madcdat` and include interfaces from
   `madcdis`.

Remaining migration work is incremental: suitable eager drivers and future
shipped `SourceAdapter` implementations may adopt the optional streaming
extension seams without changing legacy vector vtables.

## Open Questions

- Inline-value size for `madc::value`: 64 bits (NaN-boxed) vs 128
  bits (more inline capacity, higher passing cost)
- Default interning aggressiveness: which types are interned by
  default, which require annotation
- Cross-pool refcount handling: weak references, migration on
  promotion, fail-on-cross-pool reference
- Multi-writer concurrency in `shm://` pools: V1 single-writer,
  V7+ sharded multi-writer
- Snapshot format versioning: per-pool-kind or global; how to handle
  encoder evolution
- GC interaction: when (if ever) does tracing GC make sense; how do
  refcounted and traced pools interoperate
- Query language coverage: which subset of GQL ships in V3 IR, which
  parts defer to later phases

These are design decisions that need answers as implementation
proceeds. The architecture accommodates multiple answers; the
specific choices come from concrete workload requirements.

## Relationship to `madcdat`

`libmadcdat` is the optional companion library covering implementations
backed by external storage/service libraries. It depends on `madcdis` and
consumes:

- `DataDriver` interface (to implement external-library drivers)
- `Query` IR (to receive plans from the planner)
- `DriverCapabilities` (to advertise what each driver can handle)
- `DataSource` (to identify external sources)
- `SchemaInfo` and `MappingSpec<T>` (to map external records)
- Pool snapshot/restore (for file-backed pool persistence)
- Column encoding catalog (for external drivers that can store
  encoded columns natively)

`madcdat` does not modify `madcdis` runtime types or extend the core API
surface. It only registers implementations that need external libraries into
the existing registries. Dependency-free standard formats and raw transports
remain in `madcdis`; libcurl-backed HTTP/HTTPS/REST/FTP/S3 and client-backed
database/service providers remain in `madcdat`.

See `madcdat-plan.md` for the external driver bundle plan.
