# Rung 1 Step 4 — drop `TokenIdent::str` + lexer rolling-hash (CONTINUATION CONTRACT)

**Date:** 2026-06-30 · **Branch:** `develop` · **Read this fully before acting.**
This is the rung-1 CAPSTONE and the only remaining rung-1 work. It is the
biggest, most delicate piece: it touches the hot lexer loop + ~**499** `.str`
read sites (parser+lexer+cir). Do it in tranches, gated.

---

## 0. State at handoff (verify with `git log --oneline -6` + `git status`)

- On `develop`, HEAD `e74a9ebf`. Tree CLEAN (only untracked `mir-debug-support.md`
  — NOT ours, never stage).
- **`develop` is 4 commits ahead of origin/develop**, all gated green, **HELD for
  the next `/release`** (user's explicit call — do NOT push raw; push happens via
  `/release`). The 4: `be01f234` (partial_spec), `93af1fb9` (5 lookup-only template
  maps), `8c67e513` (for_each + fn_template_map), `e74a9ebf` (namespace_datatype_map
  + reserve).
- **Rung 1 Step 3 is COMPLETE.** All type/template/namespace lookup maps are on
  `madc::dis::intern_keyed_map`: datatype_map (prior session) + partial_spec_map,
  template_map, template_alias_map, var_template_map, fn_template_decl_map,
  fn_template_instantiated_vars, fn_template_map, namespace_datatype_map. Primitive
  enhancements added this session: `intern_keyed_map::for_each(fn)` (key-recovering
  enumeration, early-exit on true) and `reserve(nvals, max_id)` (pre-size the value
  pool so it doesn't relocate).
- Earlier this session (also on develop, also in the 4... no — these are FURTHER
  back, already released in v0.31.0): tag-arithmetic encoding retirement (complete),
  v0.31.0 release cut + pushed.

## 1. THE TASK — Step 4, done as ONE converged change (lexer-hash IS the str-drop)

The interning is "foundation in, realization not": the lexer already interns every
identifier into `TokenIdent::rec.spelling_id`, but tokens STILL carry a per-token
`std::string str`, so there is no malloc win yet ("no win until str is dropped").
The capstone:

> read identifier → roll `hash_step` per char in the read loop → `intern(data, len,
> hash)` once (skips the redundant `hash_bytes` re-scan) → store `spelling_id` →
> replace `.str` reads with `pgm.spelling(spelling_id)` → drop the `std::string str`
> member.

This is BOTH the user's lexer-hash idea AND the malloc-win capstone — same change.

### The substrate is already built for the hash part (no primitive change needed)
- `include/madcdis/intern_table.h`:
  - `static uint32_t hash_step(uint32_t h, unsigned char c)` — fold ONE char.
  - `static uint32_t hash_bytes(const char*, len)` — loops hash_step (the redundant
    second pass we want to ELIMINATE at lex time).
  - `uint32_t intern(const char *s, uint32_t len, uint32_t h) const` — **already
    takes a precomputed hash.** Use this from the lexer.
  - `uint32_t hash_init()` — the seed (h0). Roll: `h=hash_init(); for each c: h=hash_step(h,c);`
    The header comment GUARANTEES incremental folding == hash_bytes — verify the
    seed/order match `hash_bytes` exactly (hash_init then step per byte in order).
- `intern_keyed_map` has `find(uint32_t id)` (NO hash, O(1)) AND `find(const
  std::string&)` (hashes). Where a call site already has the token's spelling_id,
  prefer `find(id)` to skip the hash entirely.

### Where (lexer.cpp)
- Identifier WORD is accumulated in the main `_getToken` read loop and returned via
  `make_ident(word)` (def at ~2486). The make_ident return sites: ~4085, 4329, 4340,
  4371, 4421, 4593 (grep `make_ident(`). The char-accumulation loop feeding them is
  the isalpha/`_` identifier branch — find it by grepping the loop just above 4085.
  **Roll the hash in THAT loop** (per `source.get()` char).
- spelling_id STAMP sites (where `strpool.intern(str)` currently re-hashes):
  - lexer.cpp ~996-1001 (`if (r.spelling_id==0) r.spelling_id = strpool.intern(((TokenIdent*)tb)->str);`)
  - lexer.cpp ~1032 (`prev->rec.spelling_id = strpool.intern(prev->str);`)
  - lexer.cpp ~5291 (`tb->rec.spelling_id = strpool.intern(((TokenIdent*)tb)->str);`)
  - lexer.cpp ~4593-4594 already interns (`sid`) and stamps — model the plumbing on this.
  Carry the lexer-computed hash to the stamp (store it on the token/rec, or intern
  at read time and stamp spelling_id directly so 996/5291 see it already non-zero).

### The str-drop (the ~499 `.str` surface)
- `TokenIdent::str` (and any `TokenBase`-level str). Add an accessor that returns
  `pgm.spelling(spelling_id)` (returns `const char*`; valid until the next GROWING
  intern — hold the id, not the pointer). Replace `.str` reads in TRANCHES,
  compiler-guided (`-Wall`), fulltest between tranches. Then remove the member.
- CAUTION: some `.str` sites MUTATE/concatenate (e.g. lexer 1024/1029 `prev->str +=
  next->str` token-paste; comment tokens stash text in `.str` at 5324). Interned ids
  are immutable — those mutation sites need a different home (keep a str for comment
  tokens, or re-intern the concatenation). Audit `.str` writes separately from reads.

## 2. GATING (every tranche — non-negotiable)
- `make -C src fulltest` GREEN = **673 / 0 / 0 / 16**. (A madc.h/lexer.h change forces
  a FULL rebuild, ~minutes — background it; you are re-invoked on completion. Don't
  poll-loop; one heavy job at a time.)
- 0 build warnings; census 0; both drift gates GREEN; tag-arith gate 0/0.
- gcc-torture failset BYTE-IDENTICAL to the 51-name baseline
  (`docs/parity/torture-failset-current.txt`): `python3 scripts/run_gcc_testsuite.py
  --std=c17`, extract `^FAIL(` basenames, sort -u, diff vs baseline (must be empty).
- `-O0` is the dev default (src/Makefile). Don't flip to -O2 to chase numbers.
- Commit via `git commit -F -` heredoc; stage files EXPLICITLY; NEVER `git add -A`;
  NEVER stage `mir-debug-support.md`. Co-Authored-By + Claude-Session trailers.

## 3. PERF VERIFICATION (the user cares — measure, don't assert)
- Method that worked: build the pre-change binary in a worktree
  (`git worktree add -d <tmp> <commit>`; `make -C src`), then time before/after parse
  with `bin/madc <flags> --show-stats <file>` (grep "parse time", best of 3) on
  template/namespace-heavy tests: testmap, testtuple, testset, testmadc_ns.
- callgrind culprit list: `scripts/perf_vs_gcc.sh <file>` (auto-callgrinds when
  slower than the gcc baseline). NOTE: a `.mad` file is NOT in the gcc baseline, so
  it prints a SPURIOUS "SLOWER than 2x gcc FAIL" + an "na na" baseline row — IGNORE
  the verdict, USE the callgrind list, and `git checkout docs/parity/perf-baseline.tsv`
  to drop the junk row afterward.
- Baseline numbers BEFORE Step 4 (std::map era was already neutral; intern_keyed_map
  era, HEAD e74a9ebf): testmap parse ~0.909s, testtuple ~0.772s, testset ~0.918s,
  testmadc_ns ~1.219s. callgrind @ e74a9ebf: `intern_table::hash_step` 6.51% +
  `hash_bytes` 4.99% + `intern` 1.03% ≈ **11.5% in hashing** — THIS is what Step 4
  attacks. Step 4 should drop hash_bytes's lex-time share and (str-drop) the
  per-identifier std::string malloc. Re-measure after each sub-step; expect the
  hashing % and malloc to fall.

## 4. LESSONS BANKED (cost real time this session — honor them)
- **`ni`/`nti`/`mi`/`dti` are REUSED for different maps.** A blind transform of
  `X->second` broke a `token_subst` iterator (lexer/parser ~3988) that happened to be
  named `nti`. VERIFY each iterator's declared map before touching it.
- **intern_keyed_map stores values in a VECTOR** (`_vals`), so an outer `operator[]`
  insert can relocate them — a held raw pointer to a value dangles. std::map
  MOVE-construction keeps inner-map iterators valid (nodes transfer), so inner
  `datatype_map_t::iterator`s survive an outer realloc, but OUTER pointers don't.
  Mitigations in place: `reserve()` pre-sizing + re-find-after-insert at the one
  inline-namespace merge site. Keep this in mind if you re-key anything else.
- **Dedicated dense pools** (type_name_pool, template_name_pool, namespace_name_pool)
  keep each map's `_slot` array sized to its small key domain, not the global
  identifier population. strpool is the global one (identifiers) — fine for the
  spelling_id lookups, but a dedicated pool for a small keyed map keeps _slot tight.
- `find(string)` HASHES every call; `find(uint32_t id)` does not. Prefer id where the
  caller already holds the spelling_id (the whole point of Step 4).

## 5. Authoritative references
- `docs/plans/2026-06-23-arena-interning-HANDOFF.md` §2 Step 4 (the original imperative
  sequence; ~480-site estimate). `docs/plans/2026-06-09-frontend-representation-refactor.md`
  (P0..P5 phasing). `docs/plans/2026-06-29-madc-development-substrate-vision.md` (rung 1
  = primitives dogfooded by the compiler; "no parallel implementations" = consolidation
  IS the cleanup; "generalize from real consumers, not a speculative taxonomy" — do NOT
  build unused intern/arena variants).
- Vision: rung 1 is the ONLY thing in flight. Rungs 2-7 (madc::dat drivers, GQL,
  backends, time axis, MCP, federation) are gated behind develop→master parity — do
  NOT start them.
