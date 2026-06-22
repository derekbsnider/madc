# madc Front-End Performance — profile-ranked execution plan

**Date:** 2026-06-22
**Status:** PLAN
**Goal:** Close the front-end speed gap to g++ — make madc *fast at the work it
already does* (lex + parse + instantiate), independent of the embedded-header
forest (which is now deferred behind this; see that plan's DEFERRED note).

This plan is **ranked by a live profile**, not by intuition — an earlier guess
(string-identity / `dynamic_cast`) was measured and largely refuted.

**Strategic framing:** madc was built C++-convenience-first — `std::string`,
`std::deque`, `std::istream`, `std::map`, `std::stringstream` throughout the hot
path. That was a reasonable way to reach correctness fast, but the profile shows
that overhead now *is* the cost (~70% allocation + copying). This track is the
deliberate move to **lean primitives on the hot path**: arena-allocated POD
tokens, a flat `char`-buffer lexer, and **interned strings compared as pointers**.
It is a targeted swap of the hot *data structures*, not a from-scratch rewrite of
the (correct) parser logic.

---

## Measured baseline (2026-06-22)

**Wall time, `testsubscript` (`<iostream> <vector> <map> <string>`):**

| | time | vs g++ |
|---|---|---|
| madc **-O0** (old default build) | 4.40 s | ~8× g++ front-end |
| madc **-O2** | **2.53 s** | ~4.6× g++ `-fsyntax-only` (0.55 s); ~2.7× g++ `-O0` full (0.94 s) |
| g++ `-fsyntax-only` | 0.55 s | — |
| g++ `-O0` compile+link | 0.94 s | — |

`-O2` alone bought **1.74×** (4.40→2.53 s); the remaining ~3–5× is algorithmic.

**callgrind ranking (madc -O2, by instruction cost) — the evidence this plan is built on:**

| cost center | % | cause |
|---|---|---|
| malloc/free/new/delete | **~47%** | per-token heap `new`, token cloning, deque growth, string allocs |
| memcpy/memmove | ~14% | deque/string/token-clone copies |
| `deque<TokenBase*>` mgmt | ~12% | pointer copy_move on growth, init, dtor |
| `std::istream` get/peek/**sentry** | ~10% | lexer reads source char-by-char through iostream |
| `std::string` ops | ~5% | push_back, `==`, assign, strlen |
| `std::map<string,…>::find` | ~1.5% | string-keyed RB-tree lookups |
| `dynamic_cast` | **0.36%** | negligible |

**Headline:** ~70% is allocation + copying, and it traces to the **token
representation** (per-token `new` + `deque<TokenBase*>` + cloning tokens to
instantiate). Another ~10% is reading source through `std::istream`. String
identity (~5%) and `map` lookups (~1.5%) are minor; `dynamic_cast` is free.

**Malloc-caller attribution (inclusive cost, same profile) — locks the order:**
- The **token-fetch/lex path dominates** inclusive cost (`Program::_getToken`,
  `getRealToken`, `tokenize`) — i.e. allocation originates in the lexer + token
  machinery (→ P1 arena + P2 buffer lexer).
- **`std::deque<TokenBase*>` copy-constructor = ~43% inclusive** — the single
  biggest identifiable allocation source. madc clones entire token-stream deques
  (each clone = new deque + copy every token pointer) to re-parse template
  bodies. This couples **P1 (no deque, POD arena) + P4 (don't clone — copy the
  `cir_node` subtree + substitute)**: together they kill it.
- String building (`resolve_declared_type_token` ~25% inclusive) is real but
  **secondary** to the token representation — confirms P3 (interning) ranks
  *below* P1/P4, though still worthwhile.

So the locked story: **the token representation (per-token `new` + deque +
cloning) is the #1 cost; the `istream` lexer is #2; string interning is #3.**

---

## SETTLED design points (2026-06-22)

1. **Parser stays hand-written recursive descent. Do NOT build a table-driven LR
   state machine.** C++ is not LALR(1) (lexer hack, most-vexing-parse, template
   `>>`); gcc rewrote *away* from yacc to recursive descent, clang and TCC are
   recursive descent. The "switch/table" speed comes from **integer-token `switch`
   dispatch + table-driven char classification**, not LR tables.
2. **Lexer = a flat-buffer DFA (the TCC model).** Read the source once into one
   contiguous buffer with an end **sentinel**; scan with a `const char*` pointer
   (`buf_ptr`), inline the hot advance/peek (macro, no call), call a slow path only
   at the sentinel. Classify chars via a **256-entry table** (`IS_SPC|IS_ID|IS_NUM`).
   Drop `std::istream` from the hot path entirely.
3. **Tokens are POD records in an arena, not heap-`new`'d polymorphic objects in a
   `deque`.** Bump-allocate into a contiguous buffer; an index cursor; backtrack =
   index rewind.
4. **`dynamic_cast`/polymorphism-collapse is NOT a perf item** (0.36%). It may still
   be desirable for the POD-token representation, but it is not justified *by speed*.
5. **Lean primitives live in C++ classes with INLINE methods — not in C.** The DFA
   lexer and the token arena are ordinary C++ classes whose **hot accessors are
   `inline`, defined in the header** (`peek`/`advance`/`cur`, `token_at`/`next`),
   over a flat buffer. `-O2` then inlines them to the same code as a C macro, with
   type safety kept. The anti-pattern to kill is exactly madc's current
   **out-of-line** `Source::get()/peek()` (in `lexer.cpp`) wrapping `std::istream`:
   a real call + sentry per char, uninlinable. Rule: anything on the per-char or
   per-token path is a header-defined `inline` method over a flat buffer.
6. **Strings are interned (deduped/hashed) so they compare as pointers.** This is
   an allocation-killer, not just a compare-speedup (see P3) — each unique string
   is allocated once; comparison and `map` keys become pointer/int identity.

**Reference models:** lexer like TCC (`BufferedFile` `buf_ptr/buf_end` + `CH_EOB`
sentinel + `PEEKC` inline macro + `isidnum_table[256]`, in `tccpp.c`/`tcc.h` at
`/workspace/tinycc`); AST identity like g++ (interned names, `TYPE_CANONICAL`) —
the latter only matters for the minor string/identity follow-up.

---

## Phased levers — ordered by the profile (biggest first)

### P0 — Build `-O2` (DONE; wire into the build)
`-O2` is 1.74× for free. The Makefile default was `CXXFLAGS ?= -std=c++11 -Wall`
(no `-O`) → `bin/madc` shipped `-O0`. **Make the default optimized** (keep a
`-O0 -g` debug target). Verify fulltest + torture unchanged at `-O2`.

### P1 — Flat token arena (the ~70% lever)
Replace `deque<TokenBase*>` + per-token `new` (madc.h:1256) with a **contiguous
arena of POD token records + index cursor**. Bump-allocate; no per-token
new/free; growth is amortized vector realloc (cheap) not deque-of-pointers; no
per-token vtable object. Reuse `Source::_pushback_frames` (madc.h:736–825) as the
source-stack template for the ~95 injection sites (82 `pushToken` + 8
`pushCompound` + 5 `injected_tokens`). Targets ~47% malloc + ~12% deque + much of
~14% memcpy. Correctness-neutral if the token API is preserved.
*(This is P1 of `2026-06-09-frontend-representation-refactor.md`, re-confirmed as
the #1 lever by the profile.)*

### P2 — Flat-buffer lexer (the ~10% lever)
Replace `std::istream`-based `Source::get()/peek()` (the per-call `sentry`) with
the TCC model: read the file once (or `mmap`) into a sentinel-terminated `char`
buffer, scan via `buf_ptr`, inline advance/peek, 256-byte char-class table. Pull
tokens **on demand** and **append into the P1 arena** (so backtrack still works by
index rewind) — pull-based + arena are complementary, not in tension. Compounds
with lazy parsing: only lex what is actually consumed.

### P3 — String interning (dedup → kill duplicate allocation + pointer compare)
**Re-ranked up from a "minor 6%" guess:** interning's payoff is NOT just the
~6.5% of direct string-compare + `map`-find cost — it attacks the **dominant 47%
allocation** by deduping. Today every spelling / mangled key / identifier madc
builds is a fresh `std::string` heap alloc; intern (the TCC `TokenSym` model:
hash a name → one record, get back a stable id/pointer) and each unique string is
allocated **once**. Comparison becomes pointer/int identity, and hot
`std::map<string,…>` collapse to `unordered_map`/int-keyed maps **for free** (the
keys are now ids). So P3 is an allocation-killer in the same tier as P1, plus it
erases the ~6.5% compare/lookup cost. Pairs naturally with P1 (the arena and the
intern pool are the two "stop allocating" levers).

### P4 — Stop cloning tokens to instantiate (the no-reparse lever)
Today instantiation clones the template's tokens and re-parses (the
`_Deque_base` ctor/dtor churn in the profile + the 1.49 s `instantiate` bucket).
Move toward copy-the-`cir_node`-subtree-and-substitute (no reparse). Folds into
P1's allocation win *and* the algorithmic gap. (Full form is the forest's
`(node,env)` memo; the no-reparse mechanic is worth landing independently.)

### NON-goals
- Table-driven LR parser (above).
- Polymorphism-collapse *for speed* (0.36%).
- The embedded-header forest (deferred behind this track).

---

## Gates (every lever)

- **Correctness-neutral:** `make -C src fulltest` green; gcc.c-torture failset
  byte-identical to the 51-name baseline; `--emit=c11` byte-identical
  before/after (the representation changed, the emitted C must not).
- **Perf oracle:** re-time `testsubscript` (+ the spread: testmap, testtuple,
  testcompare) and **re-run callgrind** after each lever — confirm the targeted
  cost center actually dropped, and catch any new one.
- One heavy job at a time; cap every run (`ulimit -t` + `timeout`).

---

## Sequencing / relationship to other plans

- **Order (locked by the malloc attribution):** P0 (done) → **P1 (arena tokens) +
  P4 (no-clone) — the coupled dominant pair** (per-token `new` + the ~43%
  deque-copy clone) → **P2 (buffer lexer, ~10%)** → **P3 (interning, secondary)**.
  P1 lands first (the arena makes even an interim clone cheap); P4 (copy
  `cir_node` + substitute, no token clone) removes the clone entirely. Re-profile
  after each to re-rank.
- **Supersedes the *priority ordering*** of `2026-06-09-frontend-representation-refactor.md`
  with live-profile evidence: allocation/representation (P1 + P3 interning) is the
  dominant lever; the value-pool/identity work that doc framed as foundational is
  the *interning* lever here, now ranked by its allocation impact, not just compare cost.
- **Embedded-header forest is deferred behind this** — and this track makes
  *project* code fast too (the forest only removes re-parsing the *stdlib*).
- Fenced behind the develop→master CIR-parity gate like all optimization work;
  but P0 (`-O2` default) is a free, ship-now win.
