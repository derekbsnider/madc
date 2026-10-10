# The standard by extension, and madc.ini's `modules` (split arc 1A.4)

Design for Phase 1A step 4 of `2026-10-09-madc-madcide-split-arc.md`. The
owner's rulings (2026-10-09, the arc's decision 8 and "Decided"): `--std` >
file extension > madc.ini `std` > madc; `.c` → C17 until C23 nears the C17
level (ROADMAP 2.13), `.cpp`/`.cc`/`.cxx` → C++17, `.mad` → madc; madc.ini
loads modules as php.ini loads extensions (a repeatable key).

## 1. What exists (2026-10-10)

- The TABLE (1A.1): `madc::file_kind_of_path` (the extension → kind; the
  FAMILY for C / C++) and `madc::file_kind_default_standard` (a family head →
  C17 / C++17 / madc), `src/file_kinds.cpp`.
- The CLI (`src/madc.cpp`): `--std` > madc.ini `std` > madc. The extension
  plays no part: `madc foo.c` compiles in the madc dialect.
- `--project` TUs (`apply_project_tu_options`, `src/madc_cir.cpp`): the TU's
  `-std=` > `.c` → gnu17 (a private `is_c_source_file`, ".c" exactly); a
  `.cpp` TU stays in the madc dialect.
- The editor's document parse (`doc_parse_impl`, `src/ns_madc.cpp`): the
  table's standard, applied STRICT (`c17`, `__STRICT_ANSI__` defined) — a
  third answer for the same file.
- `tests/`: the runner discovers `*.mad` only; the 19 `.c` / `.cpp` files
  there are `--project` TUs (13 C, already gnu17; 6 C++ in
  `testprojectvague` / `testprojectmtiorder`, the madc dialect today) and the
  C-host smoke sources gcc builds.
- No environment variable names a standard (the arc's "madc.ini/env `std`"
  is the ini key; none is added).

## 2. Decisions

1. **One owner: `Program::adopt_file_standard(kind)`** (beside
   `set_language_standard`, `src/parser.cpp`). It reads the one table
   (`file_kind_default_standard`) and adds the one dialect rule: a C or C++
   FAMILY default is the GNU dialect of the standard, as plain `gcc` and
   `g++` default to it — `gnu17` / `gnu++17` (GCC is canon). In C that means
   no `__STRICT_ANSI__` (glibc's feature gates as plain gcc sees them); the
   C++ modes keep the strict presentation, the KNOWN DIVERGENCE recorded at
   `Program::strict_ansi_mode` (until `__float128` / `_FloatN` land). A kind that names a
   standard itself is that standard, strict; a madc kind is the dialect;
   outside the families it sets nothing and answers false.
2. **Precedence at the CLI**: `--std` > the source's extension > madc.ini
   `std` > madc. The ini `std` is still validated on every load (an unknown
   word is an error even when the extension wins), and it is what a source
   with no recognised extension (a shebang script, stdin) and a bare
   `madc -i` get.
3. **Every consumer goes through the owner**: the CLI's source (run, `--exe`,
   `--obj`, `--emit`, freeze, `-i FILE`); a `--project` TU without `-std=`
   (`is_c_source_file` is deleted — a `.cpp` TU becomes gnu++17, as g++
   compiles it); the document parse (a parse handle opened with a family
   head adopts it — the same answer as the CLI for the same file).
4. **`modules`** — madc.ini's repeatable key: each value is a module name as
   `-l` takes it (`modules = madcgit`), opened by the same opener
   (`madc_module_open`) after the command line's `-l` libraries, a name
   already named skipped; in an AOT build it joins the link line as `-l`
   does. CLI and ini are additive (a module is a capability, not a setting
   to override).
5. **`theme`** lands with its consumer, prompt syntax colour (1B.9): the
   strict reader accepts a key only when something reads it.
6. **The forest packs pin `--std=madc`** (`scripts/forest_pack*.sh`): their
   TU is a `.cpp`, and without the pin the embedded corpus's producer config
   would turn gnu++17 — every madc-dialect compile would then miss the exact
   bind. The madc.ini gate (`scripts/forest_config_gate.sh`) runs its ini
   `std` legs on a source with no extension and gains the extension legs.
7. **A consequence for the owner** — a size trade: a `.c` / `.cpp` source
   now compiles under gnu17 / gnu++17, which the packed corpus (the madc
   dialect's) does not match, so it falls through to a live parse with no
   notice — what a `--std=c17` compile always did (the multi-dialect
   contract, `Program::forest_missing_fallback`). Before, such a source bound
   the corpus because it compiled, wrongly, as madc. A C or C++ profile in
   the pack would restore the bind at the cost of image size — the owner's
   call (memory: >2x product-dimension trades are owner decisions).

## 3. Commits

1. The owner + its three consumers, with reducers `tests/teststd_ext_c.c`
   and `tests/teststd_ext_cpp.cpp` (oracle: `gcc` / `g++` with no `-std`:
   `__STDC_VERSION__` 201710L with `__STRICT_ANSI__` undefined and a C++
   keyword usable as a C identifier; `__cplusplus` 201703L with C++17-only
   constructs) and a unit test of the owner; `testparsehandle_std` gains the
   family heads. The C++ TUs among the 19 (`testprojectvague`,
   `testprojectmtiorder`) re-measured under gnu++17. (A C++17 structured
   binding is refused in every mode — `BUGS.md` B38, widened; a parser
   feature for its own session.)
2. `modules`: the reader's unit test (`tests/unit/test_config_file.cpp`)
   and legs in the madc.ini gate — a fixture library the program links only
   when the ini names it, and named once when `-l` names it too (the suite
   runner passes `--no-config`, so a suite test cannot name an ini).
3. Docs (the CLI reference, `docs/build.md`'s madc.ini section) + mirrors.

## 4. Thread safety

`adopt_file_standard` writes its own Program's state (one Program per
thread, the existing contract); the table is constant. The config struct is
read once by the CLI before anything runs.
