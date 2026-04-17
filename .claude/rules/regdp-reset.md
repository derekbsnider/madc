# regdp Reset Rule

- Every loop and conditional `compile()` method (`TokenFOR`,
  `TokenWHILE`, `TokenDO`, `TokenIF`) must reset `regdp.first` and
  `regdp.second` to `NULL` before compiling each sub-expression
  (condition, body, increment, else-branch).
- Reset means two assignments, immediately before the sub-compile call:

```cpp
regdp.first  = NULL;
regdp.second = NULL;
condition->compile(pgm, regdp);

regdp.first  = NULL;
regdp.second = NULL;
statement->compile(pgm, regdp);
```

- Any new compile method that takes sub-expressions follows the same
  pattern.

See `docs/rules/regdp-reset.md` for the failure mode and the historical
bugs this prevents.
