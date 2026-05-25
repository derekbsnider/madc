# Pre-Edit Checklist — Reasoning

## Why this rule exists

During the May 2026 GCC parity push (1622→1631), three fixes required
multiple iterations because coding started before the data flow was
fully understood. Each time, 5 minutes of reading would have saved
15+ minutes of debug cycles. The pattern was identical every time:

1. See what looks like a simple fix
2. Write the code
3. Discover an edge case in the data flow
4. Iterate 2–3 times to handle it

## Failure cases

### 1. Trace the data flow — overflow ABI change

**What happened:** `__builtin_add_overflow` with unsigned result type
didn't detect unsigned wrapping. Changed the `_u64` helper to take
`unsigned long long` args instead of `long long`. Didn't trace what
happens when signed `int` values (e.g. `-4`) are passed to the unsigned
parameter — the bit pattern changes meaning. Caused a regression in
pr91450-1. Had to revert.

**What tracing would have caught:** The caller passes `-4` as `long
long` (-4). If the callee receives it as `unsigned long long`, it
becomes `0xFFFFFFFFFFFFFFFC`. The multiply `0xFFFFFFFFFFFFFFFC * 2`
gives a different infinite-precision result than `-4 * 2`. For
pr91450-1 (signed inputs, unsigned result), both paths happen to give
the correct *truncated* result, but the overflow detection disagrees.

### 2. Search for existing handling — alignment attribute

**What happened:** Added `__attribute__((aligned(N)))` consumption
between qualifiers and the member name (line ~9937). The attribute
was never seen because the struct parser already consumed it at line
9900 — but that code discarded the value. Wasted ~20 minutes adding
debug prints to discover the attribute was already gone.

**What grepping would have caught:** `grep -n "attribute" src/parser.cpp`
in the struct-parsing region would have immediately shown line 9900's
existing consumption. The fix was 2 lines: change from discard to
capture.

### 3. Identify the write-back target — bitwise AND coercion

**What happened:** `double d = i & 7` produced 0.0. First fix: changed
`regdp.second` from double to int inside `TokenBand::compile()`. This
caused `TokenAssign::compile()` to reject the type mismatch ("Not
expecting rval to be numeric"). Second fix: called `emit_ir_value`
twice (once from `emit_plain_bitop2`, once manually). This caused
`InvalidInstruction` from double-emission. Third fix: null out
`regdp.first` before `emit_plain_bitop2` so it returns a bare integer
register, then restore and coerce.

**What tracing the write-back would have caught:** `emit_plain_bitop2`
stores through `regdp.first` if non-null (line 2239–2242). The caller's
`regdp.first` is a double Mem. Storing an integer Gp to a double Mem
via integer type reinterprets the bits. The fix was always to prevent
the premature store by nulling `regdp.first`.

### 4. Global va_list write-back

**What happened:** `va_arg(gap, double)` on a global va_list didn't
advance `gap`. The write-back went to the cached register, but
`voperand` reloads from `var->data` on next access.

**What tracing would have caught:** For globals, `voperand` returns a
register that's a *copy* of `var->data`. Writing to the register
doesn't update the backing store. The write-back must also store
through `var->data`.
