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
- **The future idiom** is a capacity-1 channel: spawn the worker, have
  it send its result, `chan_recv` where you need it. The `await`
  spelling arrives with MT-5's keywords.

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
- Channels, `select`, futures/`await`, scopes with cancellation, and
  actor/supervision patterns arrive with MT-2/MT-3 (see the design
  doc's slice cut).
