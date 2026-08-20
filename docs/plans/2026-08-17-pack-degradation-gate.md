# Pack degradation gate (task #63) — 2026-08-17

Gate the forest pack's **silent** degradation. A pack run exits 0 while
tolerating parse failures, and a bind can lose an entire aggregate without a
word. Task #64 was exactly that, and it shipped.

## 1. What #64 did, and why nothing caught it

A class-nested enum tag was **stamped** a forest type-id and never
**recorded**. `[basic.scope.class]/1` makes such a tag a member, so
`TokenENUM::parse` deliberately keeps it out of `datatype_map` — and the
freeze's enum-recording walk read exactly that map. `std::ios_base` carries
`event_callback *__fn_`, whose signature takes `ios_base::event`, so at bind
the function pointer could not be rebuilt, `__fn_` would not swizzle, and the
aggregate fill bailed on a bare `continue`. Members flatten from bases, so one
lost member killed eleven aggregates — the whole darwin iostream family. The
first observable symptom was `struct has no member __vptr` on a Mac, a layer
later and a day's work away.

v0.82.0 added the three diagnostics that name this class. This task adds the
gate that reads them, plus a ratchet over the pack's own tolerated parse
errors.

## 2. The measurement — and the correction it forced

The prior handoff recorded a single baseline of **58** pack parse errors. That
number is real, but it is **the darwin pack's**, not the Linux one's. Evidence:
`tmp/logs/macos-82.log` splits at its two `forest_pack_darwin: OK` lines into
**58 + 58** anchored diagnostics (arm64, x86-64 — identical), and its classes
are libc++'s. Linux and win64 both read **93**, whose classes are libstdc++'s
and share almost nothing with darwin's:

| profile | pack-errors | dominant classes |
|---|---|---|
| linux | 93 | 51 `__cerb`, 12 `_M_dispose`, 10 `max_size`, tail |
| win64 | 93 | same corpus — mingw ships libstdc++ 13.2 |
| darwin | 58 ×2 | 15 lambda parsing, 10 facet member lookup, 4 `static_cast` tail |

One number for all three would have been **35 free slots** for regressions on
the two libstdc++ profiles. Hence a per-profile baseline
(`docs/parity/pack-degradation-baseline.txt`), keyed by the pack scripts' own
names for themselves. Both darwin arches check the same key against their own
logs, so a divergence between them still fails loudly.

### Counting with the right pattern

Three patterns, three answers, on the same log:

| pattern | linux pack | why it is wrong |
|---|---|---|
| `grep -c error` | 339 | matches `filesystem_error` in the deliberate `pack drop:` lines |
| `grep -c 'error:'` | 102 | matches `system_error:616:6:` in a warning's file path |
| `grep -c ': error: '` (ANSI-stripped) | **93** | anchors on the diagnostic's own shape |

madc prints `<file>:<line>:<col>: \e[1;31merror:\e[1;37m <message>`
(`Program::print_diagnostic`), so the anchored pattern only exists *after* the
colour strip. The gate normalizes first: NULs dropped (a `-v` dump carries
them), ANSI stripped, CR dropped (wine's CRT writes CRLF).

## 3. Two halves, three strictnesses

### Producer — ratchet

Tolerated parse errors may only go DOWN. Each is a header construct madc could
not parse; the gate prints the normalized class breakdown, which **is** the
burndown list. Long generated identifiers collapse to `<T>` so six copies of
one missing overload read as one class of six rather than six classes.

### Consumer — hard zero for two, ratchet for two

Measured on the packed product, `-v` over a real C++ program:

| counter | linux | win64 | verdict |
|---|---|---|---|
| `materialize filter:` | 1 | 1 | **the marker** — proof the probe materialized at all |
| `materialize fill: DROPPED` | 0 | 0 | **hard zero** |
| `forest_restore_decls: SKIPPED` | 0 | 0 | **hard zero** |
| `materialize derived: UNRESOLVED` | 139 | — | not counted |
| …of those, `kind=0` | 57 → **55** | 57 → **55** | ratchet (census) |
| `materialize closure: DROPPED` | 1 → **0** | 3 → **0** | ratchet |

The right-hand values are after the `long double` fix in §4. Pinning slot 18
did not merely silence 2 census lines — it cleared **every** closure drop on
both profiles, because the dropped records were the long-double-bearing ones.
The container now carries strictly more than it did.

**Hard zero** for `fill: DROPPED` and `restore: SKIPPED`: each is a record the
consumer ADMITTED and then could not use, a bound unit is reported SERVED
either way, so no live parse rescues it, and neither has a legitimate instance.
`fill: DROPPED` is the counter #64 would have tripped — it was added at #64's
own bail site.

**`UNRESOLVED` alone must not be gated**: 139 on a Linux probe are normal — a
derived type whose operand sits outside the bound closure.

**`kind=0` ratchets, and finding that out was the point.** `get_def_at` returns
false for pinned/system ids and `has_def()` is literally
`get_def_at && kind != DK_NONE`, so `kind=0` does mean "a project id the freeze
stamped and no record walk wrote". The prior handoff called that "no legitimate
instance". Measuring it proved otherwise: **55 of 57** are DEPENDENT operands
(`ref _Tp*`, `allocator__Tp1*`, `polymorphic_allocator__Up*`) — a pointer to a
template parameter has no concrete record by construction, and the consumer
re-instantiates from the pattern instead. Hard-zeroing that counter would have
gone red on day one on a legitimate class, and the obvious "fix" would have
been to delete the check. A ratchet still catches #64: its nested enum ADDED
one, and a rise is a failure.

### The marker is the check's own negative control

All the load-side diagnostics are `DBG`-gated. A load log from a run that never
bound anything contains zero of everything and would pass vacuously. No marker,
no verdict: the gate fails loud instead of reporting success for a test it
never ran.

## 4. What widening the diagnostic revealed: `long double` at bind

`materialize derived: UNRESOLVED` printed `kind=` **only** in the fn-ptr-param
arm. A `DK_NONE` reached as a ptr/ref/const/carray *operand* printed a bare
`operand`, indistinguishable from the routine outside-the-closure case — and
those chains are the majority of derived ids. Fixed where the diagnostic is
composed (`CirFrozenForest::materialize_from_arena`): the operand arm now
spells `kind=N`, or `(no record)` when there is no record at all, mirroring the
param arm.

That is what made the census measurable — and 2 of the 57 were real:

```
materialize derived: UNRESOLVED ptr long double* (operand kind=0)
```

`MADC_TYPEID_LONG_DOUBLE` (slot 18) was still marked *"reserved: P0 wide-value
work"* in `madc_primitive_for_slot()`, left NULL "until its DataDef exists" —
but `ddLDOUBLE` has existed since real long double landed in `114b13a8`
(v0.78.0). With no PINNED id, the freeze minted `long double` a PROJECT id that
no record walk writes, so at bind the type could not swizzle. The consequence
was not cosmetic:

```
struct fbgate_ld { long double lo; long double *pp; int tag; };

live:  ld=1.5 2.5 7 32      (== g++ 13 == clang++ 18)
bind:  rc=1  error: Unidentified member 'lo' in 'fbgate_ld'
```

**Any struct with a `long double` member lost that member when bound from a
container.** Same loss family as #64, on a primitive instead of an enum. Fixed
by pinning slot 18; gated by `tests/unit/test_datadef.cpp` (the slot maps to
`&ddLDOUBLE` and stamps its own number) and by `forest_bind_gate`'s new
`[ldouble]` case, which carries the g++/clang oracle above and proves
bind == live == g++.

Slots 21/22 (`COMPLEX_FLOAT`/`COMPLEX_DOUBLE`) stay correctly reserved:
`DataDefCOMPLEX` is constructed per element type, so there is no global
singleton to pin.

## 5. Where each half runs

| profile | producer ratchet | consumer half |
|---|---|---|
| `linux` | `scripts/forest_pack.sh` | same script — a `-v` run of the packed product |
| `win64` | `scripts/forest_pack_windows.sh` | same script — a `-v` run of the shipped PE under wine |
| `darwin` | `scripts/forest_pack_darwin.sh` (once per arch) | `scripts/mac_battery.sh` leg 3c, on a Mac |

Darwin's split is structural, not an omission: the darwin pack is a **cross**
freeze, so the build container cannot execute the consumer that would
materialize the container. That is the same gap task #60 (the macOS headerless
cell) exists to close — on a Mac with Xcode a degraded unit is silently rescued
by live-parsing the SDK, which is why this class stays invisible there.

The Mac leg gates the two hard-zero halves and **reports** the census, because
there is no baseline file to ratchet against on the far side of a cross-freeze.
Its numbers get recorded here when a Mac run reports them (still unmeasured).

`mac_battery.sh` must stay self-contained (it runs on a Mac from an unpacked
tarball, with no repo beside it), so it carries its own copy of the token
strings. `forest_pack_gate.sh --selftest` leg 11 fails if that copy drifts —
otherwise a renamed diagnostic would leave the Mac leg vacuously green.

## 6. The gate's own negative control

`bash scripts/forest_pack_gate.sh --selftest` (wired into `make -C src
fulltest`) is hermetic — no madc, no pack. Eleven legs, both directions of
every boundary:

- pack count exactly at baseline → GREEN; one over → RED
- the loose-pattern decoys (`filesystem_error`, `system_error:616:6:`) → GREEN
- a load log with no marker → RED (blind)
- a routine `UNRESOLVED`, and closure drops at baseline → GREEN
- the DK_NONE census at baseline → GREEN; one over → RED
- `fill DROPPED`, `restore SKIPPED`, closure-over-baseline → RED
- the token contract is still shared with `mac_battery.sh`

Three traps the harness itself walked into first, all now closed:

- It invoked `"$0"`, and the script ships without the exec bit — so every
  expect-RED leg passed on rc=126 "Permission denied". It now runs `bash "$0"`.
- Expect-RED accepted *any* nonzero rc, which a usage error or a crash also
  gives. It now demands exactly rc=1, the RED verdict.
- The token-contract leg was negative-controlled by hand: renaming
  `materialize fill: DROPPED` in a scratch copy of `mac_battery.sh` made leg 11
  fail with the missing token named, and the file was restored by checksum.

## 7. Found here, NOT fixed here: both macOS packs ship with no MIR cache

The gate reads the pack log, and the pack log has been saying this the whole
time — tolerated, exit 0, in every macOS release including the promoted one:

```
../obj/hosted-arm64-macos/forest.bin.tu.cpp:  mir cache: MIR error during module
    compile: wrong result type in proto proto138 — blob skipped (consumers fall back)
../obj/hosted-x86-64-macos/forest.bin.tu.cpp: mir cache: MIR error during module
    compile: func duration_double_std____1__ratio_1_1000000000___operator%=:
    in instruction 'umods': unexpected operand mode for operand #1.
    Got 'ldouble', expected 'int' — blob skipped (consumers fall back)
```

Linux packs a 467 KB blob and win64 a 497 KB one. **Both darwin arches pack
none.** Correctness survives — that is exactly why it exits 0 — but every
consumer compile on macOS then pays full compile price, which puts it next to
task #25 (startup latency).

Byte-identical in the pre-change and post-change runs, so it is pre-existing and
not a side effect of the `long double` pinning. It is now GATED as
`darwin mir-blob-skips = 1` rather than left silent, so the burndown is visible
and cannot grow.

Two separate causes, both real:

- **x86-64:** `std::chrono::duration<double, nano>::operator%=` is instantiated
  and lowers `%` to an integer `umods` with a floating operand. `%` on a
  floating rep is ill-formed C++, so the candidate layers are (a) whichever pass
  speculatively instantiates that member during the pack, and (b) the `%`
  lowering choosing an integer opcode from a floating operand type. Reducer
  shape: a `duration<double, nano>` `%=` in a libc++ TU.
- **arm64:** `wrong result type in proto proto138` — a MIR prototype/result-type
  mismatch, no source function named, so it needs the module dumped to localize.

Too large for this session honestly — each needs its own reducer and layer
walk — so it is the next item, not "eventually". It is filed with this evidence
rather than recorded as a bare gap.

## 8. Lowering a baseline

Fix a class, re-run that profile's pack, and the gate prints the exact new
number (`IMPROVED — lower '<profile> <metric>' to N`). Lower it in the **same
commit** as the fix. Raising a baseline needs a stated reason in the commit;
the end state for `pack-errors` is a corpus that parses.
