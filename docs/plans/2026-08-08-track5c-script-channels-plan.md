# Track 5C slice 1 — script-facing channels: `madc::channel` (2026-08-08)

## Implementation state (compaction checkpoint, session #71b)

**DONE — C1 @ this branch (`feature/script-channels-claude`, off
develop/v0.71.0):** `ProcessOptions.inherit_stderr` + PATH resolution
in `Process::start` (committed pre-test as a bank; unit tests land with
C2).

**NEXT — C2 (all decisions settled, just write it):**
- `pump_process`: stderr leg must SKIP an unreadable stderr channel
  (`process.stderr_channel().capabilities().read == false` → no stderr
  thread, `stderr_ok = true`) or inherit_stderr would terminate the
  child spuriously.
- `ExecDataChannel` in `madc_process.cpp` (anon ns, beside Process):
  holds `std::unique_ptr<Process>`; `name()="exec"`; capabilities =
  stdout.read / stdin.write / half_close=true, never seek;
  read→stdout_channel().read, write→stdin_channel().write,
  close_write→close_stdin, close() = close_stdin + stdout
  close_read + wait + reset (child on EOF/EPIPE exits; Process dtor
  SIGKILLs a survivor).
- `ExecChannelFactory::open`: split `source.path()` on single spaces
  (skip empty tokens; first = command, rest = argv; empty command =
  error), `inherit_stderr=true`, `Process(DataSource("exec://" +
  command), options)`, `start(err)` (null on failure), wrap.
  Registration: `detail::register_exec_channel_factory(registry)`
  declared in `madc_datachannel_internal.h`, called from the
  `DataChannelRegistry` ctor after the socket factories.
- Unit tests `tests/unit/test_exec_channel.cpp`: registry-opened
  `exec://sort` round-trip (write lines, close_write, drain, expect
  sorted; caps truthful), `exec:///nonexistent/x` → null + "process
  exec failed", `exec://sort -r` argv split.

**THEN — C3:** `include/madcdis/channel.h` (+ `include/libmadc/channel.h`
stub) + `src/madc_channel_object.cpp` (add to CORE_OFILES in
src/Makefile). `class channel { void *impl_; }` — impl =
`ChannelState { unique_ptr<DataChannel> channel; error last_error;
std::string pending; bool eof, failed; }`. Semantics: `readline` strips
the newline and returns the final unterminated tail, false at EOF;
`read` serves `pending` first; `readall(string&)` drains; writes go
through `write_all`; `close_write` = flush + half-close; modes
"r"/"w"/"rw"/"a"; non-copyable (private copy ctor). Opens via
`DataChannelRegistry::instance().open(DataSource(uri), mode,
&last_error)`.

**THEN — C4:** extend `include/madc/ns_madc` with the declaration-only
class twin (layout-contract comment both sides, `SysInfo` precedent);
the three `.mad` tests + `.expect` fixtures per the Design section.
Risk to validate FIRST in C4: `madc::channel` is the first madc-OWNED
class resolved mangled-direct from an embedded header (std::string
proves the machinery for real headers) — if a method import fails,
compare madc's mangling of the symbol against `clang++`'s for the same
declaration (madc_mangle.cpp is the fix site, not the header).

**Cadence:** batch gate (fulltest + libcxxjit) before push; /dupaudit
scoped to process/channel + full battery pre-merge; merge ⇒ release
(v0.72.0). SMAUG under madc already does raw socket()/bind()/listen()
— script-side socket calls in the tcp tests are proven ground.

## Mission (owner directive)

The data-channel substrate stopped short of the language: `tcp://` and
`exec://` work and are unit-tested at the C++ layer, but not one line of
madc can touch them, and `tests/*.mad` shows nothing. Deliverables:

- `.mad` integration tests for `tcp://` and `exec://`;
- `httpget.mad` at few-lines size;
- an `exec://sort` round-trip.

## Design

**The script surface is a class, not a function zoo.** `madc::channel`
— one URI-addressed byte channel with line helpers — lands as a REAL
C++ class in the host (`include/madcdis/channel.h`, cpp-first: it is
simultaneously the embedder's convenience wrapper), declared
declaration-only in the `<ns_madc>` embedded header and resolved
mangled-direct, exactly like the eval family and `std::string`'s
method surface. Layout contract: a single `void *impl_` member
(append-only, same rule as `SysInfo`).

```c
#include <ns_madc>

madc::channel sorter("exec://sort");
sorter.write("pear\napple\n");
sorter.close_write();                  // EOF to the child
string line;
while ( sorter.readline(line) )        // newline stripped, false at EOF
    printf("%s\n", line.c_str());
```

Surface (slice 1): ctors (`uri`, `uri, mode` with `"r" "w" "rw" "a"`),
`ok()`, `last_error()`, `read(buf, cap)`, `readline(string&)`,
`readall(string&)`, `write(const char*)`, `write(const char*, long)`,
`write(string&)`, `close_write()` (half-close), `close()`, dtor closes.
Non-copyable (private copy ctor — a script copy fails loudly at
resolve time).

**`exec://` becomes a first-class channel scheme.** An
`ExecDataChannel` (in `madc_process.cpp`, beside the machinery it
wraps) adapts a started `Process`: write → child stdin, read → child
stdout, `close_write` → stdin EOF, `close` → stdin EOF + reap;
capabilities read+write+half_close, never seek. Registered in the
channel registry as `exec` next to the socket factories.

Two deliberate semantics, decided here:

- **argv in the URI splits on single spaces** (`exec://sort -r`).
  This is NOT a shell: no quoting, no globbing, no variables, no
  redirection — an argument that itself contains a space cannot be
  expressed in the URI form and must use the C++
  `ProcessOptions.args` API (or a later script-side overload). The
  split happens in the exec channel factory only; `Process` keeps
  exact argv boundaries.
- **stderr is inherited, not piped** (`ProcessOptions.inherit_stderr`,
  new, default false so existing `Process` users keep their stderr
  channel). Pipe-and-never-drain would deadlock a chatty-stderr child;
  inheriting gives shell-pipe semantics and keeps diagnostics visible.

**`Process` gains PATH resolution** (deepest layer — the C++ API wants
it as much as the URI form): a path with no `/` resolves against the
spawn environment's `PATH` before `execve` (posix_spawnp shape). The
exec-errno self-pipe already reports failures loudly.

## Hermetic `.mad` tests (no network, no fixtures beyond `.expect`)

- `testexecchannel.mad` — `exec://sort` round-trip (ASCII input, so
  locale cannot reorder), plus a failed-spawn probe
  (`exec:///nonexistent` → `ok()` false / open error).
- `testtcpchannel.mad` — single-process loopback: the script creates a
  listening socket with raw libc calls (`socket`/`bind`/`listen` on
  127.0.0.1:0, `getsockname` for the port — the TCP handshake
  completes against the backlog without `accept`), connects a
  `madc::channel("tcp://127.0.0.1:<port>")`, `accept`s the server
  side, exchanges bytes both ways, checks half-close EOF.
- `testhttpget.mad` — the owner's few-liner in test form: the same
  loopback listener serves one canned `HTTP/1.0 200` response; the
  client side is exactly the real-world shape (connect, write the GET,
  `readline` loop). The real-host variant goes in the language docs,
  not the suite (tests stay hermetic).

## Slices

| Slice | Content | Ships with |
|---|---|---|
| C1 | `ProcessOptions.inherit_stderr` + PATH resolution in `Process` | process unit tests |
| C2 | `ExecDataChannel` + factory + registry entry | C++ unit test: registry-opened `exec://sort` round-trip; claims stay truthful (no seek) |
| C3 | `madc::channel` host class (`madcdis/channel.h`, impl beside the channel core) | C++ unit tests over memory/exec channels |
| C4 | `<ns_madc>` declaration + the three `.mad` tests + `.expect` fixtures | the suite legs the owner asked for |
| C5 | docs example (real-host httpget), CHANGELOG, mirrors | release per cadence |

## Non-goals (slice 1)

- No script-side `DataSet`/query surface (later 5C slices).
- No listener/server channels (`tcp://` is connect-only today; the
  test's listener is raw libc by design).
- No argv-with-spaces in the URI form; no shell features, ever.
- No pty; `exec://` children see pipes, not terminals.
