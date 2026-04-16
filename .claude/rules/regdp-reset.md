# regdp Reset Rule

## Always reset regdp before sub-compilations in loops and conditionals

All loop and conditional `compile()` methods (FOR, WHILE, DO, IF) must reset
`regdp.first` and `regdp.second` to NULL before compiling sub-expressions
(condition, body, increment). Failure to do this causes the sub-compilation
to inherit a stale destination register from a previous iteration or sibling
statement, leading to infinite loops or corrupted values.

```cpp
// CORRECT — reset before each sub-compilation
regdp.first  = NULL;
regdp.second = NULL;
condition->compile(pgm, regdp);

regdp.first  = NULL;
regdp.second = NULL;
statement->compile(pgm, regdp);
```

This was the root cause of:
- For-loop counter clobber (fixed in Phase 3.5+)
- Do-while(0) infinite loop when two do-whiles in sequence (fixed in Phase A)
