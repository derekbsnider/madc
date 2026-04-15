# Debug Output Rules

## DBG Macro

All debug-only statements are wrapped in `DBG(x)`:

```cpp
DBG(cout << "some trace: " << value << endl);
```

The macro is defined in each source file as:
```cpp
#define DBG(x) do { if(madc_verbose){x;} } while(0)
```

`madc_verbose` is declared in `include/datadef.h` (first header included everywhere)
and defined in `src/madc.cpp`. It is set to `true` when `-v`/`--verbose` is passed on
the command line.

## Rules

- **Never remove DBG() calls** — they are essential for diagnosing JIT compilation issues.
- **Always use `do { if(madc_verbose){x;} } while(0)`** — the `do-while(0)` idiom prevents dangling-else bugs when DBG appears inside an if/else chain.
- **Multi-statement DBG blocks** that share local variables must be a single DBG call:
  ```cpp
  // CORRECT
  DBG(
      static FileLogger logger(stdout);
      logger.setFlags(FormatFlags::kMachineCode);
      code.setLogger(&logger);
  );
  // WRONG — logger defined in first block, unavailable in second
  DBG(static FileLogger logger(stdout));
  DBG(logger.setFlags(FormatFlags::kMachineCode));  // won't compile
  ```
- The `throwit` stream and `throwstream`/`throwbuf` error mechanism always prints errors to stderr regardless of the verbose flag — don't gate errors behind DBG.

## Unit Tests

Unit test files must define the macro before including project headers:
```cpp
bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)
```
