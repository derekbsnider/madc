# Data Channels, Streaming Flows, and Process Endpoints Plan

**Status:** Proposed implementation plan  
**Date:** 2026-08-07  
**Repository:** `derekbsnider/madc`  
**Primary areas:** `libmadc`, `madcdis`, `madcdat`  
**Audience:** Codex / implementation agents

## 1. Purpose

Mad-C already has a strong typed data/storage substrate built around:

- `madc::DataSource`
- `madc::DataDriver`
- `madc::DataSet<T>`
- `madc::Cursor<T>`
- `madc::DataMapper<T>`
- `madc::FormatAdapter<T>`
- `madc::SourceAdapter`
- query/federation infrastructure

The next step is to generalize data movement without turning `DataSource` or
`DataDriver` into catch-all abstractions.

The desired end state is that data may be read from any reasonable source,
optionally filtered or transformed, and written to any reasonable destination.

Examples include:

- memory -> function -> memory
- file -> parser -> `DataSet<T>`
- database -> filter -> database
- HTTP/REST -> decode -> transform -> SQLite
- IMAP -> MIME parser -> filter -> database
- database -> encode -> SMTP
- executable stdout -> parser -> `DataSet<T>`
- `DataSet<T>` -> formatter -> executable stdin
- database -> formatter -> executable stdin -> executable stdout -> parser -> API
- process output -> transform -> file
- any combination of local, remote, in-memory, storage, IPC, and process
  endpoints where the participating capabilities make sense

The architecture must remain layered and capability-honest.

The central design decision is:

> `DataSource` identifies a location or external conduit.  
> `DataChannel` performs raw byte I/O.  
> `Cursor<T>` and `Sink<T>` perform typed streaming.  
> `DataDriver` remains the richer structured-storage abstraction.  
> Processes expose channels and lifecycle rather than pretending to be databases.

Do not collapse these concerns into one universal interface.

---

## 2. Existing Design Constraints

The implementation must preserve the current architectural split.

### 2.1 `DataSource` remains core and passive

`include/libmadc/datasource.h` remains the general external-conduit identity
object.

It must continue to represent location/identity rather than policy.

A `DataSource` may identify:

- file/storage locations
- database endpoints
- service endpoints
- sockets and IPC
- named pipes
- shared memory
- process executables
- other future conduits

Do not put transport policy, credentials, process argument vectors, environment
variables, HTTP headers, timeouts, retry policy, or backend tuning parameters
into the URI.

### 2.2 `madcdis` owns typed data mechanics

The current permanent homes for dataset, driver, mapping, query, relation, and
schema interfaces are under `include/madcdis/`.

The similarly named `include/madcdat/*.h` files are compatibility forwarding
shims and must not become parallel implementations.

### 2.3 `madcdat` owns optional external implementations

`madcdat` remains the optional external-driver/service layer.

It may provide implementations for:

- HTTP / HTTPS
- REST
- FTP
- S3
- SMTP / SMTPS
- IMAP / IMAPS
- external databases
- source adapters for external formats

Core interfaces must not depend on `madcdat`.

### 2.4 Low-level `madc::dis` primitives remain domain-independent

This work must not couple the following primitives to channels, processes,
protocols, or pipeline semantics:

- `arena`
- `id_table`
- `intern_table`
- `pod_record`
- `snapshot`
- `value_pool`

These remain substrate utilities.

---

## 3. Current Implementation Gaps

The public surface already looks partly streaming, but the actual driver
boundary is still eager.

### 3.1 `Cursor<T>` exists but is embedded in `dataset.h`

Current shape:

```cpp
template <typename T>
class Cursor
{
public:
    virtual ~Cursor() {}
    virtual bool next(T &out) = 0;
    virtual void close() = 0;
};
```

This is the correct conceptual primitive but should no longer be owned by
`dataset.h`.

### 3.2 Driver scans materialize into vectors

`DataDriver` currently exposes operations such as:

```cpp
virtual bool execute_query(
    const Query &query,
    std::vector<value> &out,
    error *err = nullptr) const;

virtual bool scan_records(
    std::vector<value> &out,
    error *err = nullptr) const = 0;
```

This forces eager materialization.

### 3.3 `DataSet<T>` wraps eager vectors in `VectorCursor`

`DataSet<T>::scan()` currently refreshes an internal snapshot, which:

1. asks the driver for a full `std::vector<value>`
2. decodes all rows into a full `std::vector<T>`
3. wraps that vector in `VectorCursor<T>`

Local query fallback similarly scans all driver rows into a vector before
filtering.

This is not suitable for:

- unbounded sources
- large remote responses
- process streams
- long-running pipes
- low-memory streaming ETL
- backpressure-aware pipelines

### 3.4 `FormatAdapter<T>` is already stream-shaped

`FormatAdapter<T>` currently has:

```cpp
virtual bool read_one(
    std::istream &is,
    T &out,
    error *err = nullptr) const = 0;

virtual bool write_one(
    std::ostream &os,
    const T &input,
    error *err = nullptr) const = 0;
```

This is useful and should be retained initially.

A channel-to-`streambuf` bridge can make existing format adapters work over
files, pipes, process channels, and remote response/request bodies without
redesigning every adapter.

### 3.5 `SourceAdapter` is also eager

`SourceAdapter::extract()` currently returns a
`std::vector<ExtractedRecord>`.

Keep compatibility initially, but eventually add a streaming extraction seam.

---

## 4. Architectural Model

The intended layering is:

```text
                    DataSource
                 identity/location
                       |
          +------------+-------------+
          |                          |
          v                          v
     DataChannel                  DataDriver
      raw bytes              structured storage
          |                          |
          v                          v
  parser / formatter           Cursor<value>
          |                          |
          +------------+-------------+
                       |
                       v
                    Cursor<T>
                       |
               filter / transform
                       |
                       v
                     Sink<U>
                       |
          +------------+-------------+
          |                          |
          v                          v
      DataChannel                 DataSet<U>
```

A process is modeled separately:

```text
                     Process
             lifecycle + exit status
               /        |        \
              v         v         v
           stdin      stdout    stderr
          channel     channel   channel
```

This allows a process to participate in a pipeline without forcing it through
the structured-storage CRUD interface.

---

## 5. Core Types to Add or Refactor

## 5.1 Extract `Cursor<T>`

Create:

```text
include/madcdis/cursor.h
```

Move the generic `Cursor<T>` interface out of `dataset.h`.

Move or recreate the vector-backed compatibility implementation as an internal
adapter, for example:

```cpp
namespace detail {
template <typename T>
class VectorCursor;
}
```

### Error propagation

The current `bool next(T&)` conflates end-of-stream and failure.

Change the underlying virtual contract to support an error object while
preserving source compatibility for callers:

```cpp
template <typename T>
class Cursor
{
public:
    virtual ~Cursor() {}

    virtual bool next(T &out, error *err) = 0;

    bool next(T &out)
    {
        return next(out, nullptr);
    }

    virtual void close() = 0;
};
```

Contract:

- `true` means one value was produced
- `false` with no error means clean end-of-stream
- `false` with a populated `error` means failure

All internal cursor implementations must be updated accordingly.

Do not introduce exceptions as the primary streaming error mechanism.

---

## 5.2 Add `Sink<T>`

Create:

```text
include/madcdis/sink.h
```

Initial shape:

```cpp
template <typename T>
class Sink
{
public:
    virtual ~Sink() {}

    virtual bool put(const T &value, error *err = nullptr) = 0;
    virtual bool close(error *err = nullptr) = 0;
};
```

Provide small adapters rather than a large sink hierarchy.

Initial useful sinks:

- function/callback sink
- container/back-insert sink
- `DataSetSink<T>`
- formatter/channel sink

A `DataSetSink<T>` should simply call `DataSet<T>::insert()` for each incoming
value.

Do not add distributed transaction semantics to `Sink<T>`.

---

## 5.3 Add lazy cursor adapters

Create:

```text
include/madcdis/flow.h
```

Initial operations:

```cpp
filter(...)
transform(...)
copy(...)
```

Possible internal classes:

```cpp
FilterCursor<T>
TransformCursor<In, Out>
FunctionCursor<T>
FunctionSink<T>
```

Required properties:

- lazy
- one input element at a time
- no implicit full materialization
- propagate upstream errors
- close upstream resources correctly
- allow arbitrary C++ functions/lambdas as local transforms

Example conceptual API:

```cpp
auto active = madc::filter(
    users.scan(),
    [](const User &u) {
        return u.active;
    });

auto names = madc::transform<User, std::string>(
    std::move(active),
    [](const User &u) {
        return u.name;
    });

madc::copy(std::move(names), output_sink, &err);
```

Exact ownership syntax may be adjusted to fit existing project conventions,
but ownership must be explicit and leak-free.

### Important planning boundary

A lambda passed to `filter()` or `transform()` is a local runtime operation.

Do not claim it is automatically query-pushdown capable.

Pushdown remains driven by the existing `Query` IR and driver capability
mechanism.

---

## 5.4 Add `DataChannel`

Create:

```text
include/libmadc/datachannel.h
```

This is a raw byte I/O abstraction.

It is not schema-aware and not query-aware.

Suggested V1 contract:

```cpp
struct ChannelCapabilities
{
    bool read = false;
    bool write = false;
    bool half_close = false;
    bool seek = false;
};

class DataChannel
{
public:
    virtual ~DataChannel() {}

    virtual const char *name() const = 0;
    virtual ChannelCapabilities capabilities() const = 0;

    virtual bool read(
        void *buffer,
        std::size_t capacity,
        std::size_t &bytes_read,
        error *err = nullptr) = 0;

    virtual bool write(
        const void *buffer,
        std::size_t size,
        std::size_t &bytes_written,
        error *err = nullptr) = 0;

    virtual bool flush(error *err = nullptr)
    {
        return true;
    }

    virtual void close_read() {}
    virtual void close_write() {}
    virtual void close() = 0;
};
```

Read contract:

- `true`, `bytes_read > 0`: bytes were read
- `true`, `bytes_read == 0`: clean EOF
- `false`: error

Write contract:

- partial writes are allowed
- helpers that require full writes must loop
- `false`: error

Do not rely on `size_t == 0` alone to distinguish EOF from error.

---

## 5.5 Add `DataChannelRegistry`

Create either:

```text
include/libmadc/datachannel_registry.h
```

or keep the registry beside `DataChannel` if that better matches project style.

It should parallel `DataDriverRegistry`, not replace it.

One scheme may legitimately have registrations in more than one registry.

Examples:

- `file://`
  - raw `DataChannel`
  - structured `DataDriver`
- `http://`
  - raw/request-body channel implementation
  - optional structured/service driver
- `exec://`
  - process endpoint, not a `DataDriver`

Do not impose a one-scheme-one-abstraction rule.

V1 may implement only local channel factories.

Remote channel/service implementations belong in `madcdat`.

---

## 5.6 Add channel/iostream bridge

Create:

```text
include/libmadc/channel_stream.h
```

or an equivalent internal/public split.

Implement `std::streambuf` adapters over `DataChannel` sufficient to expose:

- `std::istream`
- `std::ostream`

This allows the existing `FormatAdapter<T>` interface to work without a
redesign.

The first implementation should favor correctness and bounded buffering over
complex zero-copy optimization.

---

## 6. Evolve `DataDriver` to Streaming Without a Flag Day

Do not immediately remove the existing vector APIs.

Add streaming methods with default compatibility wrappers.

Suggested transitional shape:

```cpp
virtual std::unique_ptr<Cursor<value>>
scan_cursor(error *err = nullptr) const;

virtual std::unique_ptr<Cursor<value>>
execute_query_cursor(
    const Query &query,
    error *err = nullptr) const;
```

Default implementations may call the existing vector methods and return a
`VectorCursor<value>`.

This provides immediate compatibility for every existing driver.

Then migrate individual drivers to native streaming one at a time.

After all shipped drivers have native streaming paths, the vector methods may
be deprecated or retained only as convenience helpers.

### Capability advertisement

Consider adding:

```cpp
bool streaming_scan = false;
bool streaming_query = false;
```

to `DriverCapabilities`.

These flags should mean "natively streams without full materialization", not
merely "a cursor wrapper exists".

Do not block use of the cursor API when the flag is false; the compatibility
adapter still works.

---

## 7. Change `DataSet<T>` to Preserve Streaming

`DataSet<T>::scan()` and `DataSet<T>::query()` should consume driver cursors
directly.

Introduce a decoding cursor that wraps `Cursor<value>`:

```text
driver Cursor<value>
        |
        v
storage->logical mapping
        |
        v
DataMapper<T>::decode
        |
        v
    Cursor<T>
```

Do not decode an entire result set before returning a cursor.

### Preserve STL iterator behavior separately

The existing `begin()` / `end()` path may remain snapshot/materialization based
if that is required to preserve iterator stability and current semantics.

The distinction should be explicit:

- `scan()` / `query()` = streaming
- `begin()` / `end()` = snapshot/container-style iteration

Do not silently change iterator lifetime semantics in the same patch.

### Local query fallback

Rewrite local fallback so it filters as it pulls from the driver cursor.

Do not call `scan_records(vector)` first.

Projection may be applied row-by-row.

---

## 8. DataSource Extensions

Update `include/libmadc/datasource.h`.

### 8.1 Process scheme

Add:

```text
exec://
```

Example:

```text
exec:///usr/bin/jq
exec:///opt/madc/bin/normalize-users
```

The URI identifies the executable only.

Add a process/execution classification rather than pretending it is a
database.

Preferred shape:

```cpp
domain::execution
family::process
```

and:

```cpp
bool is_process() const;
```

`exec://` is local.

It should be parsed as a path-like local source.

### 8.2 Remote mail schemes

Reserve/classify service schemes:

```text
smtp://
smtps://
imap://
imaps://
```

Classification does not imply that a driver is already linked.

If no provider is registered, existing "no provider for scheme" behavior
should remain clean and explicit.

### 8.3 No shell URI

Do not add `shell://` in the first implementation.

Shell interpretation is not the default process contract.

A shell script with a valid shebang can be executed directly.

If explicit shell interpretation is required, the caller may invoke:

```text
exec:///bin/sh
```

with arguments such as:

```text
-c
<command>
```

This keeps shell parsing explicit.

---

## 9. Process Abstraction

Create:

```text
include/libmadc/process.h
```

Implementation may live in an appropriate `src/` file.

Suggested types:

```cpp
struct ProcessOptions
{
    std::vector<std::string> args;
    std::map<std::string, std::string> environment;
    std::string working_directory;

    // Add only options actually needed by the first implementation.
};

class Process
{
public:
    Process(const DataSource &source, const ProcessOptions &options);

    bool start(error *err = nullptr);

    DataChannel &stdin_channel();
    DataChannel &stdout_channel();
    DataChannel &stderr_channel();

    bool close_stdin(error *err = nullptr);
    bool wait(error *err = nullptr);

    bool exited() const;
    int exit_status() const;

    void terminate();
};
```

Exact return types may use smart pointers/references according to ownership
needs.

### Process execution rules

- no implicit shell
- arguments are passed as an argument vector
- environment is explicit
- process URI identifies executable path only
- stdin/stdout/stderr are separate channels
- child exit status is not encoded as EOF
- EOF on stdout is not by itself proof of successful exit
- stderr must never be silently merged into stdout unless requested
- close-on-exec discipline must be correct
- unused pipe ends must be closed in both parent and child
- child process resources must be reaped

Use the repository's existing platform/portability style.

On POSIX, prefer `posix_spawn` / `posix_spawnp` when practical. If the required
features make `fork`/`exec` clearer, isolate that decision behind the process
implementation.

Do not leak process implementation details into `DataSource`.

---

## 10. Process-as-Transformer Requires Concurrent Pumping

This is a hard requirement.

A process used as:

```text
source -> child stdin -> child stdout -> destination
```

must not be implemented as:

1. write all input
2. close stdin
3. read all output

That can deadlock when the child produces enough stdout before it finishes
consuming stdin.

Likewise, stderr can fill its pipe and deadlock the child if nobody drains it.

Add a process-pump helper only after basic process/channel functionality works.

Possible API direction:

```cpp
ProcessPumpResult pump(
    Cursor<ByteChunk> &input,
    Process &process,
    Sink<ByteChunk> &output,
    Sink<ByteChunk> *stderr_sink,
    error *err = nullptr);
```

The exact byte-chunk wrapper may differ.

Implementation must concurrently:

- feed stdin
- drain stdout
- drain stderr
- observe closure
- reap the child

A POSIX implementation may use:

- nonblocking file descriptors + `poll`
- nonblocking file descriptors + `select`
- a small bounded set of pump threads

Choose one approach and document it.

Do not use unbounded buffering as a deadlock workaround.

---

## 11. Typed Formatting Over Channels

Add adapters that combine `DataChannel` and `FormatAdapter<T>`.

Conceptually:

```text
DataChannel
   |
istream bridge
   |
FormatAdapter<T>::read_one()
   |
Cursor<T>
```

and:

```text
Sink<T>
   |
FormatAdapter<T>::write_one()
   |
ostream bridge
   |
DataChannel
```

Possible implementation helpers:

```cpp
template <typename T>
class FormatCursor;

template <typename T>
class FormatSink;
```

These should live in `madcdis` if they are generic typed-data mechanics, while
the raw channel and stream bridge remain in core `libmadc`.

This is the initial bridge between raw conduits and typed data.

---

## 12. Function and Memory Endpoints

The flow layer must make programmatic functions first-class participants.

Provide helpers equivalent to:

```cpp
from_generator<T>(...)
to_function<T>(...)
filter(...)
transform(...)
```

Examples:

```cpp
auto generated = madc::from_generator<Event>(
    [&](Event &out, error *err) -> bool {
        // produce one item or finish
    });
```

and:

```cpp
auto sink = madc::to_function<Event>(
    [&](const Event &event, error *err) -> bool {
        consume(event);
        return true;
    });
```

Memory/container adapters may initially use `std::vector<T>` or generic
back-insert semantics.

Do not create a separate "memory driver" merely to represent a C++ callback.

---

## 13. `SourceAdapter` Streaming Migration

Do this after the cursor/driver path is stable.

Add a streaming extraction method, for example:

```cpp
virtual std::unique_ptr<Cursor<ExtractedRecord>>
extract_cursor(
    const DataSource &source,
    const std::string &type_name,
    error *err = nullptr) const;
```

Default implementation:

1. call existing eager `extract(...)`
2. wrap the vector in `VectorCursor<ExtractedRecord>`

Then migrate large/unbounded adapters when useful.

Do not remove the existing vector API in the first pass.

This enables future use cases such as:

```text
IMAP mailbox
   |
SourceAdapter
   |
Cursor<Message>
   |
filter / transform
   |
DataSet<Message>
```

without forcing full mailbox materialization.

---

## 14. Remote Protocol Integration

Remote protocol implementations are not required for the first core streaming
milestone.

After the core abstractions are proven locally, add optional `madcdat`
implementations.

Candidate families:

- HTTP / HTTPS
- REST
- FTP
- S3
- SMTP / SMTPS
- IMAP / IMAPS

### Important rule

Do not force every protocol to look like an undifferentiated raw TCP stream.

Protocol implementations may expose the abstraction appropriate to their
semantics:

- HTTP response body as readable channel
- HTTP request body as writable channel
- FTP object as readable/writable channel
- SMTP message submission as typed or formatted sink
- IMAP message enumeration as cursor/source
- S3 object body as channel

They may internally use `DataChannel`, `Cursor<T>`, `Sink<T>`, or a
`DataDriver`, as appropriate.

Capability honesty is more important than uniformity.

---

## 15. Relationship to Query Federation

This work is complementary to query federation, not a replacement for it.

### Query path

Use when operations are describable in `Query` IR and may be pushed down.

```text
Query
  |
planner
  |
DataDriver capabilities
  |
remote/local execution
```

### Flow path

Use for arbitrary runtime data movement and functions.

```text
Cursor<T>
  |
lambda filter / transform
  |
Sink<U>
```

The planner must not pretend an opaque C++ lambda can be pushed down.

Later, a planner-produced cursor can feed a flow, and a flow can write into a
dataset or service.

That is enough integration for the first implementation.

---

## 16. Recommended File Layout

Target direction:

```text
include/libmadc/
    datasource.h
    datachannel.h
    datachannel_registry.h      # optional separate header
    channel_stream.h
    process.h

include/madcdis/
    cursor.h
    sink.h
    flow.h
    dataset.h
    driver.h
    mapper.h
    query.h
    relation.h
    schema.h
    ...

include/madcdat/
    source_adapter.h
    ... compatibility shims ...
    ... external protocol/driver headers ...
```

Implementation sources should follow existing repository naming conventions.

Do not create duplicate interfaces under `madcdat/`.

---

## 17. Phased Implementation

Each phase should compile and pass tests before continuing.

## Phase A — Extract cursor and add error propagation

Tasks:

- [ ] create `include/madcdis/cursor.h`
- [ ] move `Cursor<T>` out of `dataset.h`
- [ ] move/adapt `detail::VectorCursor<T>`
- [ ] add error-aware virtual `next(T&, error*)`
- [ ] keep convenience `next(T&)`
- [ ] update `DataSet`, `Relation`, and tests to include/use the new header
- [ ] no behavioral changes beyond error plumbing

Acceptance:

- all existing unit tests pass
- no `Cursor<T>` duplicate definitions
- old caller syntax `cursor->next(row)` still compiles

---

## Phase B — Add streaming driver compatibility seam

Tasks:

- [ ] add `scan_cursor()`
- [ ] add `execute_query_cursor()`
- [ ] default both to vector-backed compatibility
- [ ] optionally add native-streaming capability flags
- [ ] update `DataSet<T>::scan()` to use `scan_cursor()`
- [ ] update `DataSet<T>::query()` to use `execute_query_cursor()` when possible
- [ ] rewrite local query fallback as row-at-a-time cursor filtering
- [ ] retain vector driver APIs during migration
- [ ] preserve STL iterator snapshot semantics

Acceptance:

- `scan()` does not require `DataSet` to materialize all decoded rows
- a fake streaming driver can prove laziness
- existing drivers continue working unchanged through the fallback adapter

---

## Phase C — Add typed sink and local flow adapters

Tasks:

- [ ] create `sink.h`
- [ ] create `flow.h`
- [ ] implement `FilterCursor`
- [ ] implement `TransformCursor`
- [ ] implement function source/sink helpers
- [ ] implement `copy()` / pump from typed cursor to typed sink
- [ ] implement `DataSetSink<T>`
- [ ] add container/memory helpers as needed

Acceptance:

- pipeline advances one item at a time
- filter does not pull more than necessary
- transform does not materialize
- errors propagate to the caller
- downstream failure closes upstream cleanly

---

## Phase D — Add raw `DataChannel`

Tasks:

- [ ] create `datachannel.h`
- [ ] create registry/factory support
- [ ] implement memory channel for tests
- [ ] implement local file channel if not already reusable
- [ ] implement pipe/FIFO channel where appropriate
- [ ] add read/write/EOF/partial-write tests
- [ ] do not touch `DataDriverRegistry` semantics

Acceptance:

- raw bytes can move source -> destination without `DataDriver`
- partial writes are handled
- EOF and error are distinguishable
- one scheme can coexist in channel and driver registries

---

## Phase E — Add channel stream bridge and format cursors/sinks

Tasks:

- [ ] implement channel-backed `streambuf`
- [ ] expose input/output stream wrappers
- [ ] implement `FormatCursor<T>`
- [ ] implement `FormatSink<T>`
- [ ] prove an existing `FormatAdapter<T>` can operate over a non-file channel

Acceptance:

- format adapter API remains unchanged
- one-record-at-a-time decoding works
- one-record-at-a-time encoding works
- bounded buffering only

---

## Phase F — Add `exec://` and `Process`

Tasks:

- [ ] update `DataSource` scheme classification
- [ ] add `domain::execution`
- [ ] add `family::process`
- [ ] add `is_process()`
- [ ] add `ProcessOptions`
- [ ] add `Process`
- [ ] provide separate stdin/stdout/stderr channels
- [ ] implement wait/exit status
- [ ] explicitly avoid implicit shell execution
- [ ] build a repository-owned helper executable for tests

Acceptance:

- spawn helper executable
- send bytes to stdin
- read bytes from stdout
- read stderr independently
- observe exact argument boundaries
- observe child exit status
- no zombie process
- no descriptor leaks

---

## Phase G — Add safe process-through-flow pumping

Tasks:

- [ ] implement concurrent stdin/stdout/stderr pump
- [ ] add bounded buffering/backpressure
- [ ] integrate process transform with raw flow
- [ ] add typed format/process/format example after raw path is proven

Acceptance:

- large bidirectional transform cannot deadlock on pipe capacity
- stderr cannot deadlock the child
- downstream failure terminates/closes the flow cleanly
- child failure is reported independently of normal EOF

---

## Phase H — Stream `SourceAdapter`

Tasks:

- [ ] add `extract_cursor()`
- [ ] default to vector compatibility
- [ ] convert one existing adapter as proof
- [ ] keep eager API

Acceptance:

- existing adapters continue working
- at least one adapter proves incremental extraction

---

## Phase I — Optional `madcdat` remote endpoints

Tasks should be selected by actual need rather than implemented all at once.

Possible order:

1. HTTP/HTTPS response/request body
2. REST convenience layer
3. FTP object stream
4. S3 object stream
5. SMTP message sink
6. IMAP message source

Each implementation must advertise only meaningful capabilities.

Do not make remote protocol work block the core local-streaming milestone.

---

## 18. Tests to Add

Prefer deterministic unit/integration tests that do not require public network
access.

### Cursor tests

- vector cursor compatibility
- clean EOF
- error propagation
- lazy filter
- lazy transform
- early downstream close
- chained filter + transform

### Driver streaming tests

Create a fake driver that:

- owns N rows
- increments a counter each time a row is requested
- fails after a configurable row

Verify:

- requesting one result does not read all N rows
- `limit(1)` does not pull the full source
- local filtering streams
- errors are preserved

### Channel tests

Use memory-backed and OS-pipe-backed channels.

Verify:

- partial reads
- partial writes
- EOF
- closed read end
- closed write end
- flush behavior
- binary data including NUL bytes

### Process tests

Do not depend on `/usr/bin/jq`, `/bin/cat`, or another environment-specific
binary for core tests.

Build a tiny test helper executable that can:

- echo stdin to stdout
- transform bytes
- write controlled stderr
- return a selected exit code
- echo argv boundaries
- produce output while still consuming input

Use it to verify:

- no shell interpretation
- argument boundaries
- stdin/stdout
- stderr separation
- exit status
- concurrent pump deadlock resistance
- descriptor cleanup

### Format tests

Use a simple existing or test format adapter.

Verify:

- channel -> stream -> adapter -> `Cursor<T>`
- `Cursor<T>` -> adapter -> stream -> channel
- multiple records
- malformed record error

### DataSet flow tests

Verify:

```text
DataSet<A>
 -> scan cursor
 -> filter
 -> transform<A,B>
 -> DataSetSink<B>
```

with no full intermediate vector.

---

## 19. Compatibility Rules

Codex must follow these rules during implementation.

1. Do not remove the existing vector driver APIs in the first implementation.
2. Do not change `DataSource` into a configuration object.
3. Do not put argv/environment/options into URI query parameters.
4. Do not make `DataDriver` inherit every channel/process/service operation.
5. Do not add a monolithic `DataFlow` god object in V1.
6. Do not require remote protocol libraries for core `libmadc`.
7. Do not require `madcdat` for in-memory/local flow composition.
8. Do not change STL iterator snapshot semantics while making `scan()` streaming.
9. Do not introduce implicit shell execution.
10. Do not equate process EOF with process success.
11. Do not sequentially "write all then read all" for a duplex child process.
12. Do not materialize an entire stream merely to adapt it to `Cursor<T>`.
13. Do not couple `madc::dis` allocator/intern/snapshot primitives to this layer.
14. Do not teach the query planner to introspect arbitrary C++ lambdas.
15. Preserve existing public names and forwarding shims unless a phase explicitly
    requires migration.

---

## 20. Suggested First Codex Slice

The first implementation PR should be deliberately narrow.

Implement only:

1. `madcdis/cursor.h`
2. error-aware cursor contract with compatibility convenience overload
3. `DataDriver::scan_cursor()` compatibility wrapper
4. `DataDriver::execute_query_cursor()` compatibility wrapper
5. streaming `DataSet<T>::scan()`
6. streaming local query fallback
7. tests proving laziness
8. no process code yet
9. no remote protocol code yet
10. no flow DSL yet

This slice establishes the architectural seam everything else depends on and
can be reviewed independently.

The second PR should add `Sink<T>` + filter/transform/copy.

The third PR should add `DataChannel`.

The fourth PR should add `exec://` + `Process`.

Do not combine all phases into one change.

---

## 21. Example End-State Usage

These examples are directional, not syntax requirements.

### Typed dataset transform

```cpp
auto input = users.scan(&err);

auto active = madc::filter(
    std::move(input),
    [](const User &u) {
        return u.active;
    });

auto export_rows = madc::transform<User, ExportUser>(
    std::move(active),
    [](const User &u) {
        return ExportUser(u);
    });

madc::DataSetSink<ExportUser> sink(exports);
madc::copy(std::move(export_rows), sink, &err);
```

### Process output as source

```cpp
madc::ProcessOptions opts;
opts.args.push_back("--emit-events");

madc::Process proc(
    madc::DataSource("exec:///opt/madc/event-source"),
    opts);

proc.start(&err);

madc::ChannelInputStream input(proc.stdout_channel());
EventFormatAdapter format;

auto events = madc::format_cursor<Event>(input, format);
madc::copy(std::move(events), madc::DataSetSink<Event>(event_db), &err);

proc.wait(&err);
```

### Typed data through an executable transformer

```text
DataSet<User>
   |
Cursor<User>
   |
JSON-lines encoder
   |
process stdin
   |
exec:///opt/madc/normalize-users
   |
process stdout
   |
JSON-lines decoder
   |
Cursor<User>
   |
DataSetSink<User>
```

The implementation of this full-duplex shape must use the concurrent process
pump described earlier.

### Remote endpoint

```text
IMAP message source
   |
Cursor<Message>
   |
filter
   |
transform
   |
SMTP message sink
```

Neither endpoint is required to pretend to be a CRUD database.

---

## 22. Definition of Done

The architecture is considered established when:

- `DataSource` still identifies location only
- `Cursor<T>` is a standalone reusable primitive
- storage scans can stream across the `DataDriver` boundary
- `DataSet<T>::scan()` no longer inherently materializes the full dataset
- local filters/transforms compose lazily
- typed sinks exist
- raw channels exist independently of storage drivers
- existing `FormatAdapter<T>` implementations can operate over channels
- `exec://` identifies executable endpoints
- a `Process` exposes stdin/stdout/stderr separately
- a process can safely participate as a streaming transformer without pipe
  deadlock
- existing storage drivers continue to work during migration
- remote protocol implementations can be added in `madcdat` without changing
  the core abstraction

At that point Mad-C has a coherent foundation for data movement across memory,
files, databases, APIs, processes, pipes, and user functions without
sacrificing the existing typed-storage and federation model.
