# Handoff — real-header parsing: remaining gaps are PREPROCESSOR, not parser

Date: 2026-06-06. Branch to resume on: `feature/realhdr-parse-gaps2-claude`
(off `develop` @ `f98afa9`). Tool: `scripts/probe_real_headers.sh`.

## TL;DR

The preprocessor *environment* (`-D`, configure C++ include paths, predefined
macros) is on `develop`, and two parser gaps are fixed there (brace-init `T x{}`,
opaque enum `enum class E:T;`). **Re-measurement shows the C++ PARSER now handles
essentially the whole std:: surface** — `scripts/probe_real_headers.sh` reports the
gcc-preprocessed ("pp") column as **OK** for type_traits, utility, memory, string,
string_view, iosfwd, ostream, istream, iostream, sstream, fstream, vector, map, set,
algorithm. **So the remaining failures are in madc's own PREPROCESSING, not the
parser.** This was the original task's real shape, finally pinned.

Run `bash scripts/probe_real_headers.sh` to see the live two-column status (the
"madc" column = madc's own preprocess+parse; "pp" = gcc-preprocessed, parser-only).

## The remaining gaps (worklist)

1. **`#include_next` unsupported** — `string`, `algorithm` fail
   `Failed to open include file: /usr/include/c++/13/next <stdlib.h>`. madc reads
   the `next` keyword of `#include_next <stdlib.h>` as part of the path. This is a
   concrete, isolated **lexer/preprocessor** fix (handle the `#include_next`
   directive — search the include path list *after* the directory of the current
   header). Likely unblocks string/algorithm immediately. **Start here.**

2. **`_GLIBCXX_*` macro expansion divergence** — the streams family (char_traits,
   string_view, iosfwd, ostream, istream, iostream, sstream, fstream) fails
   `Expecting type after 'typedef'`; vector/memory fail `Too many parameters`;
   map/set fail `Expecting type in class definition`. All are `pp`=OK, so madc
   mis-expands some macro, producing a broken typedef / parameter list / class body.
   Method: compare madc's preprocessing of the header to `gcc -E` and find the
   divergent macro. (madc has `--emit=c11`/`--dump-cir` post-parse, but you may
   need a pre-parse token dump or a reducing search to localize the macro. The
   earliest known suspect is around the `_GLIBCXX_BEGIN_NAMESPACE_*` /
   visibility-attribute macros.) Likely one or a few root macros unblock many
   headers.

3. **`__gnu_cxx::char_traits` name resolution** (char_traits) — the ONE genuine
   non-preprocessor gap: fails `'char_traits' is not a member of namespace
   '__gnu_cxx'` even in the "pp" column. A namespace-member lookup / using-decl
   resolution issue, independent of (1)/(2).

## Method / tools

- `scripts/probe_real_headers.sh [header...]` — the burn-down instrument. The
  two-column output is the diagnosis: `madc` fails + `pp` OK ⇒ preprocessor gap;
  both fail at the same construct ⇒ parser gap; `pp` fail only ⇒ semantic.
- Isolation technique: `g++ -std=c++17 -E <hdr> | grep -v '^#' | bin/madc --std=c++17 --emit=c11 -`
  (madc chokes on `# line` markers — token "type 7" — so strip them).
- Re-measure after every fix; the worklist should shrink to OK.

## State / done (all on `develop` @ `f98afa9`, pushed)

- Preprocessor env: `-D` (1bd2edf), configure C++ paths (738746d), predefined
  macros (d6fc5cb). Generated `src/sys_include_paths.cpp` + `src/predefined_macros.cpp`
  are gitignored, regenerated each build (`make clean` refreshes).
- Parser gaps: brace-init `T x{}` (f5f3dcd), opaque enum (f98afa9). Tests:
  `testdefineflag`, `testpredefmacros`, `testbraceinit`, `testopaqueenum`.
- fulltest baseline: **520 passed / 4 failed / 0 timed out / 26 skipped**
  (reds: testdefer, testfstream, testlargesizeofquery, testloop — all pre-existing).
- KG: `Gap{real_header_parse_worklist}`, `Decision{real_header_pch_pipeline}`.

## After the worklist clears

Resume the pipeline (docs/plans/2026-06-06-real-header-pch-pipeline.md): per-file
`.pchz` capture of the (now-parseable) closures → `#embed` archive → flip
`#include` resolution to prefer the PCH → retire the curated std:: stubs. The 3
`ofstream` reds + the curated `<string>`/`<vector>` fall out at the flip.
