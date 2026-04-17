# Debug Output — Reasoning

See `.claude/rules/debug.md` for the rules themselves.

## Why `do { if(madc_verbose){x;} } while(0)` and not just `if`

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

Because `DBG(x)` expands to `do { if(madc_verbose){x;} } while(0)`, each
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
the `extern bool madc_verbose;` declaration there means every file
gets the declaration for free — no per-file include gymnastics.
