# madcdat: External Storage and Service Drivers Plan

## Status

Active design track. `madcdat` is the optional companion library
for external storage and services, depending on the `madcdis`
core substrate which now lives in `libmadc`.

This plan supersedes the earlier `madcdat-plan.md`. The previous
version conflated the in-memory substrate with external drivers
and placed both behind an optional library; this revision
separates them. The substrate is now core (`madcdis`); only
external drivers are optional (`madcdat`).

Pair this plan with `madcdis-plan.md`, which covers the core
substrate that `madcdat` builds on.

## Name and Boundary

`madcdat` ("madc data external") is the external/optional half of
the data system. It ships as `libmadcdat`, depends on `libmadc`
(which includes `madcdis`), and adds drivers for:

- file-format storage (CSV, FLR, VLR, plain files)
- local databases (BerkeleyDB, GDBM, QDBM, SQLite)
- network databases (MySQL, PostgreSQL, Redis, FalkorDB)
- service APIs (HTTP, REST, MCP, FTP, S3, etc.)
- structured text source parsing (SMAUG areas, mbox, TOML, INI)

A Mad-C program that only needs in-memory or shared-memory data
does not link `libmadcdat`. Linking it enables additional driver
schemes through the existing `DataDriverRegistry` in core.

## Intent

`madcdat` is purely an extension surface. Its job is to:

- implement the `madcdis::DataDriver` interface for external
  backends
- implement the `madcdis::SourceAdapter` interface for structured
  text source parsing
- register drivers and adapters into the core registries at load
  time
- provide nothing else — no new types, no new core API, no new
  query semantics

All data model concerns (datasets, relations, query IR, planner,
in-memory drivers) live in `madcdis`. `madcdat` plugs into them.

## Non-Goals

- No core data abstractions in `madcdat` (those live in
  `madcdis`).
- No driver-specific extensions to `Query` IR. Drivers receive
  plans and advertise capabilities; they do not extend the IR.
- No URI query-parameter DSL for backend options. Driver options
  go in driver/config objects passed alongside `DataSource`.
- No compiler integration. `madcdat` is pure runtime; it cannot
  add language syntax. Language-level data syntax is part of the
  Mad-C compiler and consumes `madcdis`.
- No per-backend hand-coded field maps as the default workflow.
  Schema inference from type metadata, as defined by `madcdis`,
  applies uniformly.

## Core Principles

### 1. `madcdat` depends on `madcdis`, never the reverse

The dependency edge is one-way. `madcdis` knows nothing about
`madcdat` and works fully without it. `madcdat` consumes
`madcdis` interfaces and types.

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

Drivers do not lie about capabilities for convenience. A SQL
driver does not pretend to support `graph_match` by simulating
it with recursive CTEs internally; if it cannot push the graph
operation down efficiently, it declines, and the planner walks
the data locally instead. This keeps performance predictable.

### 4. Indexes over external sources are derived artifacts

For structured text sources, indexes (FLR/VLR/QDBM/BDB/SQLite)
are explicit companion artifacts of the original source, not
hidden state. The original file remains the source of truth;
indexes can be rebuilt deterministically.

This matches `madcdis`'s discipline for in-memory indexes.

### 5. One source may yield many record families

A single `DataSource` may contain multiple logical record types:

- SMAUG area files with `#MOBILES`, `#OBJECTS`, `#ROOMS`, etc.
- mbox files with messages, headers, MIME parts, attachments
- TOML/INI files with repeated section families

`SourceAdapter::discover_types` enumerates the families a source
contains. `extract` pulls records of a specified family. This
maps cleanly onto multiple `DataSet<T>` instances over the same
source.

### 6. Drivers register themselves into core registries

`register_optional_storage_drivers(registry)` is the canonical
entry point. Mad-C programs that link `libmadcdat` get the
external drivers registered automatically at startup. Programs
that do not link it get a clean "no driver for scheme X"
error if they request an external scheme.

## Driver Families

### File-format drivers

Local file formats:

- `file://`         — opaque file content, byte-level access
- `dsv://`          — delimited/separated values (CSV, TSV)
- `flr://`          — fixed-length record files
- `vlr://`          — variable-length record files

Capabilities:

- read, write, scan
- point lookup (by record locator)
- limited range lookup (sequential scan with filter)
- filter pushdown only for indexed columns
- snapshot/restore from/to pool format for hand-off with
  `madcdis` pool drivers

Used heavily for legacy data import (SMAUG areas, ed-friendly
text), cold storage, and as a checkpoint format for in-memory
pools.

### Keyed local database drivers

Embedded key-value stores:

- `bdb://`          — BerkeleyDB
- `gdbm://`         — GNU dbm
- `qdbm://`         — QDBM

Capabilities:

- read, write, scan
- point lookup, range lookup (ordered backends)
- filter pushdown for key-prefix queries
- transaction support (where the backend provides it)
- soft delete (where the backend provides it)

Bridge between raw file formats and full relational/graph
engines. Natural fit for `Relation<A,B>` with index/payload
separation: one keyed store holds the index, another holds the
payload.

### Relational database drivers

SQL-speaking backends:

- `sqlite://`       — local file SQLite
- `mysql://`        — remote MySQL/MariaDB
- `pgsql://`        — remote PostgreSQL
- `postgres://`     — alias for pgsql

Capabilities:

- full CRUD
- filter, project, sort, limit pushdown
- join pushdown for queries entirely within one database
- transaction support
- no native graph_match (planner does graph traversal locally
  over relational results, or refuses)

The driver translates pushed-down portions of the Query IR into
SQL and executes them. Remaining operations come back to the
planner for local execution.

### Graph database drivers

Native graph backends:

- `falkordb://`     — FalkorDB (Redis-based graph)
- (future) `neo4j://`, `memgraph://`, others as needed

Capabilities:

- full graph match, edge expand, path search
- node and edge property storage
- filter pushdown on node/edge properties
- limited tabular aggregation (depends on backend)
- transaction support (depends on backend)

The driver translates graph portions of the Query IR into the
backend's native query form (Cypher for Neo4j/FalkorDB/Memgraph,
GQL where supported). Tabular post-processing happens locally.

### Document and cache drivers

Document and key-value stores:

- `redis://`        — Redis (key-value with structured values)
- (future) `mongodb://`, document DB schemes

Capabilities:

- read, write, scan
- point lookup by key
- limited filter pushdown (depends on backend indexing)
- TTL support (where the backend provides it)

### Service API drivers

Remote services accessed over network protocols:

- `http://`, `https://`  — generic HTTP endpoints
- `rest://`              — REST API conventions on top of HTTP
- `mcp://`               — Model Context Protocol endpoints
- `ftp://`               — FTP file transfer
- `s3://`                — S3-compatible object storage

Capabilities:

- read-mostly
- point lookup by URL or object key
- very limited query pushdown (only what the API supports)
- no transaction support
- explicit error paths for non-meaningful operations

Service drivers are typically narrower than storage drivers.
The planner treats them as scan-and-filter sources unless the
underlying API supports richer query semantics.

## Source Adapters

Source adapters parse structured text into records. They
implement `madcdis::SourceAdapter` and feed records into
datasets through `SourceAdapter::extract`.

Initial set:

- **SMAUG area file adapter** — discovers `#MOBILES`, `#OBJECTS`,
  `#ROOMS`, `#RESETS`, `#SHOPS`, `#SPECIALS`, etc. Extracts each
  family into typed records.
- **mbox adapter** — discovers messages, headers, MIME parts,
  attachments. Handles nested multipart.
- **TOML/INI adapter** — discovers section families.

Source adapters are used to:

1. Bootstrap typed datasets from legacy text files
2. Build derived indexes (FLR/VLR/QDBM/BDB/SQLite) over the
   parsed records
3. Validate that the original source can be round-tripped

The original source file remains canonical; the extracted
records are logical views, and the indexes are derived artifacts.

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

(matches `madcdat-plan.md` original V1 scope, retained)

Drivers:

- `file://`, `dsv://`, `flr://`, `vlr://`

Source adapters:

- legacy tagged-text adapter (SMAUG-style or simpler sibling
  format as proof target)

Why first:

- proves the `DataDriver` interface against external storage
- proves automatic type inference from `madcdis` metadata
- proves the snapshot/restore handoff between pool storage and
  file formats
- avoids network/auth/query complexity initially

### V2 — Keyed local databases

Drivers:

- `bdb://`, `gdbm://`, `qdbm://`

Why here:

- bridge between raw files and full DB engines
- exercises keyed access, ordered lookup, index/payload
  workflows
- natural fit for `Relation<A,B>` with separate index and data
  stores

### V3 — Relational and document backends

Drivers:

- `sqlite://` (first slice already exists in current code)
- `redis://` (key-value with structured values)
- initial Query IR → SQL translator
- capability advertisement and pushdown for SQL

### V4 — Federation and graph backends

Drivers:

- `falkordb://` (first graph backend)

Planner work (in `madcdis`, not `madcdat`):

- cross-source joins
- graph-lift of `Relation<A,B>` for graph backends
- federation V1: scans, lookups, filters, projection, limit
- federation defers: distributed aggregation, distributed
  sorting, cross-source transactions

### V5 — Service drivers

Drivers:

- `http://`, `https://`, `rest://`, `mcp://`, `ftp://`, `s3://`
- additional graph/document backends as needed

These are typically narrower in capability and require explicit
error paths for operations the underlying service does not
support.

## Migration from Current State

The current code has many `madcdis`-belonging types in
`include/madcdat/`. The split is:

**Move out of `madcdat/` into `madcdis/`:**

- `schema.h`, `mapper.h`, `query.h`, `relation.h`, `dataset.h`
- `driver.h` (the interface, not the implementations)

**Stay in `madcdat/`:**

- `source_adapter.h` (text-source parsing is external-content
  territory)
- driver implementation source files
  (`madcdat_storage_*.cpp`)
- `register_optional_storage_drivers` declaration

**Update everywhere:**

- Driver implementation `.cpp` files: include from `madcdis/`
  for the interface, keep their own implementation headers in
  `madcdat/` if needed.

The migration is mechanical — header path changes and include
updates. No behavioral changes to existing drivers.

## Open Questions

- Whether MongoDB-style document drivers should ship in V3 or
  defer to a later phase
- Whether to support graph-on-relational emulation in SQL
  drivers (probably not — let the planner handle it locally)
- How transaction coordination across drivers should work
  (deferred past V5; single-driver transactions only initially)
- Whether the SMAUG adapter should be a first-class shipped
  adapter or a reference example users can adapt
- How driver-specific options should be configured (driver-side
  config objects, not URI parameters)

## Relationship to `madcdis`

`madcdat` consumes the following `madcdis` surface:

- `DataDriver` (interface to implement)
- `DataDriverRegistry` (where to register implementations)
- `DriverCapabilities`, `RecordLocator` (driver contract types)
- `Query` IR (received from the planner)
- `DataSource` (identifies external sources)
- `SchemaInfo`, `SchemaField`, `MappingSpec<T>` (schema metadata)
- `value` / `MadValue` (record payload representation)
- `SourceAdapter`, `ExtractedRecord`, `SourceLocator`
  (text-source parsing interface)
- Pool snapshot/restore (for hand-off with file drivers)

`madcdat` does not extend or modify any of these. It implements
the interfaces and registers the implementations.

See `madcdis-plan.md` for the core substrate that defines these
surfaces.
