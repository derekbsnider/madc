# MIR fork → upstream probe (fix-tier classification)

**Date:** 2026-07-23 · **Status:** DECIDED (owner, same day) — bugfix
wave GO; features await real discussion with Vladimir; fork-future
steering deliberately none (not shy about fork-only features)
**Fork:** github.com/derekbsnider/mir `develop` @40fdf81b
**Upstream:** github.com/vnmakarov/mir `master` @a8ab7c31 (2026-06-19)

## Owner decisions (2026-07-23)

1. **No steering** between the two fork futures (upstream absorption
   vs monolith vendoring) until actual discussion with Vladimir;
   fork-only features continue freely meanwhile.
2. **Bugfix PRs approved** — continuing an existing practice: we have
   already contributed PRs upstream, each carefully **authored by us**,
   verified a **real, still-present** bug, and **not already fixed**
   upstream. Same bar applies here; owner reviews staged PRs before
   submission.
3. **Features (SIMD, _Complex, cleanup, debug) need Vladimir
   discussion first** — only bugfixes are sure-accepts today. No RFCs
   without that discussion.

## Authorship audit (gates PR eligibility)

Of the 26 Tier-A commits, **15 are Derek's** (PR-eligible) and **11
are Cyan Ogilvie's** (not ours to send: the aarch64 clobber family,
x86-64 va_start offsets with block args, long-double va_arg alignment,
GVN store-forwarding extension loss, jump_opt label liveness, LADDR
output marking, simplified-RA-for-indirect-jumps, paramless varargs,
and the big-blk-arg2 test). Cyan's fixes stay in the fork; upstreaming
them is Cyan's call.

## Context

The fork carries 164 non-merge commits over upstream. As of today
upstream HEAD **equals our merge-base** — zero upstream drift — so any
subset cherry-picks onto upstream master conflict-free. This is the
cheapest moment to upstream anything we ever intend to.

Why bother (fork endgame, see `project_backend_decision` memory /
`docs/adr/0001-cir-c2mir-backend.md`): every fix upstream absorbs
shrinks the divergence we maintain forever; what upstream declines
stays deliberate divergence (owner: divergence is NOT to be minimized
for its own sake — this probe targets only the tier that is
*obviously* upstream's gain).

## Tier A — standalone bugfix PR candidates (small, self-contained, test-carrying)

Codegen/ABI correctness, ranked by upstream value and independence:

| Group | Commits | What it fixes |
|-------|---------|---------------|
| SysV varargs ABI | c40ed469, d14ed83e | register-save-area boundary + mixed-class struct va_arg; va_start offsets with block args |
| ff_call XMM slots | cc74fef7 | skipped XMM slots for mixed-class block args |
| aarch64 clobbers | e1306884 (+ports 7e1e61aa, test b2d79499), 4beb13e7, 9ab36fbe | arg-reg clobber by by-ref block-arg copies (riscv64/s390x/ppc64 ports); x9 clobber in bb thunk; long double stack alignment in va_arg/ff |
| Narrow-value extension | bde8658d, ff01f808, 3de6283f | force_val must extend ALL narrow int reg values (gcc pr34099-2); narrow address-taken values; GVN losing sign/zero extension when forwarding stores |
| SSA/RA hazards | 058893f5, 6c831111, 8e0e569e, 290024c5, 65b99fc6 | lost-copy in out-of-SSA self-loops; GVN CSE of fixed-place VA_BLOCK_ARG; LADDR output-operand marking; jump_opt label liveness; simplified RA with indirect jumps |
| c2mir layout/semantics | 01f999bb, caa6ff96 + 74adb6a6, 772efebd, 8f97e4fc, 4aa628ba, 7a909601 + 95e52f9c | auto-local layout by real scope depth (decl_cmp); struct/union statement-expression copy-out + value context; zero-length-array member offset; string-literal const-addr at any scope (pr53084); static-initializer elements out-of-line (PR middle-end/24109); union alias classes (incl. through subscripts) |
| Misc | 2158356f, ebf825f7 | deterministic long double serialization (bootstrap self-consistency); paramless vararg functions |

Note: madc's CIR backend *depends on* several of these (decl_cmp,
stmt-expr copy-out, varargs/_Alignas ABI per `MIR_COMMIT` rules) —
upstream absorption removes divergence exactly where it costs us most.

**Attribution caveat:** 8864a739 adopted 5 proven fixes from community
forks (cyanogilvie et al.). Those are NOT ours to PR — either credit
the original authors explicitly or leave them to their forks.

## Tier B — feature tracks (upstream-plausible, needs discussion first)

- **SIMD / vector_size / ext_vector_type** (~55 commits, `feat(mir|c2mir)`
  v128 stack): explicitly *designed for upstream* (KG
  `Decision{simd_raise_mir_upstream}`, Track 1.6). Biggest win, biggest
  review. Propose as an RFC issue before any PR.
- **Native C99 `_Complex`** (~20 commits from fc04a5a5 through ae131cfd,
  e0872fda ABI): complete, gcc-differential tested; touches TP_* core
  enums — needs upstream buy-in.
- **`__attribute__((cleanup))`** (8 commits f53a46e2..53cdb85f): full
  matrix (fall-through, return, break/continue, goto), gcc-differential
  MATCH test.
- **`__int128` + overflow builtins** (1ee0961a, 545ad469): standard-C
  pressure upstream already feels; good candidates.
- **Debug stack** (mir-debug: GDB-JIT DWARF, line stepping, variable
  inspection, .debug_frame CFI, spill-all mode): high user value;
  UPLINE question already open (see `docs/mir-debug-support.md` —
  originally cyanogilvie's design, extended here; shared attribution).
- **Small API surface**: MIR_set_inline_permission (e411c6da), code
  length expose (4821f131), contiguous code-holder reservation
  (c1278247, 5df536f6) — easy sells if motivated.

## Tier C — madc-specific, stays in the fork

- External-AST / libc2mir API (128c134a..062dd977, op iterators,
  check-gate, privatize_for_link) — the madc IR seam; upstream has no
  consumer.
- madc language extensions in c2mir (class keyword, e0f94eef, ea895959,
  06c203fc) — deliberately madc.
- **MIR_object ELF stack** (R2/R4/R4b/R5/R6: .o emission, direct
  ET_EXEC/ET_DYN, object loader, DWARF-in-artifacts, addrpool PIC) —
  flagship fork capability; upstreamable only as a major RFC if
  Vladimir wants native AOT at all. Not first-wave.

## First wave (approved — Derek-authored fixes only)

One branch per PR off upstream/master, cherry-picked from the fork,
upstream suite green on each, owner reviews before submission:

1. **SysV varargs/ff ABI (x86-64):** c40ed469 (register-save-area
   boundary + mixed-class struct va_arg) + cc74fef7 (ff_call skipped
   XMM slots for mixed-class block args).
2. **Narrow-value extension:** bde8658d (force_val extends ALL narrow
   int reg values, gcc pr34099-2) + ff01f808 (narrow address-taken
   values).
3. **Gen/SSA hazards:** 058893f5 (lost-copy in out-of-SSA self-loops)
   + 6c831111 (GVN must not CSE fixed-place VA_BLOCK_ARG).

Second wave (after first responses): union aliasing (7a909601 +
95e52f9c), scope-depth auto-local layout (01f999bb),
statement-expression semantics (caa6ff96 + 74adb6a6), layout/init
corners (772efebd, 8f97e4fc, 4aa628ba), deterministic long-double
serialization (2158356f).

SIMD / _Complex / cleanup / debug: parked until the Vladimir
discussion (owner decision #3).

Nothing here changes madc's pin discipline: `MIR_COMMIT` keeps pinning
the fork regardless of what upstream takes; absorbed fixes fall out of
the divergence naturally at the next upstream-base bump.
