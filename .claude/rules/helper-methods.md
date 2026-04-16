# Helper Methods Over Ad-Hoc Checks

When adding conditional logic that checks token state, type relationships, or
context (e.g. "is this operator in unary position?", "does this type need
coercion?"), always extract it into a named helper method or function.

- **Never** inline a multi-condition check and then copy it elsewhere
- If the same check appears twice, it should already be a method
- If a check has more than 2 conditions, make it a method even if used once
- Name the method after what it answers: `isUnaryPosition()`, `needsCLibRedirect()`
- Put parse-time helpers on `Program`, type-system helpers on `DataDef`
- Existing examples: `isUnaryPosition()`, `isPostfixPosition()` in madc.h
