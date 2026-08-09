# Track 5C slice 1 — script-facing channels: `madc::channel` (2026-08-08)

## Implementation state (session #71b)

**DONE — C1 @fc06f723:** `ProcessOptions.inherit_stderr` + PATH
resolution in `Process::start`.

**DONE — C2 @a74a5189:** `ExecDataChannel` + `ExecChannelFactory` +
registry entry (space-split argv, inherit_stderr, loud spawn failure),
plus the C1 follow-through fixes (execve consumes the resolved path,
child dup2 tolerates the absent stderr pipe, pump_process skips its
stderr leg when the stderr channel is unreadable). Unit tests:
`test_exec_channel.cpp` (4 cases) + 3 new `test_process.cpp` legs.

**DONE — C3 @95edaa3a:** `madc::channel` host class
(`include/madcdis/channel.h`, impl `src/madc_channel_object.cpp`,
`ChannelState` behind one `void *impl_`). One deviation from the
checkpoint sketch, settled: `write(string&)` is NON-const (eval-family
precedent; const-qualified script types are still an active track).
Unit tests: `test_channel_object.cpp` (5 cases).

**DONE — C4 @28bb7ed6:** `<ns_madc>` class twin + the three suite legs
(`testexecchannel.mad`, `testtcpchannel.mad`, `testhttpget.mad` +
`.expect` fixtures). The flagged risk was probed FIRST
(`tmp/probe_channel.mad`) and did not materialize — madc's mangling of
its own class resolved cleanly; no `madc_mangle.cpp` change. All three
tests green in all three lanes (JIT / EXE / OBJ, 3/3 each).

**DONE — C5, SLICE 1 COMPLETE:** `docs/language/channel.md` + overview
index + CHANGELOG; full battery all-green (JIT 1002/0/9skip, EXE 981/0,
OBJ 981/0, libc++ JIT 998/0/13skip, packed 1002/0); released as
**v0.72.0**, merged --no-ff to develop @2604561f, pushed, binaries
rebaked. Later 5C slices: script-side `DataSet`/query surface, listener
channels (`tcp://` is connect-only).

**Lane wall (hit and fixed in-branch):** the first batch gate ran
fulltest GREEN but the libc++ JIT lane failed exactly the three new
tests — undefined MIR imports for `channel::readline/write(string&)`
mangled `NSt3__1` (script flavor) while the host exports `NSt7__cxx11`.
The task #69 flavor-marshalling hook SAW the method calls and
classified them as candidates; the sole defect was the host-twin mint:
`host_flavor_fn_symbol` (a namespace-function mint) fed a METHOD
produced garbage (`_Z0P7channel…` — empty name, no class scope, __this
mangled as an explicit param). Fix at that layer:
`Program::host_flavor_method_symbol(FuncDef*)` re-runs the ONE method
mint (`itanium_mangle_member_sub`, the bind_declared_cpp_symbol owner)
under `MangleHostFlavorScope`, owning class read from the hidden
`__this` receiver; `flavor_marshal_thunk` selects fn-vs-method mint by
`method_display_name`. The thunk generator itself needed nothing —
`__this` passes through as a kind-0 pointer, and by-ref string
out-params (readline's shape) are the proven `eval_unit(string&,…)`
shape.

**Dupaudit (pre-merge, scoped to channel/process) — done.** Known
overlapping families re-checked, no regrowth (copy-loops owner still
1, seekable probe still 1; the fd-write/cloexec/storage-seam gates
re-verify in fulltest). ONE new family found and fixed in-branch:
`runtime_error_composition` — three sites composed
`error(severity::error, phase::runtime, …)`, the third copy being
added by this branch itself. Consolidated @f5a64821 (one-arg
`detail::set_channel_error` = ERROR-COMPOSER-OWNER, siblings
delegate), gated by `scripts/check-one-error-composer.sh` in fulltest
(negative control verified), recorded gated in the KG. Judged
NOT duplication (tie-breaker): PATH-split vs argv-split (two rules —
POSIX empty-field="." vs skip-empty tokens); `readall` vs
`copy_channel` (buffered line-reader with shared pending buffer vs
channel→channel drain). Cosmetic, dropped: the two test-side drain
helpers (`read_channel` / `drain`). `scheme_factory_registry_twins`
stays open, unchanged by this branch, assigned to 5B.7's opening
commit.

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
