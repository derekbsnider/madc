# Debug Output — Reasoning

See `.claude/rules/debug.md` for the rules themselves.

## Why `do { if (…) { x; } } while (0)` and not just `if`

The do-while(0) idiom prevents dangling-else bugs:

```cpp
if (cond)
    DBG(cout << "trace" << endl);
else
    do_something();
```

With a bare `if (madc_verbose) …`, the `else` would bind to the wrong
branch. The do-while(0) wrapper makes `DBG(...);` behave like a single
statement that always takes a trailing semicolon.

## Why multi-statement DBG blocks must be combined

Because `DBG(x)` expands to `do { if (…) { x; } } while (0)`, each
call is its own compound statement / scope. A variable declared in one
`DBG(...)` is not in scope for the next:

```cpp
// CORRECT — single DBG block, logger visible throughout
DBG(
    static FileLogger logger(stdout);
    logger.setFlags(FormatFlags::kMachineCode);
    code.setLogger(&logger);
);

// WRONG — logger declared in first block, cannot be used in second
DBG(static FileLogger logger(stdout));
DBG(logger.setFlags(FormatFlags::kMachineCode));  // compile error
```

## Why DBG calls must not be removed

Every DBG trace was added because someone hit a bug that couldn't be
diagnosed without it. Removing them loses that future diagnostic
capability. When a trace is genuinely stale (e.g. the variable it
prints no longer exists), update it — don't delete it.

## Why errors are not gated behind DBG

Errors indicate the user did something the compiler can't handle. They
should always reach stderr, whether or not the user asked for verbose
output. Users who never pass `-v` still need to see why their program
failed to compile. The `throwit` / `throwstream` infrastructure writes
to stderr unconditionally for this reason.

## Why `madc_verbose` is in `datadef.h`

`datadef.h` is the first header included by every source file. Putting
the `extern thread_local bool madc_verbose;` declaration there means every
file gets the declaration for free — no per-file include gymnastics.

## Why DBG has one definition

Thirty-seven source files used to spell their own `#define DBG(x)`, most of
them in files that never called it. The copies were identical only by
convention: when the macro's test had to change (the pre-test below), every
copy was a place to miss, and a missed copy keeps the old test silently —
the program still builds. One definition beside the flag it reads, and a
gate that refuses a second, make the macro's behaviour a single fact.
Unit-test binaries are separate programs that link libmadc with their own
`madc_verbose` definition; they keep their own prologue.

## Why DBG reads a process-wide flag first

`madc_verbose` is `thread_local` (a worker thread's tracing is its own), and
libmadc is a shared library: under the global-dynamic TLS model every read
of a thread_local is a call to `__tls_get_addr`. A front-end run executes
DBG's test hundreds of thousands of times (837K on SMAUG's act_wiz.c, about
0.5% of the whole compile) with verbose off. The `initial-exec` TLS model
would make the read a single load, but it needs the library's TLS in the
static block: libmadc's TLS segment is 1,448 bytes, and glibc reserves
only 512 for libraries loaded later, so a host that `dlopen`s libmadc could
fail to load it. `madc_verbose_requested` is a plain atomic that is set,
never cleared, whenever any thread turns verbose on (`madc_set_verbose`, the
one writer). DBG tests it first: a process that never asked for `-v` reads
one plain flag; a process that did reads each thread's own `madc_verbose`
as before.
