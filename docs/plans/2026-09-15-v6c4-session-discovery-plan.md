# V6c-4 — Session discovery: the advertisement a client finds a session by

> **SHIPPED 2026-09-15** — `e2e7c8281` (the feature), `dc30cf5c5` (the gates),
> `a63bacb10` (the extension + docs). Every task below landed as planned; what
> the execution added is recorded in **As landed** at the foot of this file.

**Owner request, 2026-09-14:** *"madcide should create a dot-lockfile similar
to neovim … a json file that contains all the details so that a client session
can connect to the server session … rather than saying 'this file is already
open/locked' blah blah blah, it allows another session to attach."*

## The one reframe

**It is an ADVERTISEMENT, not a lock.** madcide does not refuse an already-open
file today — there is no swapfile mechanism at all, so there is no refusal to
remove. The ask is purely DISCOVERABILITY: a client should *find* a session
instead of being *told* its address. The file never excludes anyone; it
publishes how to reach a session. Two sessions in one root both advertise.

This is the last piece of the V6c-3c attach story. V6c-3c made an editor, an
agent and a browser able to drive one running session — but only if a human
typed the port into three places. This removes the typing.

## What the recon settled (verified, not assumed)

The banked handoff named two engine gaps. **Both are wrong — nothing is
missing.** Probed at live HEAD in the JIT pass *and* the native-exe pass
(`tmp/probe_env_pid.mad`, `tmp/probe_disc2.mad`):

| Need | Answer | Status |
|---|---|---|
| environment | `getenv()` — a registered builtin with the REAL C shape (`parser.cpp` ~23031, beside `system`/`setenv`) | ✅ bare in dialect code |
| process id | `getpid()` — resolves through the dlsym fallback; correct in JIT and `-o` exe | ✅ |
| working directory | `madc::canonical_path(out, ".")` — the engine's standing canonicalizer | ✅ |
| timestamp | `time(0)` (epoch seconds; there is no `php::date`) | ✅ |
| directory scan | `perl::glob(array &out, pattern)` — returns FULL paths | ✅ |
| file I/O | `php::file_put_contents` / `file_get_contents` / `file_exists` / `unlink` | ✅ |
| mkdir | `php::mkdir(dir[, mode])` — **single level only**, no recursive flag: create each ancestor in order | ✅ |
| ephemeral port | `madc::channel::local_endpoint()` reports the address actually bound after `:0` | ✅ |
| the TUI can host a listener | `ui_term.cpp` `read_keys` parks on `{stdin, io waiters, timers}` through `taskio::host_wait_readable` whenever `__madc_task_live() != 0` — a parked accept task is woken while the editor waits for a key | ✅ |

**So this slice is pure dialect code.** No `src/`, no `include/`, no rule
trailers, no engine risk on a branch whose battery has not run.

## The three rulings

**R1 — the file lives in a STATE DIR, not the project root.** The owner said
"dot-lockfile like neovim"; neovim's own state (swapfiles, server sockets)
lives in `$XDG_STATE_HOME/nvim`, not beside the file. A `.madcide.lock` in a
project root lands in the git status of every project madcide ever opens and
needs gitignoring in each. Resolution order:
`$MADCIDE_SESSION_DIR` → `$XDG_STATE_HOME/madcide/sessions` →
`$HOME/.local/state/madcide/sessions`. The first entry is not a convenience —
it is how the tests stay hermetic and never write into a developer's real
state directory.

**R2 — the TUI listens by default.** Nothing is discoverable otherwise. A
loopback ephemeral port, advertised; `--no-serve` opts out. ⚠️ The caveat is
the existing `--serve` caveat, now on by default: **there is no auth and no
TLS**, so on a multi-user box any local user can attach to a loopback port.
Bind loopback, tunnel over ssh, never expose the port. Stated in the docs and
on `--sessions`.

**R3 — a second TUI does NOT silently become a remote client.** `--lsp` and
`--mcp` attach for free (the V6c-3c relay already exists). A second *TUI*
attaching needs a TUI-over-api thin client that does not exist — that is the
gateway vision's thin client and it is its own slice. v1 behaviour when the
TUI opens a file a running session holds: **one status line naming the session
and the attach command.** Do not grow a remote TUI inside this slice.

## Degradation

Every failure here is non-fatal and silent-but-honest: a state dir that cannot
be created, a port that cannot be bound, a file that cannot be written — the
editor opens anyway with no advertisement. Discovery is a convenience; an
editor that refuses to start because it could not advertise is a worse editor.

## The record

One JSON object per running session, `<sanitized-root>-<pid>.json`:

```json
{
  "madcide": 1,
  "pid": 21561,
  "started": 1789438228,
  "version": "0.99.2",
  "host": "madc-devbox",
  "root": "/workspace/madc",
  "endpoint": "127.0.0.1:41273",
  "url": "http://127.0.0.1:41273/",
  "serves": ["api", "ws", "http", "lsp", "mcp"],
  "documents": ["/workspace/madc/tools/madcide/madcide.mad"]
}
```

`serves` is what the connection seat accepts on that port: the three web seats
always, plus `lsp`/`mcp` through the V6c-3c declared-dialect hello.

`documents` is a HINT — it is written at publication and refreshed whenever the
serving layer already runs (start, and each accepted connection). **`root` is
the authority for matching**, so a stale `documents` list is never wrong, only
incomplete. Matching order: `documents` contains the file canonically (exact),
else `root` is a path prefix of it (covering).

**Staleness is a CONNECT TEST, never a pid test** — a pid is reused. A file
whose endpoint refuses connection is stale: remove it and carry on.

## Tasks

### T1 — `tools/madcide/madcide_discover.inc` (new)

The advertisement's one owner: where the directory is, what the record holds,
how to publish, scan, match and withdraw. Included after `madcide_core.inc`
(it reads the session) and before every face that listens.

- `void session_dir(var &out)` — R1's resolution order; `""` = no home.
- `bool session_dir_ensure(var &dir)` — mkdir each ancestor in order.
- `void session_file(var &out, var &dir, var &root, long pid)`
- `void session_documents(var &out, IdeSession &S, long doc)` — the `buffers`
  table when seeded, else the launch document's path; canonical, deduped.
- `void session_advertise(var &out_path, IdeSession &S, long doc, const char *endpoint)`
- `void session_withdraw(var &path)`
- `bool session_alive(const char *endpoint)` — the connect test.
- `void session_scan(var &out)` — every advertised session, stale files removed.
- `bool session_find(var &out, const char *file)` — R-matching, best first.
- `void session_report(var &out, var &rec)` — the one listing line (`--sessions`,
  the TUI's status line, the attach hint) — one renderer, not three.

### T2 — the faces publish

- `run_serve` (`madcide_serve.inc`): advertise after the listener binds,
  withdraw before return.
- `run_lsp` with `--serve`: same, on the endpoint it already resolves for
  `$/madc/serve`.
- Refresh `documents` on each accepted connection (free, in the accept loop).

### T3 — the TUI listens by default (R2)

`run_ide` binds `127.0.0.1:0` unless `--no-serve`, spawns the same
`serve_conn_task` accept loop inside a `scope`, advertises, and withdraws at
exit. ⚠️ Trap #3 from this arc: the accept task parks, and `scope` joins it —
wake it with the self-connect poke `lsp_serve_task` already uses, or the editor
hangs at exit. Before advertising, `session_find` the launch document and, on a
hit, put R3's one line in the status message.

### T4 — the CLI

- `--attach` with NO address = discover; refuse with prose when nothing matches.
- `--sessions` — list live sessions and exit (no file argument required).
- `--no-serve` — opt out of R2.
- Usage lines.

### T5 — the extension

`madcide.attach: "auto"` passes `--attach` with no address and lets madcide
discover. The scan is NOT reimplemented in JavaScript — one implementation of
the matching rule, in madcide.

### T6 — gates

`tests/testmadcide_discover.mad` (+`.expect`, `.exe_skip`, `.timeout`), driving
a hermetic `MADCIDE_SESSION_DIR`:

1. a `--serve` session advertises; the record parses and carries the endpoint it
   actually bound;
2. `session_find` matches the launch document exactly AND a sibling file by root;
3. a record pointing at a dead endpoint is reported stale and REMOVED;
4. ⚠️ **negative control** — a record whose root does not cover the file does
   NOT match (the bug this catches is a prefix test that matches everything);
5. the file is GONE after the session exits;
6. `exit-status: 0` — the accept-task wake works (trap #3).

`tests/testmadcide_attach.mad` gains the discovery path: `--attach` with no
address finds the running session.

---

## As landed

Everything above shipped. Four things the execution added or corrected.

**Two consolidations rather than two more copies.** `run_serve` had its own
accept loop beside `serve_accept_task`, and it had already diverged — no
shutdown poke, no advertisement refresh. It now calls the same function the LSP
process and the editor spawn as a task, so there is ONE accept loop; the gate
`check-madcide-one-accept-loop.sh` keeps it that way. Likewise the endpoint a
session listens on lived in two slots: `lspserve` (set only by `--lsp --serve`)
and, new here, `advertep` (set by every face that binds). `$/madc/serve` now
reads `advertep`, so an editor ATTACHED to an ordinary editor session is told
about its window face too — which it was not before.

**The ring-lifetime trap, paid for again.** A `var`'s `c_str()` is a
RING-lifetime rendering, not a pointer into the carrier. `session_record` calls
out several times (canonical_path, the document scan, format) before it places
its endpoint, and passed as a `const char *` that pointer had been recycled: the
first run advertised `"endpoint":"tmp/discdoc.mad"` — the file path. Carriers
cross a function boundary as `var &`.

**Two things the gate's own writing cost.** A DISCOVERING client spends a
connection on the liveness probe BEFORE it opens the real one, so a seat that
accepts exactly once answers the probe and then deadlocks waiting for a relay it
has already refused to hear — the gate drives the production accept loop
instead, which is what it should have done anyway. And a relay must be DRAINED
to EOF before closing, or the child is cut off mid-exit and the parent waits
forever (`testmadcide_attach` established that discipline; this test had to
learn it).

**Verified beyond the gate.** A plain `madcide file.mad` under a pty, sitting at
its buffer waiting for a keystroke, was discovered from its advertisement by a
separate process and answered a real api command — which is ruling R2's whole
premise. A second `madcide file.mad` on the same file reported the first in its
status line and exited 0 (the accept task woken and joined), withdrawing its own
advertisement; and `--lsp --attach` with no address joined the TUI session.
