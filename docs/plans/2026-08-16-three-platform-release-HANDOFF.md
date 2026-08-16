# HANDOFF — the three-platform release (session #96, 2026-08-16)

**Read this fully before acting. Assume a cold start.** Run `bash scripts/resume.sh`
first — it prints live git state and orphaned background jobs, which a compaction
summary cannot.

**OWNER DIRECTIVE (verbatim intent):** release all three platforms — Linux,
macOS, Windows — *working properly*. A macOS regression is NOT acceptable:
"we worked on the MacOS release for almost a month". Do not ship a darwin
artifact that is worse than what is already published.

Branch: **`fix/darwin-stdarg-vprintf-claude`** @ `ee2b46fe`, based on `develop`
@ `ce610caf` (v0.81.0).

---

## 1. What is DONE

### The stdarg fix (committed, `ee2b46fe`) — a real regression, fixed

madc's embedded `<stdarg.h>` declared `vsprintf`/`vsnprintf`/`vfprintf`/`vprintf`.
Darwin generates its prelude with `clang -E -dD` at `_FORTIFY_SOURCE=2`, so
`#define vsprintf(str,...) __vsprintf_chk_func (str, 0, __VA_ARGS__)` is live as
soon as anything pulls the stdio chain in. Line 54 then expanded *inside madc's
own header* into `__builtin___vsprintf_chk (char *, 0, __darwin_obsz(...), ...)`
and failed to parse — killing every `hosted-*-macos` forest pack.

Fixed by DELETING the declarations: gcc 13's and clang 18's own `stdarg.h`
declare **zero** stdio functions, and `<stdio.h>` owns those names. Latent on
Windows too (mingw `strsafe.h` poisons `vsprintf`). Reducer
`tests/testfortifiedvprintf.mad` reproduces on **Linux** with the unfixed binary,
so the ordinary suite gates the class — no macOS lane needed.

**Validated:** Linux `fulltest` **1054 / 0 failed / 0 timed out / 9 skipped**
(rc=0, +1 = the new reducer, skips unchanged); rule-trailer gate 430/0. Both
darwin arches now pack `forest_pack_darwin: OK (835 units)` and both
`verify_macho_release` pass. `win build rc=0`.

---

## 2. What BLOCKS the release — task #64

**macOS is regressed and must be fixed before it ships.** Measured on the real
Mac (`derek@192.168.1.79`, macOS 15.3.2 arm64, `LC_ALL=C`), same
`mac_battery.sh`, two artifacts:

| artifact | battery |
|---|---|
| v0.77.0 (last macOS release built) | **7 passed / 3 failed** |
| v0.81.0 + this fix | **3 passed / 7 failed** |

Four legs regressed, one root: `cout << "hi"`, emit-C sret on-target, emitted-C
runtime archive, native AOT executable. Three failures are PRE-EXISTING and
shipped in v0.77.0 — do NOT chase them here: C++ groves, value intrinsic
(SIGSEGV 139), exec:// channel.

Full detail, reducer, ruled-out hypotheses and the located layer are in **task
#64** — read it before touching anything. The short version:

- Binding the darwin forest breaks C++ resolution; `--forest-bind` off ⇒ rc=0.
- ⚠️ The `Unknown namespace or class 'ios_base'` messages are **NOISE** —
  `flush_forest_pending_globals` wraps default-arg re-derivation in `try/catch`
  and `throwit` prints to stderr even when swallowed. Do not chase `ios_base`.
- The REAL errors: `shift operands should be of an integer type` (`operator<<`
  never resolved, so `<<` stayed a literal shift) and
  `struct has no member __vptr` ×6.
- `__vptr` is **synthetic**, emitted by `cir_builder.cpp` ~9836-9885 gated on
  `cdd->has_vptr_slot`; just above it
  `bool opaque = cdd->members.empty() && !cdd->has_vptr_slot;` emits the class as
  a featureless `long _w[words]` filler.
- The freeze DOES transport the state (`cir_freeze.cpp:2584-2612` restores
  `has_vtable`/`has_vptr_slot`/`vtable_groups`/base specs).
- **Next step:** dump `has_vptr_slot` + `members.size()` for the restored
  `basic_streambuf`/`ios_base` vs the live-parsed ones (same TU, bind on vs off).
- RULED OUT: `fe88af34` (reverted, rebuilt, identical failure). NOT a corpus
  change — `forest_pack_headers_darwin.txt` unchanged since v0.76.0, and both
  v0.76.0 and HEAD pack 835 units / 116 tolerated errors. So it is the
  RESTORE/SERVE path, v0.78-v0.81.
- No lane could see it: darwin is the ONLY libc++ forest.

---

## 3. Other findings banked this session

- **#63** — `forest_pack_darwin` returns 0 while the parse fails; v0.76.0's own
  verified-good build emitted 339 `error:` lines and still reported
  `OK (835 units)`. Wants a ratchet, not a hard zero. Not a release blocker.
- **v0.76.0 cannot rebuild its own macOS lane today** — it dies at
  `verify_macho_release: FAILED — extracted forest does not read back`, fixed
  later by `46afd344`. Informational.
- **Build hygiene (owner raised it).** `obj/` persists across `git checkout`, and
  two inputs are undeclared: `obj/mir/<variant>` (MIR moved external →
  `third_party/mir`, so hopping that migration silently mixes two MIRs) and
  `src/embedded_headers.cpp`, a TRACKED file the build rewrites — which makes
  `git checkout` REFUSE and silently leave you on the wrong tree. Proposed, not
  yet done: drop `src/embedded_headers.cpp` from git (write it only to
  `$(OBJDIR)`, as the hosted modes already do), stamp each obj tree with its MIR
  source root using the existing config-axis stamp idiom
  (`.forest-shape-`/`.config-file-`/`.link-shape-`), and add `make bisect-clean`.

## 4. Process rules this session paid for

- **A `git bisect` never tests its endpoints.** Asserting a "good" endpoint
  produced a bisect that blamed a two-shell-script commit for a compiler parse
  regression. Run the test on BOTH endpoints by hand first.
- **No build failure is evidence until it reproduces from a clean `obj/`.**
- **Guard every scripted checkout** (`git checkout -f` + assert the resulting
  SHA); a dirty generated file silently redirected two runs onto the wrong tree.
- With a minimal reducer in hand, **stop bisecting and diagnose** — `-v` found
  the stdarg root in minutes after ~2h of bisecting.

## 5. Remaining to finish the release

1. Fix #64 (the blocker).
2. Re-run the Mac battery — target is **at least** v0.77.0's 7/3.
3. Finish Windows: `wine` suite (running), `headerless-win`, `win_battery.sh` on
   the real box.
4. Linux `exe` + `obj` + `release` + `packed` + `headerless`.
5. `/release` then `/promote`.
