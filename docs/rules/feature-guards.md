# Feature Guards — Why

Feature guards (`#ifdef FEATURE_NAME`) protect in-progress code from breaking the stable build.

During the function pointer implementation, treating bare function names as variable references broke `endl` and all stream manipulators — a change in the `parseExpression()` operator stack broke `cout <<`. The `#ifdef` guard allowed isolating the exact change that caused the regression without reverting or losing any code.

Without guards, the alternative was `git checkout` on working files, which destroyed an entire Phase 2 implementation — hours of work lost.

Guards are removed once the feature is stable and all tests pass.
