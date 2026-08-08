# madcdat: External Storage and Service Drivers Plan

## Status

Active design track. `madcdat` is the optional companion library
for external storage and services, depending on the `madcdis` core
substrate that ships with `libmadc`.

This plan is paired with:

- `madcdis-plan.md` — the core substrate that defines the
  interfaces and types `madcdat` consumes
- `madcdis-memory-research.md` — the technical research grounding
  the overall architecture

## Name and Boundary

`madcdat` ("madc data external") is the external/optional half of
the data system. It ships as `libmadcdat`, depends on `libmadc`
(which includes `madcdis`), and adds implementations that require
external libraries for:

- Local databases (BerkeleyDB, GDBM, QDBM, SQLite)
- Network databases (MySQL, PostgreSQL, Redis, FalkorDB)
- Service APIs (libcurl-backed HTTP/HTTPS/REST/FTP/S3, MCP, mail)
- File formats and structured sources whose implementation needs an
  optional parser/client library

Dependency-free standard transports and formats are core `madcdis`:
memory, files, FIFOs, TCP, UDP, UDS, processes, DSV, FLR, and VLR.

A Mad-C program that only needs in-memory or shared-memory data
does not link `libmadcdat`. Linking it enables additional driver
schemes through the existing `DataDriverRegistry` in core.

## Intent

`madcdat` is purely an extension surface. Its job is to:

- Implement the `madcdis::DataDriver` interface for external
  backends
- Implement the `madcdis::SourceAdapter` interface when an adapter
  needs an optional external dependency
- Register drivers and adapters into the core registries at load
  time
- Provide nothing else — no new types, no new core API, no new
  query semantics

All data model concerns and dependency-free implementations (datasets,
relations, query IR, planner, typed flows, raw channels, processes,
standard file drivers, in-memory drivers, value system, refcounting,
interning, multiplicity dedup, column encoding, derivation) live in
`madcdis`. `madcdat` plugs external-library implementations into them.

## Non-Goals

- No core data abstractions in `madcdat` (those live in `madcdis`).
- No driver-specific extensions to `Query` IR. Drivers receive
  plans and advertise capabilities; they do not extend the IR.
- No URI query-parameter DSL for backend options. Driver options
  go in driver/config objects passed alongside `DataSource`.
- No compiler integration. `madcdat` is pure runtime; it cannot
  add language syntax.
- No per-backend hand-coded field maps as the default workflow.
  Schema inference from type metadata, as defined by `madcdis`,
  applies uniformly.

## Core Principles

### 1. `madcdat` depends on `madcdis`, never the reverse

The dependency edge is one-way. `madcdis` knows nothing about
`madcdat` and works fully without it. `madcdat` consumes `madcdis`
interfaces and types.

This keeps the core small, lets embedded users skip the driver
bundle, and ensures the substrate's design is not warped by
specific external driver needs.

### 2. Drivers implement existing interfaces, not new ones

Every `madcdat` driver implements `madcdis::DataDriver`. Every
source adapter implements `madcdis::SourceAdapter`. No driver
introduces new core types. Backend-specific configuration is
internal to the driver implementation.

### 3. Capability honesty

Each driver advertises through `DriverCapabilities` exactly what
it can do. SQLite cannot natively traverse graph paths; it says
so. FalkorDB cannot do complex tabular aggregations; it says so.
The planner respects these declarations and handles unsupported
operations locally.

Drivers do not lie about capabilities for convenience. If a driver
cannot push down an operation efficiently, it declines, and the
planner walks the data locally instead. This keeps performance
predictable.

### 4. Drivers may leverage madcdis primitives

External drivers can compose with `madcdis` primitives where it
makes sense:

- A SQLite driver may use the column encoding catalog when storing
  to SQLite tables (SQLite supports BLOB columns; encoded chunks
  fit naturally)
- A BerkeleyDB driver may use interned values as keys (saving
  storage in the index)
- A FalkorDB driver may use `madcdis::Symbol` for edge labels
  (avoiding string allocation per traversal)
- File-format drivers may use pool snapshot for the disk format
  (the encoded form is canonical)

The key is that drivers consume `madcdis` types; they don't
duplicate `madcdis` functionality with their own variants.

### 5. Indexes over external sources are derived artifacts

For structured text sources, indexes (FLR/VLR/QDBM/BDB/SQLite) are
explicit companion artifacts of the original source, not hidden
state. The original file remains the source of truth; indexes can
be rebuilt deterministically.

This matches `madcdis`'s discipline for in-memory indexes.

### 6. One source may yield many record families

A single `DataSource` may contain multiple logical record types:

- SMAUG area files with `#MOBILES`, `#OBJECTS`, `#ROOMS`, etc.
- mbox files with messages, headers, MIME parts, attachments
- TOML/INI files with repeated section families

`SourceAdapter::discover_types` enumerates the families a source
contains. `extract` pulls records of a specified family. This maps
cleanly onto multiple `DataSet<T>` instances over the same source.

### 7. Drivers participate in federation

External drivers are full participants in the `madcdis` federated
planner. A query may join data across `mem://` pools, `shm://`
pools, mmap'd files, SQLite, FalkorDB, and an HTTP API — the
planner orchestrates pushdown and local execution.

External drivers also participate in derivation tiering: an
`shm://` pool of recent log entries may have a `madcdat`-managed
SQLite or file backend holding older keyframes, with retention
policies pruning the `shm://` data as keyframes become canonical.

### 8. Drivers register themselves into core registries

`register_core_storage_drivers(registry)` registers dependency-free DSV,
FLR, and VLR support unconditionally. `register_optional_storage_drivers`
is the separate canonical entry point for BDB, GDBM, QDBM, SQLite, and
future external-library providers. Programs without `madcdat` retain all
core drivers and get a clean "no driver for scheme X" error only when
they request an optional scheme.

## Driver Families

### Core file-format drivers (`madcdis`)

Local file formats:

- `file://` — opaque file content, byte-level access
- `dsv://` — delimited/separated values (CSV, TSV)
- `flr://` — fixed-length record files
- `vlr://` — variable-length record files
- `snapshot://` — `madcdis` pool snapshot files (the canonical
  serialized form)

Capabilities:

- Read, write, scan
- Point lookup (by record locator)
- Limited range lookup (sequential scan with filter)
- Filter pushdown only for indexed columns
- Snapshot/restore handoff with `madcdis` pool drivers

Used heavily for legacy data import (SMAUG areas, ed-friendly
text), cold storage, and as a checkpoint format for in-memory
pools.

The `snapshot://` driver is special: it reads/writes `madcdis`
pool snapshots directly, providing the fastest possible disk
persistence path. Mad-C programs can write a hot `shm://` pool to
a `snapshot://` file and reload it byte-for-byte; no encoding
translation required.

DSV, FLR, and VLR are implemented in `madcdis_storage.cpp`; DSV scan
is natively cursor-backed, while compatibility APIs remain available.
An optional file-format implementation belongs in `madcdat` only when
it links an external library.

### Keyed local database drivers

Embedded key-value stores:

- `bdb://` — BerkeleyDB
- `gdbm://` — GNU dbm
- `qdbm://` — QDBM

Capabilities:

- Read, write, scan
- Point lookup, range lookup (ordered backends)
- Filter pushdown for key-prefix queries
- Transaction support (where the backend provides it)
- Soft delete (where the backend provides it)

Bridge between raw file formats and full relational/graph
engines. Natural fit for `Relation<A,B>` with index/payload
separation: one keyed store holds the index, another holds the
payload.

These drivers can use `madcdis` interning for repeated keys (e.g.,
when storing graph edges where labels repeat across millions of
edges, the labels are interned in the process and the keyed store
holds the interned handle as the key prefix).

### Relational database drivers

SQL-speaking backends:

- `sqlite://` — local file SQLite
- `mysql://` — remote MySQL/MariaDB
- `pgsql://` — remote PostgreSQL
- `postgres://` — alias for pgsql

Capabilities:

- Full CRUD
- Filter, project, sort, limit pushdown
- Join pushdown for queries entirely within one database
- Transaction support
- No native graph_match (planner does graph traversal locally over
  relational results, or refuses)

The driver translates pushed-down portions of the Query IR into
SQL and executes them. Remaining operations come back to the
planner for local execution.

For derivation tiers, a SQL driver can hold keyframes natively:
the derived dataset's storage is a SQL table, and the planner
rewrites queries that target the keyframe time range into SQL.

### Graph database drivers

Native graph backends:

- `falkordb://` — FalkorDB (Redis-based graph)
- Future: `neo4j://`, `memgraph://`

Capabilities:

- Full graph match, edge expand, path search
- Node and edge property storage
- Filter pushdown on node/edge properties
- Limited tabular aggregation (depends on backend)
- Transaction support (depends on backend)

The driver translates graph portions of the Query IR into the
backend's native query form (Cypher for Neo4j/FalkorDB/Memgraph,
GQL where supported). Tabular post-processing happens locally.

`madcdis::Symbol` is used for edge labels and node labels passed
through the driver, avoiding string allocation per traversal.

### Document and cache drivers

Document and key-value stores:

- `redis://` — Redis (key-value with structured values)
- Future: `mongodb://`, document DB schemes

Capabilities:

- Read, write, scan
- Point lookup by key
- Limited filter pushdown (depends on backend indexing)
- TTL support (where the backend provides it)

### Service API drivers

Remote services accessed over network protocols:

- `http://`, `https://` — generic HTTP endpoints
- `rest://` — REST API conventions on top of HTTP
- `mcp://` — Model Context Protocol endpoints
- `ftp://` — FTP file transfer
- `s3://` — S3-compatible object storage

Capabilities:

- Read-mostly
- Point lookup by URL or object key
- Very limited query pushdown (only what the API supports)
- No transaction support
- Explicit error paths for non-meaningful operations

Service drivers are typically narrower than storage drivers. The
planner treats them as scan-and-filter sources unless the
underlying API supports richer query semantics.

## Source Adapters

Source adapters parse structured text into records. They implement
`madcdis::SourceAdapter` and feed records into datasets through
`SourceAdapter::extract`.

Initial set:

- **SMAUG area file adapter** — discovers `#MOBILES`, `#OBJECTS`,
  `#ROOMS`, `#RESETS`, `#SHOPS`, `#SPECIALS`, etc. Extracts each
  family into typed records.
- **mbox adapter** — discovers messages, headers, MIME parts,
  attachments. Handles nested multipart.
- **TOML/INI adapter** — discovers section families.

Source adapters are used to:

1. Bootstrap typed datasets from legacy text files
2. Build derived indexes (FLR/VLR/QDBM/BDB/SQLite) over the parsed
   records
3. Validate that the original source can be round-tripped

The original source file remains canonical; the extracted records
are logical views, and the indexes are derived artifacts.

When records are loaded into `madcdis` datasets, they participate
in interning automatically. Repeated strings across SMAUG area
files (room names, message templates, item descriptions) deduplicate
to canonical instances in the process-wide intern table.

## Tiered Storage and Derivation

`madcdat` is the primary mechanism for cold/warm tier storage in
the derivation pattern:

```
[ shm:// pool   ]  hot raw events (last N days)
[ snapshot://   ]  warm pool snapshots (rolling backup)
[ sqlite://     ]  warm keyframes (queryable summaries)
[ s3://         ]  cold archive (compliance retention)
```

The derivation worker periodically:

1. Reads recent source records from `shm://`
2. Applies the derive function to produce keyframes
3. Writes keyframes to the configured tier (typically `sqlite://`)
4. Tracks coverage in the derivation relation
5. Prunes covered source records from `shm://` (or migrates them to
   `snapshot://` for short-term backup)

The federated planner sees all tiers and routes queries accordingly.
Queries against recent time ranges hit `shm://`; queries against
older ranges hit the keyframe driver; cold-tier queries trigger
on-demand retrieval from `s3://`.

## Federation Across madcdis and madcdat

The `madcdis` planner federates uniformly across in-memory and
external drivers. A query may:

1. Filter candidate users in `mem://` (fast local scan)
2. Traverse friendship edges in `falkordb://` (pushed down)
3. Read profile details from `sqlite://` (pushed-down filter)
4. Join all three locally in `madcdis`

The planner does not care which library owns each driver — it
sees them all through the `DataDriver` interface and
`DriverCapabilities` advertisement.

`madcdat` adds drivers; it does not extend planning logic.

## Delivery Phases

### V1 — Local file family

Drivers:

- `file://`, `dsv://`, `flr://`, `vlr://`
- `snapshot://` (madcdis pool snapshot format)

Source adapters:

- Legacy tagged-text adapter (SMAUG-style or simpler sibling
  format as proof target)

Why first:

- Proves the `DataDriver` interface against external storage
- Proves automatic type inference from `madcdis` metadata
- Proves the snapshot/restore handoff between pool storage and
  file formats
- Avoids network/auth/query complexity initially

### V2 — Keyed local databases

Drivers:

- `bdb://`, `gdbm://`, `qdbm://`

Why here:

- Bridge between raw files and full DB engines
- Exercises keyed access, ordered lookup, index/payload
  workflows
- Natural fit for `Relation<A,B>` with separate index and data
  stores
- Validates `madcdis` interning of repeated keys for storage
  savings

### V3 — Relational and document backends

Drivers:

- `sqlite://` (first slice already exists in current code)
- `redis://` (key-value with structured values)
- Initial Query IR to SQL translator
- Capability advertisement and pushdown for SQL

### V4 — Graph backends and federation maturity

Drivers:

- `falkordb://` (first graph backend)

Planner work (in `madcdis`, not `madcdat`):

- Cross-source joins
- Graph-lift of `Relation<A,B>` for graph backends
- Federation V1: scans, lookups, filters, projection, limit
- Federation defers: distributed aggregation, distributed sorting,
  cross-source transactions

### V5 — Derivation tier drivers

Drivers and integrations:

- Derivation worker can target any registered driver as the
  keyframe destination
- Source adapter for archived snapshot files (cold tier read-back)
- Time-partitioned dataset support (logs split by time window)

### V6 — Service drivers

Drivers:

- `http://`, `https://`, `rest://`, `mcp://`, `ftp://`, `s3://`
- Additional graph/document backends as needed

These are typically narrower in capability and require explicit
error paths for operations the underlying service does not support.
Where libcurl is used for HTTP/HTTPS/REST/FTP/S3, that provider remains
in `madcdat`; raw `tcp://`, `udp://`, and `uds://` channels are core
`madcdis` transports and do not depend on libcurl.

The `s3://` driver in particular enables cold-tier archive of
keyframes and historical data — the long-term storage tier for
the derivation pattern.

## Migration from Current State

The core interface migration is complete:

**Canonical in `madcdis/`:**

- `schema.h`, `mapper.h`, `query.h`, `relation.h`, `dataset.h`
- `driver.h`, `source_adapter.h`, Cursor/Sink/Flow, DataChannel, Process
- dependency-free DSV/FLR/VLR and memory/file/FIFO/TCP/UDP/UDS support

**Stay in `madcdat/`:**

- Driver/adapter implementations that link external libraries
  (`madcdat_storage_bdb.cpp`, GDBM, QDBM, SQLite, and future clients)
- `register_optional_storage_drivers` declaration and implementation

**Update everywhere:**

- External implementation `.cpp` files include their interfaces from
  `madcdis/` and keep dependency-specific headers in `madcdat/` if needed.
- Compatibility headers under older paths remain forwarding-only.

The remaining migration is behavioral only where a suitable eager driver
adopts the optional native streaming extension; legacy vector APIs remain.

## Open Questions

- Whether MongoDB-style document drivers should ship in V3 or
  defer to a later phase
- Whether to support graph-on-relational emulation in SQL drivers
  (probably not — let the planner handle it locally)
- How transaction coordination across drivers should work
  (deferred past V6; single-driver transactions only initially)
- Whether the SMAUG adapter should be a first-class shipped
  adapter or a reference example users can adapt
- How driver-specific options should be configured (driver-side
  config objects, not URI parameters)
- Whether `snapshot://` should support compression on write (LZ4
  for fast warm-tier, ZSTD for tighter cold-tier)
- Whether the derivation worker should run in-process or as a
  separate process attached to `shm://`

## Relationship to `madcdis`

`madcdat` consumes the following `madcdis` surface:

- `DataDriver` (interface to implement)
- `DataDriverRegistry` (where to register implementations)
- `DriverCapabilities`, `RecordLocator` (driver contract types)
- `Query` IR (received from the planner)
- `DataSource` (identifies external sources)
- `SchemaInfo`, `SchemaField`, `MappingSpec<T>` (schema metadata)
- `value` (record payload representation)
- `Symbol` (for interned identifiers passed through driver boundaries)
- `SourceAdapter`, `ExtractedRecord`, `SourceLocator`
  (text-source parsing interface)
- Pool snapshot/restore (for hand-off with file drivers)
- Column encoding catalog (for drivers that store encoded columns)
- `DerivationRelation` (for drivers participating in tiered storage)

`madcdat` does not extend or modify any of these. It implements the
interfaces and registers the implementations.

See `madcdis-plan.md` for the core substrate that defines these
surfaces.
