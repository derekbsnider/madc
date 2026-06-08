# MADC — CODEX INTEGRATION + SMAUG REVIVAL HANDOFF (2026-06-08)

> **READ-FIRST for the CURRENT situation.** This is the live state after
> integrating Codex's 2.5h real-header advancement and starting a careful SMAUG
> revival. For the prior `<type_traits>`/template/enum arc (the madc feature
> history, code anchors, methodology), the companion doc
> `docs/plans/2026-06-08-FULL-rehydration-handoff.md` is still valid. This doc
> supersedes it for *what's active right now and what to do next*.

---

## 0. ONE-SCREEN STATE

- **Repo:** `/workspace/madc`. **Branch:** `feature/realhdr-parse-gaps2-claude`.
  **HEAD:** `ecc92e4` (Codex's advancement, committed + active `bin/madc`).
  Working tree clean. **NOT pushed. develop untouched. Do NOT promote.**
- **Codex's madc work is IN and triple-preserved:** committed as `ecc92e4`,
  plus `git stash@{0}` ("codex-header-parse-WIP-2026-06-08"), plus branch
  `codex-header-parse-eval` (`72f9a64`). The user explicitly wanted this kept —
  **do NOT pull back to the pre-Codex `1eb5cd4`** (already tried; user vetoed it).
- **MIR pin `2ffebff`** (unchanged).
- **madc gates (clean-build verified on the byte-identical `72f9a64`):**
  fulltest `534 passed / 4 failed / 0 timed / 26 skipped` (the 4 are pre-existing:
  testdefer, testfstream, testlargesizeofquery, testloop); gcc.c-torture
  `1566 / 31 / 57 / 1` = baseline; my six capability tests still pass.
- **SMAUG: NOT currently booting from a fresh compile** with *either* binary.
  This is the active problem (see §3).

---

## 1. WHAT CODEX ADDED (committed as `ecc92e4`, authored by the Codex agent)

Substantive, additive C++ feature work on top of the `<type_traits>` arc; it
**preserved** all prior work (A1 keystone, A2-rest, anon-enums, const-fold —
my six capability tests still pass). Files: `include/{datadef,madc,tokens}.h`,
`src/{parser,cir_builder,lexer,pch}.cpp` (~992 insertions, parser.cpp +1097).

Features (from the diff):
- **Variadic template parameter packs** — `TemplateDef`/`TemplateAliasDef` gain
  `typeparam_is_pack`.
- **Inline namespaces** (`inline_namespace_children`) + **namespaced alias
  templates** — `template_alias_map` went single → per-namespace **vector**;
  `instantiate_template_alias_use`/`find_template_alias`/`register_template_alias`
  gained a namespace hint.
- **`friend` declarations** — `DataDefCLASS::friend_class_names`.
- **C++17 `if`-with-initializer / declaration-in-condition** —
  `TokenIF::condition_decl` + `CirBuilder::translate_if` emits the init-decl + an
  `N_IF` wrapped in a block.
- **`max_align_t`** builtin type — `ddMAX_ALIGN_T` (datatype_map + pch spelling).
- **Functional cast in constant context** — `try_parse_constant_functional_cast`.
- `parsePostfixChainFrom` helper.

**Verified effects (clean build):**
- Real **system** `<string>`/`<iostream>`/`<sstream>`/`<fstream>` parse via
  `--std=c++17 --no-embedded-headers --emit=c11`.
- Embedded-header survey 11 → **13** parse (`<tuple>`, `<string_view>`, `<array>`
  newly OK; `<functional>` advances). Current still-FAIL set:
  `<ostream>`/`<istream>` (`less`), `<memory>` ("member name in class
  definition"), `<algorithm>` (string in fn-ptr typedef), `<functional>`
  ("Unsupported parenthesized member declarator"), `<bitset>` ("basic_string<>
  expects 3 args got 1").
- The prior handoff's §7 blocker is **RESOLVED**: the numeric_traits namespace
  reducer `std::__are_same<float,float>::__value` now folds to **`26 55`**
  (correct vs g++). Quick repro (default mode):
  ```
  namespace nn { template<typename,typename> struct are_same{enum{value=0};};
                 template<typename T> struct are_same<T,T>{enum{value=1};}; }
  template<typename V> struct nt{ static const int md=(2+(nn::are_same<V,float>::value?24:53)); };
  // printf nt<float>::md, nt<double>::md  -> 26 55
  ```

**Caveat:** Codex skipped the **torture and SMAUG gates** in its own run, and
also edited `SMAUG.mad` (see §2). The madc changes themselves pass torture
(verified by me on `72f9a64`). SMAUG is a separate problem (§3).

---

## 2. SMAUG.mad WAS EDITED BY CODEX — REVERTED

Codex modified `/workspace/MadSMAUG/src/SMAUG.mad`: it **deleted the working
instrumented `main()`** (replacing it with a comment "entry point is comm.c's own
main()") and **commented out `#include "upstream_src/services.c"`**. That broke
SMAUG independently of any madc binary.

- **SMAUG.mad is now RESTORED** to its committed version (`git checkout` in the
  MadSMAUG repo). **Codex's edited version is preserved at
  `/tmp/SMAUG.mad.codex-version`** (decide later if any of it — e.g. the
  services.c dedup rationale — is worth keeping).
- **The SMAUG C sources are NOT broken** (careful diff vs the pristine tarball
  `upstream/smaug1.8.tgz`):
  - `db.c` = **byte-identical** to pristine (the user's suspicion was unfounded;
    the May 30 mtime is just extraction).
  - Entire `upstream/smaug1.8/src` tree pristine **except** `comm.c` (2 lines =
    `patches/smaug-echo-color-prompt.patch`) and `imc.c` (4 lines) — both Apr 30,
    long-standing and expected.
  - The sources live in `upstream/smaug1.8/` (extracted from the tarball, **not
    git-tracked**); `src/upstream_src` → symlink to `upstream/smaug1.8/src`.
- Untracked scratch debris from Codex's session litters `MadSMAUG/src/`
  (`*.mir`, `*.err`, `*.out`, `a.bmir`, ~44 files) — harmless, can be cleaned.

---

## 3. THE SMAUG REVIVAL — CURRENT BLOCKER + THE CACHING TRUTH

**Fresh-compile reality (single runs, no loops):**
- **master `/usr/local/bin/madc`** + original SMAUG.mad → fails at
  `Expecting member name in struct definition` (an **anonymous-enum-in-struct** —
  a construct master never supported; my f6ef0a8/ecc92e4 fix added it).
- **feature `ecc92e4`** + original SMAUG.mad → gets **further**, fails at
  `SMAUG.mad:1261:18: Expecting type name after elaborated type specifier`.

**=> Neither binary compiles SMAUG from a fresh source compile right now.** This
confirms the user's suspicion: the earlier "Realms of Despair ready…" runs this
session were almost certainly running **cached `.mir` objects**, NOT a fresh
recompile. Treat any past "SMAUG ready" as UNVERIFIED until a clean from-source
compile is achieved. (Note: `MadSMAUG.sh` does invoke `madc` fresh each run —
`exec "$MADC" "$REPO/src/SMAUG.mad" "$@"`, line 109 — and modified-vs-original
SMAUG.mad gave different errors, proving the *umbrella* is reparsed; the caching,
if any, would be in a separate `.mir`/object reuse layer worth confirming.)

**THE ACTIVE BLOCKER to fix (forward, on the Codex base — do NOT revert madc):**
`Expecting type name after elaborated type specifier` — thrown at
`src/parser.cpp:3134` (the `tkCLASS`/`tkSTRUCT`/`tkUNION`/elaborated-type arm:
fires when the token after `struct`/`union`/`enum`/`class` is NOT a contextual
identifier). The reported coordinate `SMAUG.mad:1261` is **bogus** (SMAUG.mad is
an ~80-line umbrella; madc mis-attributes included-file lines to the umbrella —
the known included-header coordinate bug). The caret was on a `}`.

### 3.1 CRITICAL diagnostic correction (user-flagged): use the proper `--std=`
- `MadSMAUG.sh` runs `madc SMAUG.mad` with **no `--std=`** (default STD_MADC).
  **Determine and use the proper `--std=` for SMAUG** when reproducing — the user
  explicitly flagged this. SMAUG is C89; the right invocation likely needs an
  explicit C std (e.g. `--std=c89`/`gnu89`) OR the default-mode path that
  MadSMAUG.sh uses — confirm which before trusting any reducer.
- **The `-E` two-step bisection MISLEADS here.** `madc -E SMAUG.mad` →
  reparse-the-flat-file gave a FALSE `use of undeclared identifier 'stderr'`
  (the embedded `stdio.h` globals like `stderr`/`stdout` are lazy-registered and
  are NOT re-triggered on a flat reparse, so they appear undeclared). Do NOT use
  the flat `/tmp/smaug_pp.cpp` reparse to locate the real construct — it's an
  artifact. (`/tmp/smaug_pp.cpp` exists, 147k lines, from this session — ignore
  the 'stderr' error at line 6375; it is NOT the real blocker.)

### 3.2 How to find the REAL construct behind the 1261 error (next session)
Options, in order of reliability:
1. **Fix the coordinate attribution first** (best long-term): the elaborated-type
   throw site (parser.cpp:3134) has the failing token `tn` — make madc print the
   token's *real* file/line/col (tokens carry it; the umbrella misattribution is
   the bug). Even a temporary `DBG(std::cerr << tn->file << ':' << tn->line)` at
   3134 gives the true `upstream_src/<file>.c:line` immediately.
2. Or bisect by `#include`: SMAUG.mad `#include`s the `upstream_src/*.c` list in
   order; binary-search which include first triggers 3134 by commenting includes
   (on a COPY, never the committed SMAUG.mad).
3. Then read the real construct. "elaborated type specifier" = `struct/enum/union
   /class X` where `X` isn't seen as an identifier. Likely candidates after
   Codex's changes: a name that now tokenizes as a keyword/type (e.g. did
   `max_align_t` or a new reserved word collide with a SMAUG identifier used after
   `struct`/`enum`?), or a struct/enum tag that Codex's struct/enum/alias rework
   no longer accepts. **This MAY be a Codex regression** (it touched struct/enum/
   template/alias heavily) — fix it at the deepest layer on the Codex base.

### 3.3 Validation discipline for the SMAUG work
- **Single runs, NO loops** (user instruction, repeated). Use a `timeout` +
  `run_in_background`, read the log ONCE on completion. Never poll with
  `until/sleep` against a SMAUG boot.
- **Random free port** (`P=$((6000+RANDOM%800))`); "Address already in use" is a
  shared-box collision, not a madc bug.
- To get a TRUE from-source compile, ensure no cached `.mir`/objects are reused —
  inspect `MadSMAUG.sh` §"Set up the runtime data tree" + any object/`.mir`
  caching, and clear it for the test (the untracked `*.mir` in src/ are suspect).

---

## 4. EXACT COMMANDS (current, correct)

```bash
# madc state
cd /workspace/madc
git rev-parse --short HEAD            # ecc92e4
git branch --show-current             # feature/realhdr-parse-gaps2-claude
git status --short                    # clean
git stash list | head -1              # stash@{0} codex-header-parse-WIP-2026-06-08
git branch | grep codex               # codex-header-parse-eval (72f9a64) preserved

# build (NAS mtime trap: touch before make when iterating one file; clean-rebuild
# when results look impossible)
make -C src clean && make -C src 2>&1 | grep -iE ': error|: warning'

# madc gates
make -C src fulltest 2>&1 | grep -E "passed,|FAIL: tests" | tail -8   # 534/4
python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc  # 1566/31/57/1 (run ALONE)

# capability tests (all match g++)
for t in testpartialspec testtypetraits teststaticconstmember testtemplateidvalue \
         teststaticconstsibling testanonenum; do echo "$t:"; bin/madc tests/$t.mad; done

# header survey
for H in tuple string_view memory ostream functional array bitset; do \
  printf '#include <%s>\nint main(){return 0;}\n' "$H" > tmp/_h.mad; \
  bin/madc --std=c++17 --emit=c11 tmp/_h.mad 2>&1 >/dev/null | grep -m1 error: \
  && echo "  ^ <$H>" || echo "OK <$H>"; done

# SMAUG (SINGLE run, no loop; USE THE PROPER --std — TBD, see §3.1):
cd /workspace/MadSMAUG
git status --short src/SMAUG.mad      # blank = committed/original (restored)
# MADC=/workspace/madc/bin/madc MADC_CPU_LIMIT=0 MADC_MEM_LIMIT=0 \
#   timeout 300 ./MadSMAUG.sh $((6000+RANDOM%800)) > /tmp/smaug.log 2>&1   # run_in_background, read once
```

---

## 5. KEY ANCHORS (HEAD ecc92e4 — line numbers shifted from prior handoff by Codex's +1097)
Re-grep these on resume (Codex's diff moved everything; use symbol search, not raw lines):
- Elaborated-type throw (the SMAUG blocker): `grep -n "Expecting type name after elaborated type specifier" src/parser.cpp` (was 3134).
- Codex's new symbols: `typeparam_is_pack`, `inline_namespace_children`,
  `friend_class_names`, `condition_decl`, `try_parse_constant_functional_cast`,
  `find_template_alias`, `register_template_alias`, `parsePostfixChainFrom`,
  `ddMAX_ALIGN_T`.
- My prior-arc symbols (preserved): `fold_constant_qualified_member`,
  `resolve_current_class_static_member_const_value`,
  `capture_constant_initializer_value`, `match_partial_specialization`,
  `evaluate_type_trait`, "Template-id in expression context".
- The two `goto redo_expression_token` call sites (A1) in `parseExpression`.

---

## 6. IMMEDIATE NEXT STEPS (priority order)
1. **Find the proper `--std=` for SMAUG** and reproduce the compile correctly
   (§3.1). Don't trust the `-E` flat reparse (stderr artifact).
2. **Get the REAL file:line for the 1261 elaborated-type error** (§3.2 option 1:
   temporary DBG of `tn`'s file/line at parser.cpp:3134).
3. **Read the real construct**, decide if it's a Codex regression or a genuine new
   SMAUG gap, fix at the deepest layer **on the Codex base** (`ecc92e4`) — never
   revert madc to pre-Codex.
4. **Confirm a TRUE from-source SMAUG compile** (clear any `.mir`/object cache;
   single run; proper `--std`). Only then is "SMAUG works again" real.
5. Re-gate (fulltest + torture ALONE + the fresh SMAUG compile) and commit the fix.
6. Decide whether any of Codex's SMAUG.mad changes (`/tmp/SMAUG.mad.codex-version`
   — services.c dedup) are worth re-applying deliberately.

## 7. PROCESS LESSONS (cost time this session)
- **NAS mtime staleness:** `make` can report "up to date" and skip recompiling an
  edited file → stale `bin/madc`. `touch src/<f>.cpp` before `make`; clean-rebuild
  when results look impossible.
- **SMAUG object/`.mir` caching:** "ready" can be a cached artifact — insist on a
  fresh from-source compile.
- **`-E` flat-reparse misleads on SMAUG:** lazy-registered embedded globals
  (`stderr` etc.) appear undeclared on a flat reparse. Diagnose via real
  token file/line, not the flat file.
- **Single runs, no loops** for SMAUG (user, repeated). **Do not revert to
  pre-Codex madc** (user, repeated). **Use the proper `--std=` for SMAUG** (user).
- **Never lose code:** Codex's work is committed + stashed + on a branch.

END OF HANDOFF.
