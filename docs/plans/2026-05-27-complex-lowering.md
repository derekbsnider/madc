# _Complex Struct-Based Lowering for Transpiler

**Date:** 2026-05-27
**Status:** Approved
**Tests unblocked:** 13 (_Complex tests)

## Design

Lower `_Complex T` to `struct { T re; T im; }` in emitted C. Runtime
helpers for arithmetic compiled by clang++ as `extern "C"`. Mirrors
the legacy `DataDefCOMPLEX` internal representation exactly.

See commit history for implementation details.
