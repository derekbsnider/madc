# Cooperative Tasks — `go` and `yield` (MT-1)

madc's multitasking starts here: **stackful cooperative tasks on one OS
thread**, spelled with Go's syntax, owned by Kotlin-style structured
lifetimes, and costed like Java's virtual threads — blocking is the
programming model, so the entire existing surface (`php::`, `perl::`,
`madc::channel`, `println`) works unchanged inside a task. The design
and its industry recon live in
`docs/plans/2026-08-26-madc-multitasking-design.md` and
`...-multitasking-recon.md`.

Both spellings are **madc-dialect statement heads** (`--std=madc`
only), claimed *contextually* by the UFCS error-shape rule: they fire
only where the statement would otherwise be ill-formed. A variable,
function, or type in scope named `go` or `yield` always wins, and
strict `--std=c*`/`--std=c++*` modes are byte-identical (gated by
`tests/testgogate.mad`, precedence by `tests/testgoident.mad`).

```cpp
void worker(long id, long hops)
{
    long i = 0;
    while ( i < hops )
    {
        println("w{} hop {}", id, i);
        yield();            // reschedule: run whoever is next
        i = i + 1;
    }
}

int main()
{
    go worker(1, 2);        // spawn — the spawner keeps running
    go worker(2, 1);
    yield();                // let the workers have a turn
    println("main continues");
    return 0;               // the ROOT SCOPE JOIN drains live tasks
}
```

## Semantics

- **`go f(args);`** evaluates the arguments **at spawn, in order** (Go
  semantics), then enqueues the call as a new task. The spawner
  continues; nothing runs until a blocking point.
- **`yield();`** (or `yield;`) re-enqueues the current task at the
  tail and runs the head. With nothing else runnable it is a no-op.
- **Scheduling is strict FIFO run-to-yield — deterministic by
  construction.** The same program produces the same interleaving every
  run (`tests/testgo.mad` pins a complete schedule byte-for-byte).
- **The root scope joins AT MAIN'S END (MT-2b).** `main` returning does
  not kill live tasks (the ruled Kotlin-scope semantic): in a
  `--std=madc` TU that spawns (`go` appears), the user's `main` is
  emitted as `__madc_main` behind a synthesized `int main(...)` wrapper
  that forwards the real arguments, then drains every live task BEFORE
  returning — before any teardown (atexit handlers, TLS/static
  destructors), identically in the JIT, `-o` native, and `-r` object
  lanes (`tests/testgojoin.mad` pins it). A program that never spawns
  keeps its unwrapped, runtime-free main. Strict-mode (`--std=gnu17`
  etc.) mains are untouched; a mixed-TU project whose main TU does not
  spawn still drains via the runtime's atexit belt. A
  drained-to-deadlock state aborts loudly.

## Accepted shapes (MT-1) — refusals are loud

Arguments: integers (any width/signedness, bools, enums), `float` /
`double`, and pointers (including `const char *` and function
pointers). Each rides an 8-byte slot; the callee's prototyped call
converts it back — value-identical to a direct call.

Refused with a compile-time message naming the later slice: class-typed
/ reference / SIMD / `_Complex` / `long double` arguments; variadic,
multi-return, class-returning, method, and function-pointer callees.

## Channels (MT-2)

Value channels are the synchronization primitive — Go's contract,
value-first (`madc::` publics; keyword/operator sugar arrives with
MT-5):

```cpp
void producer(long c)
{
    value v;
    v = 42;
    madc::chan_send(c, v);      // parks when the buffer is full
    madc::chan_close(c);
}

int main()
{
    long c = madc::chan_make(1);    // capacity 0 = rendezvous
    go producer(c);
    value v;
    while ( madc::chan_recv(v, c) ) // parks until a value or close
        println("got {}", v);       // false = closed AND drained
    return 0;
}
```

- **`chan_make(cap)`** → a channel handle; capacity 0 is a rendezvous
  (send and recv meet), capacity N buffers N values.
- **`chan_send(c, v)`** copies `v` in; parks while full. Sending on a
  closed channel throws (`catch (const char *)`).
- **`chan_recv(out, c)`** copies out; parks while empty. On a closed
  channel it drains the buffer first, then returns `false` with a null
  value.
- **`chan_close(c)`** wakes every parked sender (they throw) and
  receiver (they return false). Closing twice throws.
- **`chan_len(c)`** — buffered value count.
- **Deadlock is loud**: a blocking operation that parks the last
  runnable flow aborts with a message, never hangs.
- **`chan_select(out, chans)` (MT-4)** — fan-in over an ARRAY of channel
  handles: parks until some case can receive, returns the fired index
  with the value in `out`. DETERMINISTIC: the lowest-index ready case
  wins (Go randomizes here; madc's scheduling contract is determinism,
  so a starvation-shaped program pins its own order instead of dodging
  it). A closed-and-drained case is disabled; when every case is dead
  it returns `-1` with a null value — the natural fan-in terminator
  (each producer closes its channel when done). Buffered values still
  drain from a closed channel first.
- **`chan_try_recv(out, c)` (MT-4)** — the nonblocking default-arm
  equivalent: `1` = got a value, `0` = open but empty (would block),
  `-1` = closed and drained.
- **`madc::sleep_ms(ms)` (MT-4)** — parks this task for `ms`
  milliseconds of SCHEDULER time; wake order is deadline order, FIFO
  among equals. The time source is pluggable: real monotonic time by
  default, and under `MADC_TASK_VTIME=1` a VIRTUAL clock that jumps to
  the earliest deadline whenever only sleepers remain — timed tests run
  instantly and deterministically (`tests/testgosleep.mad` pins the
  order; `tests/unit/test_rt_vtime.cpp` pins the jump).
- **`chan_readable(channel)` (MT-4b)** — register a BYTE endpoint
  (`madc::channel` — `exec://`, process pipes) as a select case: the
  returned handle fires in `chan_select` when a `read`/`readline` on
  that channel would make progress NOW (data, or the one EOF/error
  observation the read surfaces). A fired byte case carries `out =
  null` — the script reads from the channel object it holds. A DEAD
  endpoint (EOF fully drained, or failed) disables exactly like a
  closed-and-drained value channel, so `-1` still terminates mixed
  fan-ins. Register once and reuse the handle; the channel object must
  outlive it. Channels with no waitable read side (memory, file — their
  reads never block) are refused loud. "Recv from a task channel OR
  readline from `exec://sort`" is ONE wait
  (`tests/testgoselectio.mad` pins a phased deterministic schedule).
- **Byte reads park, not block (MT-4b)**: when tasks are live, a
  `madc::channel` read/readline that would block parks on the fd
  through the scheduler's io-wait seat instead of stalling every task
  under it; solo programs keep the plain blocking read (zero
  overhead). The direct probes on the object: `poll_state()` (`1` =
  progress now, `0` = would wait, `-1` = dead), `wait_readable()`
  (park until progress or dead), `read_wait_handle()` (the raw fd for
  event-loop plumbing).
- **The future idiom** is a capacity-1 channel: spawn the worker, have
  it send its result, `chan_recv` where you need it. The `await`
  spelling arrives with MT-5's keywords.
- **`madc::task_drain()`** runs ready tasks until none remain live —
  the interim structured-join verb (MT-3 scopes will own this) for
  teardown code that closes resources a still-running task writes to
  (main's end joins anyway, but only AFTER your teardown ran; madcide
  drains before closing its world). Idempotent. **`madc::task_live()`**
  is the live spawned-but-unfinished count.
- **The compiler itself cooperates (stage 2)**: `madc::parse_open` &
  co. yield every ~1k lexed tokens and at every top-level declaration
  when other tasks are runnable — `go` a parse and keep serving your
  event loop; the tui input wait hands the CPU to runnable tasks
  between polls and delivers `{event:"wake"}` when they drain. Batch
  compiles pay one queue check per yield point and nothing else.

## Structured scopes + cancellation (MT-3)

Kotlin's ownership over Go's spelling: every spawn lands in a scope
that owns join, error, and cancel.

```c
long s = madc::scope_begin();       // this task's innermost scope
go worker(1);                       // attaches to it
go worker(2);
madc::scope_end(s);                 // JOINS: returns only after both
                                    // finished; rethrows a child error
```

- **Attachment**: `go` between `scope_begin` and `scope_end` attaches
  to that scope; with no open scope it attaches to the root scope
  main's end drains (the pre-scope semantics, unchanged). Scopes nest;
  `scope_end` must be called by the opening task, innermost first.
- **Join**: `scope_end` blocks until every attached task — and,
  transitively, every task of scopes opened inside — has finished. The
  join always completes before it throws, so nothing leaks.
- **Error**: a member's UNCAUGHT error is captured (as text) and
  cancels its siblings — the failed-child rule; `scope_end` rethrows
  the first one (`catch (const char *)`). Root tasks keep the
  abort-on-uncaught default.
- **Cancel**: `madc::scope_cancel(s)` requests cancellation of the
  scope's whole subtree and returns immediately (any task may call
  it). Every member's next blocking verb — `chan_send`/`chan_recv`/
  `chan_select`, `sleep_ms`, channel reads — throws
  `"madc: task cancelled"`, before parking and on resume; a parked
  member (even a sleeper) wakes NOW. Sticky: cleanup after
  cancellation must not use blocking verbs. A task spawned into a
  cancelled scope is born cancelled. `yield()` is NOT a cancellation
  point; compute loops poll **`madc::cancelled()`** — true when this
  task, or any scope it currently has open, is cancel-requested (the
  opener of a cancelled scope polls true, and recovers after
  `scope_end`).
- A cancelled `scope_end` throws the cancelled text after joining;
  members that never reach a cancellation point keep the join waiting
  (the Kotlin behavior).
- **The compiler cooperates with cancellation too (MT-3b)**: a
  cancelled task's `madc::build_native` / parse-handle work aborts
  CLEANLY at its next yield point — the token pumps and the top-level
  parse loop stop with a recorded "cancelled" diagnostic (never a
  throw through parser frames). The emit phase has no yield points
  yet (named residue). And a cancelled `madc::channel` (`cancel()`)
  escalates on `close()`: SIGTERM at cancel, SIGKILL after a 2s grace
  — a SIGTERM-ignoring child cannot hang the close.

## Keywords: `scope { ... }` and `await` (MT-5)

Both are CONTEXTUAL under `--std=madc` only — never reserved: a declared
`scope` / `await` name always wins, and strict C/C++ modes never see
them (the `go`/`yield` error-shape rule).

- **`scope { ... }`** — the structured-concurrency block:

  ```c
  scope {
      go worker(1);
      go worker(2);
      // ... the block's end JOINS both, then rethrows the first
      // member error (else throws "madc: task cancelled" when the
      // scope was cancelled, else falls through)
  }
  ```

  `go` inside attaches to the block (the Kotlin attachment); blocks
  nest (each joins only its own members). A throw ESCAPING the block
  quietly abandons the scope mid-unwind — members are cancelled and
  joined, the in-flight error wins, and the catch lands outside with
  the task chain clean; a throw CAUGHT INSIDE the block leaves the
  scope untouched. `return` / `goto` inside the block, and a
  `break` / `continue` that would cross it (no loop/switch opened
  inside encloses them), are refused at parse time — end the scope
  block first. The handle is deliberately not spelled: a cancellable
  job holds `madc::scope_begin()`/`scope_cancel()` (the publics);
  `madc::cancelled()` serves polling inside the block.

- **`await <chan-expr>`** — receive from a value channel, Go's `<-ch`:
  blocks until a value arrives; a closed drained channel yields the
  ZERO value (empty). Two statement shapes:

  ```c
  v = await ch;     // assigns into a var/value (the carrier)
  await done;       // receive and discard — the done-channel wait
  ```

  The ok-form (did the channel close?) stays with the public
  `madc::chan_recv(out, ch)` — `await` is sugar over that one
  implementation. Deeper expression positions (a call argument, a
  declaration initializer) are refused loud; they unlock with L3
  value-by-value returns.

## Contracts and knobs

- **Thread-safety contract**: one OS thread, cooperative. Tasks
  interleave only at yield points, so data races cannot exist yet. The
  M:N upgrade rides the F2 static-actives audit with user programs
  unchanged.
- **Stacks**: 512 KiB per task by default (lazily paged);
  `MADC_TASK_STACK_KB` overrides. Failures name the knob.
- **`MADC_TASK_TRACE=1`**: unbuffered stderr scheduler trace
  (spawn / switch / first entry / exit / join) for diagnostics.

## Known MT-1 limits (named residues)

- ~~`try`/`catch` across a `yield()`~~ **FIXED (MT-2 opener)**:
  exception state (the try/cleanup chains and the in-flight exception)
  is per-task, saved and restored at every switch like registers —
  each task catches its own throws whatever the interleaving
  (`tests/testgotry.mad` pins it; the pre-fix state segfaulted).
- Ring-lifetime `const char *` text (e.g. a held `format(...)` result)
  rots across a yield exactly as it rots across any ring call — copy
  into a `value` first (the standing ring discipline).
- A `-static-libmadc` artifact that spawns fails its link loudly (the
  task runtime is a hosted object, not yet on the AOT ledger).
- Channels (MT-2), `select`/io/timers (MT-4), scopes with cancellation
  (MT-3), and the `scope`/`await` keywords (MT-5) have all landed;
  actor/supervision patterns and a `select` STATEMENT spelling remain
  future arcs (see the design doc's slice cut and its deferred-select
  ruling).
