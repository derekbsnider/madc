# Data Storage & Federation Plan

## Status

Exploratory future track. Not on the critical path ahead of the current
Phase 4 `libmadc` embedding work, but important enough to shape that API
so we do not paint ourselves into a corner.

## Official Name

This storage/federation/indexing subsystem is now officially named
`madcdat`.

That name applies to the design track and the eventual host-facing
surface under `madc::`. It does **not** mean the physical library split
has started yet.

Current implementation reality:

- the work still lives inside the existing `libmadc` codebase and build
  surfaces
- `madc::DataSource`, `DataSet<T>`, `Relation<A,B>`, drivers, mapping,
  and query work remain part of the current Phase 4 repository work

Core/system distinction:

- `madcdis` is just a conceptual label for the core/internal madc side
  of the system, not a separate subsystem name
- `madcdat` is the first explicitly named outward-facing subsystem layer
- `madc::DataSource` stays in core madc because it is a general
  external-conduit abstraction, not just database plumbing

Planned future boundary:

- `libmadc`
  - core embedding/runtime API
  - includes `madc::DataSource`
- `libmadcdat`
  - optional `madcdat` companion library for storage/federation/indexing
  - depends on core `madc::DataSource`

Planned public surface split:

- core headers continue under `include/libmadc/`
- `madcdat` headers now live under `include/madcdat/`
- temporary forwarding compatibility headers may remain under
  `include/libmadc/` during the transition
- `./configure --enable-madcdat` now gates the subsystem in the repo build
- `make libmadcdat` and `make install-madcdat` now provide a separate
  static archive/install seam
- the remaining future step is shared-library/package-level
  `libmadcdat` work rather than the current in-tree gated object set

So the name is official now, while the optional `libmadcdat`
configure/build split remains a later implementation step.

## Intent

madc should be able to work with existing in-memory types and persist or
query them without forcing the user to hand-map every member.

The core promise is:

- if the user already has `int`, `string`, `MadValue`, a `struct`, or a
  `class`, that type should be usable as a storage-facing type
- automatic mapping should be the default
- high-level declarations should be able to define structure and
  storage mapping together when the user wants a schema-first workflow
- explicit metadata should only be needed for keys, joins, edge labels,
  backend-specific naming, or places where the default is ambiguous
- direct/programmatic access is the primary API
- query-builder and optional GQL/SQL are higher-level convenience
  surfaces
- one query can span multiple different backends when the planner can
  federate them

This belongs under `madc::` as a host/runtime subsystem first. Any
script-level syntax should come later and wrap the same implementation.

## Non-Goals

- No compiler special-casing for individual database products.
- No requirement that users write GQL just to read or write data.
- No "remote object" semantics where ordinary member access triggers
  hidden network or file I/O.
- No per-backend hand-coded field maps as the default workflow.
- No URI query-parameter DSL for backend options.

## Core Principles

### 1. Types stay local values

`struct` and `class` instances remain ordinary madc/C++ values in
memory. Storage layers materialize them in and out of backends.

This avoids:

- proxy-object lifetime problems
- hidden blocking I/O on field access
- cache invalidation complexity
- destructor / mutation ambiguity

### 2. `DataSource` is core and identifies location only

`madc::DataSource` should carry only scheme plus location.

Examples:

- `file:///tmp/users.csv`
- `file:///tmp/users.flr`
- `sqlite:///tmp/app.db`
- `falkordb://127.0.0.1:6379/social`
- `redis://127.0.0.1:6379`

Backend options belong in driver/config objects, not embedded in the
URI. That keeps identity separate from policy and transport settings.

`DataSource` stays on the core madc side because it is broader than
storage:

- files
- databases
- sockets / IPC
- pipes
- embedded host bridges
- other external conduits that madc may need later

### 3. Mapping is inferred first, configured second

madc already knows a lot about user data from the existing type system:

- scalar width and signedness
- string/object/container distinctions
- struct/class member names and types
- method metadata on classes

That metadata should drive a default schema/record mapping automatically.

Explicit metadata should only be needed for:

- primary keys
- foreign keys / joins
- graph edge definitions
- custom field names
- exclusion of members that should not persist

### 4. Query is optional, not foundational

The primary API is direct/programmatic:

- open a source
- bind a type
- iterate, fetch, insert, update, delete
- declare relations

Query-builder and optional GQL are layered above that same substrate.

### 5. Binding is the real abstraction, not schema alone

The system needs one normalized concept that can represent:

- a type inferred from an existing `struct` or `class`
- a schema declared without an existing type
- mapping hints such as keys, renames, layout, or edge endpoints

That means the internal unit is not just `SchemaInfo`, and not just
`MappingSpec<T>`, but a combined binding layer that can carry both.

The plan should therefore distinguish:

- `SchemaInfo`
  - what the data looks like logically
- `MappingSpec<T>`
  - how an existing type should be persisted
- `StorageBinding`
  - the combined structure + mapping + relationship intent, whether
    type-first, schema-first, or hybrid

### 6. Avoid monolithic metadata objects

The system should stay explicitly layered and composable.

We do not want one universal object that simultaneously owns:

- source identity
- parser rules
- record schemas
- storage layout policy
- index definitions
- relation definitions
- query planning

That would be low-cohesion and high-coupling by construction.

Instead, the architecture should separate:

- source identity
- source parsing/extraction
- dataset-local record mapping
- cross-dataset relations
- derived index/materialization policy
- query/planning

Those concerns must collaborate, but they must not collapse into one
god object.

### 7. One source may yield many record families

Not every source is a flat row file. A single source may contain many
different logical record types.

Examples:

- SMAUG area files with `#MOBILES`, `#OBJECTS`, `#ROOMS`, etc.
- mbox files containing messages, headers, MIME parts, attachments, and
  nested multipart sections
- TOML/INI/tagged text files with repeated section families

The design must therefore support:

- one `DataSource`
- one parser/extractor for that source family
- many extracted record families
- relations among those families
- indexes over one or more extracted families

This is a better fit than forcing every source into one dataset with one
flat schema.

### 8. Indexes are derived artifacts, not hidden source state

For structured text sources and semi-structured legacy formats, indexes
should be explicit companion artifacts.

Typical shape:

- the original file/object remains the source of truth
- extracted records are logical views over that source
- FLR/VLR/QDBM/BDB/SQLite indexes are derived materializations
- reindex rebuilds or refreshes those artifacts deterministically

That keeps ownership clear and makes rebuild/revalidation a first-class
workflow instead of a side effect.

## Proposed Class Model

### `madc::DataSource`

Location only.

Responsibilities:

- parse and normalize scheme/location
- expose scheme, authority, path, and opaque location string
- no backend tuning parameters

Example:

```cpp
madc::DataSource users_src("sqlite:///tmp/app.db");
madc::DataSource profiles_src("file:///tmp/profiles.csv");
```

Longer-term, `DataSource` also needs enough classification to support
heterogeneous federation cleanly:

- structured-record sources
- blob/object sources
- remote service/object sources

Examples of eventual object/blob-style schemes:

- `file://`
- `http://` / `https://`
- `ftp://`
- `s3://`
- Google Drive-style file/object sources
- `rest://`

That does not mean `DataSource` should become a policy blob. It still
identifies the source. Capability and execution policy belong in the
driver/adapter layer.

### `madc::DataDriver`

Backend implementation for a scheme or storage family.

Responsibilities:

- open/close backend handles
- advertise capabilities
- expose low-level scan/lookup/mutate hooks
- optionally accept pushed-down query subplans
- return structured errors when an operation does not make sense

Drivers should also advertise coarse execution capabilities and a small
set of planning-relevant traits.

Examples:

- scan
- point lookup
- range lookup
- filter pushdown
- projection pushdown
- limit/sort pushdown
- transaction support
- locator stability guarantees

Those traits are intentionally coarse. The real execution gate remains
query-shape validation per backend, but the capability model gives the
planner a fast first-pass filter before asking a driver to accept or
reject a normalized subplan.

Examples:

- `FileDriver`
- `DsvDriver`
- `FlrDriver`
- `VlrDriver`
- `BdbDriver`
- `GdbmDriver`
- `QdbmDriver`
- `SqliteDriver`
- `RedisDriver`
- `FalkorDriver`

### `madc::DataDriverRegistry`

Maps source scheme or backend family to a driver factory.

This keeps parser/compiler internals unaware of specific products.

### `madc::SchemaInfo`

Normalized description of a type as persisted data.

Responsibilities:

- field list
- field names
- field types
- nullability/default info where meaningful
- scalar vs object vs dynamic-value shape
- packed/fixed/variable layout hints

This should be derivable automatically from:

- scalar `DataDef`
- `DataDefSTRUCT`
- `DataDefCLASS`
- `MadValue`
- public `madc::value`

This should also be usable as the normalized target schema for imported
text formats whose source representation is not itself strongly typed.

### `madc::StorageBinding`

Normalized binding definition that ties logical structure to storage
semantics.

Responsibilities:

- hold a `SchemaInfo`
- optionally point at an existing bound type
- carry mapping metadata such as keys, renames, excluded fields, fixed
  or variable layout intent, and string truncation policy
- carry relation/node/edge metadata where the declaration defines it
- serve as the lowering target for high-level helper classes and
  declarative schema-definition surfaces

This is the right home for declarations that define structure and
mapping at the same time.

Examples:

- infer from `struct User` + add `.key("id")`
- `sql::create TABLE users FOR User (...)`
- `gql::create NODE TYPE User FOR User { ... }`
- `cypher::create RELATIONSHIP FRIEND_OF (...)`

### `madc::DataMapper<T>`

Automatic or customized mapping of type `T` into backend records.

Responsibilities:

- derive `SchemaInfo`
- encode/decode one `T`
- expose default member mapping
- allow targeted overrides without full manual remapping

Default behavior should be:

- scalar types map as single-value records
- `string` maps as text/blob depending backend family
- `struct` / `class` map member-by-member using existing metadata
- `MadValue` / `madc::value` map as dynamic object/array/scalar trees

JSON support should be treated as a representation/view, not as a
separate foundational type requirement. Dynamic values already give us a
natural bridge:

- `MadValue` for current internal scripting-side mixed values
- `madc::value` for public host-side structured values

Backends that want JSON documents can serialize either dynamic value
shape into JSON when appropriate.

### `madc::FormatAdapter<T>`

Optional parser/formatter layer for non-tabular, irregular, or legacy
text/binary formats whose on-disk representation does not naturally look
like rows, columns, or fixed records.

Responsibilities:

- parse one source record/object from a textual or binary stream
- emit one source record/object back to that format
- bridge legacy formats into normalized `SchemaInfo` / `DataMapper<T>`
  flows
- support import/export pipelines between ad hoc file formats and other
  backends

This is where MUD-style formats such as SMAUG area/player/object files
fit. Those files are not really CSV, not really SQL rows, and not
always fixed binary records. They are domain-specific serialized text
structures. A format adapter lets madc treat them as just another data
source family once parsed.

Examples:

- `SmaugAreaAdapter<Room>`
- `SmaugAreaAdapter<Mob>`
- `SmaugAreaAdapter<Object>`
- `LegacyTaggedTextAdapter<T>`

This layer is important if madc is meant to become a serious
data-porting tool rather than only a typed persistence/query layer.

For more complex sources, the system will eventually need an adapter
family that can extract multiple record types from one source, not only
`T` one-record-at-a-time formatting. A simple `FormatAdapter<T>` is
still useful, but the long-term design should make room for a higher
level extraction layer.

### `madc::SourceAdapter`

Optional parser/extractor layer above dataset-local formatting.

Responsibilities:

- segment a `DataSource` into logical record boundaries
- classify extracted records by family/type
- expose nested or child records where the format allows them
- preserve locators back into the original source
- support simple declarative rulesets and complex parser-backed
  implementations

This is the right home for sources such as:

- SMAUG tagged text files
- mbox message archives
- MIME multipart content
- TOML/INI-like structured text
- custom sectioned block formats

The key distinction is:

- `FormatAdapter<T>` is about reading/writing one normalized record
- `SourceAdapter` is about discovering and classifying records within a
  raw source

Both are useful, but they solve different problems.

### `madc::ExtractedRecordType`

Named logical record family yielded by one `SourceAdapter`.

Responsibilities:

- define the logical name of a record family
- attach the normalized `SchemaInfo` for that family
- expose locators/boundaries for records of that family
- support per-family indexing/materialization

Examples:

- `RoomRecord`
- `MobileRecord`
- `ObjectRecord`
- `MessageRecord`
- `MimePartRecord`
- `AttachmentRecord`

### `madc::DataSet<T>`

Typed storage-facing collection. This is the main direct API surface.

Responsibilities:

- bind `T` to a source and driver
- optionally bind a `FormatAdapter<T>` when the raw source format is
  irregular
- expose CRUD/scan/find interfaces
- behave more like an STL container than like a SQL text shell

Example shape:

```cpp
madc::DataSet<User> users(users_src);
auto u = users.get(42);
users.insert(new_user);
for (auto row : users.scan()) { /* ... */ }
```

### `madc::Relation<A, B>`

Declares semantic linkage between two typed datasets.

This is the one place where explicit metadata is expected routinely,
because keys and joins are inherently relational and often cannot be
inferred safely.

Responsibilities:

- define which member(s) on `A` connect to which member(s) on `B`
- define relationship name/edge label where useful
- support same-backend and cross-backend links
- lift non-graph data into graph semantics when needed
- support different relation kinds explicitly:
  - positional sidecars
  - offset/pointer-style links
  - key-match relations
  - graph edges

Examples:

- SQL foreign key
- FalkorDB edge label
- FLR index file -> VLR data file
- B+tree key set -> payload file
- CSV row key -> SQL table key

The first concrete relation kinds to support are:

- `positional`
  - record/bit `n` in one dataset maps to record `n` in another
  - example: tombstone bitvector -> live FLR file
- `offset`
  - a field in dataset `A` points to an offset or record position in
    dataset `B`
  - example: FLR index file -> VLR payload file
- `key_match`
  - one or more key fields in `A` match key fields in `B`
  - example: dead-record archive -> live ordered FLR by logical key
- `graph_edge`
  - semantic graph relationship between node datasets
  - example: `FRIEND_OF` between `User` nodes

This relation layer should also be extensible enough to connect
structured datasets to object/blob sources, not only row-to-row links.
That is a separate relation shape from ordinary key-match or offset
record bindings and should not be faked as “just another table row”.

### `madc::IndexDefinition`

Declares a derived index/materialization artifact over a source or
extracted record family.

Responsibilities:

- define which fields or locators are indexed
- define which backend stores the derived index
- define whether the index is primary, secondary, positional, offset,
  full-text, or sidecar-style
- define rebuild/refresh/validate behavior

Examples:

- CSV/TOML/DSV source -> FLR sorted key index
- structured text source -> QDBM secondary lookup index
- FLR live file -> packed-bit tombstone sidecar
- extracted mail messages -> SQLite searchable header index

This layer is where “easy to index and re-index structured text
sources” belongs. It should not be hidden inside the parser or driver.

### Common Sidecar Patterns

The storage layer should treat several composable file/database patterns
as normal, not as odd edge cases:

- tombstone sidecar
  - live FLR file + packed-bit sidecar with one bit per physical record
  - delete = set bit
  - undelete before reap = clear bit
  - the bit count must always equal the current physical live-record
    count
- dead-record archive
  - compaction/reap can move tombstoned records into a separate archive
    dataset
  - restore after reap should reinsert by logical key into sort order,
    not by stale physical slot
- FLR -> VLR payload index
  - fixed record file stores key/metadata/offset
  - variable record file stores payload bytes
  - relation kind is `offset`
- structured text -> derived side indexes
  - source file remains authoritative
  - extracted record families are indexed into FLR/QDBM/BDB/SQLite
  - reindex can rebuild the side indexes from source

These patterns are central to the long-term goal of making madc a data
porting and storage-structure tool, not just a query wrapper.

### Reindex Workflows

Reindexing should be a first-class workflow for structured text and
semi-structured sources.

The design should support:

- full rebuild
  - discard and rebuild all derived indexes from source
- append-only refresh
  - catch up from a known source locator where the format allows it
- validate-only
  - confirm that derived indexes still match source
- invalidate-and-rebuild
  - explicit fallback when incremental guarantees are not available

This needs to be generic across formats such as:

- DSV/CSV
- TOML/INI/tagged text
- MUD flat files
- mailbox/message archives

The important point is that “reindex” belongs to index/materialization
policy, not to any one storage driver.

### `madc::Cursor<T>`

Streaming/iterative result access for direct scans and query results.

This avoids forcing eager materialization of whole datasets.

## Query IR and Planning Boundary

madc should treat data queries the same way it treats language work in
other parts of the system: as a compiler/planner pipeline with a clean
intermediate representation boundary.

The useful split is:

- logical query IR
  - intent-level operations such as scan, filter, project, join, and
    relation traversal
- physical query IR
  - executable plan nodes such as dataset scan, keyed lookup, pushed
    driver subplan, local join, or local projection

Execution should stay cursor-oriented:

- open
- next
- close

That keeps the planner free to stream results and only materialize when
it actually has to.

### `madc::Query`, `madc::QueryBuilder`, `madc::QueryExecutor`

Higher-level structured query API.

This should sit above direct dataset/relation APIs and compile to a
madc-owned query IR.

### Optional `madc::parse_gql(...)` and `madc::parse_sql(...)`

Textual query front-ends. Convenience only.

They should parse into the same query IR the builder API uses. Drivers
should not be required to parse raw GQL or raw SQL text themselves.

## Automatic Mapping Rules

The default user experience should be:

```cpp
struct User {
    int64_t id;
    std::string name;
    int32_t age;
};

madc::DataSet<User> users("sqlite:///tmp/app.db");
users.insert(u);
```

No field-by-field manual map should be required for the common case.

### Scalar Types

Supported by default:

- integer types
- floating-point types
- `bool`
- `char`
- `string`

These should map as a single persisted value or a one-column/one-field
record depending backend family.

### Struct Types

Default mapping:

- one persisted field per struct member
- member name becomes logical field name
- member `DataDef` drives storage type choice

Binary backends may persist the packed/inferred layout directly when the
layout is compatible and the caller opts into that policy.

Textual/SQL/graph backends should map by logical field names and types,
not by raw byte layout.

### Class Types

Default mapping should treat a class like a struct for persisted data:

- data members persist
- methods do not persist
- static const/method metadata are ignored by default

The persisted shape is the instance state, not the behavior.

### Dynamic Types

`MadValue` and `madc::value` should be first-class mappable types.

Default mapping:

- scalar dynamic values map as scalar records
- arrays map as ordered collections
- objects map as named-property documents

This gives us a natural bridge to:

- JSON documents
- graph node property bags
- Redis hashes
- schemaless document stores

## Metadata Without Full Manual Mapping

The system still needs a way to express things the type itself does not
say, especially:

- primary key
- unique key
- foreign key
- edge label
- alternate field name
- non-persistent member

The design should prefer small overlays instead of full remaps.

Example direction:

```cpp
auto users = madc::bind<User>(users_src)
    .key("id");

auto profiles = madc::bind<Profile>(profiles_src)
    .key("user_id");

auto user_profile = madc::relate(users, profiles)
    .from("id")
    .to("user_id")
    .name("has_profile");
```

This keeps the default member mapping inferred, while letting the user
add only the relational facts.

## Type-First, Schema-First, and Hybrid Modes

The storage system should support three equally valid modes.

### Type-first

The user already has a type:

```cpp
struct User {
    int64_t id;
    std::string name;
    std::string email;
};
```

madc infers the schema and default mapping automatically.

### Schema-first

The user wants to define the persisted structure directly, even if a
host-side type does not already exist.

Examples:

```c
sql::create TABLE users (
    id INT PRIMARY KEY,
    email TEXT
);
```

```c
cypher::create NODE User (
    id INT,
    name STRING,
    email STRING
);
```

```c
gql::create NODE TYPE User {
    id INT PRIMARY KEY,
    name STRING,
    email STRING
};
```

These should all lower into `StorageBinding`, not into backend-specific
one-off machinery.

### Hybrid

The user already has a type, but wants to refine the storage meaning at
the same time.

Example direction:

```c
sql::create TABLE users FOR User (
    id INT PRIMARY KEY,
    name TEXT,
    email TEXT UNIQUE
);
```

This should mean:

- keep `User` as the in-memory type
- attach storage-level key/constraint/naming metadata
- avoid forcing a full manual mapper

This hybrid mode is likely the most important long-term workflow.

## High-Level Helper Front-Ends

High-level schema definitions should be optional helper-class style
front-ends, parallel to the direct API, not replacements for it.

They should behave more like builders/helpers over the core storage
layer than like privileged syntax that the architecture depends on.

### `sql::create`

Purpose:

- relational/tabular schema-first declaration
- table/column/key/constraint definition
- optional binding to an existing type

Examples:

```c
sql::create TABLE users (
    id INT PRIMARY KEY,
    email TEXT
);
```

```c
sql::create TABLE users FOR User (
    id INT PRIMARY KEY,
    name TEXT,
    email TEXT UNIQUE
);
```

### `cypher::create`

Purpose:

- graph-flavored node and relationship declaration
- natural fit for backends such as FalkorDB
- optional binding to an existing type or relation

Examples:

```c
cypher::create NODE User (
    id INT,
    name STRING,
    email STRING
);
```

```c
cypher::create RELATIONSHIP FRIEND_OF (
    FROM User,
    TO User,
    since DATE
);
```

### `gql::create`

Purpose:

- graph-schema declaration in a portable, higher-level graph model
- sibling to `cypher::create`, not a separate execution substrate

Examples:

```c
gql::create NODE TYPE User {
    id INT PRIMARY KEY,
    name STRING,
    email STRING
};
```

```c
gql::create EDGE TYPE FRIEND_OF {
    FROM User,
    TO User,
    since DATE
};
```

### Lowering rule

All of the above should lower into the same normalized internal layer:

- `SchemaInfo`
- `StorageBinding`
- relation/edge metadata

Drivers should then materialize the result if their backend supports it.
This keeps the helper surfaces expressive without making the parser or
runtime backend-specific.

## Data Porting Goal

A major long-term advantage of this design is not just querying data in
place, but moving data between incompatible stores with type-aware
translation in the middle.

Target workflow:

- parse legacy source format into typed values
- infer or refine schema from existing madc types
- optionally declare relations/keys
- write out to a different backend or format

Examples:

- SMAUG text areas -> SQLite
- SMAUG text areas -> FalkorDB graph
- CSV -> FLR/VLR
- BDB/QDBM key/value store -> relational tables
- Redis hashes -> packed binary records

The direct typed API should therefore support ETL-style flows as a
first-class use case, not just "query where the data already lives."

## Backend Families

The same logical dataset API should map differently by family.

### Binary record families

Examples:

- `flr://`
- `vlr://`
- `qdbm://`
- `gdbm://`
- `bdb://`

Preferred defaults:

- leverage fixed/packed record layout where valid
- support direct binary encode/decode for structs/classes/scalars
- use relation metadata for index-to-payload linkage

### Legacy / irregular text families

Examples:

- SMAUG-style tagged text data
- line-oriented custom flat files
- block/tag-based legacy exports

Preferred defaults:

- parse through `FormatAdapter<T>`
- materialize into typed values using existing madc type metadata
- support export into any other backend once normalized
- support best-effort round-trip formatting where required

### Textual tabular families

Examples:

- `dsv://`
- CSV/TSV-like file backends

Preferred defaults:

- serialize fields textually by logical name
- support schema inference from type metadata
- support direct row iteration and filtered scans

### SQL families

Examples:

- `sqlite://`
- `mysql://`

Current local slice:

- `sqlite://` is implemented as the first SQL-backed keyed driver

Preferred defaults:

- map members to columns
- infer table-like schema from type metadata
- support pushdown for filter/project/join/sort/limit where possible

### Graph/document/key-value families

Examples:

- `falkordb://`
- `redis://`

Preferred defaults:

- map structs/classes to nodes/documents/hashes
- use `Relation<A, B>` as graph-edge or link definition
- support property-bag mapping from dynamic values

### Remote/service families

Examples:

- `rest://`
- `http://`
- `https://`
- `mcp://`
- `ftp://`

These should be treated as drivers with narrower capabilities and more
explicit error paths. Not every direct mutation/query operation will be
meaningful on every transport.

## Federated Query Model

One query should be able to span multiple different backends when the
planner can do so sensibly.

That means:

- madc owns the query parser/builder and query IR
- drivers advertise capabilities
- planner pushes down what each driver can do
- remaining joins/filter/projection happen in madc when needed

Example:

- users in SQLite
- friendships in FalkorDB
- profile details in CSV

One higher-level query may:

1. filter candidate users in SQLite
2. traverse edges in FalkorDB
3. read supplemental fields from CSV
4. join the results in madc

The planning pipeline should stay explicit:

1. query input
2. logical IR
3. binding/relation resolution
4. driver capability annotation
5. pushdown planning
6. physical IR
7. cursor execution

That keeps the system honest about where work happens and avoids
smuggling planner behavior into drivers or query-text front-ends.

## High-Level Query Positioning

GQL and SQL should both be supported as optional high-level convenience
surfaces.

Rules:

- users must not be forced to use either one
- query-builder, GQL, and SQL all lower into the same structured query
  IR
- drivers receive subplans/capabilities, not raw query text
- drivers may reject unsupported graph/relational operations with
  structured errors

This avoids making every driver a parser/planner and avoids building two
separate execution engines.

Recommended division of labor:

- query builder
  - primary structured front-end
  - backend-neutral
- high-level helper front-ends
  - `sql::create`, `cypher::create`, `gql::create`
  - define structure and mapping together
  - lower into `StorageBinding`
- GQL
  - graph-oriented convenience front-end
  - natural fit for node/edge traversal and pattern matching
- SQL
  - tabular/relational convenience front-end
  - natural fit for projection, filtering, grouping, joins, and
    aggregate workflows

Both textual layers should operate at the same level in the architecture.
Neither should be privileged over the other internally.

## Recommended Delivery Order

### V1 — Direct typed storage API

Land first:

- `DataSource`
- `DataDriver`
- `DataDriverRegistry`
- `SchemaInfo`
- `StorageBinding`
- `DataMapper<T>`
- `FormatAdapter<T>`
- `DataSet<T>`
- `Relation<A, B>`
- `Cursor<T>`

Backends:

- `file://`
- `dsv://`
- `flr://`
- `vlr://`
- initial legacy text adapter path (proof target: SMAUG-style tagged
  records or a simpler tagged-text sibling format)

Why first:

- proves automatic type inference
- proves direct binary/textual mapping
- proves legacy text import into typed values
- proves relation metadata without full manual remaps
- avoids network/auth/query complexity initially

### V2 — keyed local database backends

Add:

- `bdb://`
- `gdbm://`
- `qdbm://`

Why here:

- these are the bridge between raw file formats and full relational or
  graph engines
- they force the API to support keyed access, ordered lookup, and
  index/payload workflows early
- they fit naturally with `Relation<A, B>` for index/data separation

### V3 — SQL/document backends

Add:

- `sqlite://` (first slice implemented)
- initial `redis://` or document-style backend
- structured query builder
- capability advertisement and pushdown

### V4 — Federation + graph lift

Add:

- federated planner
- cross-source joins
- graph-lift of `Relation<A, B>`
- `falkordb://`

Federation V1 should stay deliberately narrow:

- reuse the existing structured builder path
- push scans/lookups/filters/projection/limit where a backend can honor
  them
- execute remaining joins, relation traversal glue, and projection
  locally
- avoid distributed aggregation, distributed sorting, or cross-source
  transactional claims in the first cut

### V5 — Optional textual query front-ends

Add:

- helper/schema front-ends
  - `sql::create`
  - `cypher::create`
  - `gql::create`
- SQL parser
- SQL-to-IR lowering
- GQL parser
- GQL-to-IR lowering
- graph-pattern and relational-text convenience surfaces

## Fit With Current Phase 4

This work should not begin as compiler magic.

It fits naturally as a `libmadc`/runtime subsystem because:

- current Phase 4 is already building host-facing API objects
- `madc::value` and `madc::error` are already public C++ facade types
- storage and federation want host/runtime abstractions, not token-level
  parser special-cases

The implementation should therefore be:

- `madc::` host/runtime classes first
- internal type metadata reuse from `DataDef`, `DataDefSTRUCT`,
  `DataDefCLASS`
- optional language syntax later, wrapping the same API

The long-term physical split should reflect that layering:

- `libmadc`
  - core embedding/runtime API
  - `DataSource`
- `libmadcdat`
  - optional data/federation/indexing subsystem

That keeps the storage/federation work available to ordinary C++ hosts
without forcing every `libmadc` consumer to carry the full data layer.

It also lines up well with future `madc::eval(...)` planning on the core
side: the core runtime owns compilation/execution/policy plus the
general outward-facing `DataSource` abstraction, while `libmadcdat`
owns storage/federation concerns and can be brought in only when the
host wants that surface.

## Open Questions

- whether JSON should be a dedicated public type or remain a view over
  `madc::value` / `MadValue`
- how much packed-binary persistence should trust current struct layout
  by default versus requiring explicit opt-in
- whether relation metadata should be configured fluently, by trait
  specialization, or by both
- whether `DataSet<T>` should own its mapper by type inference alone or
  allow per-instance override of backend policy
- how much transactional behavior belongs in `DataDriver` v1

## Recommended Next Step

The Phase 4-facing API sketch and first local backend family are now in
place. FLR tombstone/reap/restore lifecycle support is working, the
first FLR index -> VLR payload offset relation is working, and VLR
locator stability is now defined through an append-only tombstone-
sidecar contract. A first real query pushdown path now exists too:
typed `DataSet<T>::query(...)` can carry builder metadata into the
driver layer, and `sqlite://` plus the keyed local stores can execute
simple equality filters directly when the query shape matches their
capabilities.

The next concrete storage step is to move from single-backend CRUD
toward federated planning across the SQLite and keyed local backends,
while broadening pushdown beyond equality+limit and deciding how
relation/index maintenance should react to payload rewrites that
intentionally tombstone old VLR rows.
