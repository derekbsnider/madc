# libmadc Phase 4 Plan

## Goals

- Embed madc safely in a Node.js server.
- Expose madc as a reusable library for native C++ hosts.
- Compile once and execute many times.
- Support fork-based isolation with IPC.
- Keep a stable C ABI for Node bindings and other FFI consumers.

## Non-Goals

- In-process untrusted script execution inside the main Node.js process.
- Async runtime integration in v1.
- Arbitrary host object exposure in v1.
- Full sandboxing through language rules alone.

## Product Direction

`libmadc.so` should serve two host classes:

- **Node.js hosts** using a safe worker/supervisor model.
- **Native C++ hosts** linking directly against the library.

The recommended shape is:

- **C++-first internally** for natural engine implementation and native host ergonomics.
- **C-ABI-first externally** for stability, Node integration, and future FFI use.

That means:

- a rich C++ API for native hosts
- a thin `extern "C"` wrapper over it
- Node built on the C ABI, not on the C++ ABI directly

## Architecture

### Deliverables

- `libmadc.so`
  Core engine/library.
- `madc_worker`
  Worker binary linked against `libmadc.so`.
- Node.js supervisor / addon
  Starts warm workers, forks children, performs IPC, enforces timeouts.

### Layering

- **Layer 1: engine internals**
  Compiler/runtime state, namespace registry, policy, value marshaling.
- **Layer 2: public C++ API**
  Native host embedding surface.
- **Layer 3: public C ABI**
  Stable wrapper for Node/FFI clients.
- **Layer 4: Node integration**
  Control plane, worker orchestration, IPC glue.

### Planned optional sublibrary split

The growing storage/federation subsystem should not bloat the core
embedding library surface indefinitely.

Planned direction:

- `libmadc`
  - core engine/runtime/embedding API
  - `engine`, `program`, `value`, `error`, `DataSource`, policy, invoke limits
- `libmadcdat`
  - optional inner data/federation/indexing subsystem
  - drivers, mappings, relations, source adapters, extracted-record
    families, indexes, reindex workflows, query/planning

The subsystem name for that future boundary is now `madcdat`. The name
is official before the physical split: current storage/federation work
still lands inside the existing `libmadc` tree, while `libmadcdat`
remains a later optional-build outcome of that layering.

`DataSource` stays on the core `libmadc` side of that line. It is the
general external-conduit abstraction that `madcdat` builds on, not a
storage-only concept to be extracted with the rest of the data layer.

Why split:

- keep the core embedding/runtime API cohesive
- keep optional storage/database dependencies out of the base build
- keep the storage/federation work independently extensible
- preserve a small, comprehensible `libmadc` surface for hosts that do
  not need data federation at all

Build direction:

- `./configure --enable-madcdat`
- later optional backend flags can hang under that umbrella
- do not block current Phase 4 embedding work on the physical split, but
  plan for the boundary now

## Safety Model

The Node.js embedding story should treat madc as an **out-of-process
tenant runtime**, not as an in-process extension language.

### Authority model

Capability belongs to the invoker, not to the script.

Recommended authority modes:

- `system_locked`
  - system policy can lock settings
  - user or script cannot widen capability
- `user_controlled`
  - user controls policy for a user-owned installation
- `host_authoritative`
  - embedding host owns the final policy
  - script cannot widen capability

This implies:

- a system-installed `madc` binary can be locked down by system config
- a user-installed/private `madc` can run with user-controlled policy
- `libmadc` callers fully control capability when embedding

### Default safe mode

- `safe_mode = true`
- block `dlopen`
- block `#load`
- allow-list dlsym symbols instead of deny-listing
- refuse dangerous host registrations by default
- enforce CPU / memory / output / process-count limits
- execute user scripts only in forked child processes

### OS-level sandbox direction

- `fork()` per request or from a warm fork pool
- `setrlimit()` for:
  - `RLIMIT_CPU`
  - `RLIMIT_AS`
  - `RLIMIT_FSIZE`
  - `RLIMIT_NPROC`
- dedicated worker uid/gid where possible
- optional seccomp filter
- optional namespace / chroot isolation

The key rule is:

- Node passes data in
- madc returns data out
- scripts do not get ambient host authority unless explicitly granted

### Policy/config model

Canonical external policy format:

- TOML

Canonical implementation model:

- C++ objects owned by `MadcEngine`

Recommended layers:

1. built-in defaults
2. system TOML
3. optional user/project TOML when allowed
4. environment / CLI overrides
5. explicit host API overrides

Library rule:

- `libmadc` should not auto-read policy files unless the caller opts in

CLI rule:

- the `madc` binary should auto-discover TOML config in standard locations
  like `/etc/madc/madc.toml` and `/usr/local/etc/madc/madc.toml`

Recommended precedence:

1. explicit host API settings
2. CLI flags
3. environment overrides
4. project-local config
5. `/usr/local/etc/madc/madc.toml`
6. `/etc/madc/madc.toml`
7. built-in defaults

### Capability model

Policy should support broad grants plus fine-grained exceptions.

Recommended grant layers:

- header/module groups
  - `stdio.h`
  - `iostream`
  - `string.h`
  - `stdlib.h`
  - `syslog.h`
  - `dlfcn.h`
  - `unistd.h`
  - `fcntl.h`
- risk categories
  - `filesystem_read`
  - `filesystem_write`
  - `process_spawn`
  - `dynamic_loading`
  - `network`
  - `syslog`
  - `fd_redirect`
  - `env_mutation`
- per-symbol overrides
  - `allowed_symbols`
  - `denied_symbols`
- namespace/builtin overrides
  - `allowed_namespaces`
  - `denied_namespaces`
  - `allowed_builtins`
  - `denied_builtins`

Rule:

- deny wins over allow

Example:

- allow all of `stdio.h`
- deny `gets`

### C surface vs curated madc surface

Default safe posture should treat raw libc as high-risk and curated madc
helpers as preferred.

Recommended default:

- deny most raw C / libc capability in safe mode
- allow a curated `madc::` surface
- selectively reopen raw libc only when host policy allows it

This means the standard C interface should be policy-gated more tightly than
engine-owned C++ or `madc::` wrappers.

### Safe wrapper strategy

Prefer engine-owned `madc::` helpers over raw libc when a safer wrapper can
enforce stronger invariants.

Examples:

- `madc::strdup`
  - track source size
  - allocate `size + 1`
  - guarantee null termination
- `madc::puts`
  - validate/copy into a known NUL-terminated buffer before calling libc
- `madc::printf`
  - use a constrained, validated formatting surface rather than raw variadic
    passthrough

These wrappers should be the preferred allowed surface in restricted modes,
while raw libc remains optional and policy-gated.

### Namespace preference as host policy

The current source-level `prefer ...;` directive should also be available as
engine/program configuration.

Recommended behavior:

- host can preset namespace preference before parse
- script-level `prefer` may refine it only when policy allows
- locked-down modes may ignore or reject source-level `prefer`

This lets `libmadc` callers set a default resolution order like:

- `madc, std, c`
- `rust, madc, c`

without requiring the script to contain a `prefer` directive.

## Node.js Roadmap

### Target model

Use a **precompiled warm parent** process:

1. parent starts
2. parent creates engine/program
3. parent registers allowed host functions
4. parent loads source
5. parent compiles once
6. parent optionally runs one-time init
7. parent waits idle with compiled code resident
8. on request, parent `fork()`s
9. child reads request over IPC
10. child invokes one madc entrypoint
11. child writes result/error over IPC
12. child exits

This gives:

- compile once
- execute many times
- copy-on-write sharing of compiled state
- per-request isolation
- clean state reset on child exit

### Phase N1

- build `libmadc.so`
- build `madc_worker`
- implement JSON IPC
- no Node addon yet; prove the worker protocol first

### Phase N2

- add a Node supervisor or N-API control wrapper
- parent process stays warm
- Node submits requests, reads replies, enforces deadlines

### Phase N3

- optional fork pool instead of pure fork-per-request
- structured logs and better error reporting
- metrics / health checks

### Phase N4

- optional binary IPC format after JSON is stable
- optional persistent workers for trusted workloads

## Core API Shape

### C++ API

Suggested types:

- `madc::engine`
- `madc::program`
- `madc::value`
- `madc::error`
- `madc::policy`
- `madc::compile_options`
- `madc::invoke_options`

Suggested responsibilities:

- `engine`
  - owns registry, policy, and shared configuration
  - creates programs
- `program`
  - loads source
  - compiles source
  - exposes entrypoint calls
  - runs one IPC session
- `value`
  - marshaled argument/result container
- `error`
  - structured diagnostics

### Suggested C++ sketch

```cpp
namespace madc {

struct error {
	std::string message;
	std::string file;
	int line = 0;
	int column = 0;
};

struct compile_options {
	bool safe_mode = true;
	bool allow_dlopen = false;
	bool allow_load_directive = false;
};

enum class authority_mode {
	system_locked,
	user_controlled,
	host_authoritative
};

template <typename T>
struct policy_value {
	T value;
	bool locked = false;
};

struct security_policy {
	authority_mode mode = authority_mode::host_authoritative;

	policy_value<bool> safe_mode{true, false};
	policy_value<bool> allow_dlopen{false, false};
	policy_value<bool> allow_load_directive{false, false};
	policy_value<bool> allow_file_read{true, false};
	policy_value<bool> allow_file_write{false, false};
	policy_value<bool> allow_process_spawn{false, false};
	policy_value<bool> allow_fd_redirect{false, false};
	policy_value<bool> allow_syslog{false, false};

	std::set<std::string> allowed_headers;
	std::set<std::string> denied_headers;
	std::set<std::string> allowed_categories;
	std::set<std::string> denied_categories;
	std::set<std::string> allowed_symbols;
	std::set<std::string> denied_symbols;
	std::set<std::string> allowed_namespaces;
	std::set<std::string> denied_namespaces;
};

struct invoke_limits {
	uint64_t cpu_ms = 0;
	uint64_t memory_bytes = 0;
	uint64_t output_bytes = 0;
};

struct invoke_options {
	invoke_limits limits;
};

class value {
public:
	enum class kind {
		null,
		boolean,
		integer,
		real,
		string,
		bytes,
		array,
		object
	};
};

class engine {
public:
	static std::unique_ptr<engine> create();
	bool register_function(...);
	bool register_namespace_function(...);
	bool load_policy_file(...);
	bool set_namespace_preference(...);
	std::unique_ptr<class program> create_program();
};

class program {
public:
	bool load_file(const std::string& path, error* err = nullptr);
	bool load_source(const std::string& name, const std::string& src, error* err = nullptr);
	bool compile(const compile_options&, error* err = nullptr);
	value eval(const std::string& src,
	           const invoke_options& opts = {},
	           error* err = nullptr);

	bool has_function(const std::string& name) const;
	value call(const std::string& fn,
	           const std::vector<value>& args,
	           const invoke_options& opts = {},
	           error* err = nullptr);

	bool warm(error* err = nullptr);
	bool run_ipc_session(int input_fd, int output_fd, int error_fd,
	                     const invoke_options& opts = {},
	                     error* err = nullptr);
};

}
```

### `madc::eval(...)` direction

`madc::eval(...)` is worth planning explicitly because it becomes very
powerful when combined with:

- sandbox policy
- parser/namespace restrictions
- allowed/denied function registration
- invoke limits
- future specialized source/parser adapters

But it must not become a side door around those controls.

Rules:

- `eval(...)` uses the same `security_policy` and namespace/builtin
  restrictions as file-based program execution
- `eval(...)` uses the same parser/registration model as ordinary
  `load_source(...)/compile(...)`
- `eval(...)` honors the same `invoke_limits`
- in safe/worker modes, `eval(...)` still executes under the same
  process-isolation story as other script execution

Semantically, `eval(...)` should be treated as “compile this source
string under the current engine/program policy, run it, and marshal a
`madc::value` result back”, not as a privileged internal escape hatch.

## Stable C ABI

The C ABI should wrap the C++ layer and stay conservative.

### Suggested C ABI surface

```c
typedef struct madc_engine madc_engine;
typedef struct madc_program madc_program;
typedef struct madc_error madc_error;
typedef struct madc_value madc_value;

madc_engine *madc_engine_create(void);
void madc_engine_destroy(madc_engine *);

madc_program *madc_program_create(madc_engine *);
void madc_program_destroy(madc_program *);

int madc_program_load_file(madc_program *, const char *path, madc_error *);
int madc_program_load_source(madc_program *, const char *name, const char *src, madc_error *);
int madc_program_compile(madc_program *, const madc_compile_options *, madc_error *);

int madc_program_has_function(madc_program *, const char *name);
int madc_program_call(madc_program *, const char *fn,
                      const madc_value *args, size_t argc,
                      const madc_invoke_options *,
                      madc_value *result, madc_error *);

int madc_program_run_ipc_session(madc_program *,
                                 int input_fd, int output_fd, int error_fd,
                                 const madc_invoke_options *,
                                 madc_error *);
```

## IPC Contract v1

Use JSON first. It is slower than a binary protocol, but much easier to
debug and adequate for the first safe worker model.

### Request

```json
{
  "entrypoint": "handle_request",
  "args": [
    {
      "type": "object",
      "value": {
        "user_id": 123
      }
    }
  ],
  "limits": {
    "cpu_ms": 100,
    "memory_bytes": 134217728
  }
}
```

### Success response

```json
{
  "ok": true,
  "result": {
    "type": "object",
    "value": {
      "status": "ok"
    }
  }
}
```

### Error response

```json
{
  "ok": false,
  "error": {
    "message": "runtime error",
    "file": "script.mad",
    "line": 42,
    "column": 9
  }
}
```

## Host Function Model

Two trust levels should exist from the start:

- **trusted/native host mode**
  - richer function registration
  - potentially direct native object integration later
- **tenant safe mode**
  - curated pure functions and restricted host services only

Recommended registration buckets:

- pure value transforms
- IO-less helpers
- explicitly controlled host services
- forbidden-by-default process/system functions

## State Model

Separate these carefully:

- compile-time state
- warm-parent immutable state
- child runtime state
- per-request invocation state

The parent should hold reusable compiled artifacts only. Mutable script
execution should occur in children so request state is reset on exit.

## Phase Breakdown

### Phase 4.1 — Decouple static globals

This is the real prerequisite for `libmadc.so`.

Tasks:

- move process-global compiler/runtime state into engine/program objects
- make namespace registration instance-owned
- make source/error/build state instance-owned
- isolate CLI-specific behavior from library code
- define a policy object for safe mode and symbol allow-lists

### Phase 4.2 — Public C++ embedding API

Tasks:

- add `madc::engine`
- add `madc::program`
- add `load_source`, `compile`, `call`
- plan `eval(...)` on the same policy/limits surface, not as a bypass
- add value and error types
- add host function registration

### Phase 4.3 — Shared library + C ABI

Tasks:

- build `libmadc.so`
- add `extern "C"` wrapper API
- ensure CLI uses the same underlying engine
- keep ABI surface smaller and more stable than the C++ surface
- keep `libmadcdat` optional and separate from the minimal C ABI unless
  the caller explicitly opts into the data subsystem

### Phase 4.4 — Worker model + Node

Tasks:

- build `madc_worker`
- implement JSON IPC
- implement warm parent + child fork execution
- implement Node supervisor / addon
- add timeout, reaping, and logging support

## First Practical Deliverable

The smallest meaningful v1 should support:

- create engine
- load source
- compile source
- call one named function
- exchange JSON-like values
- run in safe mode by default
- execute requests in child processes only

## Suggested Repo Work

- `include/madc/api.hpp`
  Public C++ API
- `include/madc/api.h`
  Public C ABI
- `src/libmadc.cpp`
  API wrappers / library entrypoints
- `src/madc_worker.cpp`
  Worker binary
- future optional split:
  - `include/madcdat/`
  - `src/madcdat_*.cpp`
  - optional `libmadcdat` build target gated by `--enable-madcdat`
- `src/Makefile`
  Shared library and worker build targets
- `docs/plans/libmadc-phase4.md`
  Authority, TOML policy, capability grouping, and safe-wrapper design

Tests to add:

- library compile/load/call smoke tests
- multiple independent program instances
- worker request/response round-trip
- safe-mode restrictions
- forked child execution
- policy merge / lock tests
- header allow + symbol deny tests
- host-preset namespace preference tests

## Phase 4.1 Inventory

This is the current state inventory for the library split. The goal of
Phase 4.1 is to decide what can remain process-wide, what must become
`engine` state, what must become `program` state, and what should stay
CLI-only.

### A. Mutable `Program` state that already wants to be `program`-owned

These fields already live on `Program` and are conceptually per loaded
script set. They should migrate almost directly into the future
`madc::program` implementation object.

- tokenization/parsing state in [include/madc.h](/workspace/madc/include/madc.h)
  - `tokens`
  - `injected_tokens`
  - `ast`
  - `pending_funcs`
  - `compounds`
  - `_prv_token`
  - `_cur_token`
  - `_braces`
  - `source`
- script symbol tables in [include/madc.h](/workspace/madc/include/madc.h)
  - `funcdef_map`
  - `literal_map`
  - `define_map`
  - `macro_map`
  - `lazy_map`
  - `included_files`
  - `ptr_type_cache`
- type/namespace registration owned by the active compile in [include/madc.h](/workspace/madc/include/madc.h)
  - `datatype_map`
  - `datadef_map`
  - `struct_map`
  - `namespace_map`
  - `namespace_preference`
  - `current_namespace`
- compile/runtime state in [include/madc.h](/workspace/madc/include/madc.h)
  - `jit`
  - `code`
  - `cc`
  - `root_fn`
  - `tkProgram`
  - `tkFunction`
  - `cur_func_name`
  - `jit_anchor_labels`
  - `jit_source_map`
  - `jit_source_map_enabled`
  - `label_map`
  - `_pack_stack`
  - `loopstack`
  - `ifstack`
- script invocation inputs in [include/madc.h](/workspace/madc/include/madc.h)
  - `script_argc`
  - `script_argv`
- parse/compile mode flags in [include/madc.h](/workspace/madc/include/madc.h)
  - `parsing_extern_decl`
  - `parsing_static_decl`
  - `_include_iostream`
  - `_include_stdio`

**Phase 4.1 action:** keep this state together and make it possible to
construct/destroy multiple independent program instances in one process.

### B. Process-global mutable state that must not stay global

These are the main blockers for embedding and multiple independent
library users.

- `bool madc_verbose` in [src/madc.cpp](/workspace/madc/src/madc.cpp:31)
  - currently process-global debug mode
  - should become engine/program logging configuration
- `static MadcAsmjitErrHandler g_madc_asmjit_err` in [src/compiler.cpp](/workspace/madc/src/compiler.cpp:1039)
  - currently one shared diagnostic handler rebound to whichever
    `Program` compiled most recently
  - should become per-program compile diagnostics state
- JIT crash/source-map globals in [src/compiler.cpp](/workspace/madc/src/compiler.cpp:1044)
  - `g_madc_jit_map`
  - `g_madc_jit_map_size`
  - `g_madc_jit_code_base`
  - `g_madc_jit_code_size`
  - these exist only so the CLI crash handler can translate a faulting
    RIP into source locations
  - should move behind a library/worker crash-reporting hook, not remain
    public mutable globals
- `throwstream throwit` in [src/madc.cpp](/workspace/madc/src/madc.cpp:33)
  - legacy process-global error stream object
  - likely removable or CLI-only

**Phase 4.1 action:** eliminate direct mutable globals here or wrap them
behind instance-owned state and explicit callbacks.

### C. Process-global immutable metadata that can probably remain singleton

These are not the same kind of blocker, because they are effectively
read-only metadata after startup.

- base `DataDef` singletons in [src/parser.cpp](/workspace/madc/src/parser.cpp:657)
  - `ddVOID`, `ddINT`, `ddSTRING`, `ddARRAY`, etc.
- primitive pointer-type singletons in [src/parser.cpp](/workspace/madc/src/parser.cpp:684)
  - `ddVOIDptr`, `ddCHARptr`, `ddINTptr`, `ddINT32ptr`
- static lexer keyword/datatype token instances in [src/lexer.cpp](/workspace/madc/src/lexer.cpp:145)
  - `tkIF`, `tkFOR`, `tkSTRUCT`, `tkBOOL`, `tkC23BOOL`, etc.
- embedded header blob table in [src/embedded_headers.cpp](/workspace/madc/src/embedded_headers.cpp:5)

These can likely stay process-wide if they remain immutable. They do not
need to block `libmadc.so` unless later API work requires allocator or
engine-specific ownership of type metadata.

**Phase 4.1 action:** explicitly mark these as read-only metadata and
avoid mixing mutable registration state into them.

### D. Built-in function and namespace registration that should become `engine` state

Current built-ins are registered through parser init helpers:

- `Program::add_functions()` in [src/parser.cpp](/workspace/madc/src/parser.cpp:1194)
- `Program::add_globals()` in [src/parser.cpp](/workspace/madc/src/parser.cpp:1244)
- `Program::add_iostream()` / `add_stdio()` in [src/parser.cpp](/workspace/madc/src/parser.cpp:1252)
- `Program::add_namespaces()` and language namespace registration in [src/parser.cpp](/workspace/madc/src/parser.cpp:1333)
- `_parser_init()` in [src/parser.cpp](/workspace/madc/src/parser.cpp:1393)

Today, every `Program` instance self-registers the whole built-in world.
For library use this should split into:

- **engine-owned registry**
  - built-in native functions
  - built-in namespaces
  - safe-mode policy
  - host-registered functions
- **program-owned view**
  - references to the registry active for that compile

This matters especially because some current registrations are unsafe for
tenant embedding:

- `system`
- `getenv`
- `setenv`
- `unsetenv`
- `dlopen`
- `dlsym`
- `dlclose`
- `dlcall`

Those live in [src/parser.cpp](/workspace/madc/src/parser.cpp:1194) today
as ordinary built-ins. Safe mode needs them to be policy-gated.

**Phase 4.1 action:** separate built-in registration from per-program
compile state and make the registry configurable by policy.

### E. CLI-only concerns that should not leak into the library surface

These are appropriate for the CLI binary or worker binary, but not for
the core library object model.

- `main()` flow in [src/madc.cpp](/workspace/madc/src/madc.cpp:213)
- crash-handler installation in [src/madc.cpp](/workspace/madc/src/madc.cpp:45)
- resource-limit env parsing in [src/madc.cpp](/workspace/madc/src/madc.cpp:182)
- `prog.colors = true` in [src/madc.cpp](/workspace/madc/src/madc.cpp:224)
- direct `argv` wiring into `Program` in [src/madc.cpp](/workspace/madc/src/madc.cpp:241)

The library should instead expose:

- explicit logging configuration
- explicit sandbox/limits configuration
- explicit invocation arguments
- explicit error/callback hooks

**Phase 4.1 action:** move CLI bootstrapping into a thin adapter over the
future engine/program API.

### F. State classification target

Recommended target ownership:

- **process-wide immutable metadata**
  - base `DataDef` instances
  - static token descriptors
  - embedded header source text
- **engine-owned**
  - built-in function registry
  - namespace registry
  - safe-mode policy
  - host registration tables
  - logging / diagnostics configuration
- **program-owned**
  - parsed AST
  - typedef/struct maps
  - macros / defines / include set
  - compiled code
  - resolved entrypoints
  - script-global state
- **invocation-owned**
  - request arguments
  - result buffers
  - per-call limits
  - per-call error context
- **CLI-only**
  - signal handlers
  - env-driven resource-guard defaults
  - terminal color output
  - command-line parsing

## Phase 4.1 Checklist

### Step 1: isolate engine-facing registry setup

- extract built-in registration from `_parser_init()`
- create a registry object that can be reused across programs
- move policy-gated built-ins behind allow/deny configuration
- make namespace-preference defaults engine-owned

### Step 2: kill mutable process globals

- remove `g_madc_asmjit_err` as shared mutable state
- remove or encapsulate the JIT source-map crash globals
- move `madc_verbose` into explicit config
- delete or quarantine `throwit`

### Step 3: separate CLI bootstrapping from core engine behavior

- move signal/resource setup behind CLI/worker-specific code
- stop relying on process env vars directly inside core compile paths
- make script invocation inputs explicit API parameters

### Step 4: prove multiple independent program instances

Add tests that:

- create two separate program instances in one process
- compile different sources in each
- execute both without symbol or state bleed
- verify macros / typedefs / globals do not leak across programs

### Step 5: prepare the worker model

Once steps 1 through 4 are done:

- add engine creation/destruction API
- add program creation/destruction API
- add load/compile/call entrypoints
- then build `madc_worker`

### Step 6: add authority + TOML policy loading

- add `MadcSecurityPolicy` and `MadcLimits` objects on `MadcEngine`
- add TOML parsing for engine/security policy
- support system config discovery in CLI only
- keep library config loading explicit opt-in
- implement lock semantics for system/host policy

### Step 7: enforce capability groups and carve-outs

- support header/module allow-lists
- support header/module deny-lists
- support symbol-level additive and subtractive overrides
- make deny win over allow
- gate `#load`, `dlopen`, `dlsym`, and dangerous built-ins through this model

### Step 8: grow the curated `madc::` safe surface

- add safe wrappers for selected libc functionality
- prefer `madc::` wrappers in safe mode
- keep raw libc access explicitly policy-gated

## Open Design Questions

- exact `madc::value` representation
- whether globals are per-program or invocation snapshot state
- whether warm init is explicit or implicit
- host-function error-reporting contract
- whether Node uses a native addon supervisor or a plain sidecar process first

## Recommended Starting Point

Start with **Phase 4.1**, not with `.so` build mechanics.

The first concrete implementation task should be:

- inventory current static/global state
- decide what becomes `engine` state vs `program` state
- move one slice at a time until multiple independent program instances are realistic

Only after that should the shared library and worker surfaces be added.
