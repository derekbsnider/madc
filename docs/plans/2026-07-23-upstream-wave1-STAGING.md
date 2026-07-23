# Upstream wave 1 — staged PRs (OWNER REVIEW before submission)

**Date:** 2026-07-23 · **Status:** staged, validation in progress —
NOTHING submitted yet. Worktree: `/workspace/mir-up` (upstream/master
@a8ab7c31). Probe doc: `2026-07-23-mir-upstream-probe.md`.

Dedup result (owner's "not already fixed" bar, verified by
reverse-apply + repro at upstream HEAD): everything else in Tier A is
either already merged (#444–#459), Cyan Ogilvie's to send, only
reachable through fork-only features (vectors, external-AST), or not
reproducible at upstream HEAD (union-alias GVN pair). Wave 1 is
therefore exactly two PRs.

---

## PR 1 — branch `fix-c2mir-uninit-narrow-local` @7d1b3d91

The feedback-driven one. Vladimir's comment on #459: *"This solution is
too conservative and worsens generated code in many cases"* — his
follow-up (2a157cc2) gates read-extension on `addr_p`, but that relies
on "a narrow reg value is born from an extending store", and an
UNINITIALIZED narrow local violates it (stale 64-bit garbage).
gcc.c-torture pr34099-2 aborts at upstream HEAD under `-eg -O0/-O1`
(verified 2026-07-23 on the container). Our fix honors his concern:
keep reads extension-free, emit ONE extension at the declaration of an
uninitialized narrow auto local.

Generated-code shape (probe, fork build): initialized narrow vars —
zero per-read extensions (unchanged); uninitialized — exactly one
`ext8 x, x` at the decl.

### Issue draft (file first; PR says "Fixes #NNN")

> **Title:** c2mir: reading an uninitialized narrow local yields an
> out-of-range value under MIR-gen (-O0/-O1)
>
> `char x; x / 1000` can return nonzero: whatever indeterminate value
> `x` holds must still be within char range, but the read yields the
> backing reg's stale 64-bit garbage.
>
> Reproducer: gcc.c-torture `execute/pr34099-2.c` aborts with
> `./c2m -O0 pr34099-2.c -eg` (also `-O1`; `-O2` passes because the
> folds mask it).
>
> Cause: since the issue-458 follow-up, reads of narrow reg-homed
> decls emit no extension, relying on the invariant that a narrow reg
> value is born from an extending operation. An uninitialized local's
> pseudo-reg was never stored to, so the invariant does not hold for
> it. The invariant itself is good — the repair belongs at the birth
> site (PR follows).

### PR body draft

> Reading an uninitialized narrow (`i8`/`u8`/`i16`/`u16`) auto local
> can yield a value outside the type's range: its backing reg holds
> stale 64-bit garbage, and since the issue-458 follow-up reads of
> narrow reg-homed decls are (rightly) extension-free.
> gcc.c-torture `execute/pr34099-2.c` (`char x; x/1000` must stay in
> char range) aborts under MIR-gen at `-O0`/`-O1`.
>
> ## Fix
>
> Emit a single sign/zero extension for an uninitialized narrow auto
> local at its declaration point, so every read keeps relying on the
> invariant that a narrow reg value is born extended. Generated code
> is otherwise unchanged: initialized variables get no new
> instructions, and the cost is one insn per uninitialized narrow
> local (instead of one per read, the concern with the original #459
> approach).
>
> ## Reproducer (fail before, pass after)
>
> ```
> $ ./c2m -O0 gcc.c-torture/execute/pr34099-2.c -eg   # aborted, now exit 0
> $ ./c2m -O1 gcc.c-torture/execute/pr34099-2.c -eg   # aborted, now exit 0
> ```
>
> Test added as `c-tests/new/uninit-narrow-local.c` (original code,
> covers char/signed char/unsigned char/short/unsigned short with a
> reg-dirtying helper so the failure is deterministic).
>
> Fixes #NNN.
>
> 🤖 Generated with [Claude Code](https://claude.com/claude-code)

## PR 2 — branch `fix-mir-alias-ctx-alloc-size` @b62c2ae4

One-liner: `mir.c` allocates `ctx->alias_ctx` with
`sizeof (struct string_ctx)` (copy-paste). Latent today (the structs
are layout-identical) — lands as PR-only, no issue (nothing
behavioral to report; owner may prefer an issue anyway — say so).

### PR body draft

> `_MIR_init` allocates `ctx->alias_ctx` with
> `sizeof (struct string_ctx)` — a copy-paste from the `string_ctx`
> allocation in the same condition. The two structs are currently
> layout-identical (a `VARR` pointer plus an `HTAB` pointer), so the
> bug is latent, but any field added to `struct alias_ctx` would
> silently overflow the allocation.
>
> 🤖 Generated with [Claude Code](https://claude.com/claude-code)

---

## Fork side (lands regardless of upstream)

`/workspace/mir` develop: restore the upstream `addr_p` read gate in
`force_val` (superseding bde8658d's unconditional re-widening) + the
same birth-extension. Net: fork and upstream converge on the improved
scheme; madc keeps pr34099-2 green with better codegen than the
re-widened form. `MIR_COMMIT` bump rides the same madc push.

## Validation ledger

- [x] upstream HEAD c2m: pr34099-2 aborts -O0/-O1 (bug real)
- [x] fork + improvement: pr34099-2 passes -O0/-O1/-O2; issue-458
      repro passes -ei/-eg
- [x] MIR-dump probe: no per-read exts (initialized), one ext at decl
      (uninitialized)
- [x] fork `make test` green (incl. bootstrap -O0/-O1/-O3 + parallel)
- [x] madc battery green: 751/0/0/9, exe 735/0, packed 751/0/0/9
- [x] gcc torture sweep: 1614/1/9/0/61 — baseline byte-identical
- [x] fork develop committed @9c7e7f3b + pushed; `MIR_COMMIT` bumped
- [ ] PR branches: build + `make test` + new test fail-before/pass-after
      at upstream HEAD
- [ ] OWNER final review → file issue, rename nothing (test name is
      descriptive), `gh pr create` from fork branches

## Submission mechanics (after owner OK)

1. Push both branches to `origin` (derekbsnider/mir).
2. File issue 1 (`gh issue create -R vnmakarov/mir`), take its number,
   set `Fixes #NNN` in the PR-1 commit/body (amend + force-push branch).
3. `gh pr create -R vnmakarov/mir` from each branch, bodies above.
