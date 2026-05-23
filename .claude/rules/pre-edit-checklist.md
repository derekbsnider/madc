# Pre-Edit Checklist

Before modifying any source file, complete these three checks.
Skipping them is the single most common cause of multi-iteration
fixes and regressions.

- **Trace the data flow.** For every variable the fix touches, write
  down what it holds at each step from input to output. If you can't
  state what `regdp.second`, `ap_op`, or `elem_type` contains at the
  point of your edit, you don't understand the code path yet.
- **Search for existing handling.** `grep` the file for the keyword,
  attribute name, or pattern before adding new code. If the file
  already consumes `__attribute__` or handles the type, modify that
  code — don't add a parallel path.
- **Identify the write-back target.** For any value that gets modified
  (advanced pointer, coerced type, computed result), trace where it's
  stored back. Register? Memory? Global backing store? The destination
  must match what downstream consumers will read.

If any check reveals an unknown, read more code before editing.

See `docs/rules/pre-edit-checklist.md` for the failure cases that
motivated each check.
