# Script mode — PHP-style top-level statements with a synthesized `main()`

**Status:** PLAN (2026-07-21). Owner request: madc should support a
non-C-like script mode where a program needs no `main()` and statements can
appear at file scope, PHP-style:

```mad
#include <stdio.h>
int greetings = 3;
for (int i = 0; i < greetings; i++)
    puts("Hello World");
```

Owner-confirmed shape: **wrap the non-declaration top-level statements into a
synthesized `main()` when no user `main` is provided.** (This is what PHP
itself does — Zend compiles top-level code into a pseudo-main op array.)

## Why this is cheap here

The deep plumbing already exists; this is a parser-surface feature:

- **Ordered file-scope execution is live machinery.** The CIR builder already
  collects file-scope dynamic initializers into `m_global_ctor_stmts` in
  source order, synthesizes `__madc_global_init` (`cir_builder.cpp` — one
  home for global ctors), injects its call at the top of `main`, and wires it
  to `DT_INIT` for `-shared`.
- **`main`'s shape is already synthesizable.** The `void main → int main`
  lowering (task #85) established the CIR builder as the owner of main's C
  signature.
- **Statement parsing is done** — it is the same statement grammar used inside
  function bodies, including the statement-vs-declaration disambiguation that
  block scope already performs.
- Because everything resolves before c2mir (Tier-1 lowering — the synthesized
  main is ordinary C11), the JIT lane, the `--exe`/native AOT lane, and
  `--emit=c11` output all work with no backend or fork changes. Emitted C11
  shows a real `int main` containing the script body.

## Semantics

1. **Trigger:** a TU contains at least one file-scope statement (a token
   sequence that does not start a declaration). Declarations at file scope
   keep their normal meaning.
2. **Synthesis:** if the TU has no user `main`, the collected statements
   become the body of a synthesized `int main(int argc, char **argv)`, in
   source order. `argc`/`argv` are in scope for scripts.
3. **Conflict:** file-scope statements AND an explicit user `main` in the same
   program is a **hard compile error** ("top-level statements conflict with an
   explicit main"). No PHP-style include-time execution phase.
4. **Top-level variables are globals** (visible to functions defined in the
   file, live for the program). No PHP `global` keyword needed.
5. **`return expr;` at top level** returns from the synthesized main (exit
   status), as in PHP.
6. **Initialization order:** dynamic global initializers run first (the
   existing `__madc_global_init` call at the top of main), then the top-level
   statements in source order. This matches the C++ mental model (globals
   initialized before main) and keeps the first slice simple. Full
   source-order *interleaving* of dynamic initializers with statements is a
   possible later refinement if real scripts need it — decide then, don't
   pre-build it.
7. **Headers stay declaration-only.** A file-scope statement in an
   `#include`d file is an error at the include site.
8. **Multi-TU (`--project`):** exactly one TU may be a script (it provides
   main); a second script TU is an error. First slice may simply restrict
   script mode to single-TU compiles.

## Dialect gating (vision invariants I1–I8)

Script mode is a **dialect feature, not a hardcoded mode**: active in
`STD_MADC` (madc *is* the scripting dialect), inactive under explicit
`--std=c*` / `--std=c++*`, where a file-scope statement stays the standard
error. When the P2.11 keyword/feature registry lands, this registers there
like any other gated feature; until then the `language_std == STD_MADC` check
is the gate, placed in the deepest shared helper per the std-enum gatekeeping
rule.

## Implementation slices

**S1 — parser: file-scope statement dispatch.** In the top-level parse loop,
when the next tokens do not start a declaration (reuse the block-scope
statement-vs-declaration decider — do NOT write a second disambiguator), parse
a statement and append it to a per-program ordered list of top-level
statements. Gate on `STD_MADC`. Error paths: statement under `--std=c*`,
statement inside an included header.

**S2 — CIR builder: main synthesis.** At end of TU, if top-level statements
exist and no user `main` was defined, build `int main(int argc, char **argv)`
whose body is the statement list, then let the existing machinery do the rest
(`__madc_global_init` injection, implicit `return 0` via the void-main/C11
path). If a user `main` exists alongside top-level statements, emit the hard
error (pre-c2mir, with file/line of the first offending statement).

**S3 — surface checks.** `--emit=c11` renders the synthesized main as plain
C11 (verify by eye + oracle test). `--exe` native lane and `-c`/`-o` AOT work
unchanged. `--dump-cir` shows the synthesized function like any other.

**S4 — tests.** Integration tests + fixtures per `test-fixtures.md`
conventions, e.g.:
- `testscriptmode.mad` — statements + declarations interleaved, functions
  called from top level, `.expect`.
- `testscriptargv.mad` — reads `argc`/`argv`, `.argv` fixture.
- `testscriptreturn.mad` — top-level `return` sets exit status.
- `testscriptmainconflict.mad` — statements + explicit main, `.expect_err`.
- `testscriptstd.mad` — same source under `--std=c17` via `.flags`,
  `.expect_err`.

## Non-goals

- REPL / `eval` (mode-4) — separate deferred track; this plan doesn't touch
  `eval_unit`.
- PHP `global` import semantics, include-time execution, or `<?php`-style
  open/close tags.
- Interleaved dynamic-initializer/statement ordering (see Semantics #6).

## Risks

- File-scope statement/declaration disambiguation corner cases (`T * x;`
  style). Mitigation: reuse the block-scope decider verbatim; any divergence
  between the two is itself a bug to fix in the shared helper.
- Grammar ambiguity with existing top-level error recovery — make sure a
  genuine syntax error in a declaration doesn't silently reparse as a
  "statement" and produce a worse diagnostic. The statement path should
  engage only when the declaration decider says "not a declaration", not as
  an error fallback.

## Validation

`make -C src fulltest` (JIT + packed arbiter) and
`bash scripts/run_tests.sh --exe` (the synthesized main must be native-clean),
per `testing-fulltest.md`.

## Results (2026-07-22, feature/script-mode-claude) — LANDED

The feature came in cheaper than planned: file-scope statements ALREADY
parsed through the same `parseStatement` dispatch block scope uses (the
owner example failed only at `main() not found`), so S1 needed no second
disambiguator — only classification and gates.

**What landed (S1+S2 merged, both in the parser):**

- **Pre-parse starter** (`file_scope_statement_starter`): arms script
  parsing for starts that can never begin a declaration — statement
  keywords, literals, `(`/prefix operators, identifier followed by an
  expression continuation (`=`, `(`, `[`, `++`, `--`, `.`, `->`,
  compound-assigns, `:` label). `knr_supported()` is false in STD_MADC, so
  `name(` never collides with C89 implicit-int definitions. Include-origin
  and non-madc dialects never arm.
- **Post-parse result classifier** (`script_statement_result`): positive
  list (statement keywords, labels, call/operator/subscript/member
  expression results). Unknown result kinds keep the pre-script behavior —
  under explicit C standards, expression-shaped results stay ignored (C89
  implicit-declaration side effects depend on this); only control-flow
  keyword results become the standard "expected a declaration" error.
- **Adoption** (`adopt_script_statement`): dialect gate (STD_MADC), header
  gate (statement in an `#include`d file = error at its own location),
  then append to the synthesized main in source order.
- **Synthesized main** (`ensure_script_main`, lazily at the first adopted
  statement): a real `TokenFunc int main(int argc, char **argv)` built with
  parseFunction's exact registration shape (FuncDef + funcdef_map + global
  name Variable + Method + param Variables), finalized into ast/
  pending_funcs at end of parse. Every downstream surface (CIR builder,
  `--emit=c11`, `--dump-cir`, `-o` native ELF, `-c`) sees an ordinary main
  — zero backend changes; the `__madc_global_init` call injection fires
  automatically (semantics #6 for free).
- **argc/argv resolution**: `findVariable`'s miss path consults
  `script_param_lookup`, armed ONLY while a top-level script statement is
  parsing — never inside function bodies or declaration initializers.
- **Conflict checks** (semantics #3): main-declared-before-statements
  errors at the first statement; main-declared-after errors at
  parseFunction entry, citing the first statement's location.

**Two pre-existing gaps the feature surfaced, fixed at the deep layer:**

1. **C++ dynamic global initialization** (cir_builder): a file-scope
   scalar initializer that reads a variable or calls a function is not a
   C11 constant; madc emitted it into the SPEC_DECL and c2mir rejected it
   (`int scaled = base * 2;` failed even in normal C++ mode). Now
   `var_decl` classifies the translated initializer (conservative walk:
   `N_CALL`/value-context `N_ID` ⇒ dynamic; address formation, sizeof,
   casts of constants stay static), emits bare storage, and
   `collect_global_ctors` queues the full source assignment into
   `__madc_global_init` in declaration order — the g++ model. C modes keep
   the standard constant-only diagnosis.
2. **Exit status** (madc.cpp): main's return value now propagates as the
   process exit status in the JIT lane (was squashed to 0); gcc parity for
   `./prog; echo $?`. No suite test depended on the old squash.

**Tests (S4):** testscriptmode (statements+declarations interleaved,
functions called from top level, dynamic global init), testscriptargv
(`.argv`), testscriptreturn (top-level return; post-return statement
writes stderr + `.expect_quiet` proves early exit; nonzero status verified
by hand: 2 args → exit 2), testscriptmainconflict (`.expect_err`),
testscriptstd (`--std=c17` `.flags` + `.expect_err`).

**Legacy fallout:** `tests/test.mad` / `tests/test5.mad` (the original
demo files) carried a file-scope `hello = ...;` beside an explicit main —
dead code pre-script-mode (the CIR backend never walked top-level
statements), now the semantics-#3 hard error. The dead line was removed
from both; their behavior is byte-identical.

**Validation:** fulltest 734/0/0/13 (was 729 + the 5 new tests); --exe
lane green (see status mirror for the count); owner example, `--emit=c11`
(bare global + `__madc_global_init` assignment + synthesized main),
native ELF run, header gate, and exit status all verified by hand.

**Known S1 corners (documented, not blocking):** a statement BEGINNING
with a qualified name (`std::cout << argv[0];`) is classified by result
(tier B), so argc/argv are not in scope during its parse — statements
beginning with keywords, literals, or plain identifiers (the PHP-style
norm) have them. Widening the starter to qualified names is a follow-on.
A `--freeze` of a script TU is untested (scripts are never packed as
headers).
