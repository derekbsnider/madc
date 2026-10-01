# madc REPL + Thonny-style madcide — revised proposal

**Date:** 2026-09-24  
**Status:** research + design proposal; does not authorize code  
**Scope:** (1) the REPL facilities that belong in `madc` / `libmadc`, and (2) the madcide features that consume those facilities to provide a beginner-friendly, Thonny-like C/C++ environment.  
**Code cross-reference:** checked against `develop` @ `3c5ae4344` on 2026-09-24. The notes marked **Code check** below correct the plan where the code disagrees; the full evidence and the eval-vs-REPL split are in Part F (§39–§41). **The owner's decisions of 2026-09-25 (§42) supersede the plan text they name.**

---

## 0. Executive summary

The previous plan had the right architectural foundation, but it put too much product emphasis on the stepper/debugger and not enough on the interactive programming experience itself.

The revised product direction is:

> **Make C, C++, and the madc dialect feel as immediate and exploratory as Julia/IPython, while keeping real C/C++ semantics visible and available. Put that REPL inside a deliberately simple, Thonny-like madcide.**

The priority order should be:

1. **Julia / IPython — primary REPL user experience.** These define what modern developers already understand: persistent sessions, bare-expression display, completion, history, help/introspection, source loading, recoverable errors, and a small set of interactive commands.
2. **Thonny — primary IDE experience.** The editor and REPL are two views onto the same live programming session; the initial UI is intentionally small, zero-setup, and teaching-oriented.
3. **Clang-Repl / Cling — C/C++ semantic and implementation precedent.** They prove incremental C++ compilation, automatic expression display, undo, library loading, and declaration shadowing are feasible. Their user-facing UX should not be the primary model.
4. **Existing madc architecture — hard implementation constraints.** One MC11 IR, one semantic model, existing live parse/session machinery, event journal, views, `madc::eval_*`, MIR interpreter/JIT, and data-driven command/key registries must remain the basis.

The command-line REPL is important in its own right, but its main strategic value is that it defines a **single reusable interactive-session contract** that madcide can present graphically. madcide must not implement a second REPL.

The old BASIC-style stored-program idea is explicitly deferred from this proposal. The deferral covers BASIC's *vocabulary* (`LIST`, `RUN`, line-numbered program entry), not the functionality: editing a file buffer at the prompt with ed/ex commands is in scope (D24, owner 2026-09-25).

---

## 1. Corrections and refinements to the 2026-09-21 plan

The existing `docs/plans/2026-09-21-repl-and-teaching-ide.md` remains valuable, especially its inventory of already-built substrate, but several statements should be revised after further research.

### 1.1 C/C++ do have real REPL implementations

The old plan says Cling / Clang-Repl "fake persistence by rebuilding a translation unit per line." That is not an accurate description of current Clang-Repl or Cling.

- **Clang-Repl** uses Clang's incremental parser and compiler infrastructure, incrementally produces AST/LLVM IR, and JIT-executes it. It has `%undo`, `%lib`, `%help`, and automatic value synthesis for expressions without a trailing semicolon.
- **Cling/ROOT** maintains a persistent interactive C++ environment, command history, completion, meta-commands, file/library loading and unloading, state inspection, and expression result printing. ROOT's interpreter mode also supports declaration shadowing so variables, functions, and classes can be redefined interactively.

The actual gap is therefore not "C++ has no REPL." It is:

> **C/C++ lacks a widely adopted REPL with the polish and development ergonomics users expect from Julia/IPython, and lacks a beginner IDE that makes that REPL the natural companion to source editing.**

### 1.2 The stepper is important, but it is not the first product pillar

The previous plan made expression-level stepping the most prominent differentiator. It remains a strong later teaching feature, but the first-order user experience should be:

1. easy to start;
2. easy to try code;
3. easy to inspect what C/C++ thinks;
4. easy to move between source and REPL;
5. only then, unusually good stepping/memory visualization.

A beginner should find madcide useful and pleasant before ever pressing Debug.

### 1.3 Thonny's simple front door should arrive earlier

The prior plan left the "teaching front door" until Stage 5. That is too late. A minimal Thonny-like editor + REPL experience should ship as soon as the core REPL is trustworthy enough to use.

### 1.4 Keep the old architectural decisions

The following parts of the existing plan are still strong and should remain:

- REPL and stepper consume the existing MC11 IR; no parallel compiler tree.
- MIR interpreter and JIT are two executors of the same semantics.
- Error-tolerant parsing is a prerequisite for a pleasant REPL.
- Automatic result display uses the established value carrier direction rather than inventing another public value system.
- The REPL is a session client/service, not a disconnected side program.
- madcide owns presentation; engine/compiler logic stays in madc/libmadc.
- Every discriminator is an enum, not a stringly typed protocol.
- Every new engine capability states a thread-safety contract.

---

## 2. Research findings: the modern REPL vocabulary

### 2.1 Julia — best primary interaction model

Julia 1.12's REPL provides a particularly coherent model for a JIT-compiled language:

- persistent interactive environment;
- a bare complete expression evaluates immediately;
- the last expression's value is displayed automatically;
- a trailing semicolon suppresses display;
- the last result is available as `ans`;
- parser-aware multiline entry;
- searchable persistent history (`Ctrl-R` / `Ctrl-S`);
- tab completion for language names, members, paths and context-sensitive cases;
- dedicated help mode entered with `?`;
- dedicated shell mode entered with `;` at an empty prompt;
- package mode entered with `]`;
- prompt paste, so copied REPL transcripts can be pasted back intelligently;
- configurable keybindings;
- integration with an external editor;
- compilation remains largely invisible to the user.

For madc, the key lesson is not Julia syntax. It is the feeling that **a compiled/JIT language can behave like a natural interactive environment without making the user think about compilation on every entry.**

### 2.2 IPython — best developer-toolbox model

IPython 9.17.1 adds a richer developer shell around Python. Its most relevant ideas are:

- numbered inputs and outputs (`In[n]`, `Out[n]`);
- persistent input history across sessions;
- output caching and references to previous results;
- `_`, `__`, `___` for recent output;
- powerful semantic completion, including type/signature information;
- `object?` for quick introspection and `object??` for deeper information;
- explicit commands for definition, source, documentation and files;
- `%run`, `%load`, `%edit`, `%save`, `%rerun`, `%recall`, `%reset`;
- `%timeit`, profiling and debugger integration;
- shell commands and aliases;
- true multiline editing, syntax highlighting and autosuggestions;
- extensible magic commands for things which are not naturally expressed as Python syntax;
- optional automatic source reloading;
- interactive-only conveniences such as top-level `await`.

For madc, the key lesson is that a REPL becomes much more valuable when it can answer **"what is this?", "where did this come from?", "which overload is this?", "show me the source", "run this file", and "repeat that experiment"** without leaving the session.

### 2.3 Clang-Repl — important C++ baseline, not the UX target

Current Clang-Repl proves several important C++ behaviors:

- real incremental compilation using Clang AST -> LLVM IR -> JIT;
- functions, loops, lambdas, classes and declarations can be entered interactively;
- an expression without a trailing semicolon can be synthesized and printed automatically;
- the printed result is retained internally as `LastValue`;
- `%undo` retracts the previous incremental input;
- `%lib` loads a dynamic library;
- `%help` exposes its built-in commands;
- the interpreter can also be embedded as a library.

Its UX is intentionally compiler-oriented and comparatively sparse. Multiline examples still rely on explicit continuation syntax, and the official REPL surface is much less rich than Julia/IPython.

For madc, use Clang-Repl as proof that incremental C++ and value synthesis are legitimate, not as the primary interaction design.

### 2.4 Cling / ROOT — important implementation lessons

Cling/ROOT has a much longer interactive-C++ history and offers useful precedents:

- no-semicolon expression display;
- persistent command history and reverse search;
- tab completion;
- `.L` load file/library, `.U` unload, `.x` load-and-run;
- `.undo [n]`;
- include-path inspection and modification;
- object/class inspection;
- interpreter-state inspection;
- declaration shadowing / interactive redefinition.

The redefinition work is especially relevant. ROOT describes a scheme where each new definition is internally isolated so it retains a unique identity, while normal lookup is adjusted to expose the newest definition. This permits variables, functions and classes to be redefined without pretending the one-definition rule never existed.

This is a better implementation precedent for madc than simply mutating an existing C++ declaration in place.

### 2.5 Thonny — best IDE model for the target audience

Thonny's current design reinforces several choices:

- one installer and a working language environment out of the box;
- an intentionally stripped-down initial UI;
- editor + shell as the core layout;
- shell accepts ordinary language expressions/statements, including multiline input;
- F5 "Run current script" is surfaced through the shell as a magic operation;
- shell history and simple system-command access;
- Variables view shows how both programs and shell commands affect state;
- code completion is available without turning the UI into a professional IDE cockpit;
- debugger has "big step" and "small step" rather than requiring breakpoint expertise;
- small steps can expose subexpression evaluation;
- stepping into calls gives visually separate frames;
- syntax errors and scope relationships are visually explained;
- beginner-simple and more advanced modes coexist.

A particularly useful Thonny behavior is the distinction between a normal Run and a run that preserves the existing interpreter. The ordinary beginner workflow favors a clean, predictable run; advanced users can deliberately preserve state.

For madcide, that suggests **clean F5 by default plus an explicit hot/reload-in-current-session action**.

### 2.6 Community signal: what C++ developers ask for

Community discussion is not statistically representative, but recurring requests are consistent:

- Python-style rapid experimentation without creating `main()` and recompiling a scratch file;
- keeping expensive data/state loaded while iterating on analysis code;
- a REPL available in the debugger or IDE;
- easy integration with existing libraries/projects rather than an isolated toy environment;
- autocomplete and semantic tooling;
- reliable redefinition;
- better multiline input;
- simple installation/setup;
- avoiding repeated replay of all previous commands because side effects make that incorrect;
- avoiding REPL quirks that differ unpredictably from compiled C++.

There is little evidence that mainstream C++ developers want to replace their editor with a prompt. The stronger use case is **a persistent REPL pane beside the editor**.

---

## 3. Existing madc substrate to reuse

This proposal assumes the current repository state plus the technical inventory in the 2026-09-21 plan.

### 3.1 Compiler/runtime substrate

Already present or explicitly reserved:

- `madc::eval_*` and `_ctx` forms in `src/ns_madc.cpp`;
- runtime expression and unit evaluation through the active `Program`;
- MC11 `cir_node` as the single source-preserving IR;
- every `cir_node` retains source tokens, parse subtree and source position;
- c2mir -> MIR backend;
- MIR JIT;
- MIR interpreter reserved for interpreter/REPL/debug work by ADR 0001;
- source/C11/MC11/C++ rendering from the same IR;
- frozen header forest / self-contained compiler environment;
- madc dialect top-level statements and no-required-`main()` behavior;
- common native/runtime declarations available without conventional boilerplate in the madc dialect.

> **Code check:** `madc::eval_*` / `_ctx` are **not** REPL substrate. Each call builds a fresh child `Program` and a fresh MIR context, runs once, and discards both; a ctx is a read-only bag of values copied in as constants (`src/madc_program.cpp:4560-4602`, `:2484-2522`). eval and the REPL are separate clients of a new incremental-session core; see §40. The MIR interpreter is wired only in unit tests (`tests/unit/test_cir*.cpp`); every product path is the JIT. Script mode (top-level statements, no `main`) exists for `--std=madc` only and works on a whole TU at a time (`parser.cpp:76165-76383`).

### 3.2 madcide substrate

Current `madcide` already has much more infrastructure than a teaching IDE needs:

- the editor holds the live parse; the compiler is not spawned as an external process;
- TUI, GUI/webview and line-mode faces share one command/composition model;
- source / MC11 / C11 / C++ Views;
- split-tree editor region;
- Problems / Output / Terminal panel;
- Build / Run commands;
- projects and manifests;
- command registry and data-driven menus;
- data-driven keybinding profiles;
- event-sourced history/journal;
- multi-client sessions and presence carets;
- LSP, MCP, attach/session discovery;
- permission tiers.

The plan should therefore add a **new interaction profile**, not rebuild the IDE framework.

> **Code check:** the inventory is real, with limits:
> - Commands and panel kinds are compiled enums (`madcide_enums.inc:25-58`, `:460-463`), and each panel's content is a `switch` (`compose_chrome_pane`, `madcide_core.inc:6989`). Only menus, keys and layout placement are data.
> - Startup hard-codes the `joe`/`default` profile set (`init_view_es`, `madcide_core.inc:7696-7718`).
> - The event journal persists only when a project manifest is open.
> - There is no completion (the LSP rejects `textDocument/completion`), no toolbar, no Debug or Variables view, and no readline-style line editor to reuse.

### 3.3 Important architecture boundary

`madc` / `libmadc` must own:

- what a REPL input means;
- when input is complete;
- expression/declaration/statement classification;
- persistent compiler/runtime session state;
- redefinition semantics;
- value capture and display data;
- completion and introspection queries;
- source loading/reloading semantics;
- compiler/session history operations;
- error rollback.

`madcide` must own:

- where the shell pane appears;
- toolbar/menu presentation;
- editor-to-REPL actions;
- Variables / Memory / Frames visualizations;
- beginner-friendly diagnostic wording and affordances;
- debugger/stepper visualization;
- teacher/student UI and invitation workflow;
- simple vs advanced layout/profile.

**madcide must never parse REPL syntax or implement a second redefinition model.**

---

# Part A — `madc` / `libmadc` REPL

## 4. Invocation and session model

### 4.1 CLI entry

Add a canonical REPL entry point:

```text
madc --repl
```

Potential convenience behavior after the explicit mode is stable:

```text
madc
```

with no source and an interactive TTY may enter the REPL, but this should be a later compatibility decision rather than a Stage-1 requirement.

Existing compiler options remain meaningful:

```text
madc --repl --std=madc
madc --repl --std=c17
madc --repl --std=c++17
madc --repl -Iinclude -lfoo
```

Cross-target builds must refuse interactive execution just as they refuse other run lanes.

> **Decision D20 (2026-09-25):** there is **no `--repl`**. madc enters the REPL the way Julia, Python, Node and Lua do, so the convenience form above is the canonical one and the explicit flag is `-i` / `--interactive`:
>
> ```text
> madc                        # no program file, stdin a terminal  -> the REPL
> madc --std=c++17 -Iinc -lm  # options without a file              -> the REPL, configured
> madc -i file.c              # run the file, then the REPL with its names (python -i; F5, D16)
> madc -i < transcript        # force the REPL on a non-terminal stdin (transcript tests)
> madc < prog.c               # no file, piped stdin               -> compile and run stdin
> madc -o out                 # an artifact request, no input       -> "no input files" (gcc)
> ```
>
> Read every later `madc --repl` in this document as "the interactive session" entered this way. This changes today's no-file behaviour, which prints the usage line and exits 0.

### 4.2 One reusable `ReplSession`

The command-line REPL, madcide shell, tests, embeddings and future notebook/kernel frontends should all consume the same engine object.

Proposed conceptual shape (names are illustrative):

```cpp
enum class ReplInputKind {
    expression,
    declaration,
    statement,
    directive,
    meta_command,
    shell_command
};

enum class ReplInputState {
    complete,
    incomplete,
    invalid
};

enum class ReplTxnState {
    pending,
    committed,
    rejected,
    executed
};

struct ReplResult {
    ReplInputKind kind;
    ReplTxnState state;
    madc::value value;
    uint32_t type_id;
    std::string display_text;
    std::vector<Diagnostic> diagnostics;
    uint64_t input_id;
    bool displayed;
};

class ReplSession {
public:
    ReplInputState classify_input(...);
    ReplResult submit(...);
    CompletionResult complete(...);
    HelpResult help(...);
    ReplResult undo(...);
    void reset(...);
    ...
};
```

The important requirement is **one reusable semantic service**, not these exact names.

> **Code check:** the C++ class is right (`cpp-first-api.md`), but the sketch can't be madcide's API:
> - **madcide is dialect code** (`tools/madcide/*.mad|.inc`). Until L3 (value-by-value returns) lands, the script-facing surface must follow the `diagnostics(value &out, …)` precedent (`include/madc/ns_madc:74-87`): `value&` out-parameters and ring-lifetime `const char*`. It cannot return a struct holding `std::string` or `madc::value` by value (`dialect-lean.md`, `value-first.md`).
> - **`Diagnostic` already exists** (`include/madc.h:2910`) with the enum severity/phase from `bits/diag_enums`. Reuse it.
> - **No thread-safety contract is stated,** though the CLI, madcide and a teacher would share one session. See §42 Q10.

### 4.3 The session is persistent

A successful input may add:

- variables/objects;
- functions and overloads;
- types;
- templates;
- namespace bindings;
- imports/includes;
- loaded libraries;
- runtime state.

A failed parse/sema/codegen input commits nothing and leaves the previous session usable.

---

## 5. Input model

### 5.1 The unit of input is a complete syntactic entry, not a physical line

The REPL must accept at the same prompt:

```cpp
2 + 2
int x = 10;
x += 5;
for (int i = 0; i < 3; ++i) { ... }
int square(int n) { return n * n; }
struct Point { double x, y; };
#include <stdio.h>
import c as libc;
```

The parser classifies the entry. The user should not need `:let`, `:def` or a special declaration mode.

### 5.2 Parser-driven multiline entry

Do not copy Clang-Repl's explicit backslash continuation model.

The parser must report one of:

- **complete** — execute/commit;
- **incomplete** — keep collecting input;
- **invalid** — show diagnostic, reject the entry, return to a clean prompt.

Example:

```text
madc> struct Point {
... >     double x;
... >     double y;
... > };
```

The error-tolerant parse work is directly relevant here. "Missing more input" must be distinguishable from "this is already invalid."

> **Code check:**
> - Error-tolerant parse slice A is **merged** (`a5b78c58c`, 2026-08-25): top-level containment plus `SkippedTokens`.
> - The complete / incomplete / invalid classification **does not exist**. An unclosed `{` at EOF is just an error, so the classifier is new work.
> - Balanced-delimiter bookkeeping goes through `DelimDepth` (`delimiter-tracking.md`), never a hand-rolled counter.

The line editor should also provide a force-newline key (Julia uses Meta-Enter) so a user can format an otherwise complete expression across lines.

### 5.3 Pasting code

Support bracketed-paste / paste-aware input from the beginning. A pasted multi-line C/C++ block should be parsed as a block, not submitted one terminal line at a time.

Later, add Julia-style **prompt paste** so a copied `madc>` transcript can be pasted back without manually deleting prompts/output.

---

## 6. Expression display and result history

### 6.1 Julia/Cling convention

Adopt this default:

```text
madc> 2 + 2
4

madc> 2 + 2;

```

An expression without the final semicolon is accepted as an interactive extension and its result is displayed. A trailing semicolon means "execute, but do not display the result."

This is already familiar from Julia and ROOT/Cling and is implemented in current Clang-Repl.

### 6.2 Prefer Julia/IPython-style concise values over Clang-style mandatory type prefixes

Default display should be concise:

```text
madc> x * 2
30
```

not necessarily:

```text
(int) 30
```

Type information is always available through introspection and may be enabled as a display option:

```text
madc> :set display-types on
madc> x * 2
(int) 30
```

This keeps the default experience closer to Julia/IPython while retaining the C/C++-useful type view.

### 6.3 `ans` and numbered output

The session should internally number inputs and outputs even if the default prompt remains visually simple.

Recommended user model:

```text
madc [12]> expensive_call()
...

madc [13]> ans
...
```

The exact prompt decoration may be configurable, but input IDs must exist because history, rerun, diagnostics and madcide synchronization benefit from stable addresses.

In the madc dialect, `ans` can naturally be surfaced through the existing `var` direction.

In strict ISO modes, `ans` must be treated as an explicit REPL extension and should be discoverable as such. If exposing a changing dynamic-typed identifier would contaminate strict semantics, keep the actual value behind the session and expose it through `:out`/`:ans` rather than pretending it is an ordinary ISO C/C++ variable.

### 6.4 Value display protocol

Stage 1 must handle at least:

- integer/floating/bool;
- char/string forms;
- pointers (`nullptr`/address, without dereferencing by surprise);
- enums;
- simple aggregate/object identity;
- `void` / no-result.

Later display providers may add pretty printers for containers and user types.

The existing plan's decision that automatic result handling goes through the `var`/value-carrier direction stands. If arbitrary C++ objects cannot be faithfully copied into that carrier, extend it with a typed borrowed/owned handle rather than inventing a second public result system.

> **Code check:**
> - Typed capture exists only for scalars and `char*` (`value_from_storage`, `madc_program.cpp:2315-2370`). Other pointers become integers, and structs fail with "cannot marshal this result type".
> - The carrier's `instance` kind could hold a struct, but nothing builds one on this path.
> - The compile-time `php::print_r` / `var_dump` walk (`src/cir_dump.cpp`) already renders real C types, so synthesizing a display call is the shortest route to §6.4's list. See §42 Q5 for the display *format*.

---

## 7. Prompt modes and command vocabulary

### 7.1 Primary syntax: Julia/IPython familiarity, C/C++ safety

Use three levels:

1. **ordinary C/C++/madc input** at `madc>`;
2. **help/introspection** via `?`;
3. **session/compiler commands** via `:`.

Support `%...` aliases for familiar IPython/Clang-Repl/Thonny commands where useful, but do not make `%` the primary madc vocabulary.

> **Decision D13 (2026-09-25):** reversed. `%` is primary (IPython and Clang-Repl agree), and `:` is an accepted alias. Read every `:name` in §7 as `%name`. The shell is D14 (`;` mode, `%sx`/`%system`), and help is D15.

### 7.2 Help mode

At the beginning of an empty input buffer:

```text
madc> ?
help?>
```

and directly:

```text
madc> ?printf
madc> ??printf
```

Recommended behavior:

- `?name` — declaration/signature(s), resolved type, owning header/module/namespace, short documentation if available;
- `??name` — deeper information: definition/source location, overload set, origin/provider, possibly generated/lowered representation links;
- help-mode Tab completes symbols and topics;
- Backspace on empty help prompt returns to language mode, Julia-style.

### 7.3 Shell mode

C/C++ makes IPython's `!command` ambiguous because `!x` is valid C/C++.

Canonical command:

```text
:sh command
```

Optional Julia-style convenience in friendly REPL mode:

```text
madc> ;
shell>
```

A **lone semicolon on an otherwise empty prompt** may enter shell mode. Semicolons inside C/C++ input keep normal language semantics. Backspace on an empty shell prompt exits the mode.

Do not interpret arbitrary leading `!foo` as shell syntax by default.

### 7.4 Package mode

Do not copy Julia's `]` package mode until madc has a package-management concept worth exposing. Leave the key unclaimed.

### 7.5 Initial canonical commands

#### Session

```text
:help
:history [range]
:undo [n]
:reset
:clear
:status
:quit
```

#### Language/compiler

```text
:std [c17|c++17|c++20|madc]
:strict [on|off]
:includes
:defines
:libs
:loadlib <name-or-path>
```

#### Introspection

```text
:type <expr-or-name>
:which <call-expression>
:overloads <name>
:decl <name>
:where <name>
:source <name>
:explain <expr-or-call>
```

`which` is especially valuable for C++ overload resolution.

#### Source/session integration

```text
:load <file>
:reload [file]
:run <file>
:restart <file>
```

Semantics:

- `:load` — add declarations/definitions to the current session; no entry-point execution unless the file itself has top-level script statements;
- `:reload` — update a previously loaded file using redefinition rules;
- `:run` — evaluate/run the file in the current session;
- `:restart` — reset the REPL session, then run/load the file cleanly.

#### Developer tools (later Stage 2/3)

```text
:time <expr>
:profile <expr-or-call>
:emit c11|mc11|cpp <entry-or-name>
:show implicit
```

`:show implicit` is a madc-specific teaching aid: show automatic includes/declarations/imports or other conveniences the interactive environment supplied.

### 7.6 Familiar aliases

Secondary aliases may reduce friction for people coming from other shells:

```text
%help     -> :help
%undo     -> :undo
%lib      -> :loadlib
%run      -> :run
%load     -> :load
%reset    -> :reset
%timeit   -> :time (or a repeat-timing variant)
```

Do not guarantee full IPython or Clang-Repl command compatibility.

---

## 8. Completion and introspection

### 8.1 Completion is a core REPL requirement, not an IDE-only feature

The core session should provide structured completion results for:

- identifiers;
- keywords;
- locals/globals/session bindings;
- struct/class members;
- namespaces;
- functions and overloads;
- template names where practical;
- headers/modules/importable namespaces;
- file paths in commands requiring paths;
- library names where discoverable.

The CLI renders these with Tab. madcide renders the same data as completion UI.

### 8.2 Signature-aware completion

When the cursor is inside a call, expose applicable overloads and the active parameter. Julia/IPython users already understand call-signature help; in C++ it is even more valuable.

### 8.3 Overload explanation

A distinctive C++ feature should be:

```text
madc> :which foo(x, 2.0)
foo(int, double)
```

and, when useful:

```text
madc> :explain foo(x, 2.0)
selected foo(int, double)
  argument 1: exact match
  argument 2: exact match
other candidates:
  foo(long, double) — conversion required for argument 1
  foo(int, int)     — conversion required for argument 2
```

This turns a difficult compiler concept into an exploratory tool.

### 8.4 Source/header awareness

For a resolved symbol, the REPL should be able to say:

- type/signature;
- declaration location;
- definition location when available;
- header/module that made it visible;
- whether it came from madc's embedded forest, project source, runtime namespace, or loaded native library.

This uses compiler truth rather than a parallel documentation database wherever possible.

> **Code check:**
> - **Mostly existing machinery:** `:overloads`, `:type`, `:which`, `:decl`/`:where` for user code, `:libs`/`:loadlib`, and member/keyword completion. Their owners are `namespace_fn_overload_sets`, `resolved_call_funcdef`/`call_target_funcdef`, `TopDecl` and `TokenFunc` locations, `bind_module_namespace`, `DataDefSTRUCT`/`CLASS`, and `keyword_map`.
> - **Need new owners:**
>   - a completion enumeration service (`dump_registered_names` omits variables, scope locals and not-yet-materialized forest/lazy names)
>   - signature help
>   - an `:explain` trace sink passed into the rankers (at least eight call `score_arg_to_param`, and only the winner survives)
>   - a symbol→header reverse index (the forest declaration index is per-unit, frozen-headers-only, with no lines)
>   - a human-readable type renderer
>   - diagnostic codes/payloads (`Diagnostic` is text only)
> - **Lookup side effects:** parser lookups have them (forest materialization, `dlsym` registration, throwing). A query service must use or add side-effect-free forms.
> - **Location gaps:** `Variable` has no location, and declaration and definition merge into one `FuncDef`.

---

## 9. History model

### 9.1 Separate input history from semantic state history

These are related but not identical:

- **input history** — what the user typed; used by Up/Down, Ctrl-R, `:history`, rerun;
- **semantic transaction history** — compiler/session changes which can potentially be retracted by `:undo`;
- **runtime side effects** — file I/O, network I/O, native calls, external state; not generally reversible.

Do not imply that `:undo` can reverse arbitrary side effects.

### 9.2 Persistent command history

Like Julia/IPython/ROOT:

- Up/Down recall;
- Ctrl-R reverse incremental search;
- persistent history file across sessions;
- timestamps and language mode stored with entries;
- optionally project-specific history later.

### 9.3 Addressable entries

Every submitted input gets a monotonically increasing ID. `:history`, `:rerun`, diagnostics and madcide can refer to the same IDs.

Later, support ranges inspired by IPython:

```text
:history 12-18
:rerun 17
```

### 9.4 Rerun is execution, not restoration

`:rerun 17` means "execute input 17 again now." It may repeat side effects. It is not time travel.

---

## 10. Transaction and error semantics

### 10.1 Compile failure is atomic

Each normal language input is a transaction:

```text
parse -> semantic analysis -> lower -> compile -> commit definitions -> execute
```

If parse/sema/lowering fails, the input does not enter the live compiler state.

The failed text remains in history for recall/editing, but it is not a committed semantic entry.

### 10.2 Runtime failure does not poison the session

A runtime exception/fault/guard event should report the failure while keeping the REPL service alive whenever isolation makes that safe.

The existing process/isolation work may be used for operations which can crash the host. The core design goal is the Julia/IPython expectation: **one bad input does not end the interactive session.**

### 10.3 `:undo`

`:undo` retracts committed interpreter/compiler inputs where the engine can do so safely.

It does **not** claim to reverse:

- external I/O;
- arbitrary mutations performed through pointers into native libraries;
- network/process side effects;
- irreversible device operations.

If an entry had observable side effects, the UI may warn that only compiler/session definitions are being retracted.

> **Code check:**
> - **No in-`Program` rollback.** Recovery restores only the compound and class-scope stacks. A failed statement's symbol, typedef and funcdef insertions stay.
> - **Rollback primitives exist:** `registration_map` has begin/commit/rollback (`include/madc.h:1749-1777`), and `ClassRegistrationJournal` does the same for classes (`parser.cpp:33760-33870`).
> - **Process-fatal paths remain:**
>   - a MIR error outside the armed trap calls `exit(1)` (`madc_cir.cpp:442-447`)
>   - a guest fault during in-process execution kills the host
>
>   §10.2 therefore depends on where the session runs (§42 Q2).

---

## 11. Redefinition semantics

This is the hardest C/C++-specific part and should be designed explicitly rather than discovered accidentally.

### 11.1 User expectation

Interactive users expect this to work:

```cpp
int x = 1;
int x = 2;

int f(int n) { return n * 2; }
int f(int n) { return n * 3; }
```

Strict ISO source does not permit those redeclarations in the same scope. A friendly REPL should.

### 11.2 Recommended model: versioned declarations + latest-name lookup

Borrow the principle from Cling declaration shadowing:

```text
x#1
x#2   <- latest binding named "x"

f(int)#1
f(int)#2 <- latest binding for this signature
```

Each definition keeps a unique internal identity. Ordinary lookup resolves to the newest compatible definition.

This preserves the ability for older live objects/references/code to continue to refer to an older entity when required.

### 11.3 Variables

On same-name redefinition in friendly mode:

- create a new binding/storage;
- run the old object's destructor when it is actually retired and safe to retire;
- if old references/pointers remain meaningful only to old storage, preserve that storage or mark the references as dangling according to the chosen lifetime rule;
- warn when the replacement creates a subtle old/new split.

In `:strict` mode, use ordinary C/C++ redeclaration rules.

### 11.4 Functions and overloads

A new overload simply extends the overload set.

A same-signature redefinition supersedes the old one for future lookup.

For the Julia-like expectation that already-defined interactive code should call the latest function body, investigate one of two mechanisms:

1. **stable REPL dispatch cell/trampoline per signature** — redefinition updates the target; or
2. **dependency invalidation/recompile** — MC11 users of the replaced function are marked stale and recompiled on next use.

A plain Cling-style shadow without either mechanism may leave already-compiled callers bound to old code, which is technically coherent but surprising in the target UX.

The implementation spike must decide this before function redefinition is declared complete.

> **Decision D5 (2026-09-25):** callers see the newest body, achieved by dependency recompile, and a function's address stays stable across redefinitions (Julia). Variables and types are D6; late binding is D7.

### 11.5 Types/classes

Friendly mode may permit:

```cpp
struct Foo { int x; };
struct Foo { double x; };
```

but these are distinct internal types. Existing `Foo#1` objects remain `Foo#1`; new lookup resolves `Foo` to `Foo#2`.

The REPL should make this visible when relevant rather than implying an existing object's physical layout changed in place.

A first implementation may conservatively require `:reset` for structural type redefinition if safe versioning/dependency tracking is not yet implemented. Do not silently produce mixed-layout behavior.

### 11.6 Strictness

`:strict` means "enforce source-language declaration/redefinition rules." It does not disable harmless transport conveniences such as multiline editing or history.

Any interactive syntax extension which changes what source text is accepted must be listable/explainable, preserving the earlier plan's principle that the REPL can tell the user what it did beyond ISO C/C++.

> **Code check:**
> - **`:strict` has nothing to switch to.** Redeclaration rules are not enforced today:
>   - `addVariable` reportedly reuses the existing `Variable` on a same-scope redeclaration (`parser.cpp:~29445`, `~29487`)
>   - a second same-signature function body loses to the first (`fold_same_signature_overload`, `parser.cpp:3108`)
>   - `TokenFunc::is_overridden` is read but never set
>
>   If reproduced, this is a silent wrong answer and is fixed first, in its own commit (§42 Q13).
> - **Gating (restores 2026-09-21 §6.3):** every relaxation is a registered feature gated through `--std=`/`LanguageStd` (invariants I3/I4/I8), not a free-standing toggle. That includes top-level statements, bare-expression display, `ans`, redefinition and auto-supply. `:strict` is at most a view of that registry.
> - **Where "newest wins" would land:** `addVariable`, `var_index`, the `funcdef_map` reconcile, `fold_same_signature_overload`, the struct guard (`parser.cpp:49495`) and the emitted-symbol identity.

---

## 12. C/C++ friendliness without teaching a fake language

### 12.1 Use existing madc conveniences where they already exist

The default madc dialect already provides the friendliest experience:

- top-level statements;
- no required `main()` for simple scripts;
- frozen headers/toolchain;
- common runtime declarations without boilerplate;
- native libraries available directly;
- utility namespaces.

The REPL should expose those naturally rather than recreate them in REPL-only code.

### 12.2 Explicit ISO modes remain real ISO-oriented modes

When the user selects `--std=c17` / `--std=c++NN`, avoid silently teaching source which cannot leave the REPL.

For conveniences such as missing includes, prefer an **assist policy**:

- `off` — strict compiler behavior;
- `suggest` — explain the required include and offer/apply it in madcide;
- `auto` — supply it interactively but record and expose the intervention.

Recommended defaults:

- `--std=madc`: `auto` is reasonable;
- explicit ISO C/C++ in the raw CLI: `suggest` or `off`;
- madcide teaching profile: `suggest` initially, with one-click "add/include for me"; optionally `auto` for a deliberately assisted lesson profile.

### 12.3 Transparent magic

Add a way to inspect what the environment supplied:

```text
:show implicit
```

Example output might include:

```text
implicit for this session:
  <stdio.h> declaration provider for printf
  std=c++17
  stdlib=libstdc++
  library libc (process runtime)
```

The goal is "easy now, explainable later."

> **Code check:** nothing records what was supplied implicitly.
> - `printf` does not come from auto-include. It comes from silent frozen-header adoption or a `dlsym` fallback (`parser.cpp:40743`), gated by the dlfcn policy rather than `--std=`.
> - The auto-include pending set is cleared on every tokenize (`lexer.cpp:2132`, `:3038`).
> - `:show implicit` needs a new implicit-supply ledger written by auto-include, forest adoption, the `dlsym` fallback and lazy namespaces.

---

## 13. Source files and live sessions

### 13.1 The REPL must not be a cul-de-sac

IPython and Julia workflows succeed because interactive experiments and real files coexist.

At minimum, `madc --repl` should support:

```text
:load file.cpp
:reload file.cpp
:run file.cpp
:restart file.cpp
```

### 13.2 Track source ownership

Definitions loaded from a file should remember their source file/range. When the file is reloaded, the session should know which definitions came from the old revision so stale definitions can be replaced/retracted rather than merely accumulated.

This is important for madcide editor integration.

### 13.3 Hot reload is a distinct feature from Run

Do not make "Run" mean an ambiguous mixture of clean execution and live patching.

Core operations should distinguish:

- **restart + run** — predictable clean program execution;
- **reload into current session** — interactive development retaining state.

madcide can then choose the beginner-friendly default without losing the advanced workflow.

---

## 14. Core REPL test gates

### 14.1 Parser/input gates

- complete/incomplete/invalid classification corpus;
- nested braces/parens/templates/lambdas/preprocessor inputs;
- multiline paste;
- malformed input followed by valid input;
- comments/strings containing delimiters.

### 14.2 Persistence gates

Scripted sessions covering:

- variables persist;
- function definitions persist;
- overloads persist;
- includes/imports persist;
- loaded library symbols persist;
- rejected input does not alter state.

### 14.3 Display gates

- expression without `;` displays exactly once;
- expression with `;` suppresses display;
- `void` displays nothing;
- `ans`/output history follows the documented rule;
- type/value formatting stable enough for transcript tests.

### 14.4 Redefinition gates

- variable same type;
- variable different type;
- same-signature function replacement;
- new overload;
- class/type replacement policy;
- pointers/references across redefinition;
- destructor behavior;
- `:strict` rejection.

### 14.5 JIT/interpreter equivalence

Keep the existing invariant: where both executors support a program, observable program semantics must agree.

The REPL itself can JIT normal entries while the later teaching stepper uses MIR interpreter execution; they must not become two language implementations.

---

# Part B — madcide as a Thonny-like C/C++ learning environment

## 15. Product goal

madcide should stop presenting its beginner story as a small professional IDE and instead present a **purpose-built learning/exploration environment**.

It can retain all current advanced functionality, but the default teaching profile should communicate:

> write code -> Run -> see output -> try expressions in the shell -> inspect variables -> ask the compiler questions

The existing advanced project/Nexus/IR capabilities become progressive disclosure, not first-run chrome.

---

## 16. Add a `simple` / `teaching` workspace profile

Do not fork madcide. Add a layout/command-profile combination using the existing data-driven infrastructure.

Possible entry points:

```text
madcide --learn file.cpp
madcide --simple file.cpp
```

A later product decision can make it the default first-run GUI profile.

> **Code check:**
> - **Command line:** `madcide` requires the file as argv[1] and parses flags only from argv[2] on (`madcide.mad:64-232`), so `madcide --learn file.cpp` would treat `--learn` as the file. With no arguments it prints usage and exits.
> - **Profile selection:** needs a flag threaded through `run_tui` → `IdeSession::open` → `init_view_es`, plus `chthonic.layout` / `chthonic.menu` files.
> - **Toolbar:** no face has one.
>
> **Amended (owner, 2026-09-30):** the profile is a bundle in madcide's plugin model (`docs/plans/2026-09-30-madcide-plugins.md`), selected by `--profile chthonic` or `settings.json`, not a `--learn` mode. See §41.11a.

### 16.1 Initial GUI layout

Recommended default:

```text
+-----------------------------------------------------------+
| New  Open  Save      Run  Debug  Stop            Help     |
+-----------------------------------------------------------+
|                                                           |
|                       Editor                              |
|                                                           |
+-----------------------------------------+-----------------+
| madc REPL / Shell                       | Variables       |
| madc>                                   | (optional)      |
|                                         |                 |
+-----------------------------------------+-----------------+
| status: C++17 | ready | Ln 8 Col 12                        |
+-----------------------------------------------------------+
```

Key points:

- editor dominates the window;
- REPL/Shell is visible by default;
- Variables pane is optional and simple;
- no project tree unless a project is opened or the user asks for Files;
- no MC11/C11/Nexus/MCP/LSP concepts in the beginner default;
- one obvious Run button;
- one obvious Debug button;
- one obvious Stop/Restart action;
- menus remain available but do not advertise the whole power surface at once.

### 16.2 Reuse existing panes

Current madcide already has a bottom panel with Problems / Output / Terminal. Add **REPL/Shell as a first-class panel type backed by `ReplSession`**, not a pseudo-terminal running a second `madc` process.

A program's stdin/stdout terminal and the compiler REPL are different concepts and should remain distinct tabs if both are needed.

---

## 17. Editor <-> REPL relationship

### 17.1 One live programming session

The editor and REPL should feel like two views onto one engine:

```text
source editor -------+
                     +--> live compiler/session --> JIT/interpreter
REPL shell ---------+
```

The REPL should immediately know about definitions the IDE has loaded into its session, and editor actions should use the same core semantics as command-line `:load`, `:reload`, `:run`, and `:restart`.

### 17.2 F5: clean Run by default

For the teaching/simple profile, model Thonny's beginner predictability:

**Run / F5**

1. reset the program/repl execution session cleanly;
2. compile/load the current editor source;
3. execute it;
4. leave the resulting globals/objects available in the REPL for inspection and experimentation.

This prevents stale REPL state from making a beginner's program behave mysteriously.

> **Code check:**
> - **Step 4 is new engine capability, not reuse.** Run executes in a forked child that exits (`RunChannelFactory`, `src/madc_program.cpp:5065-5128`). Only the exit status and output return; globals and functions die with the child.
> - **There is no F5.** `ui::key` has no function keys and no Alt/Shift modifiers (`include/madc/bits/ui_enums:20-33`, `include/madcdis/keys.h:55-111`). Adding them is engine work in the key owners.

### 17.3 Explicit live-reload action

Add a separate advanced action, e.g.:

- **Run in current session**;
- **Reload into session**;
- shortcut/menu entry but not the primary beginner toolbar button.

This preserves Julia/Revise-style long-lived experimentation for experienced users.

### 17.4 Run selection / run expression

Useful later commands:

- Execute Selection in REPL;
- Execute Current Expression;
- Execute Current Function/Declaration;
- Send to REPL.

These should call structured `ReplSession` operations, not copy text blindly when the editor already knows the source range and parse node.

---

## 18. Shell UX inside madcide

The madcide shell should expose the same behavior as `madc --repl`:

- same prompt modes;
- same `?`, `:`, `%` aliases;
- same history IDs;
- same completion/introspection;
- same result display;
- same redefinition rules;
- same errors.

madcide adds GUI affordances:

- completion popup;
- signature help;
- clickable source locations;
- hyperlinks from diagnostics;
- context menu on a symbol: Type / Declaration / Source / Overloads / Help;
- rich display of structured values later.

This is presentation only; the answers come from the core REPL/compiler service.

---

## 19. Variables and memory views

### 19.1 Stage 1 Variables view

Start simpler than the original memory-visualization proposal.

Show current session bindings:

| Name | Type | Value | Origin |
|---|---|---|---|
| `x` | `int` | `15` | REPL #7 |
| `p` | `Point` | `{...}` | `main.cpp:12` |
| `ptr` | `int*` | `0x...` | REPL #9 |

Selecting a variable may offer:

- inspect value;
- type/declaration;
- address;
- references/pointee where known.

### 19.2 Stage 2 Memory view

Then implement the C/C++-specific teaching view from the earlier plan:

- stack frames;
- actual storage/address;
- pointers as arrows;
- object lifetime;
- uninitialized / alive / moved-from where meaningful / destroyed / freed / dangling;
- arrays and bounds;
- heap allocations.

The beginner view can start name -> value and reveal address/storage detail on demand, analogous to Thonny's simple vs reference-aware views.

---

## 20. Diagnostics and assistance

### 20.1 Keep the real diagnostic

Never replace the compiler's real message. Add a short teaching layer above/beside it.

Example:

```text
printf is not declared in this C++ mode.
It is declared by <cstdio> / <stdio.h>.

[Add include] [Show compiler message]
```

### 20.2 Assistant actions should be deterministic before they are AI-driven

For common compiler-known cases, use structured facts:

- missing include/provider;
- unknown identifier with close symbol match;
- overload candidate mismatch;
- incompatible pointer/reference type;
- missing return;
- uninitialized local;
- obvious lifetime issue detected by the teaching runtime.

An AI explanation can be additive later, but the teaching environment should not depend on an LLM to explain information the compiler already knows exactly.

### 20.3 Show what madc helped with

When interactive assistance auto-supplies something, provide a small nonintrusive indicator and a detail action tied to `:show implicit`.

---

## 21. Debugging/stepping — retain the earlier design, but stage it later

The previous plan's stepper remains strong:

- MIR interpreter for stepping;
- expression granularity using retained parse subtrees;
- source/MC11/C11 synchronized views;
- nested call frames;
- JIT/interpreter equivalence gate.

Thonny research reinforces the UX:

- **Big Step / Step Over** — statement-level;
- **Small Step / Step Into** — expression/subexpression-level;
- **Step Out**;
- **Resume**;
- optional Step Back when state snapshots make it sound.

The critical change is sequencing: the editor+REPL experience should become useful before the full stepper exists.

> **Code check / owner direction (2026-09-24):** the MIR interpreter is **not assumed**. Its one clear advantage is the central dispatch loop with a per-instruction hook (`third_party/mir/mir-interp.c:922-1180`), which suits stepping and UB events. Its costs:
> - native calls go through generated thunks, which risks every madc ABI path (varargs, `long double`, struct passing, v128)
> - the host shims, exceptions, setjmp and multi-return are all proven only under the JIT
> - a large slowdown
>
> The REPL and Run are JIT-only. The stepper's executor is decided when the stepper is designed, as a stated trade-off (for example, JIT plus statement probes versus the interpreter). This supersedes §3.1's "MIR interpreter reserved" line, the executor wording in §14.5, and Phase 7's "MIR interpreter execution".

> **Designed (owner, 2026-10-01): time travel** — [`2026-10-01-chthonic-time-travel.md`](2026-10-01-chthonic-time-travel.md). The stepper is a playhead over a recorded run: Debug runs the program ahead under the trace build (the JIT with statement probes, the executor decision above), it records each step's changes (keyframes plus deltas, nothing replayed), and a timeline strip scrubs it, with Step over / into / out / back as playhead moves and a lane per task. It ships after the master release, as the chthonic product release's headline.

---

## 22. Source/IR teaching views

Keep the existing Views feature but hide it in the beginner default until requested.

A teaching action such as **"Show how this lowers"** can open:

```text
C++ source | MC11 | C11
```

with synchronized selection/carets.

This is a powerful madc differentiator, but it should feel like an explanation tool, not required IDE chrome.

---

## 23. Teacher connection

The previous plan is still correct that most transport/session machinery already exists.

Keep this out of the first REPL milestone. Once the simple IDE is stable, add a teaching front door over existing multi-client tiers:

- **Observe** — teacher sees session, source, REPL and execution state;
- **Suggest** — teacher can propose edits/actions;
- **Drive** — teacher can control editor/REPL with explicit student approval.

The missing work is primarily invitation, naming, trust/approval and UI.

---

# Part C — implementation plan

## 24. Phase 0 — validate/reuse substrate

**Goal:** prove the new REPL is a thin layer over existing compiler machinery rather than a second evaluator.

Tasks:

1. Merge/finish the error-tolerant parse work required to return complete/incomplete/invalid.
2. Verify `madc::eval_*_ctx` and active `Program` state can support persistent declarations as well as expression context; document gaps.
3. Define the `ReplSession` ownership/lifetime model.
4. Confirm the `var`/value carrier can represent automatic results required for Stage 1; specify opaque object handle if necessary.
5. Add a REPL-specific conformance smoke corpus covering common C17/C++17 constructs.
6. Do **not** require perfect language conformance before prototyping the REPL; require that the documented REPL-safe subset is highly reliable and that unsupported constructs fail clearly.

**Gate:** a programmatic `ReplSession` test can submit several expressions/declarations, preserve state, reject a malformed input, then continue correctly.

> **Code check:** tasks 1 and 2 rest on false premises. The parse work is merged but has no completeness signal, and `eval_*_ctx` cannot persist declarations. Phase 0 is therefore **new engine work**, replaced by the proofs in §41.

---

## 25. Phase 1 — core persistent REPL

Implement in madc/libmadc:

- the interactive entry (D20): `madc` with no file on a terminal, `-i` / `--interactive`, and stdin as a program when piped;
- `ReplSession` service;
- parser-driven multiline completeness;
- expression/declaration/statement/directive classification;
- persistent variables/functions/includes/imports;
- expression display without `;`;
- semicolon suppression;
- `ans` / result handle policy;
- failed-input rollback;
- `:reset`, `:quit`, `:status`;
- minimal `:std` / `:strict`;
- transcript testing.

**Gate:** scripted CLI transcript is deterministic; malformed input never poisons subsequent valid entries.

---

## 26. Phase 2 — modern line editing, history and prompt modes

Implement/reuse UI-line facilities for:

- multiline editing;
- syntax color where the terminal supports it;
- Up/Down history;
- Ctrl-R reverse search;
- persistent history file;
- numbered input IDs;
- `?` help mode shell;
- `:` command mode;
- optional lone-`;` shell mode;
- paste-aware input;
- `%` aliases.

**Gate:** usability parity tests for the expected Julia/IPython keyboard loop.

---

## 27. Phase 3 — completion and introspection

Implement structured engine queries:

- completion;
- call-signature help;
- `?` / `??`;
- `:type`;
- `:which`;
- `:overloads`;
- `:decl` / `:where` / `:source`;
- `:includes` / `:libs`;
- `:show implicit`.

Reuse parser lookup, forest declaration index and existing LSP/source-location work.

**Gate:** answers match the actual compiler resolution, including overload selected by a compiled call.

---

## 28. Phase 4 — redefinition and transaction history

Implement deliberately, not as parser special cases:

- versioned declaration identity;
- newest-binding lookup;
- function replacement strategy (dispatch cell or dependency recompile);
- variable replacement/destruction rules;
- type redefinition conservative rule or versioning;
- `:undo` semantic transactions;
- source ownership for loaded definitions;
- `:load`, `:reload`, `:run`, `:restart`.

Use Cling's declaration-shadowing design as a reference, adapted to MC11/c2mir/MIR and madc's symbol model.

**Gate:** re-running edited source produces predictable new behavior without hidden stale definitions.

---

## 29. Phase 5 — madcide simple/teaching profile

Add presentation only; all language behavior comes from `ReplSession`.

Deliver:

- simple editor + REPL default layout;
- Run / Debug / Stop toolbar;
- Variables view;
- REPL completion/signature popup;
- clickable diagnostics/source locations;
- F5 clean restart-and-run;
- advanced "Run/Reload in current session";
- Send Selection/Expression to REPL;
- minimal beginner menu/profile;
- progressive reveal of Files/Project/Views/Nexus tooling.

**Gate:** a novice can install/open madcide, type a small C/C++ program, run it, inspect variables and call its functions from the REPL without configuring a toolchain or project.

> **Code check (2026-09-30):** the release's core of this phase (the owner: §37 + Phase 5's core, then Phase 4) is designed against the code in §41.11a.

---

## 30. Phase 6 — teaching diagnostics and memory

Add:

- deterministic plain-language explanations layered over real diagnostics;
- missing-include suggestions/assistance;
- runtime teaching events;
- memory/storage view;
- pointer/lifetime visualization;
- UB events where the interpreter can identify them reliably.

**Gate:** known reducers for uninitialized, OOB, use-after-free/dangling produce the intended teaching event; correct controls do not.

---

## 31. Phase 7 — expression stepper

> **Superseded (owner, 2026-10-01)** by the time-travel design, [`2026-10-01-chthonic-time-travel.md`](2026-10-01-chthonic-time-travel.md): the JIT with statement probes instead of the interpreter, stepping in both directions over a recorded run, a timeline strip with a lane per task. Its stages T1-T4 and gates (golden step records under JIT and `--exe`, and the traced `--emit=c11` built by gcc and clang giving the same record) replace this section's list. Expression-level stepping and the synchronized source / MC11 / C11 views remain, as its later slice.

The earlier list, kept for its history:

- MIR interpreter execution;
- statement and expression granularity;
- nested frames;
- synchronized source/MC11/C11 views;
- Step Over / Step Into / Step Out / Resume;
- optional step-back if snapshots are semantically sound.

**Gate:** JIT/interpreter equivalence plus deterministic expected step traces.

---

## 32. Phase 8 — teacher connection front door

Add invitation/trust/UI over the existing Nexus/session transport:

- observe/suggest/drive;
- explicit consent;
- shared REPL state and execution view;
- teacher cursor/presence;
- session naming/invite workflow.

---

# Part D — default behavior specification

## 33. Proposed first-run transcript

```text
$ madc
madc 0.x — C/C++ interactive session
std: madc   (? for help, :help for commands)

madc [1]> int x = 10;

madc [2]> x * 2
20

madc [3]> int square(int n) {
       ...>     return n * n;
       ...> }

madc [4]> square(x)
100

madc [5]> ?square
int square(int n)
defined in REPL input 3

madc [6]> :type square(x)
int

madc [7]> :which square(x)
square(int)

madc [8]> square(x);

madc [9]> :history 3-8
...
```

The point is that nothing about JIT, c2mir, MIR, object files or linking is required to use the shell.

---

## 34. Proposed madcide beginner workflow

1. Open madcide.
2. Editor and REPL are already visible.
3. Type:

```cpp
#include <stdio.h>

int square(int n) {
    return n * n;
}

int main() {
    printf("%d\n", square(5));
}
```

4. Press **Run**.
5. Program prints `25` in output/shell.
6. REPL remains available:

```text
madc> square(12)
144
```

7. Variables view shows program/session state where applicable.
8. Edit `square`, press **Run** again for a clean result.
9. Later, choose **Reload in Current Session** when intentionally doing Julia-style live experimentation.

This is the core product experience. Everything else is additive.

---

# Part E — explicit non-goals for this proposal

## 35. Deferred

- BASIC-style verbiage at the REPL prompt (`LIST`, `RUN`, line-numbered program entry). Editing a buffer with ed/ex commands is NOT deferred (D24);
- Jupyter kernel/notebook protocol;
- a package manager REPL mode;
- replacing CLion/VS Code as a professional IDE;
- full debugger parity with gdb/lldb;
- automatically reversible external side effects;
- hiding all build/link concepts forever;
- AI as a required component of diagnostics/help;
- exact Clang-Repl or IPython command compatibility.

---

## 36. Invariants

1. **One parser / one MC11 IR / one semantic truth.**
2. **REPL behavior is an engine service; madcide is a client.**
3. **Friendly interactive extensions are explicit and inspectable.**
4. **Strict C/C++ modes remain meaningful.**
5. **A failed input never corrupts the live session.**
6. **History is not falsely advertised as reversible side effects.**
7. **Redefinition has specified lifetime/binding semantics.**
8. **Completion/introspection answers come from compiler truth.**
9. **The beginner UI hides complexity; it does not remove capability.**
10. **The source file remains the durable program artifact; the REPL accelerates exploration and learning.**
11. **Enums, not strings, for engine discriminators.**
12. **Every new capability declares its thread-safety contract.**

---

# 37. Recommended immediate next work

Before implementing the stepper or memory visualizer, build the smallest end-to-end slice that proves the product direction:

1. `madc` with no file (or `madc -i`) starts a persistent session (D20).
2. `int x = 10;` persists.
3. `x * 2` prints `20`.
4. `x * 2;` prints nothing.
5. a multiline function definition works without backslash continuation.
6. a syntax error leaves `x` intact.
7. Tab completes `x` / function names.
8. `?name` and `:type` work.
9. madcide adds a REPL pane backed by that exact session object.
10. F5 clean-runs the editor buffer and then permits calls into its definitions from the REPL.

If that slice feels good, the core thesis is validated. Redefinition sophistication, memory visualization, expression stepping and live teacher connection can then build on a user experience that is already useful.

---

# 38. Research sources consulted

Primary/current documentation consulted on 2026-09-24:

- Julia 1.12 REPL documentation (Julia documentation; current page showed 1.12.7).
- IPython 9.17.1 documentation: interactive tutorial, terminal options, history, magics, completion, debugging, autoawait.
- Clang-Repl documentation in current Clang documentation.
- ROOT 6.40 Cling manual / first-steps documentation.
- ROOT article on Cling declaration shadowing/redefinition.
- ROOT article on Cling -> upstream Clang-Repl work.
- Thonny official feature page.
- Thonny shell help and debugger help in the current Thonny repository.
- Thonny changelog for simple mode / run/restart behavior.
- Revise.jl current documentation for long-lived Julia source-reload workflows.

Community signal sampled from:

- multiple r/cpp / r/cpp_questions discussions of Cling, Clang-Repl and interactive C++;
- discussions of rapid prototyping with persistent data;
- requests from Python users for a C++ REPL/debugger context;
- complaints about Cling setup/redefinition/library integration and sparse C++ REPL UX;
- 2026 compiler-as-a-service / interactive C++ discussion.

madc sources cross-referenced:

- `README.md`
- `docs/madcide.md`
- `docs/plans/2026-09-21-repl-and-teaching-ide.md`
- `docs/plans/madc-ide.md`
- `docs/plans/ROADMAP.md`
- `docs/adr/0001-cir-c2mir-backend.md`
- `.claude/rules/mc11-ir.md`
- `src/ns_madc.cpp`
- `src/madc.cpp`
- `include/madc/bits/ui_enums`


---

# Part F — code cross-reference (2026-09-24, `develop` @ `3c5ae4344`)

## 39. What exists, what doesn't

| Plan relies on | Verdict | Evidence |
|---|---|---|
| Persistent session state via `eval_*_ctx` | **Missing** | Each eval is a fresh child `Program` + fresh `CirJitSession` (`madc_program.cpp:4560-4602`, `:685-742`; `madc_cir.cpp:1026-1035`). "A Program is not resettable" (`madc_program.cpp:5287`); "A RECOMPILE means a fresh session" (`madc_cir.h:91`). |
| Adding declarations to a compiled program | **Missing** | Nothing links a second module against a live context. Seams: `MIR_module_privatize_for_link` (`third_party/mir/mir.h:740-753`), the cache lane's two-module `MIR_link` (`madc_cir.cpp:1179-1214`). |
| complete / incomplete / invalid input | **Missing** | Error-tolerant slice A is merged (`a5b78c58c`) but has no "needs more input" signal. |
| Failed input commits nothing | **Missing** | No symbol-table rollback; `registration_map` transactions and `ClassRegistrationJournal` exist to build it from. |
| One bad input never ends the session | **Partial** | Containment via throwaway children only; `exit(1)` on an untrapped MIR error; guest faults kill an in-process host. |
| Result display through the carrier | **Partial** | Scalars and `char*` only (`value_from_storage`); the `var_dump` walk renders real C types. |
| Top-level statements without `main` | **Partial** | Script mode, `--std=madc` only, whole-TU (`parser.cpp:76165-76383`). |
| MIR interpreter as an executor | **Tests only** | The JIT is every product path (§21 code check). |
| Completion / signature help | **Missing** | LSP has hover/definition/references/symbols/semantic tokens, no completion. |
| `:which` / `:overloads` / `:type` / `:decl` / `:libs` | **Mostly present** | §8 code check. |
| `:explain`, `:show implicit`, symbol→header, type renderer | **Missing** | §8 and §12.3 code checks. |
| Redefinition / strict redeclaration | **Missing** (and a suspected bug) | §11.6 code check. |
| `:std` inside a live session | **Unsafe** | Keywords only accumulate (`lexer.cpp:5921`); declarations keep their mode's identity. `:std` must mean reset. |
| Line editor with history / Ctrl-R / multiline | **Missing** | `tools/texteditor` line editors are ed-style; no readline-class library in `third_party`. |
| madcide REPL panel, teaching profile | **Clean insertion points** | A panel kind beside Terminal (`compose_chrome_pane`, a `term_pump`-style pump); a profile flag into `init_view_es` plus `chthonic.*` files. |
| F5, program state after Run | **Missing** | No function keys; Run is a forked child. |

Other anchors the plan should cite:
- `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md:70-85`: `CirJitSession`, "REPL tier rides the same session, incremental modules".
- The ~100 skipped libmadc eval unit tests (`claude_status.json` gaps), which remain **eval's** specification, not the REPL's.
- `docs/plans/madc-vision-and-invariants.md` I3/I4/I8 and its checklist.

## 40. eval and the REPL are separate clients of one new core

In languages that have both, eval and the REPL differ in who calls, whose scope, how long definitions live, and what happens on error:

| | eval | REPL |
|---|---|---|
| Caller | running code | a person at the top level |
| Scope | the caller's, or a namespace passed in | one session-wide top level |
| Definitions live | per call / the namespace passed | across entries |
| Input completeness | not a question | the core problem |
| Result | returned to the caller | displayed, kept as `ans` / history |
| Errors | propagate to the caller | caught by the loop; the session continues |
| Rule relaxations | none | expected (redefinition, bare-expression display) |
| Security posture | sandboxed code inside a program | the user owns the session |

- **Julia** builds the REPL as `eval(Main, parse(input))`, eval targeting one persistent module.
- **Python's** console is `codeop` completeness + `exec(compile(src, 'single'))` into one persistent dict + `sys.displayhook`.
- **Cling/Clang-Repl** did it in the other order. The incremental interpreter came first, and programs later called into it (`gInterpreter->ProcessLine`).

madc's `eval_*` is Python's `eval(src, {ctx})` with a fresh dict per call. That is the right shape for C: compiled code cannot reach a caller's locals, so they are captured at parse time. It carries sandbox policy (fork-per-invocation, dlfcn restrictions) that a REPL must not inherit.

Decision proposed by this cross-reference:
- **One new core, an incremental session.** A chunk is compiled against a persistent namespace, committed or rolled back as a unit, lowered to its own MIR module linked into one live context, run, and its value captured.
- **The REPL is a client** that adds completeness, display, history, commands and the `--std=`-gated relaxations.
- **`eval_*` is unchanged.** Letting eval target a named session (Julia's `eval(Module, …)`) is a later, additive option, never a dependency.

## 41. Revised Phase 0 and first slice

Phase 0 is engine proof, each piece independently testable:
1. **Input classifier:** complete / incomplete / invalid over a corpus (braces, parens, templates, lambdas, strings/comments holding delimiters, preprocessor lines), built on the parser and `DelimDepth`.
2. **Persistent session proof:** entry 2 calls a function and reads a global defined by entry 1, through a second MIR module linked into the live context.
3. **Atomic rollback:** a rejected entry leaves no symbol, type, overload or module behind; the next valid entry behaves as if it never happened.
4. **Result capture:** display for scalars, `char*`, pointers, enums and structs through one owner.

Gate: the §24 gate, run as a scripted transcript that replays byte-identically (the 2026-09-21 gate).

### 41.1a The input classifier, designed against the code (2026-09-25)

**Verdicts** (an enum, not strings):
- `Complete`: run it.
- `CompleteExtendable`: a finished top-level `if` with no `else`. D11 waits one line for it.
- `Incomplete`: keep reading.
- `Invalid`: show the diagnostic, drop the entry and return to a clean prompt.

**The precedents agree on one criterion: the first error lies at the end of input.**
- JuliaSyntax tags an error node that starts past the last byte as `incomplete`.
- Python's PEG parser raises `_IncompleteInputError` when the failure is at `ENDMARKER`.
- IPython's `check_complete` and Cling's `InputValidator` check brackets first, then compile.

madc uses the same two stages. Both run on the real lexer and parser; there is no second lexer or parser.

**Stage 1: lexical, in the lexer.** Lex the pending text.
- End-of-input conditions are *incomplete*: an open block comment, an open conditional group, a trailing `\` splice. The lexer reports them as a cause on the diagnostic, never as a message to match.
- A literal cut by a new-line is *invalid*: a C string cannot continue on the next line (Julia's strings can; C's cannot, and this is the stated adaptation).
- Then `DelimDepth` runs over the entry's tokens, with the Program's lookup deciding `<`:
  - an unclosed `(` `[` `{` is *incomplete*;
  - a closer that matches nothing is *invalid*.

Balance first keeps stage 2 honest. Once the delimiters balance, the parser can only reach the end of the entry at the entry's outermost level, where few sites fail: the statement dispatch, a declaration's tail, a missing operand, a statement header.

**Stage 2: grammatical, in the parser.** Parse the balanced entry with an *end-of-entry token* appended (Clang-Repl's `annot_repl_input_end`, Python's `ENDMARKER`).
- The first error decides (Julia). An error that consumed the token, or cites it, means *incomplete*. `x +`, `if (c)`, `do {}`, `template <class T>` and `int x =` all end that way.
- Any other error means *invalid*. For example, `int x = 5 5` fails at the second `5`, not at the end.
- A parse with no error is *complete*.

**The optional final `;` (D11)** belongs to the one owner of a statement's terminator (clang's `ExpectAndConsumeSemi`). At the end-of-entry token it accepts the missing `;` only for an entry with a value to show (D10): an expression statement or an object declaration.
- A function declarator or a bare type definition still needs its `;` or body. Julia's `function f(x)` and Python's `def f(x):` are incomplete without a body.
- So `int f(int a)` followed by Enter waits for the `{` that Allman / GNU style puts on the next line.
- Likewise `struct P { … }` waits for its declarators or `;`, which C allows on the next line.

**Prerequisites found by building on the code** (each fixed in its own commit with gcc/clang-oracled tests, `CHANGELOG.md` [Unreleased]):
- Done:
  - an unterminated `/*` was accepted;
  - a literal cut by a new-line was accepted;
  - three divergent escape decoders held silent wrong values;
  - multi-character constants were wrong;
  - a C character constant was typed `char`;
  - D18 had regressed if/switch init-statement scope;
  - an unterminated `#if` was accepted.
- After these the whole JIT suite ran green (1648/0/0, 9 skipped).
- Also done: a template instantiated mid-expression moved the outer parse's current token (`012f2dfce`).
- Done (`8b624a758`, P1): **statements own their `;`**. `{ x = 3 }`, `break }`, `do {} while (0) return 0;`, `g(1));` and a lone `);` had been accepted in every mode, because the expression engine stops at a closer and no statement parser checked its terminator. The optional-`;` relaxation lives in that one terminator owner (`require_statement_terminator`).
- Done (`2bcd34fc8`): **an expression ends before a juxtaposed operand.** The engine read on past `3 4` and let a later operator bind the pair (`int x = 3 4 +;` ran as `3 + 4`). "The first error decides" needs the error where the grammar breaks. The list readers now own their separators.

**Where the verdict is tested in Phase 0:** a corpus run through `classify_entry` on a fresh `Program` per entry, needing neither persistence nor rollback. The session calls the same function inside the entry transaction once §41.2 and §41.3 land.

**Built (2026-09-25):**
- `ParseMode::InteractiveEntry` is the mode, off by default.
- `Program::classify_entry` returns an `EntryClassification` (verdict, deciding diagnostic, `shows_value`).
- `Diagnostic::cause` (`madc::diag_cause`) carries `end_of_input` from the lexer's refusals and from the entry parse.
- The corpus is `tests/unit/test_repl_input.cpp`.
- Not yet, and named here:
  - top-level statements under `--std=c*` / `--std=c++*` in interactive mode (a D3 relaxation; `--std=madc` has them already);
  - a discarded `if constexpr` branch that omits its final `;`.
- Done since (`9cc3dfe49`): an `enum {…}` definition's missing `;` is refused in file mode too, and reads Incomplete as an entry. Four enum defects found on the way were fixed in their own commits: the typedef that dropped the tag, `packed`, `sizeof(enum X)`, and enums past 32 bits.

### 41.2a The persistent session, designed against the code (2026-09-25)

**The core is `InteractiveSession`** (`include/madc_session.h`, `src/madc_session.cpp`). It owns one `Program` in `ParseMode::InteractiveEntry` and one live MIR context. §40 says the REPL is a client of this core, so the core is not named after the REPL.

**What the code does today:**
- `Program::tokenize_buffer` runs `_tokenizer_init()` and makes a new `tkProgram` on every call. `tkProgram` holds the program's variables and functions, and the init also clears the included-files list and the auto-include state.
- `Program::parse` runs `_parser_init()` on every call. That registers the builtin functions, globals and namespaces.
- `CirJitSession::build` tears the MIR context down and translates the whole Program into one module.
- MIR is already incremental. `MIR_load_module` queues a module, `MIR_link` links the queue, and imports resolve against every module loaded earlier (`third_party/mir/mir.c`, `modules_to_link`). The MIR cache lane loads and links two modules in one context today.

**Program side:**
- `begin_interactive_session`: the one-time init, done once per session. It runs `_tokenizer_init()`, creates `tkProgram` and runs `_parser_init()`.
- `parse_entry(text)`: lexes the text as one more unit into the same Program, with the end-of-entry token appended. It then runs the top-level parse loop up to that token.
  - It keeps macros, includes, types, symbols and `tkProgram`.
  - It skips script mode's `finalize_script_main` (D25).
- `parse()` and `parse_entry()` share the top-level loop. Only the start and the end differ.

**Builder side:** every entry is translated as a whole program, with one change. Anything an earlier module in the live context already defines is emitted as a declaration:
- a global becomes `extern T g;`, with no initializer;
- a function keeps its Pass 1 prototype and loses its body.

The set is `Program::session_defined`, keyed by emitted symbol. After each link, the session fills it from the exported items of the module it just linked, the way the cache lane fills `mir_cache_exports`.
- This is not merged with `mir_cache_exports`: there the consumer module wins every overlap, and here the earlier module does, until D5 redefinition exists.
- Skipping at translate time also keeps entry N from recompiling every body of entries 1 to N−1.

**JIT side:** `CirJitSession` gains an append mode. The context is initialized once, then each entry's module is loaded and linked into it, and the context is never torn down between entries. A per-module init (`tu_init_name`) needs a unique name per entry.

**Slices,** each its own commit with a unit test on the production entry points:
1. **C declarations persist.** Entry 1 is `int g = 5; int f(int a) { return a + g; }`. Entry 2 is `int h(void) { return f(2) + g; }`. The host calls `h` and gets 12, through two modules in one context. The test also writes `g` between the entries, which proves entry 2 reads entry 1's storage and not a copy. This is the §41.2 gate.
2. **Statements run** (D25). An entry's statements lower into `__madc_entry_N` in that entry's module, and it runs once. `int x = 10;` then `x * 2` gives 20 through the host.
3. **C++ vague linkage.** Template instantiations, inline functions, frozen-header bodies, vtables and synthesized destructors that a later entry would emit again go through the same `session_defined` filter at their definition sites.

**Static definitions:** `static int s;` or a `static` function in one entry is module-local in MIR, so a later entry can't import it. Re-emitting it in a later module would silently give that module a fresh copy. The first slice therefore refuses a file-scope `static` in an entry, with a clear message. The REPL rule for statics is settled with D6.

**Built (2026-09-25), slice 1:**
- `InteractiveSession` (`include/madc_session.h`, `src/madc_session.cpp`).
- `Program::begin_interactive_session`, `parse_entry` and `lex_entry`. `parse()` and `parse_entry()` share `parse_toplevel`, and `tokenize_buffer` and `lex_entry` share `lex_unit_text`.
- `Program::session_defined`. The builder reads it through `CirBuilder::session_defines` at the global declaration pass, in `collect_global_ctors` and at the function roots split.
- `CirJitSession::begin_live` and `append`. `find_item` is now the one lookup behind `function_code` and `data_address`.
- `MADC_SESSION_DUMP_TREE=1` dumps each entry's module tree.
- The gate is `tests/unit/test_repl_session.cpp`, under `--std=c17` and `--std=madc`. Its oracle is the same entries compiled as separate translation units and linked by gcc and clang: `6 12 22 11` from both.
- Under `--std=madc` a function has C++ linkage, so the host looks it up by its Itanium name.
- Found on the way, for slice 3: host-callback trampolines come from `host_callback_regs`, not from the roots, so each module would re-emit them.
- A refused entry can leave its declarations in the Program. Rollback is §41.3.

**Built (2026-09-25), slice 2, statements run:**
- An entry's statements, in source order, go into `void __madc_entry_N(void)`, which the session calls once the entry's module links (`InteractiveSession::run_entry`).
  - It is made at the entry's first statement (`Program::ensure_entry_function`). Its registration is script mode's `main`'s, and both now go through `Program::synthesize_function`.
  - N counts every run made, so a refused entry's number is never reused.
  - Its plain name needs no linkage flag, since only a parsed declaration mints a C++ symbol.
  - A later module declares it and never runs it again (`session_defined`).
- **Order.** The module init runs before the entry's run, so a global declared after the entry's first statement would initialize ahead of the statements before it.
  - clang-repl runs an entry in source order: `step(1); int y = step(2); step(3);` gives 1, 2, 3.
  - `Program::place_entry_initializers` leaves a `TokenGlobalInit` in the run for each such global. `CirBuilder::global_init_in_place` moves the global's dynamic-init group there from the module init. A constant initializer has no group, since its storage holds the value from load.
  - A global declared before the first statement keeps the module init, which already runs first.
- **Declarations persist (D25), `:=` among them.** A top-level `:=` declares a session global (`Program::short_declaration_scope`), and so do the receivers of `a, b := f();`, which the statement then assigns.
  - This needed a fix on the way (`fbf4efae7`): a file-scope `:=` outside script mode recorded no declaration, so it got no storage. `Program::record_global_top_decl` now owns that record.
- A top-level `defer` binds to the entry's run and runs when the entry ends.
- An entry's own text is the TU origin (`lex_entry` sets `tkProgram->source`). A statement from a header the entry includes is refused, as script mode refuses it.
- `argc` / `argv` do not resolve in an entry. D25's `%run` hands a program's argv to its main.
- Call statements run under `--std=c17` too, because they are classified by their parse result. (This slice's text said a C standard's `x = 3;` waited for slice 2b; it did not, since an assignment is classified by its result as well. See slice 2b.)
- Gate: `tests/unit/test_repl_session.cpp`, 9 cases.
  - The clang-repl oracle (`tmp/repl/s2/order2.repl`) gives `log=123 y=2`, `log=12345 w=4` and `x=30`, and the session gives the same.
- Known, and named here:
  - A refused entry's run stays registered by name, inert because it never reaches the builder's queues. §41.3 removes it with the entry's other leftovers.
  - Like every root function, the entry's run gets a host-call shim.

**Built (2026-09-25), slice 2b, a C or C++ standard's top-level statements (D3):**
- A session runs top-level statements under every standard: an assignment, a call, `if`, `for`, `while`, `do`, `switch`, a block and a label, under c89, c99, c17 and c++17 as under madc. The route is the post-parse classifier (`script_statement_result`), which is not gated on the dialect.
  - The pre-parse starter (`file_scope_statement_starter`) stays madc-only. In a session, arming it would add nothing: `argc`/`argv` do not resolve in an entry, and `:=` is the dialect's.
  - Gate: `tests/unit/test_repl_session.cpp`, "top-level statements run under every standard (D3)". Oracle: the same statements in a function body, gcc `-std=c89`/`c99`/`c17` and g++ `-std=c++17`: `3 6 100 105 100 1 41`.
  - clang-repl-18 cannot serve as the C oracle: in C mode (`-Xcc -xc`) it keeps no declaration from one input to the next (`int y;` then `y = 4;` is "use of undeclared identifier 'y'"). It also prints no values yet ("Not implement yet."). For C, the oracle is gcc on each entry's statements in a function body.
  - clang-repl-20 (20.1.2, installed 2026-09-25) keeps C declarations, and it agrees with this slice's per-standard reading. After `int y = 0;`, `y = 4;` is an assignment. Under `-std=c89`, an undeclared `x = 3;` is "use of undeclared identifier 'x'", and an undeclared `f();` is an implicit declaration and a call (`tmp/repl/s2b/c89.repl`). One gap: an uninitialized `int y;` fails every later input with "Duplicate definition of symbol 'y'".
- **The classifier reads the grammar's verdict** (`baac688d5`). It had recognized an expression statement by the token at the top of its tree, and a cast (`ttBase`) was not on its list. So `(void)f();` in an entry was dropped under every C and C++ standard, and `static_cast<void>(f());` in a madc script and every C++ entry. It now reads the terminator the statement owes (`StatementTerminator::Expression`, recorded by `parseExprStmt`, kept by `parseStatement` as `last_statement_terminator`).
  - An audit over the JIT suite (old list against the verdict) found one arm that hid it: a namespace-qualified statement re-entered `parseStatement` for the rest of itself. It continues in `parseStatementBody` now, and its extent starts at the namespace name.
- **A delete statement is an expression statement** (`cccf592c2`). `parseKeyword` recorded no terminator for it, so `delete p }` compiled, and a top-level `delete p;` was dropped (its destructor never ran).
- **C89's implicit-int reading, decided per standard:**
  - A declared name's `x = 3;` is an assignment in a session under every standard, K&R-era C included. gcc's file scope reads it as a redeclaration (`int x = 3`), which in a session would be a D6 redefinition; Julia and IPython assign.
  - An undeclared `f();` under a K&R-era standard (c78 to c17) keeps C's reading, an implicit function declaration and a call. madc does not read implicit-int *data* declarations in file mode either (BUGS.md B12); the session inherits that fix when it lands, for undeclared names only.
- Found on the way, and filed as off the REPL's path: B12 (a file-scope `y = 4;` after `int y;` is silently dropped in C), B13 (`int(f(3));` read as a declaration), B14 (`:=` accepted under every C and C++ standard; in a C session `g := 3;` silently leaves `g` alone), B15 (a `for`-init declaration accepted under c89).
- **Found on the way, ON the REPL's path:** an entry that failed to link poisoned the live context. It was the next step, and it is built below (§41.3's JIT half).
- For slice 3: an OUT-OF-LINE member body (`S::~S() { … }`) is re-emitted by every later entry too ("func _ZN1SD2Ev is prohibited for redefinition"), so the `session_defined` filter is needed for every class member function, not only inline and synthesized ones. That refusal is now a clean one ("multiple definition of 'S::~S()'"), no longer poisoning, but the entry is still refused.

**Built (2026-09-25), §41.3's JIT half, pulled ahead of slice 3.** A refused entry leaves the session as it was: nothing of it is live, and the next entry links as if it had never been tried. Three carriers of poison were found and fixed, each in its own commit.
- **The MIR context** (`213b335aa`). `MIR_load_module` registers a module's exports before `MIR_link` can fail, and a failed link leaves the module queued, so after `int f(void);` and a refused `f();`, every later entry was refused too (the next load: "func __madc_entry_1 is prohibited for redefinition"), with no diagnostic.
  - `MIR_module_link_check` (madc's MIR subtree) answers from the context's own tables whether a module would load and link, and changes nothing. It shares both rules with the load and the link (`func_redef_prohibited_p`, `import_binding`).
  - `CirJitSession::admits` asks it before the load. A failing module is never loaded, never joins `live_mods`, never becomes `mod`.
  - Each failing symbol is a diagnostic on the entry, in ld's words: "undefined reference to 'f'", "'f()'" for a C++ symbol (demangled, as ld shows it), "multiple definition of …". A failed translation and a MIR fatal past the check record one too.
- **The Program's emission lists** (`ad28db168`). A refused entry's definitions stayed in `pending_funcs` / `top_decls`, and the session filter knew only live exports, so every later module defined them again. A function written before a parse error came alive in the next module. A body that could not compile or link refused every later entry.
  - `InteractiveSession::refuse` runs for every refusal (parse, translation, link). `Program::withhold_entry_definitions` records the entry's own definitions by identity (`session_withheld`), over the extent `parse_entry` measures. `CirBuilder::session_defines` then declares them and never defines them, so a later use is refused by name.
  - Not withheld: vague linkage (an instantiation from a refused entry is defined by the next module that needs it), internal linkage, and a header's definitions (the header stays included).
  - This is scaffolding for the rest of §41.3, the Program rollback, which deletes it.
- **The c2mir context** (`2dd68fe93`). `c2mir_compile_tree` returned the context's lifetime error count, so after one refused compile, every later entry "did not compile". It now answers for its own tree.
- **Numbering** (`eb51551ac`). `REPL[N]` counted linked entries, so the entry after a refusal took the refused one's name. N now counts every submission (Julia's `REPL[N]`, IPython's `In [N]`).
- The oracles agree. clang-repl-18 and -20 (`tmp/repl/s2b/linkfail.repl`): "Symbols not found: [ _Z1fv ]", then `k=3`, then `f=7`. clang-repl-20 `-xc -std=c89` (`tmp/repl/s2b/c89.repl`): an undeclared `f();` is refused, then `g=7`. The session gives the same under c89, c17, c++17 and madc.
  - cling doesn't answer the case. It wraps a lone `int f();` input into the run as a block-scope declaration, so the later `f();` is "undeclared".
- Gates:
  - `test_c2mir`: the check refuses both kinds, and the context stays intact.
  - `test_cir`: a tree compiles after a refused one.
  - `test_repl_session`: 15 cases.
  - "Is this diagnostic an error" has one owner, `Diagnostic::is_error` (`check-one-error-diagnostic-scan.sh`).
- **What was left of §41.3:** the Program rollback. It is built below ("Built (2026-09-26), §41.3's Program half").
- **Still open in §41.3:**
  - A MIR fatal after the check (an internal error in the load or link) still leaves its module queued.
  - A link diagnostic carries no position (`REPL[2]:0:0`); naming the first use would need MIR's items to carry a location.
  - c2mir's own messages are not captured as rows.

**Built (2026-09-26), slice 3: C++ vague linkage and every member body.** A class, its members and its instances can be spread over any number of entries, as in clang-repl. Each fix is its own commit with a `test_repl_session` case; the oracle for each is clang-repl-18 and -20 (`tmp/repl/s3/*.repl`), which agree on every sequence.
- **The loader keeps the first linkonce copy** (`938fada4d`, madc's MIR subtree). Each entry's module is a whole TU, so it emits its own copy of every linkonce definition it needs: C2/D2 aliases, synthesized and deleting destructors, vtables, type_info. That is correct TU behaviour, and the builder already marks them linkonce. The in-process loader ignored the binding: a later func copy was "prohibited for redefinition", and a later data copy took the name over, which split type_info identity (`typeid(*bp) == typeid(C)` false across entries). `MIR_load_module` now binds a LINKONCE or WEAK duplicate to the definition already there, and `MIR_module_link_check` agrees. The same fix mends the `--project` JIT, whose TUs had the same split (`testprojectvague`: `same=0 c2=1` before, g++ `same=1 c2=2`).
  - This is the layer the plan's first sketch (a `session_defined` filter at each definition site) would have hidden; a filter cannot give two modules one address. `session_defined` keeps its job for strong definitions.
- **Two synthesized destructors marked linkonce** (`68bd5c327`): the complete-object destructor of a class with virtual bases, and the array-destroy helper. g++ emits the first weak.
- **An entry lexes like a file** (`2ad9728d9`). A file lexes all its text before parsing, so its classes reach the parser as identifiers, resolved by lookup. A session lexes each entry after earlier entries registered their types, and the lexer read them: every out-of-line member defined in a later entry was refused, and so was a local shadowing an earlier class. `Program::lexer_type_token` is the one answer, and in a session it knows only madc's own (`builtin`) types.
- **A qualified expression is a statement at an entry's top level** (`9d71ce881`): `S::count = 3;`, `S::bump();`. Gated on the REPL's parse mode (`entry_top_level_statement_at`); a file's top level still refuses it, as g++ does.
- **An entry emits an inline body only where it is used** (`431ee5ef1`), as every C++ TU and clang-repl do. An unused in-class member naming a static member a later entry defines no longer refuses its own entry. Gated on the REPL's parse mode; a file's bodies stay roots.
- cling reads these sequences differently (it wraps each input into a function), so it gives zeros and cannot serve as the oracle here.
- Not yet, and named:
  - Host-callback trampolines are strong and re-emitted per module, but a session has no `register_function` yet. When it gains one, the trampoline becomes linkonce, as its sibling call shim is.
  - B17 (a template's static data member is never defined), B18 (a file-scope lambda global), B19 (`int N::f()` out of line) and B20 (script mode's twin of the qualified statement) fail in file mode too; they are in `BUGS.md`. B21 and B22 are there too.
  - **Decided (owner, 2026-09-26; D27):** a strong definition naming an undefined symbol (`int g2() { return later2; }`) is accepted, and the refusal waits for first use, as in Julia and clang-repl. Built 2026-09-26 (§42 D27, "Built").

**Built (2026-09-26), §41.3's Program half: a refused entry leaves nothing behind** (`abd480473`, on the nesting of `b7401343c`). Each entry runs inside `Program::EntryTransaction`, from its lex to its link. The transaction commits once the entry's module links, and rolls back on any refusal: its parse, its translation, or its link.
- **The rule, from Julia.** An input that fails to parse leaves nothing behind in Julia, and a refused entry is ill-formed C or C++, so it leaves nothing either. A run that fails after the module linked is Julia's run-time error: the definitions stay, and the rollback does not apply.
- **What is rolled back:**
  - the registries a class body can write (types, functions, templates, overloads, namespaces, aliases), through the class journal in its `Entry` role;
  - the macro tables (journaled);
  - the include bookkeeping (included files, include guards, lazy surfaces, `#pragma push_macro`), saved whole because each holds a few entries;
  - the using-directives and inline-namespace children;
  - every entity a definition changes in place, saved at its first change (`Program::journal_entity`): an earlier declaration's object (`extern int x;`, a static member), its function (C's `int f(void);` then its body), and a forward-declared aggregate its definition completes.
- **Nesting.** The class journals opened inside an entry (pattern capture, pattern instantiation) nest in the entry's. Every registry's transactions now nest (`b7401343c`). The entry's journal is not counted in `class_registration_journal_depth`, so the class journals inside an entry behave exactly as in a file.
- **Deleted:** `session_withheld` and `withhold_entry_definitions`, the scaffolding that kept a refused entry's definitions out of later modules.
- **Oracles** (`tmp/repl/s4/rb.repl`, `inplace.repl`):
  - cling rolls a refused input back whole, gives `dv=4 ovd=4.5 sf=16 ev=8`, and the session agrees.
  - clang-repl-20 rolls declarations back, but it keeps a refused macro (`N=4`) and include guard, and it leaves a rolled-back namespace's symbol behind ("definition with same mangled name '_ZN1Q1vE'").
  - clang-repl-20 also drops the earlier declaration a refused definition named, and keeps a refused out-of-line member defined, then crashes on the next input.
  - madc follows Julia and cling and copies none of clang-repl's behaviour here (owner, 2026-09-26: don't model problematic cling/clang-repl behaviour where Julia offers a superior model).
- **Gates:**
  - `test_repl_session` "a refused entry leaves nothing behind (§41.3)": 19 kinds under C++17 and 10 under C17. In each, a later entry redeclares the refused entry's name as another kind, or gives a corrected definition.
  - The nesting cases in `test_class_pattern`, `test_stringpool` and `test_libmadc_dis`.
  - A scratch census of every Program container (`tmp/repl/s4/sizeprobe`) finds a refused entry leaves only interned file names and the parser's last-skipped-template scratch changed.
- **Found on the way, ON the REPL's path, and fixed** (`0d06017b8`): under a C++ standard, `#include <string>`, `<cmath>`, `<cstdlib>`, `<algorithm>` and `<iostream>` each refused its entry. The D6 check meant for a static the entry writes counted a header's `static` definitions too (glibc's byte swaps, `<iostream>`'s `__ioinit`). Every entry's module is a translation unit, so a header's statics are that module's own copy. Only the entry's own statics are refused now. clang-repl-18 and -20 give `r=7 s=abc`, then `r2=9` (`tmp/repl/s4/hdr.repl`), and so does the session.
- **Found off the path:** B23 (`(int)std::floor(4.5) + 3` is refused, in file mode too).
- **Not yet, and named:**
  - A cache that maps a name to an entity outside the journaled registries would resurrect a rolled-back entity. None is known; the census found none by size.
  - A value changed in place, not through a registry and not at a definition site that journals, is not saved. The census cannot see one.

**Thread contract:** one session is driven by one thread. Concurrent clients go through the serialized verbs of D9.

First slice: §37 items 1–6 in the CLI interactive session only (D20: `madc`, `madc -i`). Items 7–10 depend on the completion service, the madcide panel, F-keys and a surviving program session, and follow in that order.

### 41.4a Result capture, designed against the code (2026-09-26)

- **Where the value comes from.**
  - The parser already records an entry whose final statement omitted its `;` (`Program::entry_final_semicolon_omitted`, D11). `parse_entry` then wraps that statement in a show:
    - a final expression statement becomes `__madc_show(expr)` in the entry's run;
    - a final object declaration (`int x = 5`) appends `__madc_show(x)` to the run, after the object's initializer.
  - `__madc_show` is a compiler-implemented intrinsic, as `php::print_r` is (`FuncDef::inline_builtin_kind`). The session declares it under a reserved name, so no user name collides.
- **One owner of the rendering: the dump walk, with a third flavor.**
  - `CirBuilder::dfShow` joins print_r and var_dump in `src/cir_dump.cpp`. The walk is shared: the type dispatch, the member census, the access rebuilds.
  - Its primitives, `__madc_dump_sh_*` in `src/rt/rt_dump.c`, write D10's re-enterable spellings into the same sink.
  - A floating value takes `rt_format.c`'s shortest round-trip digits, the `std::format` `{}` engine. A `.0` is added when the digits alone would read as an integer.
- **The spellings (D10: the text, entered again, yields the value):**
  - an integer: decimal (`30`, `-9`);
  - `bool`: `true`, `false`;
  - `char` (the walk's one byte kind, as print_r's): `'a'`, with C's escapes (`'\n'`, `'\''`, `'\\'`, `'\0'`, `'\x7f'`);
  - floating: `1.0`, `0.3333333333333333`, `1e+100`, `2.5f` for a float, an `L` suffix for a long double, and `INFINITY`, `-INFINITY`, `NAN`;
  - `char *`: `"a\"b\n"`, with C's escapes; a null one is `NULL` in C and `nullptr` in C++;
  - any other pointer: `(int *) 0x7ffd5c1a2b3c`, or `(int *) NULL` / `(int *) nullptr`, and it is never dereferenced (§6.4);
  - an enum: its enumerator (`B`; `Color::Red` when scoped), or `(enum E) 3` in C and `(E) 3` in C++ for a value that names none;
  - a struct (slice 2): `(struct Point){ .x = 1.0, .y = 2.5 }` in C and `Point{ .x = 1.0, .y = 2.5 }` in C++, nesting inline;
  - an array (slice 2): `(int[3]){ 1, 2, 3 }` in C and `{ 1, 2, 3 }` in C++;
  - a madc `var` (slice 3): its dialect literal;
  - `void`: nothing.
- **Where the text goes.**
  - The show walks into a capture sink and hands the sink to the session (`__madc_session_show`). The session records the text on the running entry.
  - `InteractiveSession::shown()` returns it, and is empty when the entry shows nothing. The core still renders nothing: the REPL prints the text, and madcide's panel shows it.
  - A show that stops at an undefined reference (D27) shows nothing.
- **Oracle.** The installed clang-repl-18 and -20 print "Not implement yet." for a value, and cling and Julia are not installed. D10's own rule is therefore the oracle: each test enters the shown text again in a later entry and compares it with the original value.
- **Slices:**
  1. The intrinsic, its synthesis, the session's capture, and scalars, `char *`, pointers and enums.
  2. Structs, classes and arrays. A temporary is materialized once, so it is not re-evaluated per member.
  3. A madc `var`, the containers, and `ans` (D12).
- **Thread contract:** unchanged. One session is driven by one thread, and a capture belongs to the entry running on it.
- **Built (2026-09-26).**
  - **Slice 1** (`bca43a621`):
    - The parser records the kind it waived (`entry_final_owed`), and `Program::show_entry_value` wraps the final statement.
    - `CirBuilder::lower_show_call` and the `dfShow` arms of `dump_scalar`, `dump_enum` and `dump_any` do the walk. `dump_show_pointer` shows a pointer without following it, and `dump_show_type_word` spells its type, a function pointer's from its target's signature.
    - The runtime's `__madc_dump_sh_*` write the text, and the session reads it through `__madc_session_show` and `InteractiveSession::shown()`.
    - `rt_format.c`'s shortest digits are split out for float and long double, with no change to `std::format`'s output.
    - The C-literal escape rule moved into the runtime as `__madc_c_escape`, which `madc_c_escape_string` wraps and `check-one-c-escape.sh` gates.
  - **Slice 2** (`0346fc5c1`):
    - `dump_struct` and `dump_array` gain the show frame through their existing walk.
    - Two primitives: `__madc_dump_sh_chars` (text or a brace list of chars) and `__madc_dump_sh_sep`.
    - A C-style struct returned by a call is materialized once into a local.
  - **Gate:** `test_repl_session`'s D10 cases, C++ and C. Every shown text is entered again and compared with the value, except the 2-D compound literal, which madc refuses re-entered (B28).
  - **Found on the way** (`BUGS.md`): B28, the 2-D compound literal, and a second case of B23, `std::signbit(x) && …` misread as a label address.
  - **Slice 3, the values** (`a64c5f182`, `8e387433d`):
    - A madc `var` shows as its dialect literal through the runtime value walk (`show_value`): `{ 10, 20 }`, `{ "k": 1 }`. A null shows nothing.
    - A standard container shows as the C++ expression that builds it, through `dump_sequence` and `dump_iterator`: `std::vector<int>{ 1, 2, 3 }`, `std::map<int,int>{ { 1, 10 } }`, `std::set<int>{ 1, 3 }`. A `std::string` shows as its text.
    - A C++ class is named without C's `struct`.
    - Fixed in the new code: a header-typed declaration placed the entry's run at the header's token, so the run was dropped and the entry refused with no diagnostic.
    - Found off the path: B29 (whether a number-kind `var` takes arithmetic, an owner question), B30 (two initializer-list constructions refused), B31 (`std::vector`'s `operator==` refused).
  - **§41.4 is done.** `ans` (D12) changes name binding: a new binding per showing entry, re-typed each time, as Julia rebinds it. So it is its own item, with `_`, `__` and `_N`, and is not part of the display.

### 41.5a The command-line front end (D20), designed against the code (2026-09-27)

**Measured first** (the build box, 2026-09-27):
- `bin/madc` with no arguments prints a stale one-line usage (`src/madc.cpp:1765`) and exits 0. `bin/madc -` refuses "`-:0:0: error: Failed to open file`", so there is no stdin program today.
- gcc 13: `gcc`, `gcc -E` and `gcc -o out` with no input print `gcc: fatal error: no input files` / `compilation terminated.` and exit 1.
- python3 3.12 `-i`, stdin piped:
  - it prints its banner and every prompt to stderr, values to stdout;
  - it exits 0 at EOF, even after an error;
  - `sys.exit(3)` exits 3;
  - an entry still incomplete at EOF is submitted and refused with its own diagnostic;
  - `-i -c 'y = 42'` leaves `y` for the prompt.

**Where the decision is made: the tail of `main()`.** Every mode that needs no program file has returned by then: `--version`, `--help`, `--capabilities`, `--run-frozen`, `--dump-forest`, `--project`, `.o` inputs. What reaches the tail is the file lane (`filearg < argc`) and today's usage line. D20 replaces the usage line and adds two options to the loop:
- `-i` / `--interactive` sets `interactive`. madc matches options exactly, so it cannot collide with `-I` / `-isystem` (D20).
- A file argument `-` names stdin (gcc, python). It stays a positional, so `madc - a b` hands `a b` to the program.

With no program file, the tail chooses in this order:

| Case | Action |
|---|---|
| an artifact or dump request: `-o`, `-c`, `-shared`, `-r`, `--emit-object`, `--emit-executable`, `--emit=`, `--emit-pch`, `--emit-function`, `-E`, `--dump-source`, `-dM`, `--dump-cir*`, `--dump-nodes`, `--dump-registered`, `--freeze*`, `--pack-forest` | `madc: fatal error: no input files` / `compilation terminated.`, exit 1 (gcc) |
| `-i`, or stdin is a terminal | the REPL |
| otherwise (stdin piped) | the stdin program: compile and run stdin |

- The artifact test is one predicate over the flags the loop already sets. Each flag names what to do with a translation unit's output, so none applies to a session. Language and configuration options (`--std=`, `-stdlib=`, `-I`, `-D`, `-l`, `-O`, `-g`, `-w`, `--config=`, `--show-stats`, the forest flags) apply to the REPL as they apply to a file (D20).
- `-i file` (slice 2 below) runs the file first, then the REPL.
- `-i -` is `-i`: `-` names stdin, and `-i` reads it as entries.
- A cross build refuses the REPL as it refuses the other run lanes (`cross_refuse_run`, §4.1).

**The stdin program is the file lane with another reader.** The lane's one read of its input is `prog->tokenize(argv[filearg])`. For `-`, it reads stdin whole and calls `tokenize_buffer(text, "<stdin>")`, the buffer lane libmadc already uses. Everything after it is unchanged: parse, `-E`, `--emit=`, `-c` / `-o`, and `madc_cir_execute`. `madc_cir_execute` uses the source name only to name the module, never to reopen the file.
- Diagnostics cite `<stdin>` (gcc's name). The program's `argv[0]` is `-`, the spelling that named its input.
- `--emit-pch` and `--emit-function` read their input themselves, so they keep today's "Failed to open file" for `-`.

**The session serves the CLI's own Program.**
- `InteractiveSession` gains a constructor that adopts a configured `std::unique_ptr<Program>`: the one `engine.create_program()` made and the option loop and `madc.ini` configured. `-D`, `-I`, `-stdlib=`, `--std=` and the registration policy then hold in the session exactly as in the file lane.
- `-l` libraries are opened by the existing loop before the tail, so they are in scope. The default constructor stays for tests and hosts.
- `begin()` keeps the `--std=` argument for callers that did not configure the Program.

**One entry path, with the classifier in the entry transaction** (§41.1a: "the session calls the same function inside the entry transaction").
- Today `classify_entry` runs only on a fresh Program, and `parse_entry` has no balance stage.
- A fresh Program cannot classify an entry for a live session: `x +` names a session variable, and a `<` is a name question for the session's templates.
- So `classify_entry` gains the session lane:
  - under `interactive_session` it lexes with `lex_entry`;
  - it runs the balance stage over the entry's own tokens;
  - it parses with `parse_toplevel`;
  - then it runs `parse_entry`'s tail (the internal-linkage refusals, `show_entry_value`, queueing the run).
- The verdict logic is the one function's in both lanes.
- `parse_entry` becomes that lane's accept/refuse wrapper. A close that opens nothing (`1; }`), which the parser took, is now refused as §41.1a says. An unclosed delimiter is refused as "`'(' is not closed`" before the parse.
- Session verbs:
  - `offer(text)` classifies inside the entry transaction:
    - **Incomplete** or **CompleteExtendable**: the transaction rolls back and nothing is numbered, rendered or kept; the client reads another line;
    - **Invalid**: the entry is numbered and refused;
    - **Complete**: the same transaction goes on to link and run, as `submit` does. The final Enter's parse is the one that runs, so there is one parse per line.
  - `submit(text)` is the same path for a client holding a whole entry: CompleteExtendable is Complete, and Incomplete is refused with its end-of-input diagnostic.
- The entry number is taken when the verdict is final, so an incomplete attempt never uses up `REPL[N]`. The parse cites the number it would take.
- **Rendering is deferred to the verdict.** The parse runs under `DiagnosticRenderMute`: an incomplete entry's end-of-input error must not print, and neither must its warnings twice. On Complete or Invalid, the session renders the diagnostics that parse recorded, in order, with `print_diagnostic`. Translation, link and run render as they do today.
- **The token queue starts empty.** A refusal before the end-of-entry token leaves the entry's tokens queued: a lexer refusal, an open delimiter, a parse error the parser does not recover past. The entry path drains them, so the next entry never parses a stale tail.

**The loop** is a new owner, `madc_repl_run(InteractiveSession &, std::istream &in, std::ostream &out, bool terminal)` (`src/madc_repl.cpp`). It takes streams, so the unit tests drive the production loop.
- Prompts:
  - **On a terminal:** a one-line banner (`madc <version>`, and that Ctrl-D exits) and the prompt `Program::standard_canonical_name(language_std) + "> "` (D22).
  - **Continuation lines** get spaces as wide as the prompt (D22, Julia).
  - **Off a terminal:** no banner and no prompt (D23's fallback). A transcript's output is then its values and diagnostics only. This differs from python, which prints prompts to stderr.
- Lines are read cooked, with `std::getline`; the line editor is D23's. An empty line at an empty prompt does nothing. Otherwise the pending text plus the line is offered:
  - **Incomplete:** read on.
  - **Complete or Invalid:** start a new entry, first printing the shown value (`shown()`, D10) and a new-line to `out`.
  - **CompleteExtendable** (D11): wait for one more line.
    - A line whose first word is the keyword `else` continues the entry. It is looked up in the Program's `keyword_map`, so it is the standard's `else` token (`TokenID::tkELSE`), never a spelling compare.
    - An empty line runs the entry.
    - Any other line runs it, then starts the next entry.
- **At EOF:** a pending entry is submitted, so an extendable `if` runs and an incomplete entry is refused with its own end-of-input diagnostic (python and Julia do the same). On a terminal a new-line follows. The exit status is 0 (python `-i`), and a program's `exit(n)` ends the process with `n`.
- The REPL and a program read one stdin. `std::cin` is synced with stdio (the default, which madc never turns off), so an entry that calls `scanf` or `getline` reads the transcript's next line, as python's `input()` does.
- Values go to stdout, diagnostics to stderr (their existing renderers), and the prompt to stdout, flushed (Julia).

**Thread contract:** the loop drives one session from one thread (D9), with per-instance state only.

**Gates:**
- `test_repl_session`:
  - `offer` over the §41.1a corpus shapes on a live session: `x +` after `int x`, an open brace, an open comment, a stray close, and an extendable `if` followed by `else`, by an empty line, and by another statement;
  - numbering across an incomplete attempt;
  - deferred diagnostics;
  - the drained queue after each refusal kind.
- `test_repl_cli`: `madc_repl_run` over string streams, with the §37 items 1–6 transcript (`int x = 10;`, `x * 2` shows `20`, `x * 2;` shows nothing, a function over three lines, a refused entry leaving `x`), under `madc`, `c17` and `c++17`, prompt off and on.
- `bin/madc` itself:
  - `-i < transcript`;
  - `< prog.c` and `- args`;
  - `-o out` alone ("no input files");
  - no arguments on a terminal. This one is checked by hand over `ssh -t`, since a test has no terminal.

**Slices:**
1. The tail's dispatch, `-i` / `-` / the stdin program / "no input files", the configured-Program session, `offer`, and the loop. Refused until slice 2: `-i file` ("`-i` with a program file is not supported yet").
2. `-i file`, `python -i`, the CLI form of D16's F5. The file loads into the session as a translation unit:
   - its own grammar (`ParseMode::TranslationUnit`: no entry relaxations; REPL mode is a mode);
   - script mode's synthesized `main` as script mode makes it;
   - its declarations persist on the session's Program;
   - its module links into the live context.

   Then `main` runs with the remaining positionals as its argv, and the prompt follows with its names. Its return value does not end the session. Its `exit(n)` ends the process. Runner tests follow the fixture convention with no runner change: `tests/testrepl_*.mad` + `.flags` (`-i`) + `.input` (the transcript) + `.expect`, `.exe_skip` (the REPL is JIT-only, D19).
3. `ans` (D12), its own item, on the loop slice 1 gives it.

**Built, slice 1 (2026-09-27, `d9aeff7a7`):**
- Everything above except `-i file`.
- Measured on `bin/madc` (`tmp/repl/d20/probe.sh`):
  - the §37 transcript (`t1.repl`) gives `20 20 10 1 7 8` under `madc`, `c17` and `c++17`, with one diagnostic, `REPL[6]`;
  - on a pty (`script -qfec`) it prints the banner, then `c++17> `, then `3` for `int x = 3`, a 7-space continuation, and a new-line at Ctrl-D;
  - `madc < prog.c` and `madc - a b` run stdin as the program, with `argv[0]` `-`;
  - `-o` or `-E` alone gives "no input files", exit 1;
  - `exit(4)` in an entry exits 4;
  - `scanf` in an entry reads the transcript's next line.
- Found off the path: B33 (a missing right operand refused at the declaration's `=`, in file mode too).
**Slice 2, designed against the code (2026-09-27):**
- **The file and the session are one unit** (owner, 2026-09-27).
  - Measured, `tmp/repl/d20s2`: cling 1.2 (`.L` and `#include`) and clang-repl-20 (`#include`) agree.
    - Once `a.c` is loaded, an entry calls its `static` function and reads its `static` object (`helper=10 count=1`).
    - A second file defining its own `static int count` is refused whole ("redefinition of 'count'"). Its `use_b` is undeclared afterwards, and `a.c` is unchanged.
    - A `static` typed as an entry is accepted by both. Its redefinition is shadowed by cling (`s=4`) and refused by clang-repl (`s=3`).
  - Julia's `include` and python's `-i` agree, since neither has a unit-local name.
  - So a unit's own file-scope statics, an entry's or a loaded file's, become session names. A second definition is a redefinition, refused today as any redefinition is (`int t = 5;` then `int t = 6;`). D5/D6 then decide redefinition for every name at once.
- **The mechanism: the session gives a unit's statics external linkage, where the unit is finished.**
  - `finish_entry` is the one place that finds the unit's own statics (`vfSTATIC` objects and `internal_linkage` functions from the unit's own text, never a header's). Today it refuses them. Instead it journals each one (`journal_entity`, so a refused unit restores it) and clears its internal linkage.
  - Every later consumer then sees an ordinary external entity:
    - the defining module exports it, and `session_defined` records it;
    - a later module declares it `extern` (a global) or prototypes it (a function), as for any earlier definition.
  - Without this, a later module would define the object afresh, which is a silent second copy. That is why the refusal existed.
  - A C++ static function keeps the `_ZL` name it was minted with at its declaration (`emit_symbol`), so the definition and every import agree.
  - Nothing at an emission site knows about the session.
  - A header's statics stay each module's own copy, as now.
  - `static inline` was measured before it was admitted this way, since a C99 inline definition with external linkage emits no external symbol. It needs no exception: typed as an entry and in a loaded file, under `c17`, `c++17` and `madc`, `sq(6)`, `use_sq()`, `calls()`, `use_sq()` give `36 10 2 12`, as gcc does (`tmp/repl/d20s2/probe3.sh`). The function-local `static` in `calls()` is one counter.
- **Loading a file** (`InteractiveSession::load`, the core of `%load`, D25):
  - The file is read whole. It is lexed and parsed on the session's Program in `ParseMode::TranslationUnit`, so it gets its own grammar, with no entry relaxations and script mode's `main` as script mode makes it. `lex_entry` appends the end-of-entry token only in entry mode.
  - Then `finish_entry` promotes its statics, and the unit links as its own module inside an entry transaction.
  - The CIR also runs in TranslationUnit mode. A file's bodies stay roots, and a file's undefined reference is refused at its link, as gcc's link refuses it, not late-bound (D27 is an entry's relaxation).
  - Its diagnostics cite its path. It takes no `REPL[N]`, as Julia's `include` takes none.
- **Running it** (`InteractiveSession::run_main`, the rest of `%run`):
  - When the session defines `main`, it is called with the file lane's argv: the path, then the remaining positionals.
  - It runs at the entry boundary (`cir_run_at_entry_boundary`, through a `main(argc, argv)` shim), so a use of an undefined name returns to the prompt. Then the task join, as `run_main` does.
  - Its status is not the process's: the prompt follows, as python's does. `exit(n)` ends the process.
  - A file without `main` just loads: `madc -i lib.c` is a session over lib.c's names.
- **When the file fails** (python `-i`, measured): a refused file keeps nothing, and the REPL starts anyway. A `main` that stops keeps the file's definitions, as python keeps the names defined before the failing line.
- **Gates:**
  - Runner tests, with no runner change: `tests/testrepl_*.mad` + `.flags` (`-i`) + `.input` (the entries) + `.expect` + `.exe_skip` (the REPL is JIT-only, D19). One under `--std=madc`, one C file with statics, one refused file.
  - `test_repl_session`:
    - an entry's `static` object and function reached by a later entry, with their redefinition refused;
    - a loaded unit's statics reached, and a second unit's clash refused whole.

**Built, slice 2 (2026-09-27, `cf16127a9`):**
- Everything above.
- `testrepl_cfile` gives what cling 1.2 gives for the same file and entries (`.L`, then `main(1, av)`): `main ran, argc=1`, `helper=70`, `21`, `after=51`.
- `testrepl_script` runs a `--std=madc` script's statements as its `main`, then the entries.
- Measured on `bin/madc`:
  - `-i prog.c a b` runs `main` with `argc=3` and exits 0 at the end of input;
  - `-i -c` is refused;
  - a missing file reports "Failed to open file", then the REPL starts, as python's `-i` does.
- Found on the way: five parser gates meant "this unit is an entry" but asked `interactive_session`. A loaded script's statements ran at load, as an entry's run, until they asked `interactive_entry()`.
- Found on the way, on the REPL's path, and fixed first (`d901bf91c`): after the first entry, an entry naming an auto-included header (`format`, `println`, `php::`) had its tokens spliced behind the stream's cursor. It read "redefinition of 'total'", printed nothing, or crashed. Each session unit now starts on a fresh stream (`TokenStream::reset`).
- **D20 is done.**

**Not in D20, and named:** the line editor (D23), commands (D13, D24), Ctrl-C (D8), help and shell modes (D14, D15).

### 41.6a Result names (D12), designed against the code (2026-09-27)

**Measured first** (`tmp/repl/d12`). Julia 1.13.0 and IPython 9.17.1 were installed on the build box for this and run on a pty (`script -qfec`), since piped stdin gives Julia no REPL:

| | Julia `ans` | IPython `_`, `__`, `___`, `_N` |
|---|---|---|
| a shown value | kept | kept |
| a value hidden by `;` (`3+4;`) | kept: `ans` is 7 | not kept: `_` unchanged |
| no value (`println("hi")`; Python's `x = 5`) | `ans` becomes `nothing` | unchanged |
| an error | unchanged | unchanged, and the input number advances |
| read in a function body (`f() = ans + 1`, `100`, `f()`) | at the call: 101 | at the call: 101 |
| the user assigns the name (`ans = 10`) | the user's name wins from then on | the user's name wins from then on |
| `_N` for an input with no output | (none) | `NameError` |
| a mutable object changed after it was shown | seen: the result is the object | seen: the result is the object |

- IPython's `__` and `___` count shown values, not inputs: after outputs 2, 42, 2, `__` is 42.
- madc today: `ans`, `_` and `_1` are "use of undeclared identifier".

**The rule: a result is the value an entry showed, kept as a copy.**
- An entry that shows a value keeps it (D10: its final statement has no `;`), and only such an entry. This is IPython's rule, and one rule serves all five names.
  - Julia also keeps a value hidden by `;`. In C, though, `;` is how a statement is written for its effect. `ans` would become the count `printf` returned, or the value of `x += 1;`.
  - D10 already made the missing `;` the mark of a value.
- These keep nothing, and `ans` stays what it was (both precedents; C has no `nothing`):
  - a void value;
  - a refused entry;
  - an entry whose run stops before its show.

  The entry number still advances, as IPython's does.
- The kept value is a new object, initialized once from the entry's value.
  - Its type is the one `auto r = value;` would give (C23 6.7.10, C++ [dcl.type.auto.deduct]): top-level cv and references are dropped, and a function designator becomes its pointer.
  - So after `x`, then `x = 7;`, `_N` is still 5, as both precedents keep an immutable value.
  - They alias a mutable object; C and C++ copy it, and so does madc.
  - An array is kept as an array, not decayed as `auto` would decay it. It is copied element by element, as a structured binding or a lambda capture copies one (slice 2).
- A class value is kept in slice 1 when it copies as C copies a struct (`trait_is_trivially_copyable`), and so is the madc carrier, which copies through its runtime as every `var b = a;` does. Every other class object is slice 2's (revised while building slice 1, see "Built, slice 1").
- A result is an ordinary object. It can be changed, as IPython's `_` can be mutated. `ans + 1` makes a new result.

**The names:**
- `ans` and `_` are the last kept result.
  - `__` is the one before it, and `___` the one before that.
  - `_N` is the result REPL[N] kept. `N` is decimal with no leading zero, as the diagnostics number entries.
- They are found only where ordinary lookup finds nothing, so a user's name always wins, as in both precedents. Examples: a declared `ans`, a local `_`, `std::placeholders::_1` through a using-directive.
- They resolve only in an interactive entry (`interactive_entry()`), under every standard, like every entry relaxation (D3).
  - A loaded file (§41.5a) is a translation unit and has none.
  - They become rows of D3's registry when it lands.
- `ans`, `_`, `__` and `___` move with every kept result. So they are refused in code that runs after the entry: a function or lambda body, a default argument, a default member initializer.
  - Julia and IPython read them at the call, but a body madc compiles once would keep this entry's result silently.
  - The refusal names the stable spelling (`_4`).
  - D6's recompilation of dependents is what will let them follow.
  - `_N` never moves and works everywhere.

**Refusals**, each at the name, in the entry's position:
- `'ans' names the last value an entry showed, and none has been shown yet`
- `'__' names the value shown before the last one, and only 1 value has been shown`
- `'_5' names the value REPL[5] showed, and it showed none`
- `'_9' names the value REPL[9] showed, and there is no REPL[9] yet`
- `'ans' names the value REPL[4] showed, which was not kept: a class object that is not trivially copyable is kept from D12's slice 2 on` (slice 1; no type word, see "Built, slice 1")
- `'ans' changes with each value shown, so code that runs later cannot use it; name the value REPL[4] showed as '_4'`

**The mechanism: a result is a session global, made where the value is shown.**
- `show_entry_value` (parse) already moves the entry's final value into `__madc_show`. It declares the result object there.
  - The object is a Variable `__madc_result_K`, with its own serial that is never reused, like `__madc_entry_K`. It is declared through `declare_object`.
  - Its `TokenDecl` initializer is `TokenAssign(result, value)`, the `auto` declaration's own shape (`parseDeclaration`'s `auto` arm). It is recorded with `record_global_top_decl`.
  - The run gets its `TokenGlobalInit`, then `__madc_show(result)`. The value is evaluated once, into the result, and the show reads the result, so the display is unchanged.
  - A value that is not kept is shown as today, and the reason is recorded.
- Every later module declares the object `extern`, and the defining module exports it (`session_defined`), as for any entry's global.
  - A class result constructs at its place in the run and destructs at exit, as entry globals do.
  - A refused entry rolls the object back with its transaction.
  - No emission site learns about results, except the one gate below.
- **C.** The CIR routes a non-constant file-scope initializer to dynamic initialization only in C++ modes (`presents_as_cpp()`). So C's static-initializer rule would refuse `__madc_result_K = x + 1`.
  - The result object is the implementation's, not a declaration of the program, so the gate also routes the session's result objects (`Program::is_session_result`).
  - A C session's own `int y = f()` stays refused, as C and clang-repl's C mode refuse it. madc cites c2mir's words there today.
- **The table.** The Program keeps `session_results`, mapping an entry number to its object, or to the reason it was not kept.
  - After the run, if the show ran (`entry_shown` is set), the session records the entry: `keep_entry_result(N)`.
  - The parser reads it at the lookup miss, through `resolve_expression_context_identifier`, the one owner of "a name the host context supplies". Every miss path already calls it: the expression's identifier arm, `parsePostfixChain` and `parse_complex_component_operand`. The session's results sit there beside the eval API's context root.
  - An identifier's spelling becomes a result reference (the last one, k back, or entry N) once, at that boundary.
- **Thread contract.** The table and the result objects are session state. Only the submit verb changes them, and D9 serializes it; lookups happen inside that verb's parse. The result objects follow the stdlib convention.
- **Lifetime.** Every kept result lives until the session ends, like IPython's output cache. `%reset` (D6, D17) will free them.

**Found while designing, on the REPL's path, and fixed first** (each in its own commit, with a reducer and both oracles):
- **A.** An entry's final bare function name is called.
  - `f` shows 3, `g` reads "too few arguments", `&f` is refused and runs on into the next line, and `int (*p)(void) = f` is refused.
  - The function-name arm decides decay by the next token: `;` with an empty operator stack decays. The end-of-entry token stands for the omitted `;`, but it is not among them.
  - With `;`, and in a file, all four are right.
- **B.** `auto x = 5`, with no `;`, shows nothing, while `int z = 7` shows 7.
  - Traced (2026-09-27): the `;` is not the cause, since `require_statement_terminator` accepts one the construct consumed itself. `parseDeclaration`'s two `auto` arms never record a file-scope object in `top_decls`, and that record is what emits the object and what the show looks for.
  - So a file-scope `auto` was never defined, in a file as well: `auto x = 5;` read "import of undefined item x" in C++ and C23, and `auto t = s;` of a `std::string` read `""`.
- **C.** In C only, two refused entries that call a function no entry defines kill the session after a struct definition: every later entry fails "tag P redeclaration".
  - The reducer is `struct P { int x, y; };`, `int y = g()`, `int z = g()`, `1`. Each `int … = g()` is refused at c2mir, since its initializer is not constant.
  - `g` may be declared or implicit. One such entry is clean, and so is a defined `g`. C++ is clean.
  - Traced (2026-09-27): not D27. `build_tu_module` freed a refused tree's node arena while the session's c2mir context kept its checker symbols, which are keyed by the tree's scope and identifier nodes. A later entry's tree reused the memory, so its `struct P` met the refused entry's symbol. The layout decided which sequences failed. The arena now lives with the session's context, as a compiled module's does.

**Found off the path** (`BUGS.md`): B35–B41 while designing, B42–B48 while building slice 1.

**Gates:**
- `test_repl_session`, under C, C++ and madc:
  - every row of the table above;
  - `x`, then `x = 7;`: `_N` is still 5;
  - `std::move(v)` shown, then `v.size()` is still 3;
  - a `unique_ptr` shown and not kept, with its name refused;
  - the user's `ans` wins;
  - the refusal in a function body;
  - a refused entry keeps nothing;
  - `__` and `___` after three results.
- A runner test, `tests/testrepl_ans.mad` (with `.flags` `-i`, `.input`, `.expect` and `.exe_skip`). Its transcript is IPython's on the same values.

**Slices:**
1. The table, the names and their refusals, and results of scalars, pointers, enums, C structs and unions, trivially copyable classes and a madc `var`, under C, C++ and madc. An array or another class object is shown and not kept.
2. Array results and every other class object, designed against the code first. Measured in slice 1:
   - copying a class object runs its copy constructor, and madc does not lower one for every class (B45: `std::map`'s);
   - a class without a copy has none to run (`std::unique_ptr`);
   - Julia and IPython alias a mutable object rather than copy it.

   So slice 2 decides, by measurement, whether a class result is the object itself or a copy. The object itself would be an alias, as `auto &r = v;` makes one, with a prvalue materialized as `auto &&r = f();` does. Slice 2 also builds the array copy, which a structured binding needs too (B38).

**Built, slice 1 (2026-09-27):**
- The table, the names and their refusals, and the results above.
- Measured on `bin/madc`:
  - `tests/testrepl_ans` gives the lines IPython 9.17.1 gives on the same values: `42 43 430 42 43`, then `42 430`;
  - the unit table (`check_result_names`) passes under `c17`, `c++17` and `madc`;
  - in C, a struct result is a copy: `p`, then `p.b = 9;`, then `ans.b` is 2;
  - a function designator's result is its pointer, and `ans()` calls it.
- Built as designed except:
  - The class rule. Keeping a `std::map` refused its whole entry, since its copy constructor does not lower (B45). A type test cannot predict that, and the show must never refuse an entry. So slice 1 keeps only a trivially copyable class and the carrier, and the not-kept reason names no type: the source's spelling of a type is the display's (the CIR's `dump_type_word`), and the entry it cites shows the value.
  - "Code that runs later" is read from the parse: the Method of the innermost open compound, which a nested block inherits and a parameter list has too, an open class body, and a default member initializer's parse (`member_default_init_depth`). `cur_func_name` stays set after a top-level definition, so it cannot say it.
  - `hidden_object_decl` is the reference temporary's own builder, extracted: the reference temporary and the result are one shape.
- Found on the way, on the REPL's path:
  - **D.** `std::move(v)` of a class shows nothing. Traced: not the REPL's. A file refuses `std::move(x) + 0` too, and an entry's first `std::move` is typed wrong (B46).
  - **E.** A top-level `std::string("short")` was misread as a declaration ("Expecting parameter type in function pointer typedef"), though in a function body it is right. Fixed in its own commit: the two type-headed arms of `parseStatement` that ask `datatype_statement_starts_functional_expr` in a body ask it at an entry's own top level too (`statement_position_at`). The statement is placed at its use site (`use_site_type_token`), not at the header's typedef.
  - A class temporary shows only its type word (`std::string("short")` shows `<std::string>`). D10's walk reads a variable or a member, and slice 2's class results, materialized, are what it would read.
- Found off the path: B42 (`auto` from an array deduces its element), B43 (an error in a default member initializer is swallowed, silently; D12's refusal there waits on it), B44 (a namespace-scope init-capture refused), B45 (`std::map` not copied).

**Slice 2, designed against the code (2026-09-27):**
- **Measured** (`tmp/repl/d12`, on a pty). Both precedents keep an immutable value and alias a mutable object:
  - Julia 1.13: `p = P(2)` (a mutable struct), then `ans.b = 9`, then `p.b` is 9. `a = [1, 2, 3]`, then `ans[1] = 9`, then `a` is `[9, 2, 3]`.
  - IPython 9.17: `p`, then `p.b = 9`, then `_.b` is 9. `a`, then `a[0] = 9`, then `_` is `[9, 2, 3]`. A tuple rebound (`t = (5, 6)`) leaves `_` at `(1, 2)`, as a rebound int does.
  - madc today: a reference or a pointer to a struct or a class shows as the object (`P &rp = p; rp`, `*pp`). An array reached through one does not: `int (&ra)[3] = arr` and `*pa` show `<int32_t>`, with a stray c2mir warning, though their values are right (`ra[1]` is 2, and follows `arr`). D10's walk takes an array's extents only from a named array Variable.
- **The rule, revised: a scalar result is a copy, an aggregate result is the object.**
  - A scalar value (an arithmetic type, a pointer, an enum) is kept as a copy, as slice 1 keeps it.
  - So is the madc carrier, since its copy is the dialect's own value semantics, and it is deep: `var b = a;`, then `a[0] = 9;`, leaves `b` at `{ 1, 2 }` (measured).
    - So a `var` holding an array diverges from both precedents, which alias it.
    - Aliasing the carrier by its type would get a `var` holding a number wrong the other way, since IPython keeps a rebound int.
    - Whether a `var` array is shared, as a Python list or a JavaScript array is, is a dialect question for the owner, not D12's.
  - An aggregate (a struct, a union, a class or an array) is the object itself.
    - For a glvalue, the result refers to the object the entry showed. `p`, then `p.b = 9;`, then `ans.b` is 9, as in both precedents.
    - For a prvalue (a call's by-value struct or class, `std::vector<int>{ 1, 2 }`, `P{ 1, 2 }`), the result is that object, materialized into the result's storage, which owns it.
  - This revises slice 1, which copies a C struct and a trivially copyable class. It also retires the copy question for every class (B45, `std::unique_ptr`) and the element-wise array copy (B38 keeps its own).
- **What the result refers to must outlive it.** The rule reads the root of the glvalue: a `TokenVar`, a `TokenMember`'s `object` through its `parent_expr` chain, or a `TokenSubscript`'s `object`.
  - A session object, which is every entry's global: the result refers to it.
  - An object reached through a pointer (`*p`, `p->m`, `p[i]`) is the user's. The result refers to it, as `auto &r = *p;` would, and it dangles if the user frees it. Julia and IPython have a collector; C and C++ have none.
  - A part of a temporary of the entry's run (its root has automatic storage, or is a call or a literal) dies with the run. A trivially copyable one is copied (slice 1's rule), and any other is shown and not kept ("a temporary's part is not kept").
- **Mechanism.**
  - A glvalue aggregate's result is a session pointer, `T *__madc_result_K = &value` (for an array, a pointer to its array type, `array_operand_type`). It is built through the owners: `build_address_of`, and `hidden_object_decl` for the declaration.
  - The names resolve to `*__madc_result_K` through `build_indirection`, the one dereference builder. So `ans.b`, `ans[1]`, `&ans` and `sizeof ans` mean what they mean on the object. This is plain C, so the same result serves C, C++ and madc, with no reference type.
  - A prvalue aggregate's result is the object: `hidden_object_decl` as in slice 1 (a class prvalue constructs in place, `TokenObjTemp`'s arguments).
  - The show reads what the result names: `*__madc_result_K`, or the owned object.
  - D10's walk takes an array's extents from `array_operand_type`, the owner of "the array an operand denotes", not only from a named array Variable. `*pa` and a reference to an array then show their elements, with no stray warning.
- **Display.** `_N` of an aggregate shows the object as it is now. The entry itself showed it as it was.
- **Gates** (`test_repl_session`, C, C++ and madc):
  - a struct, a class and an array, each changed after its show and seen through `ans`;
  - a prvalue class result that owns its object (`std::vector<int>{ 1, 2 }`, then `ans.size()`);
  - a class whose copy constructor is deleted shown and kept by reference;
  - a temporary's trivially copyable part copied, and a class part not kept;
  - `*pa` shows its elements.

**Built, slice 2 (2026-09-27):**
- As designed, for structs, unions and classes, under C, C++ and madc.
  - `p`, then `p.b = 9;`, then `ans.b` is 9, and `&ans == &p` is 1.
  - `v`, then `v.push_back(3);`, then `ans.size()` is 3.
  - `std::vector<int>{ 7, 8 }` and a by-value `mk()` own their object.
  - A class with a deleted copy constructor is kept by reference.
  - `mkh().name`, a `std::string` member of a temporary, is shown and not kept.
- Built otherwise:
  - **Arrays.** An array stays shown and not kept, since `&arr` is typed as a pointer to the element (B50, silent: `sizeof *&arr` is 4). The pointer to the array that the result needs is that owner's answer, and correcting it retypes every `&array`: a focused session.
  - **D10's walk.** It takes an array's extents from `array_operand_type` for an array reached through a pointer or a struct (`*pa`, `s.arr`), which showed `<int32_t>` before. A named variable keeps its own branch.
- Found on the way, on the REPL's path, and fixed in its own commit:
  - **G.** A reference to an array (`int (&ra)[2] = arr;`, then `ra`) showed `<int32_t>` with a stray c2mir warning, though its values are right.
    - Traced: D12's own. `keep_entry_value` asked `array_operand_type`, which does not see through a reference, so it kept `ra` as a scalar. The result was an `int` initialized from the array's address, which is where the warning came from, and the show read that.
    - It now asks the value type too. The walk also takes a reference's extents from its referent.
- Found off the path: B49 (a file-scope compound literal's address refused), B50 (`&arr` typed as a pointer to the element).
- **D12 is done**, except its array results, which wait on B50.

### 41.7a The line editor (D23) and completion, designed against the code (2026-09-27)

**Measured first** (`tmp/repl/d23`). `probe.py` runs a REPL on a pty, sends keystrokes, and records every byte it writes back (`jl1`–`jl3`, `py1`):

| | Julia 1.13.0 | IPython 9.17.1 |
|---|---|---|
| Enter on a complete entry, caret on an earlier line | runs it | inserts a newline; only the last line runs it |
| Enter on an incomplete entry | a newline; continuation lines have no prompt and are indented to the prompt's width | a newline, a `...:` prompt, auto-indent |
| Enter again and again at the end of an incomplete entry | the first two add lines; the third in a row submits it, refused ("premature end of input") | (Python: a blank line closes the block) |
| Tab, one candidate (`xy_al`) | inserts it | inserts it |
| Tab, several (`xy_`) | inserts their common prefix; a second Tab in a row lists them under the entry, one per line, and redraws the entry below the list | opens a menu |
| Tab with no candidate, or after `1 + ` | nothing | |
| Tab where only whitespace precedes the caret | on a continuation line: four spaces; on an empty first line: nothing | |
| Up inside a multiline entry | the caret moves up a line, keeping its column | |
| `x`, then Up, then Up | the last entry starting with `x`, then the one before it | the same |
| Ctrl-R | a fuzzy-search panel, new in 1.13; Enter puts the match in the prompt, not run | an `I-search backward:` line |
| Ctrl-C | `^C`; the entry is dropped; a new prompt | |
| Ctrl-D | on an empty entry, ends the session; otherwise deletes the character under the caret | the same |
| a bracketed paste of `y1 = 10⏎y2 = 20` | `y1` runs at the pasted line break; `y2 = 20` waits in the prompt | both go into one cell; Enter runs them together |
| bracketed paste mode | on at every prompt (`ESC[?2004h`), off before an entry runs | handled |
| the history file | `# time: 2026-09-27 15:38:39Z`, `# mode: julia`, then each line of the entry after a tab; a refused entry is kept; a repeat of the previous entry is not | (SQLite) |

- **Julia's repaint** runs on every keystroke:
  - It rewrites the whole entry. It climbs to the entry's first row with `ESC[1A`, erasing each row with `ESC[0K` as it passes.
  - Then it writes the prompt, and each line after `\r ESC[7C`.
  - It places the caret with `ESC[nA` and `\r ESC[nC`.
  - It never takes the alternate screen or a scroll region.
- **madc today** reads cooked lines (`std::getline`), so it has no caret and no history, and Tab inserts a tab.

**The rule: Julia's keyboard loop, read in C's terms.**
- **Enter** asks the session whether the entry is finished, wherever the caret is (Julia):
  - Complete: the entry is taken and runs.
  - Incomplete: a newline at the caret.
  - Extendable (D11, an `if` with no `else`): a newline, so that an `else` can follow. Enter on the entry's blank last line takes it, as D11's empty line does.
  - Three Enters in a row at the end of an incomplete entry take it, refused with its end-of-input diagnostic (Julia's rule). This is the way out of an unbalanced `{`.
  - An entry is what the editor holds when Enter takes it. So `if (c) f();`, Enter, `g();`, Enter runs both lines as one entry, since the editor shows them as one.
    - Cooked lines (D20) keep D11's split, because a pipe cannot show them as one.
- **Meta-Enter** (Esc, then Enter) inserts a newline whatever the verdict (§5).
- **Tab:**
  - Where only whitespace precedes the caret on its line, Tab indents to the next multiple of four columns.
    - Julia does this on a continuation line. madc does it on the first line too, where Julia does nothing, so that one rule serves.
  - Anywhere else, it completes the word before the caret:
    - one candidate is inserted;
    - several insert their longest common prefix;
    - a second Tab in a row lists them under the entry, sorted and in columns, then redraws the entry below the list;
    - an empty word completes nothing.
  - Julia printed one candidate per line. Columns are readline's, and they fit the hundreds of names a C header brings.
- **Up and Down** move the caret between the entry's lines, keeping its display column.
  - From the first line, Up recalls history: the last entry that starts with the text before the caret, then the one before it (both precedents).
  - Down walks back toward the newest, and past it restores what was typed.
- **Ctrl-R and Ctrl-S** search history incrementally, backward and forward.
  - The form is readline's and IPython's one line: `(reverse-i-search)'text': match`. Julia 1.13's panel is not copied.
  - Enter leaves the match in the prompt, not run (Julia).
  - Ctrl-G or Ctrl-C restores the entry.
  - Any other editing key keeps the match and then applies.
- **Ctrl-C** prints `^C`, drops the entry and prompts again (Julia). Ctrl-C while an entry runs is D8's.
- **Ctrl-D** on an empty entry ends the input, as the end of a pipe does (D20). Anywhere else it deletes the character under the caret (both precedents).
- **A paste is typing, without completion and without indentation.**
  - Bracketed paste is on at each prompt and off before the entry runs (Julia's bytes).
  - The pasted text is inserted literally: a tab is a tab, not a completion.
  - Each pasted line break acts as Enter, without the three-Enter rule. So a pasted run of entries runs entry by entry, as the same text would run piped (D20) and as Julia runs a paste.
  - The text after the last line break waits in the prompt.
- **The other keys** are readline's Emacs defaults, which Julia's and IPython's defaults share:

  | Keys | Action |
  |---|---|
  | `^A`, `home` / `^E`, `end` | the caret to the start / end of its line |
  | `^B`, `left` / `^F`, `right` | one character back / forward |
  | `esc b` / `esc f` | one word back / forward |
  | `backspace`, `^H` / `del` | delete back / forward |
  | `^K` / `^U` | kill to the end / start of the line |
  | `^W` / `esc backspace` / `esc d` | kill back to whitespace / one word back / one word forward |
  | `^Y` | yank the last kill |
  | `^T` | transpose two characters |
  | `^P` / `^N` | as `up` / `down` |
  | `^_` | undo: one step per edit, where a typed run is one edit (§7.5's coalescing) |
  | `^L` | clear the screen and redraw the entry at the top |

- **Meta is the Esc prefix,** readline's model.
  - A terminal's Alt key sends Esc first, and `tui_keyparse` already delivers Esc and then the key.
  - So `esc b` is a two-key sequence in the bindings table, and no new key kind is needed. D23's recon had listed Alt and Meta as a new engine piece.
  - madcide's `emacs.keys` leaves its `M-` seats unbound because an Esc there cancels a chord. The editor binds no other meaning to Esc.
- **The prompt** is D22's. Continuation lines are indented to its width and carry no prompt.
- **The fallback** (D23) is cooked lines:
  - when stdin or stdout is not a terminal (no prompt, D20);
  - when `TERM` is unset or `dumb` (a prompt, but no editing);
  - when the console has no VT mode (Windows before 10 1809).

**History:**
- A taken entry joins the ring, a refused one included, as Julia keeps an input that failed. A repeat of the previous entry is not added (measured).
- The file:
  - It lives at `$XDG_STATE_HOME/madc/history`, which is `~/.local/state/madc/history` by default, or `%LOCALAPPDATA%\madc\history` on Windows. History is state, not configuration, which is the XDG split `madc.ini`'s own search follows.
  - `--history-file=no` turns it off, and `--history-file=PATH` moves it, with Julia's flag name.
- The file's format is Julia's `repl_history.jl`. `# mode:` names the prompt, which is the standard in force (D22), and later `help` and `shell`:

  ```text
  # time: 2026-09-27 15:38:39Z
  # mode: c++17
  	int x = 10;
  	x * 2
  ```

  - Every line of an entry follows a tab, so a line of the entry can never start a record.
- The file is read once, at the start.
  - Each taken entry is appended at once, in one write, so a crash loses nothing.
  - Two sessions' records interleave whole (`O_APPEND`). Another live session's entries appear at the next start.
- Recall and search see the entries of the session's language: C, C++ or madc. Julia's modes each search their own too.
- The file is unbounded, as Julia's is.

**Completion, the Tab hook's provider:**
- The session answers `complete(text, caret)` with the start of the word and its candidates. The editor only renders them.
- **The word** is the identifier characters before the caret. What comes before the word gives its context:
  - nothing special: a top-level name (slice 3);
  - an `a.b.` or `p->` chain of names: a member of the chain's type (slice 4);
  - `ns::` or `C::`: a member of the namespace or the class (slice 4);
  - inside a string or a comment: nothing. `#include` paths and D25's file names come later.
- **Top-level names are enumerated by walking the registries,** never through lookup. Lookups materialize forest declarations, register `dlsym` symbols and throw (§8's code check). The names are:
  - the program scope's variables and functions (`tkProgram`'s `variables`), those of every entry and every included header;
  - types (`datatype_map`), tags (`struct_map`), namespaces (`namespace_map`) and templates;
  - the standard's keywords (`keyword_map`, already gated by `--std=`);
  - macros (`define_map`, `macro_map`);
  - `ans` and the `_N` names, when a result is kept (D12).
- **Measured** (`tmp/repl/d23/names.c`, `--dump-registered`, C17): after `#include <stdio.h>`, `printf` and `FILE` are listed.
  - The dump has no top-level variable (`zq_glob`, `stdout`) and no macro (`EOF`).
  - So the enumerator is a new owner, as §8 said. `dump_registered_names` stays the oracle it is.
- **Filtering:**
  - A name that starts with `_` is offered only for a word that starts with `_` (IPython's rule). So `__builtin_*`, `_IO_*` and the reserved names of a header stay out of the way.
  - The session's own `__madc_` names are never offered.
- The candidates are sorted, with duplicates dropped, so an overload set is one name.

**The mechanism:**
- **The model** is a new engine component, `include/madcdis/line_edit.h`. It is header-only and knows no terminal.
  - Its state:
    - the entry's text;
    - the caret, as a byte offset;
    - the undo stack and the kill slot;
    - the history ring and its search state;
    - the count of Tabs in a row.
  - Its input is the `tui_event`s of `ui_apply_keys`, over the one key owner and an empty `focus_state`.
    - The key owner is `key_resolver`. Its bindings are data: a `line_action` enum's codes (enum-over-strings), with the default table built in.
  - Its output:
    - a view: the entry's lines, the caret's line and display column, and a line under the entry for the search or the list;
    - an outcome: editing, taken, dropped or end of input.
  - Its two hooks:
    - *finished*: the text in, Taken / Incomplete / Extendable out;
    - *complete*: the text and the caret in, a start and candidates out.
- **The painter** builds the bytes for Julia's full refresh of the entry. It lives in `ui_term.cpp`'s shared byte region, beside `vt_paint_bytes`. Each paint:
  - climbs from the last paint's caret row, erasing each row with `\r` and `ESC[0K`;
  - writes the prompt, then each line, indented by the prompt's width;
  - gives a line wider than the terminal as many rows as it wraps to;
  - places the caret by relative moves.

  It never takes the alternate screen or a scroll region.
- **Display width.** The painter counts columns per code point, with the C++ standard's estimated width ([format.string.std]: the East Asian Wide and emoji ranges are two columns).
  - This is a new function beside `utf8_seq_len` in `madcdis/text_utf16.h`, the owner of column arithmetic over one line.
  - None exists. The searches for `wcwidth`, "display width", "column width" and "east asian" found nothing.
  - libc's `wcwidth` needs a UTF-8 locale. Switching the host's locale would change what an entry's `printf` sees.
- **The terminal** (`src/ui_term.cpp`):
  - `term_target`'s raw mode is split from its grid screen. The inline mode shares:
    - the termios and console-mode bookkeeping;
    - the SIGWINCH handler;
    - `tui_keyparse`.

    Of the screen bytes, it writes only bracketed paste on and off.
  - The inline mode keeps type-ahead (`TCSADRAIN`), where grid mode flushes it (`TCSAFLUSH`).
  - It holds the terminal only while an entry is read. The entry runs with the terminal as a program expects it: cooked, with `ISIG` on.
- **The key parser** (`tui_keyparse`, the one owner) changes in two ways:
  - Bytes 0x80–0xFF pass as `ch` instead of being dropped (UTF-8 input, D23's named piece).
    - madcide's editor then receives the bytes it used to drop. Its grid still draws one byte per cell, its named residue.
  - `CSI 200~` … `CSI 201~`: every byte in between is a literal `ch`, so a pasted tab or line break is text, not a key.
- **The session.** `offer()` gains a *taken* callback. It is called once the entry is final, before the entry's diagnostics render or its run starts.
  - The editor finishes its display there and hands the terminal back.
  - So Enter parses once, as a cooked line does (§41.5a).
- **The REPL** (`madc_repl_run`):
  - On a VT terminal, entries come from the editor; otherwise they come from cooked lines, as today.
  - `run_repl` hands the loop the terminal target. The unit tests hand it a scripted one, with keys in and bytes out, so the tests run the production loop.
- **Thread contract:**
  - The editor's state is per instance.
  - The terminal has one target per process, confined to the thread that opened it (ui_term's contract).
  - The history file's records are whole appends.

**Slices:**
1. **The editor.** It covers:
   - the model, the painter and the inline mode;
   - UTF-8 and paste input, and the width owner;
   - `offer`'s callback, and the REPL on the editor;
   - the editing keys, Enter, Tab's indentation, Ctrl-C, Ctrl-D, Ctrl-L and undo.
2. **History:** the ring, Up and Down prefix recall, the file, and Ctrl-R / Ctrl-S.
3. **Completion** of top-level names, and the Tab list (§37 item 7).
4. **Member and qualified completion,** and the `%` / `:` command names with D24.

**Gates:**
- `test_line_edit`, a new unit test:
  - key sequences in, and the text, caret and outcome out, for every action and each Enter verdict;
  - the painter's bytes for a one-line entry, a two-line entry with the caret on line 1, a wrapped line and a wide character;
  - the width function at the edges of the standard's ranges.
- `test_tui_model`: the key parser's battery gains UTF-8 bytes and a bracketed paste holding a tab and a line break.
- `test_repl_cli`: `madc_repl_run` over a scripted target, under `madc`, `c17` and `c++17`. It covers:
  - §37's transcript typed as keys (items 1–6 again, through the editor);
  - a paste of three entries;
  - Ctrl-C and Ctrl-D;
  - from slice 3, `x` completed by Tab.
- A pty transcript of `bin/madc` on the build box (`probe.py`), read beside Julia's.

**Not in D23's slices, and named:**
- a `.keys` profile for the REPL. madcide's parser is dialect code, so one owner moves into the engine first;
- vi mode;
- syntax colour (§26);
- a pager for a long candidate list;
- auto-indent;
- stripping `c11> ` prompts from a pasted transcript (§5.3);
- madcide's `:` prompt and vised's find prompt adopting the model;
- Ctrl-C during a run (D8).

**Built, slice 1 (2026-09-27):**
- As designed. `madc` on a VT terminal edits each entry. Piped input and `TERM=dumb` read cooked lines, as before.
- **Measured on a pty** (`tmp/repl/d23/mc1.json`, `bin/madc --std=c17`):
  - the prompt, and continuation lines indented to its width;
  - Tab at a line's start indents four columns;
  - `x + 1` edited to `(x + 1)` with the arrows and `^E` runs as 11;
  - Ctrl-C prints `int q = 5^C` and drops the entry;
  - a pasted `int a = 1;⏎a⏎a` runs the first two lines and leaves `a` in the prompt, as Julia does;
  - bracketed paste is on while an entry is read and off before it runs;
  - Ctrl-D ends the session.
  - `TERM=dumb` (`mc2.json`) gets the cooked prompt, with the terminal's own echo.
- **Built otherwise:**
  - **The painter** lives in `line_edit.h`, beside the model, not in `ui_term.cpp`. It builds bytes only, as `tui_key_bytes` does, so the unit battery reads its bytes. The target writes them.
  - **A fresh row before each prompt** (`line_painter::fresh_row`), a step the design did not have. An entry's output that ends without a newline (`printf("x");`) would otherwise be erased by the prompt's first paint.
    - Julia and IPython print a line break after every entry. madc keeps D20's compact transcript and starts a new row only when the current one holds text.
    - It writes as many blanks as the terminal is wide, then a return. The terminal's wrap moves them to the next row when the row already held text.
  - **Word motion** adopts `text_buffer`'s word rule (`word_left_in`, `word_right_in`) instead of a copy. The one rule now counts a UTF-8 byte as a word byte, so `été` is one word for madcide's JOE word keys too (`test_text_buffer`).
  - **Up and Down** keep a goal column across consecutive moves, as Emacs does.
- **Recorded, not consolidated:** the line's display columns now have two rules.
  - `tui_model::expand_line` gives a byte one grid cell and shows a control byte as `?`.
  - `line_layout` counts code points by their width and shows a control byte as `^X`.
  - The grid can hold only one byte per cell, its named UTF-8 residue, so it cannot adopt the editor's rule until it holds a code point per cell.
- **Found on the REPL's path, and fixed in its own commit:** a UTF-8 string showed escaped. `"été"` showed as `"\303\251t\303\251"` (D10's `char *` spelling).
  - THE C-literal escape rule (`__madc_c_escape`) escaped every byte above 0x7e.
  - A well-formed UTF-8 sequence (Unicode's Table 3-7) is now written as itself, as Julia and Python show a string. Any other byte stays octal.
  - The rule's other readers change with it, since they must agree: `--emit=c11`'s literals, and a string token's spelling. gcc 13 and clang 18 compile the emitted `"été"` to the same five bytes.
- **Gates:**
  - `test_line_edit`: every action, each Enter verdict, pastes, Tab, the painter's bytes and the width owner;
  - `test_repl_cli`: §37's transcript typed at the editor under `madc`, `c17` and `c++17`, a paste of three entries, Ctrl-C, an entry pending at the end, and D11 and the third Enter through the session;
  - `test_tui_model`: UTF-8 bytes and bracketed paste in the key parser.

**Built, slice 2 (2026-09-27):**
- As designed.
  - `line_history` in `line_edit.h` is the ring. The host owns it and fills it; the model only reads it.
  - Up on the first line recalls by the text before the caret, and the caret stays after that prefix (Julia's, measured in `jl1`). Down past the newest restores what was typed.
  - An edit, or any key but Up or Down, makes the recalled entry the one being typed.
  - Ctrl-R / Ctrl-S use readline's one-line prompt, including `(failed reverse-i-search)`. A match is the entry's last occurrence searching back, its first searching forward.
  - Enter leaves the match in the entry. Ctrl-G or Ctrl-C restores the entry. A second Ctrl-R on an empty query takes the last query.
  - After a match is accepted, Up and Down go on from it.
- **The file** is Julia's format, `# mode:` naming the prompt's standard. It is written 0600, one `write` per record on an `O_APPEND` descriptor (a stream on Windows), and its directories are made as needed.
- **Measured on a pty** (`mc3`–`mc5`, `XDG_STATE_HOME` set):
  - a first session writes `6 * 7`, a four-line function and `f(1)`;
  - a second session recalls the whole function with Up Up and runs it;
  - Ctrl-R `* 7` finds `6 * 7`, and Enter leaves it unrun;
  - `f` then Up recalls `f(1)`;
  - `--history-file=no` makes neither file nor directory, and the session's own ring still recalls.
- **Gates:**
  - `test_line_edit`: the ring, the record format both ways, prefix recall and Down, and search (fail, accept, Ctrl-G, a key that acts, the last query);
  - `test_repl_cli`: a history file whose C++ record stays out of a C session, a taken entry appended and a repeat not, and an in-session Ctrl-R.

**Built, slice 3 (2026-09-27): §37 item 7 is done.**
- As designed. `InteractiveSession::complete` answers Tab, and `Program::complete_entry` in `src/madc_complete.cpp` is the service.
- **Where the word stands: the real lexer, not a second one** (`completion_context`). Refined while building.
  - The text before the word is lexed with a probe identifier (`__madc_completion_probe`) in the word's place. It is lexed as an attempt still being typed: `begin_entry`, `lex_entry`, inside an `EntryTransaction` that rolls back.
  - The probe is the last token of the entry's own file only when the caret is in code. An unclosed string or comment fails the lex, and a line comment or a directive swallows the probe.
  - The token before it gives the context: `.` / `->` (a member), `::` (qualified), `struct` / `union` / `enum` (a tag), `#` (a directive: nothing), anything else (a name).
  - A `#define` in the text is gone after the query (tested).
  - No character scanner decides strings, comments, raw strings or digit separators: that would be a second lexer.
- **The names** (`completion_names`) are walked, never looked up:
  - `tkProgram`'s variables;
  - `funcdef_map`, without class methods;
  - `datatype_map`;
  - in C++ and madc, `struct_map`, `namespace_map`, `template_map` and `fn_template_map`;
  - `keyword_map`, `define_map`, `macro_map` and `lazy_map`;
  - in madc, the auto-include table's words;
  - the result names once a value is kept.
  - A tag completes only after its keyword. In C, a tag alone is no name, so `Poi` offers nothing and `struct Poi` offers `Point`.
- **The auto-include table** is now one function (`auto_include_identifier_headers`). The scan's lookup and completion's enumerator (`Program::auto_include_words`) read it.
  - The member-row test (`auto_include_member_row`) and the policy check (`auto_include_permitted`) are shared by both, moved out of the scan with its behaviour unchanged.
  - So `WEB` (a member row of `ns_ui_web`) is not offered bare, and a namespace the host's policy disallows is not offered.
- **The word** is `text_buffer::word_byte`'s run before the caret, the one word rule the editor's word motion reads, now public.
- **Measured on a pty** (`mc6`, `mc7`):
  - `xylo` → `xylophone`;
  - `xy`, then Tab Tab, lists `xylophone  xyz1`;
  - `"xylo` and `x; // xylo` complete nothing;
  - `struct Poi` → `Point`, and C's bare `Poi` offers nothing;
  - in madc, `printl` → `println`, `ph` → `php` and `tot` → `total`.
- **Found off the path:** B51. An unknown directive (`#xylo`) is refused as "unexpected token type 7", where gcc says "invalid preprocessing directive".
- **Found on the REPL's path, and fixed in its own commit:** in C++, Tab listed names no top-level entry can write. After `#include <vector>`, `alloc` offered `allocator_char__allocator_char__o2` and a bare `allocator` / `vector`.
  - Measured (`tmp/repl/d23/names_probe.cpp`): madc registers a class's members and an instantiation's products as globals, a namespace's objects as globals too, and an instantiation's class under a bare key. No registry alone says what is written bare.
  - Now each entity is asked:
    - a name must be an identifier;
    - a reserved one (a leading `_`, or `__` anywhere) completes only a word shaped like one;
    - an instantiation's product (`vfINSTPRODUCT`) is skipped;
    - a namespace's member, found by identity among `namespace_map`'s values, needs a reachable namespace: the global one, one a using-directive names (not libstdc++'s own `std::__debug`), and std in the madc dialect.
  - Found off the path: B52 (madc `--std=c++17` accepts a bare `vector<int>`; g++ refuses).
- **Gates:**
  - `test_repl_session`'s two completion cases: C17 names, keywords, a header's names and macros, the contexts that complete nothing, tags, the underscore rule, the rollback, C++ class and namespace names, madc's words and member row, and the result names;
  - `test_repl_cli`: §37 item 7 at the editor (`xylo⇥` runs as 3, `twi⇥(4)` as 8, and a second Tab lists).

**Slice 4, designed against the code (2026-09-27): members after `.`, `->` and `::`.**
- **The precedents:** `x.` Tab lists x's fields in Julia and its attributes in IPython. After `.` or `::`, an empty word lists every candidate. The Name context's "an empty word completes nothing" stays the top level's rule.
- **Members of an object** (`a.`, `p->`, `a.b.c.`, `p->q.`):
  - The chain is read from `completion_context`'s tokens: identifiers joined by `.` or `->`, back to its root. A call or a subscript in the chain (`f().`, `a[0].`) completes nothing yet.
  - The root is a session name, found through the program scope's index (`TokenCpnd::findVariable`: an index probe, which materializes nothing). A result name (`ans.`, `_4.`) goes through the D12 result table, and an aliased result means its referent.
  - Each link steps through its type:
    - a reference is its referent;
    - `->` takes one pointer level (`pointer_dd_of`), and `.` on a pointer completes nothing (clang's own "did you mean `->`" is later);
    - a struct or class gives its `members`, then its `method_map` and `static_member_types`, then its bases' (`bases`), transitively;
    - the madc carrier gives its script methods (`ddARRAY`'s `method_map`).
  - A private or protected member (`member_access`) is not offered: a top-level entry is no member and no friend.
  - Constructors, destructors and operators are no identifier, so the name rule already drops them.
- **Members of a scope** (`X::`, `A::B::`):
  - A namespace gives its members (`namespace_map[X]`), its types (`namespace_datatype_map[X]`), its nested namespaces (a `namespace_map` key `X::N`), its class templates (`defining_namespace == X`) and function templates (`FnTemplateDef::ns`).
    - A scoped enum is registered as a namespace, so `Color::` lists its enumerators.
  - A class gives its static members, its methods (for `&C::m`) and its nested types (`type_aliases`).
  - **A module's namespace fills when its fragment is parsed.** `php::` in a madc session that has not used `php` has no members yet: the auto-include scan pulls `<ns_php>` at lex time, but only its parse registers the members.
    - So a qualified query parses its attempt (`parse_entry`, not only `lex_entry`), inside the same rolled-back transaction, and reads the scope before the rollback.
    - The parse stops at the probe, an unknown member, after the fragment's declarations are registered.
    - A name the parse cannot reach (a later, still-lazy forest declaration) is not offered; measured, `std::` after `<vector>` holds `vector` (a class template in std).
- **The rules of slice 3 hold:** identifiers only, reserved names only for a reserved-shaped word (so `_M_impl` stays out of `v.`), sorted, no repeats.
- **Gates:**
  - `test_repl_session`:
    - C: a struct's fields through `.`, `->` and a two-link chain;
    - C++: a class's public members, methods and a base's members, not its private ones; `std::string`'s `size`; `std::` members after `<vector>`; `geometry::` and `Color::`; `ans.` of a struct result;
    - madc: a `var`'s methods, and `php::` in a fresh session.
  - `test_repl_cli`: `p.` Tab Tab lists the fields at the editor.

**Built, slice 4 (2026-09-27): members after `.`, `->` and `::`.**
- As designed. `completion_members` steps the chain; `completion_scope_names` reads a scope. Four refinements came from measuring (`tmp/repl/d23/members_probe.cpp`, scripts `members1`–`members7`):
  - **Two transactions, not one.** The context's lex rolls back before the qualified parse begins.
    - In one transaction, the context's lex marked `<ns_php>` included, and its tokens were then dropped. The parse's own lex did not include the fragment again, so `php::` offered nothing in a fresh session.
  - **A walk inside a transaction only reads.** `template_map.for_each` journals every entry it passes, and the class-registration journal refuses to roll that back (an assertion).
    - `intern_keyed_map` gains `for_each_readonly`. Completion's walks use it, and its finds use `find_readonly`.
  - **A class scope gives what its object gives** (`&C::get`, `sizeof(C::w)`, `C::count`) and its nested types. The design listed only its static members, methods and nested types.
    - The class's alias to itself, its injected-class-name ([class.pre]/2), is not offered: after `C::` it names the constructor.
  - **A namespace's types skip instantiations** (`allocator_char`). The test is the `<`-in-spelling one the top-level walk uses, now one helper.
- **Reused owners:**
  - a data member's type: `DataDefSTRUCT::m_type`, whose parameter is now `const`;
  - a reference's referent: `TokenSubscript::referent_type`;
  - access: `m_access` and the method's flags, the inputs `access_flag_violation` reads.
- The name rule is one class, `CompletionOffer`, which all three walks share.
- **Measured on a pty** (`mc9`): `p.` then Tab Tab lists `x  y`, and `p.y` runs as 4; `geometry::a` then Tab completes `area`, which runs as 2.
- **Not in this commit:** completing the `%` / `:` command names (slice 4's other half in the slice list). It needs D24's command registry, which comes with §37 item 8.

### 41.8a Session commands, `?name` and `%type` (§37 item 8), designed against the code (2026-09-27)

§37 item 8: `?name` and `:type` work. It needs the command front D13 and D24 describe, so that comes first. The command-name completion left over from §41.7a's slice 4 rides it.

**The precedents, measured on a pty** (`tmp/repl/s8/jl_help.json`, `py_help.json`; notes in `tmp/repl/s8/oracles.md`):
- **Julia 1.13:**
  - `?sq` over two methods prints "sq is a Function.", then each method with its location: `[1] sq(n::Float64)` `@ REPL[3]:1`.
  - `?x` prints "x is of type Int64.".
  - An unknown name gets "Binding nosuchname does not exist.".
  - `?` alone at an empty prompt switches the prompt to `help?>`, and Backspace on the empty help prompt switches back.
- **IPython 9.17:**
  - `?sq` prints fields: `Signature: sq(n)`, `Docstring:`, `File:`, `Type: function`. `??sq` adds `Source:`, and `%pinfo sq` is the same as `?sq`.
  - `?x` prints `Type: int` and `String form: 10`.
  - An unknown name gets "Object `nosuchname` not found.", and an unknown magic "UsageError: Line magic function `%nosuchmagic` not found.".
  - A magic is an input like any other: it takes an `In [N]` number.
- **Neither has a type query that does not run the expression:** Julia's `typeof(sq(x))` calls `sq`. `:type` is the plan's (§7.5, GHCi's name). Its C and C++ adaptation is `decltype` and `typeof`, whose operand is never evaluated, so `%type expr` never runs anything.

**What the code has** (recon 2026-09-27):
- **No command handling anywhere.** Both REPL loops (`madc_repl_edit`, `madc_repl_run`) hand every entry to `InteractiveSession::enter`, which parses it as C. Both print a result through the one `print_shown`, so a command handled in `enter` reaches both loops unchanged.
- **Registries:**
  - `verb_table` (`include/madcdis/verbs.h`) is the hub's registry of world MUTATIONS: its bindings take a world and credentials, and the engine ships none. A session command is a query or action over an `InteractiveSession`, a different concept, so it does not register there.
  - madcide's `colon_command` (`tools/madcide/madcide_core.inc`) is a chain of string compares, which D24 moves onto the command registry. That move is a later slice (below): madcide is dialect code and needs the registry exposed to scripts.
- **The type's spelling:** `CirBuilder::dump_show_type_word` and its family (`dump_class_type_word`, `type_alias_spelling`, `strip_inline_namespaces`, `dump_scalar_type_word`) spell a type the way the entry's language writes it (`int *`, `struct P`, `std::string`, `int (*)(int)`). Every one reads only Program state. It is the "human-readable type renderer" §8's code check asked for.
- **The type of an expression without running it:** the parser's final-expression capture, `Program::keep_entry_value`, reads `operand_value_datadef(value)` of the entry's final expression (D10). The `typeof` and `decltype` arms read `parseExpression(...)->datadef()` the same way.
- **Signatures:** no function spells a function's signature (no "candidate:" or "declared here" diagnostic exists). `Method::parameters` keeps the parameter names; `FuncDef::parameters` holds only types, and hidden slots (`__this`, `__retbuf`) ride in both.
- **Locations:**
  - a file-scope object or type records `TopDecl` (`file`, `line`, `origin`), found by identity;
  - `Variable` and `FuncDef` record none;
  - a function's definition token (`TokenFunc`) has `file`/`line`, and a header declaration keeps `FuncDef::decl_file`.
- **Comments:** the lexer drops them (`getRealToken` keeps trivia only under `keep_trivia`, off for the REPL). So doc comments (D15) need their own slice.

**The design:**
- **Recognition (D13, D15).** An entry is a command when its first line, after blanks, starts with `%` or `:` followed by an identifier character, or with `?`.
  - C never starts a statement that way. `::x` (`:` then `:`) and the `%:` digraph (`%` then `:`) stay C, as does any continuation line.
  - Recognized in `InteractiveSession::enter`, before the parse, so both REPL loops and madcide's panel get it.
  - A command is one line. It is complete at its end, so `offer` takes it at once.
  - It is an input like any other (IPython): it takes the next `REPL[N]` number, it goes into history, and it keeps no result (D12's `_N` for it says REPL[N] showed nothing).
- **The registry (D24, enum-over-strings).** One table in `madc_session.cpp`: `{ spelling, InteractiveSession::command code, argument, one-line summary }`.
  - The typed name becomes the enum code once, at input; dispatch is a `switch` on the code.
  - `%` and `:` reach the same table (D13). `?name` is `%pinfo name`, IPython's own equivalence.
  - An unknown name is refused like an entry's error: `unknown command '%nosuch'`, and `%help` lists the commands.
  - This slice registers `help`, `type` and `pinfo`.
- **`%type expr` / `:type expr`: the expression's type, never run.**
  - The argument is parsed as the session's next entry inside an `EntryTransaction` that rolls back, and never linked or run. It is the same attempt completion's qualified query makes (§41.7a slice 4).
  - The command's own prefix is blanked to spaces, so a diagnostic cites the column the user typed.
  - `keep_entry_value` records the final expression's value type (a new `entry_value_type`, cleared by `begin_entry`) before any rule about keeping it, so an array or a function designator has its type too.
  - The output is that type's spelling: `int`, `const char *`, `struct Point`, `std::string`, `int (*)(int)`.
  - An argument that is no final expression (`%type int y;`, `%type x;`) is refused: "%type takes an expression". A refused expression prints its diagnostics, as a refused entry does.
- **The type spelling moves to `Program`.** The five functions above become one Program-level owner (behaviour-preserving). `CirBuilder`'s show lowering and `var_dump` call it, and so does the session.
  - `type_alias_spelling`'s walk becomes `for_each_readonly`, since `%type` spells inside a transaction (§41.7a slice 4: a mutable walk journals every value).
  - It gains what the show never needed: an array (`int [3]`) and a function type (`int (int)`).
- **`?name` / `%pinfo name`: what the session knows of a name,** in IPython's fields, with a function's overloads listed Julia's way, each with its location:
  - a function: `Signature:` once per overload (`int sq(int n)`, parameter names from `Method::parameters`, hidden slots skipped), each with `@ REPL[2]:1` or its header, then `Type: function`;
  - an object: `Type: int`, then where it was declared (`TopDecl`);
  - a type: `Type: struct Point`, where it was defined, and its public members with their types;
  - a macro: `Macro: #define N 5` (`define_map`) or its function-like form (`macro_map`);
  - a keyword: `while is a keyword of C17` (from `keyword_map`, gated by `--std=`);
  - several at once (a C tag and an object, `struct stat` and `stat`): each, in that order;
  - none: `'nosuchname' is not declared`. That is clang's phrasing for C and C++; IPython's "Object … not found" reads as a C object.
- **One top-level walk, two consumers.** `completion_names`' walk and its visibility rule (identifiers only, reserved names, reachable namespaces, instantiation products skipped) become a visitor over the entities an entry can name: `(name, object | function | type | tag | macro | keyword | result name)`.
  - Completion's visitor keeps the names that start with the word, and `?`'s the entities spelled exactly so.
  - The rule then exists once, so `?` never describes a name Tab would not offer.
- **Locations (the gap §8 named):**
  - objects and types from `TopDecl` by identity;
  - a function from its definition's `TokenFunc` in the session's tree, found by identity of its `Method`;
  - a header declaration from `FuncDef::decl_file`.
  - A name with no record prints no location. None is invented.
- **Completion (§41.7a slice 4's other half):**
  - `%ty` and `:ty` at an entry's start complete from the registry;
  - `?xy`, and the argument of `%type` and `%pinfo`, complete as an entry, through the same `complete_entry` over the argument's text.

**Not in item 8, and named:**
- `??name` (IPython's `Source:`, the defining entry's text) and doc comments (D15). The latter needs the REPL lexer to keep trivia and attach a `///` or `/** */` comment to the declaration after it.
- The prompt modes: `?` or `;` alone on an empty prompt switching to `help?>` or `shell>`, and Backspace switching back (Julia, §7.2, D14).
- `%whos` (IPython's table of the session's names, types and values).
- madcide's `colon_command` onto the registry, and the ex buffer commands (D24).

**Thread contract:** a command runs on the session's thread between entries, as every session verb does (D9). `%type` and `?` are reads: what an attempt changed rolls back, and the walks and finds are read-only.

**Slices,** each its own commit, with Tier 1 and Tier 2:
1. **The command front and `%type`:**
   - recognition, the registry, `%help`, the unknown-command refusal;
   - command-name completion;
   - `entry_value_type`;
   - the type spelling moved to `Program`, with arrays and function types.
2. **`?name` / `%pinfo`:** the shared walk, the signature spelling, the locations.

**Gates:**
- `test_repl_session`:
  - the three commands under C17, C++17 and madc;
  - `%type` of an int, a pointer, a struct, `std::string`, a function and an array;
  - `%type` of a refused expression, citing the typed column, and of a call that is not run (its side effect absent);
  - `%type` leaving nothing behind;
  - `%nosuch` refused;
  - `::x` and a continuation line staying C;
  - `?` on a C++ overload pair with both locations, an object, a struct, a macro, a keyword, and an unknown name;
  - `?` and Tab agreeing on a reserved name;
  - command-name completion.
- `test_repl_cli`: `?sq` and `%type` at the editor.

**Built, slice 1 (2026-09-27): the command front and `%type`.**
- As designed. `InteractiveSession::enter` recognizes a command before the parse (`command_text`), and `run_command` dispatches on the code the registry gives (`command_rows`, `InteractiveSession::Command`).
  - A command is taken at once and numbered. Its output is `shown()`, so both REPL loops print it with no change.
  - The name follows the REPL's one word rule (`text_buffer::word_byte`), and cannot start with a digit.
- **`%type`:** `type_command` parses the blanked argument inside an `EntryTransaction`. `keep_entry_value` records `entry_value_type`, and it is spelled before the rollback.
  - An array's type comes from `array_operand_type`, since an operand's `datadef()` is madc's flattened element: `int [3]`.
- **The type spelling** is `TypeSpeller` (`include/madc_type_spelling.h`, `src/madc_type_spelling.cpp`), moved whole from `cir_dump.cpp`. `CirBuilder`'s five words are one-line forwards, so its call sites are unchanged. The alias walk is read-only.
  - It gained a function type (`int (int)`), an array (`int [3]`), and the madc carrier spelled `var` in the dialect.
- **Measured** (`tmp/repl/s8/cmd_probe.cpp`, `cmd1.txt`; on a pty `mc_cmd.json`):
  - `%type`: `x` → `int`, `x * 2.5` → `double`, `&p` → `struct Point *`, `sq` → `int (int)`, `arr` → `int [3]`, `fp` → `int (*)(int)`, `s` → `std::string`, `x == 2` → `bool` in C++, `v` → `var` in madc;
  - `%type bump()` leaves `calls` at 0;
  - `%ty` Tab → `%type`;
  - `::x` and a body line `%b; }` stay C.
- **A divergence, stated:** `%type "abc"` prints `char *`, madc's model of a string literal (AGENTS: literals are `const char *`). g++'s `decltype` gives `const char (&)[4]`, and gcc's `typeof` `char [4]`. `sizeof("abc")` is 4, from its own token-level arm, and `array_operand_type` does not answer for a literal.

**Built, slice 2 (2026-09-27): `?name` / `%pinfo name`.**
- **Recognition and the registry.** `pinfo` is the registry's third row. `?` at an entry's start is `%pinfo` with the rest as the name, and `?` alone is `%help`, as Julia's and IPython's are. A command's name never completes after `?`: what follows it is the name, and it completes as an entry.
- **One walk, two consumers.** `completion_names`' body is `Program::visit_top_level_names`. It hands a visitor one `TopLevelName` per candidate, carrying its kind and its entity (Variable, FuncDef, DataDef, macro). Completion's visitor is the `CompletionOffer`. `describe_name` keeps the candidates spelled exactly as the name, under the same `CompletionOffer::accepts` rule, so `?` and Tab agree (a reserved name the user declared is offered for a word shaped like one, and described; the session's own `__madc_` names never).
- **`%pinfo` parses the name first,** as the session's next entry, in an `EntryTransaction` that rolls back. What a header or the madc dialect registers at a name's first use is then registered, and the description reads it before the rollback. The attempt's diagnostics go with it: a keyword or a type name is no expression, and is still described.
- **Spelling (`TypeSpeller`).** `declared(type, name)` spells a declaration with its declarator-id inside (`int a[2][3]`, `int (*cb)(int)`, `char *name`). `signature(name, fd, method)` spells a function's, with the parameter names from its `Method`, and a member's cv-qualifier-seq and ref-qualifier (`int area() const`, `int &ref() &`). A reference spells `T &` (madc lowers it as `T *`).
  - Fixed on the way, in the list both share: `parameter_list` now stops at `fixed_param_count()`, so the varargs slot madc adds is not spelled. `%type printf` printed `int (const char *, long, ...)`; it prints `int (const char *, ...)`.
- **An object's or a member's array type** is `Program::object_array_type` / `member_array_type`, from the extents `Variable::array_dims` / `DataDefSTRUCT::m_array_dims` give. `array_operand_type`'s three arms now read their extents from the same two accessors.
- **What each kind prints** (IPython's fields at column 12):
  - a function: `Signature: int sq(int n)  @ REPL[3]:1` per overload, then `Type:      function`;
  - an object: `Type:` and `Defined:`;
  - a type: `Type:` (a C tag's `struct Point`, a C++ class's `Box`) or `Typedef:` (a name for another type: `size_t` is `unsigned long`), `Defined:`, then its public members (data, static, methods with their signatures);
  - a macro: `Macro:     #define SQ(a) ((a) * (a))`;
  - a keyword: `while is a keyword of c17` (the prompt's spelling of the standard);
  - a namespace, a class template, a function template: `Type:` with the kind;
  - a result name: `Type:` and `Result:    REPL[N]`;
  - several at once, each, separated by a blank line: macro, keyword, type, namespace, templates, object, function, result (`?stat` gives `struct stat`, then the function);
  - none: `'nosuchname' is not declared`, an error at the name's column. What is no name is refused: `%pinfo takes a name`.
- **Locations, only as recorded:** an object's latest `TopDecl`; a type name's `TopDecl` of that name (a typedef records its target, so the name tells `Pt` from `Point`); a function's latest definition in `pending_funcs`, else its prototype's `decl_file`, which has no line.
- **A function template's stand-in is no function** (`FuncDef::stands_for_function_template`). That covers the placeholder an overload set seeds (the `"\x01fn-template-placeholder"` identity, now one `FuncDef::template_placeholder_spelling()` for its five sites) and a compiler-implemented public, which its fragment declares as a template (`inline_builtin_kind`: madc's `println`). It prints `Type: function template`, never the stand-in's invented `void println(void)`.
- **Measured** (`tmp/repl/s8/cmd_probe.cpp` over `cmd2.txt`, `cmd3.txt`); oracle `tmp/repl/s8/pinfo_oracle.{c,cpp}`: gcc, clang, g++ and clang++ accept every printed declaration.
- **Not recorded, so not printed:**
  - A C++ class's definition site: `TokenCLASS::parse` records no `TopDecl` (Pass 0.5 emits classes from `struct_map`, by design), so `?Box` has no `Defined:` line. Recording it is a class-parse change for its own session.
  - A header prototype's line: `decl_file` only.
  - A namespace template whose body is not retained, and that no overload set has seeded, carries no stand-in mark. `?` then prints its placeholder's empty signature. The mark is stamped only where an overload set is seeded (`register_skipped_namespace_template_function`), because stamping it earlier changes overload ranking.
- **Later, named:** `?ns::name` and `?obj.member` (a qualified name, through the scope and member walks); a template's declaration as written (with `??name`'s `Source:`).
- **A keyword's origin (owner, 2026-09-27).** Slice 2 printed the session's standard ("while is a keyword of madc"). A keyword now says where it comes from:
  - "C" or "C++" for a keyword the language's first standard has (c78, c++98, the first rows of the `--std=` table);
  - else the standard it first arrived in, among the session's languages. A C session reads C's, and C++'s only for a keyword C lacks. C++ and the madc dialect read both, by the year each standard names (`Program::standard_year`, a column of the `--std=` table): `const` is C89's, `inline` C++'s (C++98 before C99), `constexpr` C++11's;
  - "madc" (`defer`, `prefer`) or "GNU C" (`__thread`) for an extension.
  - The facts are one table, the standards' keyword lists (`src/madc_keywords.cpp`, `Program::keyword_origin`). `add_keywords`' C++ reservation and its `_Thread_local` gate read their versions from it. A unit gate checks that every keyword madc reserves under seven standards has a row.
  - A type keyword (`int`) is registered as its type, not in the keyword map. `?int` finds the type under its own spelling and describes it as a keyword too.
  - Found on the way, filed as BUGS.md B53: C23's own keywords (`constexpr`, `bool`, `nullptr`, …) are not reserved under `--std=c23`.

### 41.9a madcide's REPL pane on the session (§37 item 9), designed against the code (2026-09-27)

§37 item 9: madcide adds a REPL pane backed by that exact session object. D2 decides where the session runs: in a backend process, for the CLI and madcide alike, and the panel talks to it over a channel, not a pty. So item 9 builds D2's backend first, with the pane as its first client.

**The precedents:**
- **Thonny:** the shell talks to a backend process. Stop and F5 restart the backend, and a crash loses the state, not the IDE. The program's output shows inline in the shell, between the inputs.
- **Jupyter** (IPython's kernel, the console and the notebook): the kernel is a separate process, reached by messages. `execute_request` is answered by the stream output as it happens, then `execute_result`, then `execute_reply`; `complete_request` by `complete_reply`. The kernel reports busy/idle, an interrupt is SIGINT, and a dead kernel is restarted with its state lost. madc's channel follows that message shape, one JSON object per line.

**What the code has** (recon 2026-09-27):
- **The session is in-process only.** `run_repl` (`src/madc.cpp:485-521`) builds an `InteractiveSession` in the CLI's own process. Nothing script-facing reaches it: `<ns_madc>` has no session verb, so madcide cannot reach it today.
- **The script-facing pattern to follow** (cpp-first): `<ns_madc>` declares `namespace madc` publics, `src/ns_madc.cpp` forwards to `internal_*` in `src/madc_program.cpp`, and a handle is a `handle_table<T>` slot (`include/handle_table.h`; `parse_tu_handles()`, `madc_program.cpp:4979`) confined to the thread that opened it. Structured results are an array of objects with named fields, and the diagnostic rows have one builder, `diagnostic_rows_from_child` (`:4650`).
- **Fork-as-isolation:** `Process` with `ProcessOptions::child_body` (`include/madcdis/process.h:26-56`) forks the running madc, with no exec, and runs a body in the child with the owner's pipes, reap and cancel. `RunChannelFactory` (`madc_program.cpp:5033-5116`) is the precedent, and `map_child_status` gives a signalled child `128 + signal`. These channels carry raw bytes (a program's output), not framed values.
- **The value serializer:** `wt_value_to_json` / `wt_json_to_value` (`include/madcdis/world_text.h:115,145`), the one value↔JSON bridge, which `js::stringify` / `js::parse` and every JSON-RPC path use.
- **A request/reply precedent:** `mcp_client_call` (`tools/madcide/madcide_mcpclient.inc:191`), JSON lines matched by id over an `exec://` channel, waiting by `poll_state` and `sleep_ms`.
- **madcide's side:**
  - Panel kinds are the `ide_view` enum (`madcide_enums.inc:461`); the bottom panel is a chrome-pane group placed by `default.layout:21` (`pane panel bottom 25% tabs views problems output terminal hidden`); `compose_chrome_pane` (`madcide_core.inc:6989`) switches on the kind; `show_view` / `hide_view` show a kind.
  - The Terminal pane is a byte pump (`term_pump`, `:4584`) over a pty child. Its focus is a top-of-dispatch carve-out (`termfocus` in `apply_ide_event`), and `^]` hands the keyboard back through the baked `@terminal` scope (`modal_default_keys_text`, `:368`), which is data, not a hard arm.
  - Asynchronous sources are cooperative tasks (`go build_pump(...)`) doing `madc::chan_select` over a stop channel and the child's readability; they change the bag, and a `wake` event recomposes.
  - No pane has an input line. The one line-input widget is the modal `:` prompt (`start_prompt` / `pinput`), with no history and no completion.
  - Startup hard-codes the `joe` / `default` profile set (`init_view_es`, `:7696`).

**The design:**
- **The backend (D2), engine C++, cpp-first.**
  - `SessionClient` (C++) starts the backend with `Process` + `child_body`: the running madc forks, and the child builds an `InteractiveSession` from the parent's configured Program options and serves requests until its channel closes. Nothing execs, as madcide's Run does not (the owner's ruling: the running madc is the compiler).
  - **Channels:** the requests and replies travel on a socketpair made before the fork (one JSON object per line, through `wt_value_to_json`). The program's own output is the `Process`'s stdout and stderr, and its stdin the `Process`'s stdin (a `scanf` in an entry reads what the client writes there). So the session's replies never mix with the program's bytes.
  - **Requests:** `begin` (the standard), `offer` (the text, final or not), `complete` (the text, the caret). Each carries a `seq`, and the reply echoes it. The `op` and `state` words are text only on the wire, converted to enums once at each end (enum-over-strings).
  - **Replies:** an offer's reply carries the verdict (`taken`, `incomplete`, `extendable`), `ok`, the shown value, the rendered diagnostics (the text the CLI prints, from the session's error stream captured per request) and the diagnostic rows (the one builder, for clickable locations later). A completion's reply carries `start` and the names.
  - **Ordering:** the backend flushes the program's stdout and stderr before it writes a reply, and the client drains the output that arrived before a reply before handing that reply up, so an entry's output precedes its result, as in Jupyter.
  - **A crash** (D2): EOF on the channel. The client reaps the child (`128 + signal`), reports "the session stopped (signal 11, segmentation fault); a new one started", and starts a fresh backend, whose state is empty.
  - **An interrupt** (D8) is SIGINT to the child. The poll on back-edges that returns to the prompt is D8's own slice; until then an interrupt stops the backend, and the client restarts it.
  - **Thread contract:** a client is used from one thread or task. The backend is single-threaded, so its requests are serialized (D9).
- **The script API (`<ns_madc>`), dialect-lean** (`var`, `const char *`, `long`; no std::string):
  - `madc::session_open(const char *std)` returns a handle (`handle_table<SessionClient>`), and `madc::session_close(h)` stops it.
  - `madc::session_offer(h, text, final)` sends an entry and returns at once. `madc::session_poll(var &reply, h)` follows `poll_state`'s convention: 1 with a reply, 0 not yet, less than 0 when the backend died (then restarted). `madc::session_output(var &text, h)` drains the program's output.
  - `madc::session_complete(var &names, h, text, caret)` answers when the backend is idle, and returns false while an entry runs.
  - `madc::session_restart(h)`, and `madc::session_readable(h)`, a channel that is readable when a reply or output waits, so a pump task can `chan_select` on it beside its stop channel.
- **The pane (madcide, presentation only; §3.3: madcide never parses REPL syntax):**
  - A `viewREPL` kind (`view_name` "repl") and a `repl` command that shows it. `default.menu` gets `View repl REPL`, and `default.layout`'s bottom panel lists `repl` after `terminal`.
  - `compose_chrome_pane` renders the `[repl]` transcript (each entry with its prompt, its output, its shown value or its diagnostics) and, under it, the input.
  - **The input is a madcide buffer**, a `[repl-input]` doc under an edit node, so every editing key is the personality's, as in the editor. Only the REPL's own actions are new: `replenter` (offer the input; `incomplete` inserts a newline and keeps editing, `taken` moves the entry to the transcript), `replcomplete`, `replolder` / `replnewer` (history), `replunfocus`. They are commands, bound in a `@repl` scope: the baked defaults ride the rescue pattern `@terminal` uses, and a `.keys` file may rebind them. No key is hard-wired.
  - **Focus:** `replfocus`, a carve-out beside `termfocus`, routes keys to the input and its scope. The `repl` command focuses it, and `replunfocus` hands the keyboard back.
  - **Async:** `repl_pump`, a cooperative task like `build_pump`, selects on the session's readable channel and a stop channel, appends output and replies to `[repl]`, and wakes the UI. The prompt shows the standard (D22) and is disabled while an entry runs.
  - **Completion:** `replcomplete` asks `session_complete` over the input's text and caret. One candidate inserts; several insert their common prefix and list the candidates in the transcript, as the CLI does (a popup is §18's later GUI affordance).
  - **History:** the pane keeps its entries in memory this slice. The CLI's history file (D23) is per standard, and sharing it is a later slice.
- **The CLI (D2's other client).** `madc` and `madc -i` move onto `SessionClient`, so a segfault in an entry restarts the session instead of ending the process. The loops (`madc_repl_edit`, `madc_repl_run`) take a session interface both hosts provide. Its own slice, after the pane.

**Not in item 9, and named:** F5 (item 10); `%run` / `%load` / `%reset` as commands; the Variables pane; a completion popup and signature help; clickable diagnostics; the `--learn` / `--simple` profile (§16); a Windows backend (no fork there: a child of self, as `RunChannelFactory`'s Windows arm spawns one).

**Thread contract:** stated above. The pane's state lives on its session's bag, like every pane's.

**Slices,** each its own commit, with Tier 1 and Tier 2:
1. **The backend and its C++ client:** `SessionClient`, the protocol, output ordering, crash restart. Gate: a unit test driving a client (an entry and its output in order, a refused entry's diagnostics, a completion, `int *p = 0; *p = 1;` restarting the session with its state gone).
2. **The script API:** the `<ns_madc>` verbs. Gate: `tests/testsession_*.mad`, dialect code driving a backend (open, offer, poll, output, complete, restart).
3. **The pane:** `viewREPL`, the transcript, the input buffer, the `@repl` commands, the pump, completion. Gate: a `testmadcide_repl` model test (keys typed into the pane, the transcript's text) and a `tests/gui` case in the window.
4. **The CLI on the backend** (D2). Gate: the existing `test_repl_cli` and `testrepl_*` fixtures, unchanged, plus a crash transcript.

**Built, slice 1 (2026-09-27): the backend and its C++ client.**
- **`SessionClient`** (`include/madc_session_client.h`, `src/madc_session_client.cpp`): `start(std, factory)`, `offer(text, final)`, `complete(text, caret)`, `input(text)`, `poll(reply, output, timeout)`, `offer_wait` / `complete_wait`, `restart()`, `stop()`. The `ProgramFactory` builds the backend's Program in the child, after the fork (empty: a fresh Program). Slice 4's CLI passes one that applies its command line and madc.ini, as D20's configured Program does.
- **The fork** is `Process` + `child_body`, like the madcrun:// Run: `__madc_task_atfork_child()`, then `run_child_prologue()` (now external, shared with the Run), which puts stderr onto the output pipe. `check-live-build-owners.sh` counts this file's fork child and its scheduler reset with `madc_program.cpp`'s.
- **The wire:** one JSON object per line on the socketpair, through `wt_value_to_json` / `wt_json_to_value`. The backend's first line is a greeting saying whether `begin` succeeded, and a refusal's text becomes `last_error()`. `op` and `state` are words on the wire only, converted to enums at each end through one table each.
- **Ordering:** the backend flushes the program's stdio before each reply. The client polls the socket and the output pipe together and drains the pipe before it hands a reply up, so an entry's output comes first.
- **Stdin:** the `Process`'s stdin pipe. `input()` writes to it and waits while the pipe is full.
- **A crash:** EOF on the socket. `poll` returns -1 with a `stopped` reply carrying the exit status (`128 + signal`: 139 for `*p = 1` through a null pointer). The client is intact, and `restart()` begins an empty session.
- **Found on the way, fixed:** `InteractiveSession::begin` refused an unknown standard silently, because every host reports its own `--std=` errors. A backend has no terminal, so `begin` now says it, as the CLI does (`Unknown --std target: …`).
- **Gate:** `tests/unit/test_session_backend.cpp`, 10 consecutive runs green.
- **Not in slice 1:** a Windows backend (the stubs say so); an interrupt (D8), which is its own slice; a reply for a crash in `begin` itself beyond `last_error()`.

**Slice 2, amended against the code (2026-09-27): one select case over two streams.**
- **The gap.** A session has two streams: its replies (the socketpair) and the program's output (the `Process` pipe), kept apart so a crash's last bytes (the crash handler writes to stderr) still reach the client after EOF. A pump task must wake on either. `chan_select`'s byte case is a `madc::channel`, one endpoint with one wait handle (`read_wait_handle`), and its registry entry holds a raw pointer the channel must outlive. A session is neither: two handles, and new ones after every restart.
- **Rejected:** an aggregate epoll/kqueue descriptor in the client (a second readiness mechanism beside the reactor, which has no kqueue arm yet); a relay thread in the backend merging output into the socket (Jupyter's IOPub shape, but a crash loses the bytes still in the relay pipe); two select cases per session (the script would re-register after each restart, and a stale entry could name a reused descriptor).
- **The design: a readiness source.** `madc::taskio::readiness_source` (`src/madc_task_io.h`): `poll_state()` (1 progress now, 0 would wait, -1 dead, the byte case's contract) and `wait_handles(out)` (the handles whose readability can change that state, each with its `poll_handle_kind`). `chan_select`'s byte case reads a source, not a channel: `madc::channel` becomes one (its one handle), and `SessionClient` another (the socket and the pipe, read at each select, so a restart needs no re-registration). A select registers one `IoWaiter` per handle, all carrying the case's index.
- **Lifetime by id, not by pointer.** A session's case is registered as (resolver, session handle). The resolver looks the handle up in the session table at every select; a closed session resolves to nothing, and its case is dead, as a closed-and-drained channel's is. Handles are never reused (`handle_table`), so a stale case can never name another session.
- **Thread contract:** the source is read on the scheduler thread only, like every task verb (`madc_task_io.h`); the session table is confined to the thread that opened it, like the parse handles.
- **The verbs, all asynchronous** (Jupyter's `complete_request` is a request like any other; the backend answers it once a running entry ends): `session_open(std)` returns a handle, always, like `parse_open` (a refusal is the handle's state: `session_running` false, `session_error` its text); `session_offer(h, text, final)` and `session_complete(h, text, caret)` return the request's seq; `session_poll(var &reply, h)` returns 1 with a reply, 0 when none waits, -1 when the backend stopped (the reply is its `stopped` row); `session_output(var &text, h)` takes what the program printed so far; `session_input(h, text)`, `session_restart(h)`, `session_close(h)`, and `session_readable(h)`, the select case (made once per handle and reused).
- **A reply is a row** with named fields: `kind` and `state` as enum codes, `seq`, `ok`, `shown`, `rendered`, `submitted`, `diagnostics` (the one row builder's rows), `start`, `names`, `exit_status`. The codes' one text is `<bits/session_enums>` (`madc::session_reply`, `madc::offer_state`), which the engine aliases as it aliases `<bits/diag_enums>` (`InteractiveSession::OfferState`, `SessionClient::Reply::Kind`), so a handler switches on an enumerator the compiler checks.

**Built, slice 2 (2026-09-27): the script API.**
- **`<ns_madc>`'s verbs** (`src/madc_session_verbs.cpp`): a `handle_table<SessionHandle>` per thread (`thread_local`: the confinement contract, enforced). A handle holds its `SessionClient`, the output a poll took before the script asked for it, and its select case (made once). `session_open` accepts `c17` or `--std=c17`.
- **The enums:** `<bits/session_enums>` (`madc::offer_state`, `madc::session_reply`). `InteractiveSession::OfferState` and `SessionClient::Reply::Kind` alias them, so the engine's enumerators are now lower-case (`OfferState::taken`), as the dialect spells them.
- **The readiness source** (`madc::taskio::readiness_source`, `src/madc_task_io.h`): `chan_select`'s byte case reads one, and `madc::channel` becomes one through `ChannelReadiness`. A select registers one `IoWaiter` per handle, each carrying its case's index. `taskio::chan_readiness(resolver, id)` registers a source by id. `madc::poll_handle` (`madcdis/datachannel.h`) carries a handle with its space.
- **Found while building, fixed: a restart under a parked select hung it.** On Linux the reactor watches every descriptor with epoll, which silently forgets a closed one, while the poll() path reports it (POLLNVAL). So a pump parked on a session's streams slept through a restart that replaced them. `taskio::handle_closing(handle, kind)` wakes every task parked on a handle about to close: a select rescans, as a `chan_close` wakes it, and a plain `wait_readable` re-probes. `SessionClient` calls it before closing its streams. Negative control: with the wake disabled, `testsession_pump` hangs at the restart (timeout 124).
- **Auto-restart:** `session_poll` restarts a backend that ended during it (the design's "then restarted"), and the row's `restarted` says whether the new one started. So a pump's case never stays dead. A handle whose backend never started is not retried.
- **`last_error()`** drops the rendered block's trailing newline: it is a message.
- **Gates:** `tests/testsession_entries.mad` and `tests/testsession_pump.mad` (dialect code, no includes), under JIT, `--exe` and `--obj`; `.win64_skip` / `.wine64_skip` fixtures, since there is no fork on Windows yet. Neighbours green: 27 task/channel integration tests, 33 madcide/ui model tests, `test_rt_task`, `test_task_io`, `test_io_reactor`, `test_channel_object`, `test_process`, `test_repl_session`, `test_repl_cli`, `test_session_backend`.

**Built, slice 3 (2026-09-28): the pane.**
- **The tab:** `viewREPL` (`view_name` "repl", title "REPL" from `view_title`, the view vocabulary's display spelling; before it a tab's title was `ucfirst` of its name). `repl` shows it, starts a session (`replstd`, the engine default when empty) and gives the input the keyboard. `default.layout`, the baked layout and `default.menu` list it after Terminal.
- **The transcript** is the `[repl]` pseudo-buffer, fed through `ui::term_feed` as the Terminal's screen is: each taken entry after its prompt (continuation lines indented to the prompt's width, as the CLI indents them), what it printed, then its shown value or its diagnostics. A crash reads `[the session stopped (exit 139); a new one started]`.
- **The input** is the `[repl-input]` buffer. While it has the keyboard, every editing command (`cmd_edits_field`: the motions, the deletions, undo and redo) runs through the session's own dispatcher on that buffer. The editor's live interaction state (caret, mark, view, spans, read-only message) is parked around the call and restored after it (`repl_field_enter` / `repl_field_leave`), so the editor's buffer and caret never move. Any other command keeps its target (save saves the file), as Thonny's shell does.
- **The `@repl` scope** (baked in `modal_default_keys_text`, rebindable in a `.keys` file): enter `replenter`, tab `replcomplete`, up `replolder`, down `replnewer`, ^] `replunfocus`. `replolder` / `replnewer` step the history only from the input's first / last line (a draft is kept), and fall back to the caret's motion elsewhere.
- **Offering:** a taken entry moves to the transcript; `incomplete` inserts a newline at the caret and keeps editing; a blank last line makes the offer final (D11). While an entry runs, enter sends the line as the program's stdin and echoes it (Thonny).
- **Completion:** one candidate inserts; several insert their common prefix and list the candidates in the transcript. A reply that arrives after the input changed is dropped.
- **The pump,** `repl_pump`, is a cooperative task selecting on the stop channel and `session_readable`; it applies replies and output and wakes the UI. `IdeSession::close()` stops it and the session, beside the build.
- **The protocol gained a `running` notice** (`madc::session_reply::running`): the backend sends it from `InteractiveSession`'s taken hook, before the entry runs (Jupyter's `execute_input`). The pane echoes the entry on it, so the transcript reads prompt, entry, output, value even while the entry blocks on stdin. The client reads the socket before it drains the pipe, so the notice is never behind the output it precedes. An incomplete offer sends none.
- **`session_standard(h)`:** the standard in force, by its canonical name, carried in the backend's greeting (D22's prompt: `c11> `).
- **Found on the way, fixed in its own commit (82108777b): a focused field can take tab.** In the window, the posted tab came back as a focus event. The shared focus owner cycled focus whenever the tree had two focusables, so no edit node could take the key: the REPL's completion, and the Terminal's shell completion, never reached madcide. An edit node's `tabkey` hint (read by `web_model` and `tui_model`) now makes tab the application's key while that node has focus. The REPL input and the Terminal carry it while they have the keyboard.
- **Found off the path, filed:** B55 (tab inserts nothing in madcide's editor).
- **Gate widened:** `check-madcide-command-registry.sh` follows the core's `madcide_*.inc` includes, since a pane's commands are dispatched beside the pane. A new control drops the REPL include and must see its commands go hidden.
- **Gates:** `tests/testmadcide_repl.mad`, the model test (typed text and motion stay in the field, printed output, an unfinished entry, a refusal, the history, completion, a running entry's stdin, a crash and the fresh session, the composed node, ^]), under JIT, `--exe` and `--obj`, with stderr quiet. `tests/gui/madcide_repl.mad` runs in the window under Xvfb: the tab and its prompt, an entry typed through the page's own text and key posts with its value arriving on a wake alone, tab completing, ^]. `test_session_backend` pins the notice's order. Pinned counts moved on purpose: the registry has 52 commands, the pico help 47 rows, and the panel 4 tabs.
- **Not in slice 3:** a Windows backend; sharing the CLI's history file; a completion popup.

**Slice 4, designed against the code (2026-09-28): the CLI on the backend.**
- **What the loops use** (`src/madc_repl.cpp`): `offer` / `submit` with the taken hook, `shown()`, `complete`, and `program()` for two questions only: the standard's name (the prompt, the history's mode) and D11's "does this line start with `else`" (`Program::entry_line_continues_if`, a keyword-table lookup with no session state).
- **One interface, two implementations.** `ReplSession` (`include/madc_repl.h`): `standard_name()`, `offer`, `submit`, `shown`, `complete`, `continues_if(line)`, and `ended(status)`, which says the session ended the process (an `exit(n)` in an entry). `InteractiveSession` implements it directly (in this process, never ending), so `test_repl_cli` runs unchanged. `BackendSession` implements it over `SessionClient`, synchronously: it polls, calls the taken hook on the `running` notice, and returns the offer's verdict. The in-process implementation stays for Windows, which has no fork (the named residue: a child of self, as `RunChannelFactory` spawns one); it is deleted from the CLI when the Windows backend lands.
- **The backend's stdio is the terminal's** (`SessionClient` start option; `ProcessOptions::inherit_stdin` / `inherit_stdout` beside `inherit_stderr`). An entry writes to the real stdout, so `isatty`, line buffering and a prompt printed before a `scanf` behave as in this process, and nothing is forwarded. The pane keeps its pipes.
- **Stdin stays one stream.** In this process, an entry's `scanf` and the loop's `getline` share one `FILE`, so a piped transcript's next line is the entry's input (measured, §41.5a slice 1). Across two processes, stdio's read-ahead would split it, so both ends read their stdin unbuffered (`setvbuf(stdin, NULL, _IONBF, 0)`): the host's `getline` takes exactly one line, and the entry takes what it parses. One difference remains: the character `scanf` reads past its conversion and pushes back (the line's `\n`) stays in the backend's `FILE`, where in this process the next `getline` read it as an empty line, which is skipped. The line editor reads the terminal raw, and it holds the terminal only while it reads, so the entry reads it cooked, as today.
- **`-i file`:** two new requests, `load {path}` and `run {argv}`, the cores of `%load` and `%run` (D25), reply with `ok` (and `status` for `run`). The program's output is the terminal's. `SessionClient` gains `load_wait` / `run_wait`.
- **The parent keeps its configured Program, unbegun.** The backend's Program is the fork's copy of it (the factory moves the child's copy), so the command line and madc.ini hold in the session, and every restart copies it again. The parent asks the backend everything, D11 included (amended while building: a Program that never began has no keyword table, so the question is a request, `continues {text}`).
- **The end of a backend:** the stopped reply gains `signal` (0 when the backend exited), from a new `Process::term_signal()`, since `128 + signal` cannot tell `exit(139)` from a segfault. A signal: the CLI says `madc: the session stopped (signal 11, segmentation fault); a new one started` on stderr, restarts, and the entry counts as refused. An exit: the loop returns its status, so `exit(4)` in an entry still exits 4 (§41.5a).
- **Gates:** `test_repl_cli` and `testrepl_*` unchanged. New fixtures: `testrepl_crash` (a segfault, the message, then a name from before it undeclared), `testrepl_stdin` (a `scanf` entry reads the transcript's next line), `testrepl_exit` (an entry's `exit(4)`; the runner checks the status through a sibling fixture if one exists, else a unit case). `test_session_backend` covers `load`, `run`, `signal`, and a `BackendSession` driven by `madc_repl_run` over string streams (piped mode: the program's output goes to the loop's `out`).
- **Thread contract:** unchanged. A `ReplSession` is driven from one thread (D9).

**Built, slice 4 (2026-09-28): the CLI on the backend. §37 item 9 is done.**
- **`ReplSession`** (`include/madc_session.h`) is what the loops drive. `InteractiveSession` implements it (in this process: Windows, and the unit tests), and `BackendSession` (`include/madc_session_client.h`) implements it over `SessionClient`. `madc` and `madc -i` run `BackendSession` on POSIX. `test_repl_cli` is unchanged and green.
- **The backend on the terminal:** `SessionClient::set_inherit_stdio`, over the new `ProcessOptions::inherit_stdin` / `inherit_stdout` (both arms of the spawn owner). The child's stderr stays stderr (`run_child_prologue(merge_stderr)`), and both ends read stdin unbuffered.
- **New requests:** `load {path}`, `run {argv}` (the cores of `%load` / `%run`) and `continues {text}` (D11), with their `session_reply` kinds. The stopped reply carries `signal`, from `Process::term_signal()` (`child_term_signal`, beside `map_child_status`; both reap sites record through one helper).
- **Crash and exit:** a signal prints `madc: the session stopped (signal 11, segmentation fault); a new one started` and restarts. An exit returns its status from the loop, so `exit(4)` still exits 4. While an entry runs, the host ignores SIGINT, so Ctrl-C stops the backend and a fresh session starts (D8's interim; measured on a pty: `sleep(3)`, Ctrl-C, `signal 2, interrupt`, then `7 * 6` gives 42).
- **Found on the way, fixed in its own commit:** a diagnostic's source echo (the offending line and its caret) went to `std::cerr` while its header went to the renderer's stream, so a backend's reply carried the header alone, and the pane showed the caret line above its error. `Program::print_diagnostic` now passes its stream to the echo.
- **Gates:** `testrepl_crash` (a segfault, then `int base = 1234` accepted in the fresh session and `base + 1` giving 1235); `testrepl_stdin` (`scanf` reads the transcript's next line; D11's `if`/`else` through the backend). `test_session_backend` adds an exit that is not a signal, `load` + `run` (argv, status, the file's static as a session name, a missing file refused), a refused entry's echo in `rendered`, and a `BackendSession` under `madc_repl_run` (output order, a crash and the message, `exit(4)` ending the loop with 4). The unit suite, 54 REPL/session/madcide/diagnostic integration tests (JIT, `--exe`, `--obj`) and all 74 static gates are green.
- **Not in slice 4:** a Windows backend (the CLI keeps its in-process session there; `testrepl_crash` is skipped on win64 and wine64 with that reason); D8's interrupt that returns to the prompt with the state kept.

### 41.10a F5: run the editor buffer into the session (§37 item 10), designed against the code (2026-09-28)

§37 item 10: F5 clean-runs the editor buffer and then permits calls into its definitions from the REPL. D16 decides what that is: `%run file`, a fresh session, the file run in it, its names left for the prompt (Thonny's F5, IPython's `%run`, `madc -i file`).

**What the code has** (recon 2026-09-28):
- **No function keys.** `ui::key` (`include/madc/bits/ui_enums`) ends at `ins`, then the synthesized `resize` and `wake`. The key owners that would each need them: the spelling (`tui_key_name` / `tui_key_from_name`, `include/madcdis/keys.h`), the terminal parser (`tui_keyparse`, `include/madcdis/tui_model.h`) and its inverse (`tui_key_bytes`), and the page's key map (`include/madc/ui_web/page.js`, `named`).
- **The backend** loads a file by path (`load {path}`, slice 4) and runs its main (`run {argv}`). The editor's buffer is text, often unsaved.
- **The dialect** has no `session_load` / `session_run`.
- **madcide's Run** (the ^B rows) forks the buffer's live parse, and its output goes to the Terminal tab. The REPL tab (§41.9a) has its own session.
- **A file's name does not pick its standard.** `madc k.c` compiles under the command line's standard (madc by default): `sizeof('a')` is 1 without `--std`, 4 with `--std=c17` (measured). `madc -i file` runs the file and the entries under that one standard.

**The design:**
- **Function keys, in the key owners.** `ui::key::fkey`, appended after `wake`, carries the number in `ch` (1–12), as `ctrl` carries its letter. It is spelled `f1`–`f12` both ways. The parser reads xterm's CSI tilde codes (11–15, 17–21, 23–24, with modifier parameters dropped as for the arrows), SS3 `P`–`S`, and the Linux console's `ESC [ [ A`–`E`. `tui_key_bytes` writes the xterm forms back. The page maps `F1`–`F12`; its keydown already prevents a mapped key's default, so F5 never reloads the page. A key nothing binds does nothing (the line editor, the editor).
- **Loading text.** `load {path, text}`: with text, the text is the unit and `path` names it (its diagnostics cite it). `InteractiveSession::load_text(text, path)` is the one parse path, which `load(path)` calls once it has read the file.
- **The dialect's verbs:** `session_load(h, path, text)` (empty text: read the file) and `session_run(h, argv)` (an array of strings), each returning its request's seq; their replies are the `load` / `run` rows.
- **madcide's F5:** a `replrun` command, registered as `Build replrun Run in REPL` and bound `f5 replrun` in each profile (profile data; `pico.keys` is madcide's own vocabulary on single chords, not a pico emulation, so F5 is free there too). F5 shows the REPL tab and gives it the keyboard, then:
  - restarts the session: a fresh one, and a running entry stops with it (Thonny's F5 restarts the backend);
  - writes `%run <name>` in the transcript, as Thonny's shell writes its `%Run` line;
  - loads the buffer's text under its path, and when it loads, runs its main with argv `[path]`, whose output goes to the transcript;
  - leaves the prompt, whose entries call the buffer's functions and read its globals. A refused buffer shows its diagnostics, and the prompt is the fresh, empty session.
- **Its standard is the REPL's** (`replstd`, the engine's default when empty), as `madc -i file` runs a file under the command line's. The buffer's extension does not pick one, as it does not for `madc file`.
- **The buffer's live text runs**, unsaved edits included, as madcide's Run runs the live parse. Nothing is saved first.
- **Thread contract:** unchanged. The key owners are value code, the verbs are confined to the thread that opened the handle, and the pane's state is session-thread bag state.

**Not in item 10, and named:** `%run` / `%load` / `%reset` typed as commands (D24); "Run in current session" (§17.3); a project's manifest standard; Alt/Shift modifiers on keys.

**Slices,** each its own commit, with Tier 1 and Tier 2:
1. **Function keys** (engine). Gate: `test_keys` (both spellings), `test_tui_model` (each sequence, and every key round-trips through `tui_key_bytes`), `test_web_model` (the page's `f5`).
2. **Loading text, and the verbs.** Gate: `test_session_backend` (loaded text's names; a refused text citing its path) and `tests/testsession_run.mad` (dialect code: load text, run main, the names at the prompt).
3. **madcide's F5.** Gate: `testmadcide_repl` (F5 on a buffer: the `%run` line, main's output, then an entry calling the buffer's function; F5 again is a fresh session) and `tests/gui/madcide_repl` (F5 posted through the page's key path).

**Built, slice 1 (2026-09-28, `51a84bc35`): function keys.** `ui::key::fkey` (the number in `ch`), spelled `f1`–`f12`. The terminal parser reads xterm's tilde codes and SS3 forms and the Linux console's, through one table (`fkey_tilde_code`) that `tui_key_bytes` also reads. The page maps F1–F12 and prevents their defaults. Pinned by `test_keys`, `test_tui_model` and `test_web_model`.

**Built, slice 2 (2026-09-28, `2afb078f8`): loading text, and the verbs.** `InteractiveSession::load_text(text, path)` is the one parse path (`load` reads the file and calls it). The wire's `load` takes `text`, and `<ns_madc>` gains `session_load(h, path, text)` and `session_run(h, argv)`. Pinned by `test_session_backend` and `tests/testsession_run` (JIT, `--exe`, `--obj`).

**Built, slice 3 (2026-09-28, `438842d38`): madcide's F5. §37 item 10 is done.**
- **The command:** `replrun` (`cmdREPLRUN`), the menu row `Build replrun Run in REPL`, and `f5 replrun` in `joe.keys`, `neovim.keys`, `pico.keys` and `emacs.keys` (profile data). `case cmdREPLRUN` in the core's dispatcher calls `repl_run` (`madcide_repl.inc`) with the stored buffer, never a view of it.
- **`repl_run`:** a pseudo-buffer or an empty one is refused at the status line. "Is this path a file" has one owner, `path_is_asset`, which moved from `madcide_layers.inc` into the core so that `proj_add` and F5 read it beside the layer owner. The slice had begun a second copy, and `check-madcide-single-owners.sh` now counts the bracket test (one site, with a negative control). An empty text would read the file under `session_load`'s own rule, so the refusal keeps the buffer as what runs. Otherwise it shows the tab and gives the input the keyboard (`repl_show`). A session that existed before is restarted, so the run is clean (Thonny's F5), and the busy, running and completion marks are cleared. Then `%run <basename>` goes into the transcript after the prompt, and the buffer's live text is loaded under its path.
- **The replies** (`repl_reply`): a `load` answering F5's seq feeds its rendered text. Refused, the pane is idle again, with the fresh, empty session. Loaded, it runs main with argv `[path]`, and while main runs the pane is busy and running, so a line typed is main's stdin, as for a running entry. The `run` reply feeds its rendered text and frees the prompt. A `stopped` reply also clears F5's two seqs. The new bag keys (`replloadseq`, `replrunseq`, `replrunpath`) are in the file's state list.
- **Gates:** `tests/testmadcide_repl.mad` section 7, under JIT, `--exe` and `--obj`: F5 from the editor takes the keyboard, and the transcript reads `c11> %run madc_ide_repl.mad`, main's line, then `triple(14)` giving 42 from the buffer's `static`. The buffer's text is unsaved (the file on disk has only `add`). A name entered before a second F5 is undeclared after it, while the buffer's names are back. A refused buffer cites `madc_ide_repl.mad:1:` and leaves the pane idle. `[repl]` and an empty buffer are refused. `tests/gui/madcide_repl.mad`: with joe's key table installed through `ui::bind_keys`, an `F5` keydown dispatched on the page's keyboard element arrives as `replrun`, and main's output reaches the tab on the pump's wakes. Pinned counts moved on purpose: the registry has 53 commands, the pico help 48 rows, and the Build menu 9 items (its separator after `Run in REPL`).
- **Found by the owner after slice 3, fixed in its own commit: quitting after using the REPL tab hung.** `run_tui` runs the client loop inside a `scope` (for the serve face's accept task), and a task spawned by a scope member is a member too, so the scope's end joined the REPL tab's pump, whose stop was sent only by `IdeSession::close()`, afterwards. `IdeSession::stop_tasks()` is now the one owner that stops the session's pumps (a build through `stop_build`, the REPL tab's, and the Terminal's, whose stop had never been sent). `close()`, `run_ide`'s teardown (every client) and `run_lsp` call it before their joins. Gates: `scripts/madcide_quit_gate.sh` (the real TUI on a pty: F5, then `^K q` must exit) and `tests/testmadcide_lsp_repl` (the `--lsp` child exits after `madcide.repl`).
- **Not in slice 3:** everything item 10 names outside it (the typed `%run` / `%load` / `%reset`, "Run in current session", a manifest standard, key modifiers); a Windows backend.

### 41.11a The teaching profile (Phase 5 core, §29), designed against the code (2026-09-30)

The owner (2026-09-30): the next release is the REPL and learning IDE, which means §37 plus Phase 5's core, and then Phase 4. Phase 5's core is:
- the `chthonic` profile and its layout (amended 2026-09-30, the owner: a bundle in the plugin model, `docs/plans/2026-09-30-madcide-plugins.md`, not a hard-coded mode);
- a toolbar with Run and Stop;
- the Variables view;
- clickable diagnostics for F5;
- a beginner menu.

Held for Phase 4, because each one needs its redefinition model: "Run/Reload in current session" (§17.3) and Send Selection (§17.4). Held for Phase 7: Debug (the stepper).

**The precedents** (documented behaviour, not measured here):
- **Thonny's simple mode:**
  - A row of large buttons (New, Load, Save, Run, Debug, the step buttons, Stop), and no menu bar until the user switches to regular mode.
  - Run is F5 (`%Run file` in the Shell).
  - Stop/Restart (Ctrl+F2) restarts the backend whether or not a program runs, and the backend's state is lost.
  - The Variables view is a Name/Value table of the program's globals, refreshed after every command and run. A function shows as `<function square at 0x…>`.
  - An error in the Shell carries a link to its line in the editor.
- **IPython `%whos`:** a Variable/Type/Data table of the interactive namespace. It leaves out the names the startup put there.
- **The Julia VS Code extension's Workspace view:** the bindings of `Main` with their values and types, refreshed after each evaluation.

**What the code has** (recon 2026-09-30, `develop`-line HEAD `61b7a5615`; MC = `tools/madcide/madcide_core.inc`, MR = `tools/madcide/madcide_repl.inc`, SC = `src/madc_session_client.cpp`):
- **The command line.**
  - The file is `argv[1]`, and flags are read only from `argv[2]` on (`madcide.mad:64-145`), so `madcide --learn f.c` would take `--learn` as the file. With no arguments, madcide prints usage and exits 2.
  - A path that does not exist opens empty, as JOE's "New File" does (MC:3055-3070, 3208).
  - The TUI path is `run_tui(argv[1], ro, lvl, addr)` (`madcide.mad:232`) → `IdeSession::open(path, ro)` (MC:7907) → `init_view_es` (MC:7789).
- **The profile set is hard-coded.** `init_view_es` loads `load_status("joe")`, `load_theme("default")`, `load_menu("default")`, `load_layout("default")` and `load_profile("joe")` (MC:7800-7812). Only `toggle_profile` changes one of them at runtime (the keys). `replstd` is read when a session starts (MR:288), but nothing ever sets it.
- **The layout grammar** (`default.layout`, `parse_layout` MC:841):
  - A sidebar docks left or right, and a panel docks bottom or top. Neither splits, so a Variables view cannot sit beside the REPL inside the bottom panel. It docks as a sidebar, as Thonny's does.
  - `repl` is already a legal panel view (`default.layout:24`).
- **The menu file:**
  - `MENU COMMAND TITLE [WHEN]` rows go into the root's `menu` hint. `--gui` draws a native menu bar from it (`src/ns_ui.cpp:467`), and the TUI reads none of it.
  - The palette-only menu is found by a string compare on its title (`m["title"] == "palette"`, MC:1564), which is an enum-over-strings debt.
- **No toolbar anywhere.**
  - The UI role vocabulary (`include/madcdis/uinode.h:112-126`) has `action`. The TUI and the page both draw it as inert `[label]` text (`tui_model.h:869`, `page.js:626`), with nothing to click.
  - The data shape a strip already uses is the pane `tabs` hint: an array of `{title, action, code}`. `web_model` passes it to the page with each action's code (`web_model.h:606-640`), the page draws it (`tabStrip`, `page.js:289`), and a click posts `{kind: action}` (`page.js:905-930`). The TUI reads the same hint as a header line (`read_header`, `tui_model.h:793`).
  - The page's `.cf-btn` buttons (dialogs, confirms) already post their action.
- **Stop.**
  - The REPL tab has no Stop or Restart command.
  - F5's `repl_run` (MR:582) restarts the session and clears the busy, running, completion and F5 marks inline.
  - There is no interrupt op (D8's interim: an interrupt stops the backend).
- **Diagnostics.**
  - The backend attaches the rows of `diagnostic_rows_from_child` (`src/madc_program.cpp:4651`: `severity`, `severity_code`, `phase`, `phase_code`, `message`, `file`, `line`, `column`) only to a taken `offer` reply (SC:213-218). The `load` and `run` replies carry `ok`, `status` and `rendered` (SC:231-257).
  - `repl_reply` (MR:348) feeds `rendered` into the transcript and nothing more.
  - The Problems pane reads the bag's `diags` rows. Its writers are check, save and build (MC:1798, 4855, 5132, 5164, 5220). `diag_items` (MC:6999) gives each row the `cmdGOTO` code, and `goto_pane_row` (MC:1895) opens the row's file when it differs from the buffer's and moves the caret to its line.
  - F5's diagnostics already cite the buffer's path and line (`testmadcide_repl` section 7).
- **Bindings.**
  - `Program::visit_top_level_names` (`src/madc_complete.cpp:276`) is the one walk over what an entry can name, used by completion and `?name` (§41.8a). Its kinds are object, function, type, tag, namespace, templates, keyword, macro, header name, dialect word and result, and the locations come from `object_location` / `function_location` (`REPL[N]:line` or a file's line).
  - The value text an entry shows is the D10 walk, lowered by the CIR builder and handed off by `__madc_session_show` (`src/madc_cir.cpp:1722`).
  - `?x` prints the type and the location, not the value. `%whos` was named out of item 8 (§41.8a). The command registry is `src/madc_session.cpp:55-60`.
- **Views.**
  - `enum ide_view` (`madcide_enums.inc:468`), registered through `view_name` / `view_title` / `view_of` / `view_show_cmd`, with the composition switch in `compose_chrome_pane` (MC:7073).
  - Problems and Outline are item rows under a choice (MC:7147-7165), and that is the shape a Variables view takes.
- **Release-path bugs:**
  - B85 (SILENT): madcide saves and quits only when started from the repo root. The line editor's verbs load from cwd-relative paths, and the package does not ship them, so the released madcide can neither save nor quit, and a missing key profile leaves the user stuck (measured 2026-09-30, `BUGS.md`).
  - B84: JOE's `^W` deletes the whitespace and the next word. JOE 4.6 deletes the run of the caret byte's class (owner, 2026-09-30; measured against JOE on the container).
  - B86: madcide refuses to start on a file that does not exist, where `^K E` and every editor open a new file (owner, 2026-09-30).
  - B55: Tab inserts nothing in madcide's editor. A beginner cannot indent.
  - B8: the caret line is misdrawn on a line with a tab. Beginners' code is tab-indented, and F5's diagnostics show that line.

**The release (owner, 2026-09-30, amending the scope above):** the next release carries:
- the REPL (§37);
- the bug fixes already on this branch, and slice 0's;
- madcide plugins: the whole plugin design (`docs/plans/2026-09-30-madcide-plugins.md`), every stage;
- the Thonny-like teaching IDE as a madcide plugin, `chthonic` (the owner's name, 2026-09-30: C + Thonny, and a real word).

There is no patch release before it: B85's fix rides this release.

**Where each piece lives:** the plugin carries what is specific to teaching. A capability any profile could use lands with its owner.

| Piece | Owner |
|---|---|
| The bindings owner, `%whos`, the `bindings` wire op and verb; rows in the `load` and `run` replies | the engine (madc) |
| Stop (`replstop`), Language (`repllang`), bindings on request (`replbindings`), F5's diagnostics into Problems, the pane's reply events, the session starting when the layout shows the REPL | madcide's REPL pane (core) |
| The untitled buffer, one open rule (B86), the argv parse | madcide core |
| Bundles, the manifest, settings, `--profile`, the toolbar placement and its rendering, contributed commands, views and events, the transports | the plugin system (core) |
| Modifiers on every key and the primary modifier (step 3e) | the engine's key owner (`include/madcdis/keys.h`), the page and the terminal decoder |
| The choice list, the key-style list, selection and the clipboard, `settings.json`'s writer (steps 3 and 3e) | madcide core |
| `thonny.keys`, `vscode.keys`, and each `.keys` file's display name (step 3e) | the profiles (data) |
| `chthonic.plugin`, `chthonic.layout`, `chthonic.menu` (with its toolbar rows), its keys (`pico` until step 3e, then `thonny`), the `repl.std` setting, and the Variables view's code | the `chthonic` plugin |

- **The Variables view is the `chthonic` plugin's code:** its first real code, and the plugin system's first consumer.
  - It is a contributed view (`variables`), not a core view kind.
  - Its code asks for the bindings through a command, `replbindings`, and the rows come back in the pane's `bindings` reply event.
  - It formats them onto its view's bag key, and a row's activation goes through the core's navigation owner.
  - It never holds the session's handle, which belongs to the REPL pane. So it runs under every transport, the out-of-process host included.
- **The plugin ships as source plus a prebuilt library per platform** (`.so`, `.dylib`, `.dll`: ROADMAP 6.5). The release exercises the plugin system end to end on every platform lane.

**The design** (the pieces below keep their design; where each lives is the table above):
- **The `chthonic` profile is a bundle** (amended 2026-09-30; the plugin design's Stage A, `docs/plans/2026-09-30-madcide-plugins.md` §5.1-5.2, §8):
  - A bundle is a plugin with no code: a `<name>.plugin` manifest (JSON) naming its data contributions (keys, layout, menu, theme, status) and its settings. `default` and `chthonic` are the two madcide ships. There is no `--learn` and no workspace name in the code.
  - The active profile comes from the command line (`madcide --profile NAME`), then `settings.json` (`"profile"`), then `default`, so nothing changes without either.
  - `init_view_es` loads what the active bundle names, in place of the hard-coded `joe` and `default`. The bundle's name is kept on the bag (beside `profile_dir`) for a later runtime switch.
  - A missing or refused file that a bundle names falls back to the baked default, and the status line says so, through the rescue-announcement pattern the key profile already uses. A refused manifest refuses its bundle, with the reason.
  - A profile selects no face. It composes under the TUI, `--gui` and `--serve` alike, and a desktop launcher passes `--profile chthonic --gui`.
  - **`chthonic`'s keys are `pico` until step 3e:** single chords (`^S` save, `^Z` undo, `^Q` quit, `^W` find, F5 run), with no `^K` prefixes to teach. They clash with Thonny's keys (`^X` is discard, `^V` moves the block, `^O` is Outline, `^F` the project window, `^W` Find where Thonny closes the tab). Step 3e gives chthonic Thonny's keys, with every style a menu choice.
  - **`chthonic`'s settings** give `repl.std` (Q2 below), the REPL standard's first home (`replstd` is read today, but nothing sets it).
  - **The command line:** the file becomes the first argument that is not a flag, and flags may come anywhere (`madcide --profile chthonic f.c` and `madcide f.c --profile chthonic`). `ro` stays a positional word. The strings are compared only at this input boundary. The usage text lists `--profile`.
  - **`chthonic.layout`:**
    ```text
    @window main
    pane editor tabs views source focus
    pane sidebar right 25% tabs views variables
    pane panel bottom 35% tabs views repl problems
    ```
    The REPL is the visible panel's first tab. No project tree, no Outline, no Terminal or Output (F5's output is the REPL's).
  - **The session starts at open when the layout shows it.** A pane visible at startup whose active view is `viewREPL` starts its session in `init_view_es` (`repl_start`), as Thonny starts its backend. That is layout data, never a test of the bundle's name (Rule #7), and the default layout keeps the REPL hidden, so it forks nothing.
- **The beginner menu (`chthonic.menu`), data only:**
  - File: Open…, Save, Save As…, Quit.
  - Edit: Undo, Redo, Find.
  - Run: Run (`replrun`), Stop (`replstop`), Language… (`repllang`: a choice of C17, C++17 and madc that sets `repl.std` and restarts the session; decided 2026-09-30).
  - View: Shell (`repl`), Variables (`variables`), Problems (`problems`).
  - Help: Help.
  - No Build, no Project, no Views/MC11/Nexus rows (§16: the power surface is not advertised).
- **The toolbar.**
  - **Placement becomes an enum.** A menu row's MENU word keeps naming its menu, and two words are placements: `palette` (already) and `toolbar`. Each converts once, at load, to `menu_place { mpBAR, mpPALETTE, mpTOOLBAR }` on the item. The `m["title"] == "palette"` compare becomes a switch on the code (enum-over-strings, fixed on the way).
  - `chthonic.menu` gives the toolbar Open, Save, Run and Stop. `default.menu` gives it nothing, so the default workbench is unchanged.
  - **The composed form** is a root hint `toolbar`, beside `menu`: an array of `{label, action, code, chord}`. The chord is the one the loaded personality binds, as the menu shows it. This is the shape the tab strip's hint already has.
    - `web_model` passes it with its codes, as it passes a tab strip.
    - The page draws it as a row of `.cf-btn` buttons above the workbench, and a click posts the action through the existing handler.
    - The TUI draws one line, `[Run F5] [Stop]`.
    - The `action` role stays what it is.
  - **Stop: `replstop` (`cmdREPLSTOP`).** It restarts the session whether or not an entry runs (Thonny's Stop/Restart: a fresh backend, the state lost). The transcript reads `[stopped; a new session started]`, and the Variables rows clear.
    - F5's restart and Stop share one helper, `repl_restart`, lifted out of `repl_run`, so the marks it clears live in one place.
    - No key this release: Thonny's Ctrl+F2 needs modifiers on function keys, which §41.10a named out.
- **F5's diagnostics into Problems.**
  - The backend attaches the same rows builder's rows to the `load` and `run` replies. A run's runtime rows exist (an undefined reference is recorded with phase `runtime`).
  - The verb row already carries `diagnostics`.
  - `repl_reply`, on the reply to F5's load (and to its run), writes the rows into the bag's `diags`, the key the Problems pane reads. So each row is navigable through `cmdGOTO` / `goto_pane_row`, with no new navigation code, and a clean load clears the stale rows, as check does.
  - An entry's own rows (`REPL[N]:…`) do not go into Problems: they cite no file the editor holds.
  - The transcript keeps the rendered text. A clickable link inside the transcript is §18's later GUI affordance.
- **The Variables view** (the `chthonic` plugin's code; the engine and pane pieces are core).
  - **One engine owner: `InteractiveSession::bindings(rows)`.** It walks `visit_top_level_names` for the names the session defined: an origin from an entry or a loaded unit, never a header or the prelude (IPython's `%whos` hides the startup namespace too).
  - It keeps objects and functions. A row is `{name, kind, type, value, file, line}`:
    - `kind` is a code from `<bits/session_enums>`'s new `madc::name_kind`, which the engine's `TopLevelName::Kind` aliases, as `OfferState` aliases `madc::offer_state`;
    - `type` is the Program-level type spelling §41.8a moved (`int`, `int (int)`);
    - `value`, for an object only, is the D10 walk's text.
  - **The values come from one quiet entry.** The request compiles one internal entry whose body shows each object through the D10 walk. It takes no `REPL[N]` number, no history and no result name, and its module stays loaded, as every entry's does.
  - **The walk runs shallow here.** A pointer, at any depth, prints its address and is never dereferenced, so a dangling or wild pointer can never crash the backend from a refresh nobody asked for. Typing `p` at the prompt still shows the string, as an entry the user asked for. An aggregate prints at most 16 elements, then `…`, and a row's text is capped at 80 characters.
    - Both limits are parameters of the D10 walk, not a second walk.
    - The slice verifies that the walk calls no user code (a conversion, a getter). If some type needs it, that type's row shows its type alone.
  - **`%whos`** is the registry command that prints the same rows as a Name/Type/Value/Origin table (`src/madc_session.cpp:55`). That gives one owner and two renderings, as completion and `?` share one walk.
  - **Getting the rows to madcide:** a wire op `bindings`, the `madc::session_reply::bindings` kind, and the verb `madc::session_bindings(h)`, which returns the request's seq, as its siblings do. The reply carries `rows`.
  - **The pane's side (core):**
    - `replbindings` sends the request.
    - `repl_reply` publishes its replies as events on the event feed, one kind each: an entry taken, F5's run returned, F5's load refused, the session stopped, the bindings answered. That is the `repl_event` enum, with the reply's row. A plugin subscribes to the kinds it needs.
  - **The plugin's side (`chthonic`):**
    - It contributes the view `variables` (title "Variables", bag key `chthonic.vars`) and the command `variables` that shows it.
    - The generic contributed-view arm of `compose_chrome_pane` renders the key's rows as item rows under a choice, as Problems does: name, type, value and origin, the columns padded to the widest name and type.
  - **Refresh:**
    - The plugin's event handler runs `replbindings` after an entry is taken, after F5's run returns, and after F5's load is refused, but only while its view is visible (a hidden view costs nothing).
    - On the bindings event it writes the rows. A stopped event clears them.
    - A reply for an older seq is dropped, as the completion replies are.
  - **Activating a row** goes to its declaration when its `file` is the buffer's (`cmdGOTO` over the row's `file` and `line`). `goto_pane_row`'s `bool outline` becomes the view whose rows it reads (Problems, Outline, or a contributed view's key), so one navigation owner serves all three.
  - **What it shows:** globals only. After F5, `main` has returned, and its locals are gone, as Thonny's Variables view shows `__main__`'s globals outside the debugger.

**Enums added:** `cmdREPLSTOP`, `cmdREPLLANG` and `cmdREPLBINDINGS` in `cmd_table` (`variables` is the plugin's contributed command); `repl_event` (the pane's reply events); `menu_place`; `madc::name_kind` and `madc::session_reply::bindings` in `<bits/session_enums>`; the wire's `Op::bindings`; `InteractiveSession::Command::whos`.

**Thread contract:** unchanged from §41.9a.
- The session handle is confined to the thread that opened it, and the backend is single-threaded, so `bindings` runs between entries like every request (D9).
- The rows are a snapshot. A task the program left running may change a value after it is read, and IPython's `%whos` has the same limit.
- The active bundle's name, the toolbar rows and `chthonic.vars` are bag state on the session's thread, like every pane's. The plugin's handlers run on that thread (in-process) or through the seat (host), per the plugin design's §7. The bundle registry is built at load and read-only afterwards (the plugin design's §7).
- The shallow walk only reads the program's storage.

**Not in Phase 5's core, and named:**
- Debug and the step buttons (Phase 7).
- Run/Reload in the current session, and Send Selection (Phase 4).
- A runtime "switch to the full workbench" (Thonny's regular-mode link; it needs a layout reload at runtime).
- An object inspector for a Variables row.
- Pointer targets read safely (Phase 6's Memory view needs a fault-safe peek, and this view waits for it).
- A `chthonic.status`.
- A completion popup.
- A Windows backend.

**The release order,** each step its own commit with Tier 1 and Tier 2, a batch checkpoint after each batch, and the seam battery with every platform lane after step 9:
0. **The release-path bugs, each fixed in its own commit, B85 first (it is silent, and it is in the released package):**
   - B85 (done): one data-location owner for the profiles, the verbs and the checks (`resolve_data_dir`, which `resolve_profile_dir` now calls), the package shipping `verbs/` and `checks/`, madcide refusing to start without them, and the startup hint naming the loaded table's own chords. Gates: `scripts/madcide_save_quit_gate.sh` (fulltest: save and quit from a foreign cwd, with the profiles and with the rescue keys; no verbs refuses to start; a negative control), the install gate saving and quitting with the installed package, and the quit gate from a foreign cwd. The key fallback chain (the requested profile, then the default profile's, then the rescue set) lands with bundles (step 1), where a profile first names its keys.
   - B84 (done): a class-run extent beside `text_buffer::word_right`, read by `delword`. Gate: a model test with JOE 4.6's measured table (`BUGS.md`).
   - B86 (done): the launch's document through `open_buffer_doc`, with one rule (a missing path is a new file; an existing path that cannot be read is refused, the reason from `php::is_dir` / `php::is_readable`, both new, PHP parity). Every document is minted by `new_document`; a view buffer's name is never looked up on disk. Gate: `testmadcide_cli` (`madcide new.c` opens and says `New file new.c.`, the message `^K E` already gave, with nothing written until a save; a directory is refused with the reason, at launch and at `^K E`).
   - B55 (done): Tab is a text key of the edit core, beside Enter: `edit_key` types a tab through the one typed-text owner (`edit_text_run`), JOE 4.6's `a\tb`; the editor's node carries the `tabkey` hint while the editor has the keyboard (`editor_has_keyboard`), so the focus owner hands it the key; vi NORMAL mode types none (Tab inert). Not a profile binding, as first filed: a bound sequence resolves before the focus owner, so it would take tab from every list and dialog. Gates: `testmadcide_tab` and `tests/gui/madcide_tab` (pre-fix: a focus cycle, nothing typed).
   - B8: the caret renderer expands tabs and counts screen width, against gcc's `2:19` (D26's order: the caret drawing first, then the start-column switch). Part 1 done (bce128835): the echo and the caret through `madc::line_layout`, one layout rule with the line editor. Part 2, the start column, gets its own focused session after step 8 (design and inventory under D26), since it moves the parser's statement extents.
   - B87 (found and fixed 2026-09-30): madcide's terminal caret drifted one column right per extra byte of a UTF-8 character (`tui_model::expand_line` counted bytes; `put` wrote a byte per cell). Now one layout owner for the grid, the line editor and the compiler's caret (`madc::line_layout`, with the ^T tab width); a grid cell holds one code point and a wide glyph's tail; the viewport slices by columns (`line_columns`); `wrap_text`, composed lines' spans and the status line's alignment (`ui::text_columns`) measure columns.
1. **Bundles** (the plugin design's Stage A):
   - the manifest loader, for the data kinds and settings;
   - the plugin search path, with the user's directory;
   - `settings.json`, the argv parse and `--profile`;
   - `default.plugin`, and `chthonic.plugin` with `chthonic.layout` (without the sidebar) and `chthonic.menu`;
   - the REPL session starting when the layout shows it.

   Gate:
   - `testmadcide_cli`: `--profile chthonic f`, `f --profile chthonic`, usage.
   - A new `testmadcide_chthonic` model test: the composed panel visible with the REPL active, the menu bar's rows, no project or outline pane, a session running at open.
   - A manifest with an unknown word refused with its reason.
   - A user-directory bundle overriding a shipped one by name.
   - `"profile": "chthonic"` in a test `settings.json` (under a test configuration directory, `MADCIDE_CONFIG_DIR`) selecting it.

   Part 1 done (the loader, the search path, `settings.json`, the key fallback chain; `tests/testmadcide_bundles`; plugin design §8). Part 2 done (the argv reader and `--profile`, `chthonic.plugin` / `.layout` / `.menu`, the REPL starting when the layout shows it; `tests/testmadcide_chthonic`, `testmadcide_cli`). 1d done: no file opens an untitled buffer. The buffer can exist without a path, so it has no placeholder name. `new_document` with no path mints it with the madc kind, `doc_untitled` tests it, and `doc_name` names it `untitled` where a name is meant (the unit a parse, lens or F5 is given, the status line's `%n`, the tab). Save and ^K X ask for the name first (Save As; ^K X's answer then quits), and Save As drops the parse and git handles keyed on the old name. A native build, a build command and a project change are refused with what to do. The headless faces (`-c`, `--mcp`, `--lsp`, `--serve`) still need a file. The package smokes run `madcide --help`. Gates: `tests/testmadcide_untitled`, `testmadcide_cli`. Next: step 2.
   - `testmadcide_layout`'s pin of `default.layout` is unchanged.
2. **The plugin system's contribution points** (the plugin design's B1): contributed commands (ids interned above the built-in range), contributed views (the generic compose arm over a bag key), events (the feed, and `repl_event`), and the toolbar placement: `menu_place`, and the `toolbar` hint through `web_model`, the page and the TUI. All are exercised first by madcide's own code (`builtin`).

   Gate: `testmadcide_chthonic` (the toolbar hint's rows and codes), `test_web_model` (the hint and its codes), a model test contributing a command and a view from a built-in module, and `check-madcide-command-registry.sh` learning the contributed range.

   Done 2026-09-30, in four parts (plugin design §8, Stage B item 1): the toolbar (`menu_place`, the `toolbar` hint through `web_model`, the page and the TUI, each renderer resolving the chord); contributed commands (`plugin_command`, codes from `cmd_contrib_base()`, every input boundary through the world-aware converters, the editor tier); contributed views (`plugin_view`, kinds from `view_contrib_base()`, the layout reader and writer, `contributed_view_node`, one row verb per view, `goto_pane_row` over the view whose rows it reads); and event handlers (`plugin_event` over `repl_event`, published by `repl_reply`). Gates: `tests/gui/madcide_toolbar`, `test_web_model`, `test_tui_model`, `testmadcide_chthonic`, `tests/testmadcide_contrib`, `check-madcide-command-registry.sh`. The Build menu's palette rows now attach by the build command, not the menu's title. Next: step 3.
3. **The REPL pane's core pieces:** `replstop`, `repllang` and `repl_restart`; F5's diagnostics into Problems (the rows in the `load` and `run` replies, and `repl_reply` writing `diags`); the bindings owner, `%whos`, the wire op, the verb, and `replbindings`; the pane's reply events.

   Gate: `testmadcide_repl` (Stop while an entry blocks on stdin gives a fresh session and the transcript line; F5 on a refused buffer fills Problems, and `cmdGOTO` on its row moves the caret; a clean F5 empties Problems; Language restarts under the chosen standard); `test_session_backend` (a refused text's rows cite its path and line; the bindings rows' kinds, types, values and origins; `char *p = (char *)1;` listed by address with the backend alive, the negative control for the shallow walk); `test_repl_session` (`%whos` under C17, C++17 and madc); `tests/testsession_bindings.mad` (JIT, `--exe`, `--obj`); the refresh's cost measured over 50 bindings. The registry's pinned count moves on purpose.

   Run ▸ Language… is a row list through the one choice list that step 3e's Key bindings… also uses (rows of `{title, command, argument}`, the current row marked; a choose runs the row's command with its argument, `cmd_takes_arg`). No pane is specific to one choice.

   Progress: Stop done (1fdd421d5); `session_restart(handle, standard)` done (e037f60bd); Run ▸ Language… done on the choice list (`choice_show` / `choice_action`, the one row verb `cmdCHOICE`, `cmd_takes_arg`; Run ▸ Language… in `chthonic.menu`, Build ▸ REPL Language… in `default.menu`; `testmadcide_repl` section 9); F5's diagnostics into Problems done (engine 1370ba53a: the `load` and `run` replies carry the rows, `attach_diagnostics` / `reply_diagnostics`; madcide: `repl_problems` writes them to `diags` on F5's load and run replies; `testmadcide_repl` section 7, `test_session_backend`); the bindings owner and `%whos` done (`Program::session_bindings` over `session_units`, `InteractiveSession::bindings`, `%whos` as `Command::whos`; values from one quiet entry through `__madc_show_row` under the row's `ShowLimits`; `test_repl_session`'s three bindings cases). B94, found on the way, is worked around in its own commit (200d60b14). The `bindings` wire op and verb done (`Op::bindings`, `madc::session_reply::bindings`, `Reply::rows`, `SessionClient::bindings` / `bindings_wait`, `madc::session_bindings(h)`; `test_session_backend`, `tests/testsession_bindings`). `replbindings` and `reBINDINGS` done (`cmdREPLBINDINGS`, `repl_bindings`: the newest request's answer is published with its rows, an older seq's dropped, a restart owes none; `testmadcide_contrib` section 10, `testmadcide_repl` section 10). Step 3 is done; the Variables view that consumes the event is step 6. Found on the way: B98 (a value literal cannot nest a brace list), filed.

3e. **The key bindings** (owner, 2026-09-30: chthonic improves on Thonny with a menu that switches the key style). Its own batch, after step 3's pieces and before step 4.
   - **The key-style list:**
     - Tools ▸ Key bindings… in `chthonic.menu` (Thonny's Tools menu holds its settings) and a row in `default.menu`, through the choice list above.
     - Its rows are the `.keys` files `toggle_profile` already reads. One owner lists the profiles there are, for the cycle and the list alike, so a dropped-in `.keys` file joins with no code.
     - Each `.keys` file gains its display name as data: JOE, Vim, Emacs, Pico, Thonny, VS Code. The current style is marked.
     - The choice persists. `settings.json` gains its writer beside its reader; madcide only reads it today.
   - **Modifiers on every key** (the key owner, `include/madcdis/keys.h`, gated by `check-one-key-owner.sh`):
     - Today a key is Ctrl+letter, F1-F12 or a named key, with no Shift, Alt or Cmd. So Ctrl+Shift+S, Ctrl+F2, Shift+arrows, Ctrl+Space and Ctrl+plus cannot be bound.
     - Shift, Alt and Ctrl combine with any key. A primary modifier is Ctrl on Linux and Windows and Cmd on macOS, as Thonny binds each command twice (`<Control-…>`, `<Command-…>`).
     - The page passes the modifiers; today it reads Ctrl+letter only (`page.js`).
     - The terminal decodes xterm's modified-key sequences where the terminal sends them. A chord a terminal cannot tell apart (Ctrl+Shift+S from Ctrl+S) belongs to the GUI, and the slice lists those chords.
   - **Selection and the clipboard:**
     - Shift+motion extends a selection, which is the existing mark..caret block, adopted rather than duplicated.
     - Cut, Copy, Paste and Select all become commands. They use the system clipboard under `--gui` and an in-process clipboard in the terminal. The IDE has no clipboard today; the REPL line editor's yank is its own.
   - **Two new profiles,** data once the above exist:
     - `thonny.keys`, from the table below;
     - `vscode.keys`, from VS Code's published default keymap, read at the slice's start. Until the debugger (Phase 7), VS Code's F5 and Ctrl+F5 both run.
     - `chthonic.plugin`'s keys become `thonny`.
   - **The menus gain Thonny's rows for commands madcide has:** File ▸ New and Close; Edit ▸ Cut, Copy, Paste, Select all, Replace, Go to line and Toggle comment; View ▸ the font size. A row whose command is missing is listed in the slice, not invented.
   - **Thonny's keys** (its source, `github.com/thonny/thonny` master, each command's `default_sequence`, read 2026-09-30; Cmd for Ctrl on macOS):

     | Menu | Command | Key |
     |---|---|---|
     | File | New · Open… · Close · Close all | Ctrl+N · Ctrl+O · Ctrl+W · Ctrl+Shift+W |
     | File | Save · Save as… · Save All · Exit | Ctrl+S · Ctrl+Shift+S · Ctrl+Alt+S · Ctrl+Q / Alt+F4 |
     | Edit | Undo · Redo | Ctrl+Z · Ctrl+Y |
     | Edit | Cut · Copy · Paste · Select all | Ctrl+X · Ctrl+C · Ctrl+V · Ctrl+A |
     | Edit | Find & Replace · Go to line · Toggle comment · Auto-complete | Ctrl+F · Ctrl+G · Ctrl+3 · Ctrl+Space |
     | Run | Run current script · Stop/Restart backend | F5 · Ctrl+F2 |
     | Run | Interrupt · Send EOF | Ctrl+C (in the Shell) · Ctrl+D |
     | Run | Debug (nicer) · Debug (faster) | Ctrl+F5 · Shift+F5 |
     | Run | Step over · Step into · Resume · Run to cursor | F6 · F7 · F8 · Ctrl+F8 (Step out has none) |
     | View | Font size up / down · Full screen | Ctrl+plus / Ctrl+minus · F11 |

   - **Named out:**
     - Thonny's debugger keys wait for Phase 7's stepper;
     - Interrupt needs D8's interrupt op (Stop restarts meanwhile);
     - Auto-complete waits for the editor's completion popup;
     - VS Code's multi-cursor.
   - **Thread contract:** a key table and the clipboard belong to the session's thread. `settings.json` is written whole by the session that changed it, and between processes the last write wins.

   Gate:
   - `test_keys`: modified spellings round-trip, and a primary chord resolves to Ctrl or Cmd by platform.
   - `test_tui_model`: xterm's modified sequences decode.
   - `test_web_model` and a `tests/gui` case, under `thonny`:
     - a Ctrl+Shift+S keydown posts `saveas`;
     - Shift+Right selects;
     - Copy then Paste goes through the clipboard.
   - `testmadcide_chthonic`: Tools ▸ Key bindings… lists the six styles with the current one marked, and choosing VS Code rebinds and persists to a test `settings.json`.
   - Every existing profile's pins are unchanged.

   Progress: the key-style list done (slice 1): `key_profile_names` is the one list of profiles (the `profile` cycle and the list read it); a `.keys` file's `@title NAME` line is its display name (`keys_title_line`, the one reader: the binding parser and help pass over it); `keystyle` (`cmdKEYSTYLE`, `cmd_takes_arg`) opens the choice list or becomes its argument; the choice is kept per bundle, `"keys": { "<bundle>": "<style>" }` in `settings.json` (`keys_choice` / `keys_choice_save` / `user_settings_save`, the writer beside the reader), and the open's key chain reads it first; the `profile` cycle stays the session's own and keeps nothing; Tools ▸ Key bindings… in `chthonic.menu`, View ▸ Key Bindings… in `default.menu`; `testmadcide_chthonic` section 7. Decided: a kept style is the bundle's, so a choice in chthonic leaves madcide's keys as they ship. Modifiers done (slice 2): `tui_keyev::mods` (`ui::key_mod` bits in `<bits/ui_enums>`, xterm's parameter less one); the one owner spells them both ways (`ctrl+shift+s`, `ctrl+f2`, `shift+right`, `alt+f4`, `cmd+s`, `ctrl+plus`; a plain Ctrl+letter stays `^s`; `primary+` is Ctrl, or Cmd on macOS); a modified key nothing binds reads as its key (`key_unmodified`, the resolver's head and continuation, madcide's scope tables through `ui::key_unmodified`), so every existing profile and terminal behaves as before; a printable under Ctrl, Alt or Cmd never types (`key_types`); the terminal decodes xterm's modified cursor and function keys, CSI Z, modifyOtherKeys and CSI u, and NUL as Ctrl+Space, and `tui_key_bytes` writes each back; the page spells Ctrl, Shift and Alt (AltGr and Option characters still type). Gates: `test_keys`, `test_tui_model`, `test_web_model`. Named out: a terminal that reports no modified keys sends Ctrl+Shift+S as Ctrl+S, Ctrl+digit and Ctrl+plus/minus as nothing, so those chords are the GUI's; Cmd with a printable stays the browser's (Cmd+C / Cmd+V are its clipboard) until slice 3; `primary` resolves on the engine's platform, so a browser on a Mac driving an engine on Linux reads `primary` as Ctrl. The engine's half of selection and the clipboard done (slice 3a): Shift reaches a binding without it (`key_without_shift`, `key_lookups`: as pressed, then without Shift, then unmodified), and an action event carries the modifiers held on its last key (`key_step::mods`, the event's `mods`), so Ctrl+Shift+Left arrives as Ctrl+Left's binding with Shift held; `ui::clipboard_set` / `ui::clipboard_get` reach the platform's clipboard where the target has one (the webview host's `clip_set` / `clip_get` ops over `madcwebview_clipboard_set` / `_get`: GTK4's GdkClipboard, Cocoa's general NSPasteboard, Win32's CF_UNICODETEXT), a read answering with `\n` line ends; elsewhere (the terminal, a browser page) they answer false and the application keeps its own copy; the page sends Cmd with a printable as a key only where the engine's primary is Cmd (the page's `data-primary`), and the browser's own Paste types the clipboard's text with its line breaks. Gates: `test_keys`, `testuihostfake`, `tests/gui/ui_web_clipboard`. Named out: a terminal's own clipboard (OSC 52) is not used, and a browser page pastes the IDE's copy, since a page asks the user before it reads the clipboard. madcide's half done (slice 3b): the ONE selection rule is `selection_range` in the shared editor core (`tools/texteditor/editor_events.inc`; the edit node's highlight, a peer's presence highlight and `block_range` read it — they were three copies); a GUI selection is the es `selgui` flag beside the markers, set by Shift with a motion (`motion_select`, run by `edit_key`'s motions and by the dispatcher's word / top / bottom motions, Shift riding the chord's `mods`), Select all and a pointer drag (`select_span`); typing, Backspace, Delete, Enter and a paste replace it (`selection_replace`), a motion without Shift drops it, and the block keys leave it unset (JOE's block keeps its rules); the flag travels with the markers through the REPL field's park and the buffer rows, and a split window's parked markers come back as a block. Cut, Copy, Paste and Select all are commands (`cmdCUT`, `cmdCOPY`, `cmdPASTE`, `cmdSELECTALL`; Edit rows in `default.menu`; field commands, so the REPL input takes them) over ONE cut rule (`cut_selection`: vised's ^K, madcide's Cut and JOE's ^K Y, which were three copies), `copy_selection` and `paste_text`, and the session's own copy (es `clip`); a client that reaches the platform's clipboard pushes the fact (`IdeSession::clipboard`, from `ui::clipboards`), so a copy parks a `clipset` request and a paste a `paste` request the client answers with `cmdPASTE` carrying the platform's text (`cmd_takes_arg`). Gates: `testmadcide_select` (Thonny's and VS Code's behaviour on the same keys); `check-one-selection-rule.sh` (fulltest: the selection rule and the clipboard's writers have one owner each). Profiles and menus done (slice 4): `thonny.keys` (Thonny's table above, `primary+` for its Ctrl/Cmd pairs, Tk's word and end motions) and `vscode.keys` (VS Code's Linux sheet and default-keybindings page, read 2026-10-01; F5 and Ctrl+F5 both run, Shift+F5 stops); `chthonic.plugin`'s keys are `thonny`; `chthonic.menu` gains Thonny's Edit rows Cut, Copy, Paste, Select all and Go to line. Missing commands, listed in each profile's header and `chthonic.menu`, not invented: New, Close, Close all, Save All, Replace, Find previous, Toggle comment, the editor's completion, Interrupt and Send EOF as their own commands, move / insert line, Go to Definition at the caret, next problem, a command palette, the font size, full screen, and the debugger (Phase 7). Gates: `testmadcide_chthonic` section 7 (six styles listed, Thonny current; choosing VS Code rebinds and is kept), `testmadcide_select` (now under Thonny's and JOE's keys), `tests/gui/madcide_clipboard` (Thonny's Ctrl+A, Ctrl+C, Ctrl+End, Ctrl+V sent from the page in a real window, through the platform clipboard). Step 3e is done; its batch checkpoint follows.
4. **The `library` transport** (the plugin design's B2, G1, G5): the API table, `--build-plugin`, versioned refusal. Linux first, then macOS and Windows once `madc -shared` emits `.dylib` and `.dll` (ROADMAP 6.5, its own commits in the Mach-O and PE writers).

   Progress: the engine's half done (part 1, plugin design §8 Stage B item 2): the build verbs' `shared` kind, `build_native`'s `-I` directories, and `madc::library_open` / `library_symbol` / `library_close` / `library_suffix` (bound at open, the reason as data). Gate: `tests/testbuild_shared`. madcide's half done (part 2): `<madcide/plugin>` and its API table, the manifest's `"code"`, a library's activation in the session's world with versioned refusal (a refused activation leaves nothing behind), `settings.json`'s `"plugins"`, and `madcide --build-plugin DIR`; the packages ship the plugin headers. Gate: `tests/testmadcide_plugin_library`. Windows done (part 3): the PE writer emits a DLL (an export directory, its own entry stub), and a plugin DLL imports the engine from `libmadc-0.dll`; both gates run under wine. macOS: the Mach-O writer emits `MH_DYLIB` (part 4, `scripts/macho_dylib_gate.sh`); D5 built (part 5: `libmadc-0.dylib`, `LC_RPATH`s, the darwin emit lane loading it; the darwin skips removed, so the seam's darwin lane runs both gates). Open: madcide in the mac tarball (the native release job builds it).
5. **The `source` transport** (B3, G3): a plugin compiled into the running process when it activates, with its cost measured against `library`.

   Progress: done (plugin design §8 Stage B item 3). `madc::code_open` / `code_symbol` / `code_close` compile a source file into the running process and keep it; madcide activates a plugin's library when there is one it accepts, else its source, and a refused library's source replaces it with the reason on the status line. Measured: a library about 0.07 ms, a source about 16 ms cold and 7-8 ms warm, so activation stays at load with the library first. Gates: `tests/testcode_open`, `tests/testmadcide_plugin_library`. Filed: B102 (a namespace-scope object's destructor never runs).
6. **The `chthonic` plugin's code, the Variables view:** its contributed view and command, its event handler, `chthonic.layout` gaining the sidebar. It ships as source plus a prebuilt library per platform.

   Gate: `testmadcide_chthonic` under each transport it ships in (after F5 on §34's program plus a global, the rows read `count int 3`, `square int (int)`; an entry `int y = 7;` adds a row; Stop empties them; activating `square` moves the caret to its line), a `tests/gui` case (a click on Run posts `replrun` and main's output arrives; a click on Stop), and §29's gate walked by hand in the window: open, type §34's program, Run, `square(12)` gives 144, and the Variables view shows the globals.
7. **The `host` transport** (B4, G6): the `chthonic` plugin run in a separate process over the seat, and a plugin crash leaving madcide running.
8. **The REPL pane on the contribution points** (B5): no built-in path left beside them.

9. **The Homebrew tap, macOS and Linux** (owner 2026-09-30; packaging arc PK6): bottles on both. Homebrew's Linux base is ours (Ubuntu 24.04, GCC 13, glibc 2.39), so the Linux work is prefix independence and a full-suite lane on a Homebrew Linux install.

Then the seam battery, every platform lane's full suite, and the master release. The `chthonic` product (its own build, the Windows REPL backend, the Microsoft Store MSIX) is the release after it (plugin design §9 items 3-4); its two-unit link probe runs early.

**Decided (owner, 2026-09-30):**
1. **No file opens an untitled buffer.** Thonny's behaviour. Save asks for a name (Save As), and F5 runs it under the unit name `untitled`. It builds on B86's one open rule (slice 0). Slice 1 recons whether a buffer can exist without a path, and gives it a placeholder name if it cannot, which Save As replaces.
2. **The learning IDE's language is a Run ▸ Language… choice** (C17, C++17, madc). The one rule that `madc file`, `madc -i` and F5 share stays. It sets `repl.std` for the session (the setting's value from the bundle or `settings.json` is where it starts) and restarts the session, and the prompt already names the standard (D22). A course would otherwise teach C from a textbook while getting madc's answers without knowing it.
3. **chthonic follows Thonny's look, layout, menus, keys and features, in C/C++ form, GUI first,** so someone who learned Python in Thonny can use it with little relearning. Thonny's documented or source behaviour is the precedent every teaching-IDE command is checked against.
4. **The key style is a menu choice** (step 3e): JOE, Vim, Emacs, Pico, Thonny and VS Code. chthonic opens with Thonny's keys, and the choice persists.

## 42. Decisions (owner, 2026-09-25)

**The aim (owner, 2026-09-25):** there is a future "ideal C/C++ REPL", and everyone is headed toward it, madc included. madc bets it can get there faster. It is designed to work more like a script language (Python, PHP), and it doesn't carry gcc's or clang's baggage. So the idea is to mimic Julia + IPython. madc follows cling and clang-repl only where their functionality is to its benefit and makes sense, never to mimic them.
- What cling and clang-repl do today is a floor, not a ceiling. Where a measurement shows a limit of their design, that is where madc aims past them. Examples are redefinition refused, value printing unimplemented in clang-repl 20.1, and a refused redefinition removing the earlier definition.

**The rule:** Julia + IPython behaviour first, then cling, then clang-repl, weighted so the choice makes the most sense for C, C++ and madc language behaviour.
- When Julia and IPython disagree, **Julia decides language semantics**: binding, redefinition, value display, interrupts.
- **IPython decides the toolbox**: command names and what they do, history, introspection, numbered I/O.
- **cling comes third.** It follows in IPython's footsteps in C++, and it is closer to IPython than clang-repl is (measured below), so it is often the C++ adaptation to look at. IPython itself is far more widely used, which is one reason it ranks above cling. **clang-repl comes fourth.** It is the evolution of cling seeded into clang: cling's developers built it to upstream cling's design into LLVM. So where clang-repl deliberately changed a cling behaviour, the change is weighed as the design's newer thinking. The `%` command prefix is an example (D13). clang-repl-18 is an early point in that evolution, and clang-repl-20 (20.1.2, `/usr/bin/clang-repl-20`, installed 2026-09-25) is the later one measured. (Owner, 2026-09-25; cling was added to the order and put ahead of clang-repl.)
- madc's REPL is not competing with clang-repl or cling. Those projects have their own purpose, following and expectations. madc takes a behaviour from them only where it makes the most sense in the situation.
- A precedent that C syntax cannot host is adapted, and the adaptation is stated.

These decisions supersede the plan text they name.

### cling and clang-repl, measured (2026-09-25)

cling is the precedent for adapting IPython-style interaction to C++, so it is measured beside clang-repl. Where it differs from a decision below, the decision stands, and the difference is recorded here. The version is cling 1.2 on LLVM 18 (conda-forge, `~/.local/cling`; the owner may delete it later). Probe inputs are `tmp/repl/s2b/cl_*.repl`.

- **Value display (D10, D11).** A final expression without `;` shows `(type) value`: `(int) 6`, `(const char[4]) "abc"`, `(char) 'a'`, `(double) 3.5000000`, `(int *) 0x…`. A declaration without `;` shows its value too (`int y = 7` gives `(int) 7`), as D10 decides. With `;` nothing shows. A struct shows only its address (`(P &) @0x…`), where D10 shows its fields, and D10 prints a scalar bare (`30`, not `(int) 30`).
- **A refused input (§41.3).** The whole input is rolled back: after `int z = 1; undeclared_fn(); int w = 2;` is refused, `z` is undeclared. That is §41.3's rule. Today madc keeps the refused entry's declarations.
- **`.undo` (§41.3).** It crashed cling 1.2 (a segfault in `DeclUnloader`), so its semantics come from the documentation only.
- **Redefinition (D5, D6).** Accepted, and it shadows: after `f` is redefined, `f()` gives the new body, but `g()`, compiled earlier, still calls the old one. A variable is the same (`v` is 2, the earlier `rv()` still reads 1). D5 and D6 follow Julia instead: earlier code sees the newest binding. A redefined struct is a new type, and an old object keeps the old one (`sizeof` 4 and 8), as D6 decides.
- **A function declaration as an input.** `int f();` alone was wrapped into the input's run as a block-scope declaration (clang's vexing-parse warning), so a later `f();` was "undeclared". madc keeps a declaration at file scope.
- **Against clang-repl-18, on IPython-like features** (`tmp/repl/s2b/cmp_core.repl`). cling is the closer of the two:
  - a value without the final `;`: cling `(int) 6`; clang-repl-18 "Not implement yet.";
  - redefinition: cling accepts it; clang-repl-18 refuses it ("redefinition of 'f'");
  - commands: cling has about 30, including `.x`/`.L` (run or load a file, like `%run`), `.class`/`.g`/`.typedef`/`.namespace`/`.files` (introspection), `.undo`, `.>` (redirection) and `.dynamicExtensions` (late binding); clang-repl-18 has 3, `%quit`, `%undo` and `%lib` (which loads a shared library);
  - undo: clang-repl-18's `%undo` works; cling's `.undo` crashed on our input.
- **clang-repl-20 (20.1.2), measured the same way.** It has moved little on these features since 18:
  - a value without the final `;` is still "Not implement yet."; `"abc"` without its `;` even fails inside the value machinery ("no matching function for call to 'operator new[]'");
  - a declaration without its `;` is refused ("expected ';' after top level declarator");
  - redefinition is still refused, and in both 18 and 20 the refused redefinition also removes the earlier, valid definition (a later `f()` is "undeclared"). §41.3's rollback must not copy that: a refused entry takes only its own declarations with it;
  - a refused input is rolled back whole, as in cling, and `%undo` works;
  - C mode keeps declarations now (see slice 2b).

### Engine

- **D1. Persistence.** One live `Program` accepts appended entries. Each entry lowers to its own MIR module, linked into one live MIR context. Nothing is replayed (all three precedents are incremental). The core is new and is not built on `eval_*` (§40).
- **D2. Where the session runs.** A backend process, for the CLI session (D20) and madcide alike.
  - Julia, the IPython terminal and Clang-Repl run in-process and die on a segfault.
  - Jupyter console (IPython's kernel) and Thonny's backend survive a crash with the session state lost, and so does madc.
  - The core stays host-agnostic; the backend is its host.
  - Supersedes §16.2's "backed by `ReplSession`, not a pseudo-terminal running a second `madc`": the panel talks to the backend over a channel, not a pty.
- **D3. Gating.**
  - One *interactive* feature flag in the feature registry, independent of `--std=`, like Clang's `-fincremental-extensions`. It is on for every interactive session (D20) under every standard.
  - Each relaxation it enables is a listed registry entry per standard (I3/I4/I8): top-level statements, the optional final `;`, `ans`, redefinition, late binding, auto-supply.
  - `:strict` / `%strict` is a view of that list. It supersedes §11.6's free-standing toggle.
- **D4. Default standard.** `--std=madc`; `--std=` is honoured. `%std` resets the session (§39: switching mid-session is unsafe).
- **D5. Redefinition of functions.**
  - Calls see the newest body. This includes code compiled before the redefinition (Julia), achieved by recompiling dependents.
  - A function's address stays the same across redefinitions, so pointers taken earlier reach the new body (Julia: `f` is one object).
  - Resolves §11.4's open mechanism: dependency recompile plus a stable address.
- **D6. Redefinition of variables and types.**
  - A redefined variable gets new storage, and code that uses it is recompiled against the new binding (Julia 1.12 / Python).
  - Old storage stays alive until `%reset`, since C has no GC to decide when it is unreferenced. Destructors of replaced C++ objects run at `%reset` or exit.
  - A redefined struct is a new versioned type, and old objects keep the old type (Julia 1.12). The first slice may refuse struct redefinition with a clear message.
  - Resolves §11.3 / §11.5.
- **D7. Late binding.**
  - A function body may call a name defined later (Julia / Python). The body is held and compiled when the name is defined; calling it before then is a runtime error naming the missing definition.
  - This is a registered relaxation (D3), and it rides the D5/D6 dependency tracking.
- **D8. Interrupts.**
  - Ctrl-C returns to the prompt with the session intact (Julia / IPython), via an interrupt poll on loop back-edges in session-compiled code: a runtime hook like `--finstrument-functions`, emitted in the tree.
  - Code stuck in a native library cannot be polled, so a repeated Ctrl-C restarts the backend.
- **D9. Thread safety.**
  - Concurrent `complete` / `help` / `type` reads are safe.
  - `submit` / `undo` / `reset` are serialized verbs through the hub/channel machinery (`thread-safety.md`).
  - One session serves N clients.
- **D10. Result capture and display.**
  - Values print in re-enterable syntax (Julia `show` / IPython `repr`): `30`, `"abc"`, `'a'`, `(int *) 0x7ffd…`, `(struct Point){ .x = 1.0, .y = 2.0 }`.
  - An entry without its final `;` shows its value. This includes a declaration: `int x = 5` shows `5`, as Julia's `x = 5` does. With `;` it is silent.
  - Refines §6.2 (aggregates and pointers carry their type, as Julia's do).

### Input and prompt

- **D11. Completeness.** An entry runs as soon as it parses complete, with the final `;` optional (Julia). The one C adaptation is a completed top-level `if`, which waits one more line:
  - a line starting with `else` continues it;
  - an empty Enter runs it.

  That is IPython's compound-statement rule, narrowed to the only C construct that is complete yet legally extendable.
- **D12. Result names.**
  - `ans` (Julia).
  - `_`, `__`, `___`, `_N` (IPython). These are reserved to the implementation at file scope in C (C11 7.1.3) and in the global namespace in C++ ([lex.name]), so no legal user name collides.
  - `ans` is a registered relaxation (D3).
  - Designed against the code in §41.6a (2026-09-27), and done. A result is the value an entry showed: a scalar is kept as a copy, and an aggregate as the object (slice 2), as Julia and IPython keep them. A value hidden by `;` keeps nothing, as in IPython. A user's name always wins. The four moving names are refused in code that runs later. Array results wait on B50.
- **D13. Command prefix.**
  - `%` is primary: IPython and Clang-Repl agree, and Julia has none. `:` is an accepted alias.
  - A command is recognized only when `%` or `:` plus a name starts a new entry, so the `%:` digraph and a continuation line like `% b;` stay C.
  - Command names resolve through the command registry at input (enum-over-strings).
  - Supersedes §7.1 / §7.5 / §7.6: read every `:name` there as `%name`.
- **D14. Shell.** A lone `;` on an empty prompt enters shell mode (Julia), and Backspace on an empty shell prompt leaves it. `%sx` / `%system` are the commands (IPython). IPython's `!cmd` is not supported, since it collides with C's `!x`.
- **D15. Help.**
  - Prefix `?name` / `??name` (Julia / IPython). IPython's postfix `name?` is not supported, since it reads as an unfinished C conditional.
  - Documentation is the doc comment attached to the declaration (`///`, `/** */`), as clang/clangd attach it, plus the compiler's signature.

### Session commands

- **D16. F5 = `%run file`** (Thonny F5 / IPython `%run`): run in a fresh namespace, then leave its names in the REPL. `%run -i file` runs in the current session. `%load` is Julia's `include`.
- **D17. `%undo`** is kept (Clang-Repl, the secondary precedent), with semantics limited per §10.3, in Phase 4. Rollback of *failed* entries (§41.3) is Phase 0.

### Ordering

- **D18. Redeclaration bug first.** The same-scope variable reuse and same-signature body folding (§11.6 code check) are reproduced and fixed first, in their own commit with a gcc/clang-oracled reducer.
  - **DONE 2026-09-25.** Reproduction corrected the audit: nothing gave a silent wrong value. Ill-formed programs were accepted (conflicting types and bounds, C++ tentative duplicates), and duplicate definitions died in MIR without a location.
  - `bd3c56500`: `Program::declare_object` owns object redeclaration, and the madc dialect keeps C tentative definitions.
  - `a74fd49ba`: a second body for a C-linkage function is refused at the definition.
  - **Still open:** C++-linkage same-signature redefinition, which `fold_same_signature_overload` cannot tell from a `long` / `long long` type-model twin. It rides with D5.
  - D6's interactive redefinition branches in `declare_object`.
- **D19. The MIR interpreter is not part of this arc.** The REPL and Run are JIT-only (§21 code check).
- **D20. Entry follows the other REPL languages** (Julia, Python, Node, Lua). There is no `--repl` flag.
  - No program file with a terminal stdin enters the REPL, and language/config options still apply.
  - No program file with piped stdin compiles and runs stdin.
  - `-i` / `--interactive` forces the REPL, and with a file runs it first (`python -i`, the CLI form of D16's F5).
  - An artifact request (`-o`, `-c`, `--emit=…`, `--project`) with no input keeps gcc's "no input files".
  - `-i` is free: madc matches options exactly (`src/madc.cpp:524` on), so it cannot collide with `-I` or `-isystem`.
  - Supersedes §4.1's `madc --repl`.

### The value carrier

- **D21. `var` operators follow Julia's equality with strict arithmetic** (owner, 2026-09-25). Background: `534ba8a6e` refused every builtin operator on a `var`, because they had been running on the carrier's storage address. The strict-equality spec (`docs/superpowers/specs/2026-06-11-strict-equality-design.md` §2.5–2.6) anticipated this choice.
  - `==` / `!=` compare numbers by value across the integer and real kinds (5 == 5.0), and strings as strings. They never convert between strings and numbers (`"5" == 5` is false). This is the one change to existing behavior: an integer-versus-real comparison of two `var`s flips from false to true, so `tools/` is audited for it.
  - `===` / `!==` keep today's strict kind-and-value compare, as the carrier's own `operator===` row (spec §2.5). The pair then splits on a `var` exactly as it does on scalars.
  - Arithmetic and relational operators work on the numeric kinds. Any other kind is the catchable runtime error `as_integer` raises. The compound forms (`v += 1`) come first; the binary forms (`v + 1`) wait for L3's by-value `value` returns.
  - `if (v)` stays refused, as in Julia. Test with `as_boolean()`, `is_null()`, `empty()` or `v === true`.
  - **PHP juggling is a configurable opt-in, off by default.** It covers loose `==`, string↔number arithmetic and relations, and PHP truthiness (`"0"` and `[]` are false).
    - Proposed shape, confirmed at design time: a per-file directive modelled on PHP's `declare(strict_types=1)`, scoped to the file that states it. It is never inherited through `#include`: `dialect-lean.md` forbids carrier semantics that vary with the headers a TU parsed.
    - It is also an entry in the REPL's relaxation list (D3), so `%strict` shows it.
    - It is a feature-registry entry (I4) and never a `--std=` value, because it is not a language standard.
    - Its rules follow one pinned PHP version (8.x) and are tested against the `php` CLI.
  - Thread contract: the rows are pure functions of their operands.
  - Each row retires the matching refusal from `534ba8a6e` by construction.

### Prompt, line editor and commands (owner, 2026-09-25)

- **D22. The prompt names the standard in force.**
  - The language prompt is `Program::standard_canonical_name(language_std)` + `"> "`: `madc> `, `c11> `, `c++17> `. Aliases normalize to the canonical row: `--std=c` shows `c11> ` and `--std=c++` shows `c++11> `.
  - Mode prompts are Julia's in every standard: `help?> ` after `?` and `shell> ` after a lone `;`.
  - Continuation lines carry no prompt; they are indented to the current prompt's width (Julia).
  - Numbered input (`c11 [9]> `) is an option. The IDs exist either way (§6).
  - `%std` resets the session (D4), so the prompt's change doubles as the reset's cue.
  - Supersedes the fixed `madc> ` in the plan's examples.
- **D23. A readline-class line editor of our own, in the engine.**
  - Julia (LineEdit.jl) and IPython (prompt_toolkit) each own their editor.
  - GNU readline is GPL.
  - A vendored linenoise or replxx would be a second keyboard owner, which the one-key-owner gate (`check-one-key-owner.sh`) forbids.
  - A `line_edit` component in `include/madcdis` rides the existing input owners: `tui_keyparse`, `key_resolver`, `ui_apply_keys`.
  - It provides: a caret over the entry, multiline entries, a history ring and history file, Ctrl-R / Ctrl-S incremental search, and two hooks, *complete* and *is this entry finished?*. The REPL answers the second with `classify_entry`, as Julia's REPL answers LineEdit's.
  - Keys come from the profile data madcide already reads: Emacs-style defaults, a vi profile later.
  - New engine pieces it needs (recon 2026-09-25: none exist):
    - an inline raw terminal mode that stays on the normal screen (`ui_term`'s only raw mode takes the alternate screen);
    - Alt/Meta keys, for Meta-Enter "force a newline" (§5);
    - UTF-8 input (`tui_keyparse` drops bytes of 0x80 and above).
  - madcide's `:` prompt and vised's find prompt, which are append-only today, adopt it.
  - Graceful fallback: piped stdin has no prompt (D20); a dumb terminal gets cooked lines (madcide LINE mode's level); a terminal gets the editor.
  - Thread contract: per-instance state.
  - Designed against the code in §41.7a (2026-09-27), measured against Julia and IPython. Meta turned out to be data (the Esc prefix), not a new key kind.
- **D24. One command registry, and ed/ex buffer commands at the prompt.**
  - Every command, for the REPL and madcide alike, resolves by name to an enum ONCE, at input (D13, enum-over-strings).
  - madcide's `colon_command` is a chain of string compares today, which enum-over-strings forbids. It moves onto the registry. lined's `.madv` verbs, already in the engine's verb registry, register there too.
  - `%` names IPython-style session commands (`%run`, `%load`, `%history`, `%std`). `:` accepts those AND ex buffer commands. Ex commands are colon-only, because vim's `%` range (`:%s/a/b/`) would otherwise collide with the `%` prefix. A name shared by both sets has one meaning (`:cd` = `%cd`).
  - A buffer is a file bound to a `text_buffer` (`:e file.c`); `%run file.c` / `:source` runs or loads it (D16).
  - The command set is modern ex/vim/neovim, line-oriented, with nothing that needs visual or normal mode. Modern spellings are preferred over archaic ones.
    - **Addresses and ranges:** `N`, `.`, `$`, `%`, `N,M`, `/re/`, `?re?`, `'x` marks, offsets `+N` / `-N`.
    - **Ed core:** `p`, `n` / `number`, `l`, `a`, `i`, `c`, `d`, `j`, `=`, `r`, `w`, `e`, `q`, and the `!` forms (`q!`, `e!`).
    - **Ex/vim additions:**
      - `s/re/rep/flags` with `&` repeat;
      - `g/re/cmd` and `v/re/cmd`;
      - `m`, `t` / `copy`, `>` / `<`;
      - `u` / `redo` (`text_buffer`'s undo);
      - `wq` / `x`, `update`, `saveas`;
      - `r !cmd`, `w !cmd`, `!cmd` (colon-prefixed, so it does not collide with C's `!`, cf. D14);
      - `sort`, `retab`, `cd` / `pwd`, `mark` / `marks`;
      - `set` for `number`, `tabstop`, `shiftwidth`, `expandtab`;
      - `ls` / `b N` / `bn` / `bp` for more than one buffer;
      - `h` / `help`, which maps to the REPL's `?`.
    - **Excluded:** anything that needs visual or normal mode (`normal`, `visual`), and vi's archaic `open` / `z`.
    - **Regular expressions** use the modern extended syntax (ERE: unescaped `+ ? | ( )`) rather than ed's BRE. The exact flavour is settled at design time.
  - The REPL can therefore edit a file line by line and run it, with no IDE. madcide extends the same registry and buffers with panes and views.

### Execution model (owner, 2026-09-25)

- **D25. The REPL has its own execution space; a program's `main` is just one of its functions.**
  - **The mode rule.** REPL mode works differently from non-REPL mode, by design (owner: "do not try to force/shoehorn REPL mode and non-REPL mode together").
    - Interactive entries have their own lowering. Script mode (a `--std=madc` file's top-level statements synthesized into `main`) stays exactly as it is.
    - Neither path is bent to serve the other. Shared machinery is shared only where the behavior is genuinely the same.
  - **Entries (Cling / Clang-Repl).** An entry's declarations become persistent session definitions (`int x = 5` is a session global). Its statements lower into a uniquely named entry function (`__madc_entry_N`) in that entry's own MIR module (D1), which runs once.
    - Script mode's rule "top-level statements conflict with an explicit main()" belongs to script mode only. Loading a program that defines `main` into a session is normal.
  - **A codebase as a playground.**
    - `%load prog.c` defines everything, `main` included, and runs nothing.
    - `%run prog.c args` runs `main` with that argv, as the command line would, and leaves the program's names callable (D16).
    - `madc -i prog.c` runs it first (D20, `python -i`).
    - The loaded code's globals and statics persist between calls; `%reset` or a fresh `%run` gives clean state. Static initializers run once, at load.
  - **Calling `main` directly** (`main(2, argv)`) is legal C. C++ forbids it (`[basic.start.main]/3`; g++ and clang only warn), so under `--std=c++*` it is a listed interactive relaxation (D3).
  - **`exit()` from session code returns to the prompt (proposed).** `exit`, `_Exit` and `quick_exit` flush stdio, show the status, and unwind to the entry's boundary, the same point where a failed entry rolls back (§41.3). IPython's `%run` catches `SystemExit` the same way. Whether `atexit` handlers run is settled at design time.
    - A crash still restarts the backend with the state lost (D2).
  - Feeds §41.2 (persistent session) and §41.3 (rollback).

### Diagnostic positions (owner, 2026-09-25)

- **D26. A diagnostic cites where the problem STARTS, in gcc's screen columns, with the token underlined.**
  - **Measured (2026-09-25).** Every tool reports the start: gcc 13, clang 18, Python 3.12 and Node 22 on the dev host. From their documentation: Julia (JuliaSyntax), Rust and LSP.
    - They differ only in units:
      - gcc: screen columns, tab stops every 8, one column per character;
      - clang: bytes;
      - Python: characters;
      - LSP: UTF-16, 0-based.
    - They also differ in whether a range's end is exclusive. The end appears only as the far side of a range, never as the headline position.
    - The GNU Coding Standards specify gcc's form (1-based, tabs every 8); Emacs compilation mode and vim's quickfix list jump to it.
  - **madc today.** A token's `column` is its last byte (gcc's "finish"), and consumers compute the start as `column - spelling length`. Measured consequences:
    - `foo` at columns 28–30 is cited at 30, where gcc cites 28.
    - A token split by a line splice is cited on its last line (madc `2:1`, gcc `1:28`); the subtraction goes negative there.
    - The caret line counts bytes as spaces under a line printed with its tabs raw: a tab indent puts the caret 7 columns left of the token, and `éé` earlier on the line puts it 2 columns right.
  - **The decision:**
    - The lexer records each token's start (line, byte column) when it begins reading the token. `column` becomes the start (gcc's caret), and the token's end is kept for ranges.
    - Bytes stay the stored unit. Diagnostics print gcc's screen columns, and the LSP layer converts to UTF-16 in its one owner.
    - The caret line expands tabs, counts screen width, and underlines the token (`^~~`, gcc's form).
    - The `column - spelling` compensations are deleted from the highlighter, the code graph and the LSP diagnostics path.
  - **Order:**
    - The caret drawing is a display bug under either anchor, so it is fixed first, in its own commit.
    - The start-column switch is suite-wide: the fixtures that pin columns change, and the frozen-header pack stores token columns, so its format version is bumped. It rides the next merge wave.
  - **Part 1 done 2026-09-30** (bce128835): the caret line through `madc::line_layout`.
  - **Part 2's inventory (recon 2026-09-30, every read of a token's column):**
    - The stamp: `Program::getRealToken` (`src/lexer.cpp`) sets `tb->column = source.column()` after the token is read; `Source::column()` counts the bytes consumed on the line. The start is captured before the read. Parser-built tokens take `TokenBase::_parse_column` (the last consumed token's column), so they move from its end to its start with it.
    - About 260 COPY sites (a position handed from token to token): neutral when every column changes together.
    - About 45 DIAGNOSTIC sites (`add_diagnostic`, `print_diagnostic`, `throwbuf::sync`, `madc_error`, the problems rows, the CIR dump): they print the start; the headers print gcc's screen column through `line_layout` of the line.
    - 12 END-DEPENDENT sites, each given an end of its own: `highlight_token_rows` (`t->column - spelling`, `prev_end_col`), `graph_token_start`, `graph_extent_of`, `parseCompound`'s `}` end, the statement wrapper's three `end_column` stamps (`_parse_column`, the `;`, the `head_tok` equality), madcide's `lsp_diag_range`.
    - About 22 MIXED or AMBIGUOUS sites: `TokenFunc::column` (0, a `{`, a name token's end) read by `enclosing_func_at` and the outline; `graph_at`'s 0-based start contract; `diagnostic_cause_for`'s end-token equality; the pragma and `eoe` stamps; `lsp_name_span`'s call-site limit.
    - 2 persistent formats: the CIR forest's positions (`CIR_FOREST_FORMAT_VERSION` 50 → 51) and `.madh` token records (the `compiler_hash` signature, since `FORMAT_VERSION` accepts older files).
    - About 15 fixtures: `testprojecterrline`, `testcompilerdata`, `testparsehandle`, `testparserecoverh`, `testparsespans`, `teststringspans`, `testspansmacro`, `testlexspans`, `testgraphaccessors`, `testgraphedit`, `testgraphpast`, `testmadcide_serve_edit`, `testmadcide_lsp`, and `test_repl_session` (`:1:10:` → `:1:7:`).
  - **Part 2's design (2026-09-30), for its own focused session** (the owner's rule for parser-owned machinery: the statement extents feed the code graph and its edit validators):
    - The start: `_getToken` records the cursor (`source.line()`, `_column + 1`) just before its first `source.get()`; a directive that recurses into `_getToken` re-records, so the start is the returned token's. `getRealToken` stamps a real token's `line` / `column` from it. A macro-synthesized token (tfSYNTHPOS) keeps today's frozen invocation stamp; citing the invocation's START instead is a follow-on (the pushback frame would carry the name token's start).
    - The end: a token's lexical end is recorded beside it when it is lexed (a new field; `TokenRec` and the shared-prelude image carry it, or a reader falls back to start + source length). `end_line` / `end_column` stay the construct extents they are.
    - The statement extents: the wrapper's `end_line` / `end_column` and the `head_tok` match take the last consumed token's END. `nextToken` records it beside `_parse_column`, and the 31 save/restore sites of `_parse_*` carry it too (one position struct, not four statics), so a pattern parse keeps a consistent pair.
    - The readers: `highlight_token_rows` and `graph_token_start` read the start directly (the compensations go); `lsp_diag_range` takes the start; the diagnostic headers print gcc's screen column (`line_layout` of the line) while the stored unit stays bytes (the problems rows, the LSP's UTF-16 conversion).
    - The formats: `CIR_FOREST_FORMAT_VERSION` 50 → 51; the `.madh` `compiler_hash` signature changes.
    - The gates: the fixtures above, `test_diag_caret` through the real printer (the reducer's header `2:19`, the caret under `f` with `^~~`), and an A/B over the tests/ suite of every non-diagnostic output (spans, graph extents, outline) unchanged apart from the anchors.
    - Scheduled: after step 8, before the seam battery (release order); earlier if the owner says.

- **D27. An undefined reference is refused at its first use, not at its entry** (owner, 2026-09-26: "the refusal should wait for first use to match behavior of Julia and clang-repl").
  - **The rule.**
    - An entry whose code names a symbol no entry defines yet is accepted, and it links.
    - The use fails when it runs, with ld's words ("undefined reference to 'f'"), as a run-time error of that entry.
    - A symbol defined by a later entry is the one a later use reaches. clang-repl materializes a body at its first call and reports "Symbols not found" there. Julia compiles a body at its first call and binds names late.
  - **What changes:**
    - `int g2() { return later2; }` is accepted, and after `int later2 = 4;`, `g2()` gives 4.
    - `int z = 5; f();` with `f` declared and never defined links. Its run fails at `f`'s call. `z` is kept: the entry linked, and a run-time failure keeps its definitions, as in Julia.
    - Before D27 both were refused at link by `MIR_module_link_check`, and the entry was rolled back (§41.3).
  - **Measured (2026-09-26).** clang-repl-18 and -20 agree:
    - `extern int later2; int g2() { return later2; }`, then `int later2 = 4;`, then `g2()`: 4.
    - `int h(); int calls_h() { return h(); }`, then `h` defined: `calls_h()` is 11.
    - `extern int dv; int *pdv = &dv;`, then `int dv = 3;`: `*pdv` is 3. ORC links pdv's module only when pdv is first read.
    - `int f(); int z = 5; f();`: "Symbols not found: [ _Z1fv ]", and `z` cannot be used after it ("Failed to materialize symbols: z"). The same for `int z2 = 6; int bad = f();`.
    - Julia keeps `z`. madc follows Julia here.
    - madc before D27 refuses all of these at link.
  - **Designed (2026-09-26).**
    - **A function gets a stub, and its definition replaces the stub.**
      - An entry's module may link except for functions that nothing defines. The session then gives each such function a stub: a weak definition, in a module of its own, whose body reports the undefined reference (below). The entry links against the stubs.
      - A later entry's definition replaces the stub. This is ld's rule, added to MIR's loader: a strong function definition loaded after a weak one adopts the weak one's address.
        - Every reference already bound reaches the definition: a call, a function pointer, a vtable slot. A session vtable is a linkonce copy per module (slice 3), so its slot for a virtual function defined later is a function import like any other.
        - The function keeps one address.
        - MIR already never inlines a weak function.
      - A library loaded later (`import`, `#load`) may provide the function. After each entry, the session redirects every stub that the resolver now finds.
      - A stub is never in `session_defined`, so a later definition is still emitted.
      - MIR's imports do not say whether they are functions, so the builder does: `CirBuilder::declares_function`. The stubs are made only after every other link check passes, so a refused entry loads nothing.
    - **An object is reached through a cell the builder emits.**
      - In an interactive entry, code may name an object that an entry declared and nothing defines yet (`extern int later2;`, a class's static member). That code reads the object through a session cell: `(*(cell ? cell : __madc_session_unbound(...)))`.
      - The session owns the cell. It binds the cell at link when the object resolves, and after every entry that defines it. Code translated once the object is defined names it directly.
      - Only code is indirected. A static initializer that takes such an object's address (`int *pdv = &dv;`) is refused at its entry, because that entry is the use. clang-repl accepts it only because it links pdv's module at pdv's first read.
      - An object a library provides (`stdout`, `std::cout`) is named directly: no later entry defines it.
    - **The failing use unwinds to the entry's boundary.**
      - The stub and the cell call `__madc_session_unbound`. It records "undefined reference to 'f()'" on the running entry. It then runs the exception runtime's cleanups down to the entry's mark, restores that runtime's state, and returns to the boundary: the session's guarded call around the entry's init and its run.
      - The entry stays linked, with its definitions: `z` is 5.
      - It is not a C++ exception, so `catch (...)` does not see it. Neither ld's failure nor clang-repl's is an exception.
      - Reached from a task other than the one the entry runs on, it aborts with the message, as the frozen trap does.
      - This is the boundary the proposed `exit()` handling returns to (D25).
    - **Still refused at the entry:**
      - a strong duplicate definition;
      - a vtable or type_info that nothing emits;
      - a static initializer naming an undefined object.
  - **Built (2026-09-26).**
    - **Slice 1** (`60dc36dd5`): functions.
      - MIR's loader: `replaced_weak_func`. A function definition that is not weak (strong, or a LINKONCE copy) replaces a WEAK one and takes its thunk. A LINKONCE copy replaces one too, since no valid program has an inline definition and a different weak one. On x86-64 a call to a weak function is never patched into a direct call.
      - The session:
        - `admits()` collects the functions nothing defines. `CirBuilder::declares_function` says which imports are functions.
        - Once every other check passes, `make_function_stubs` loads the weak stubs ahead of the entry.
        - `rebind_late_stubs` redirects a stub to a library that provides its function later.
      - The boundary: `__madc_session_unbound` and `cir_run_at_entry_boundary`, around the TU init (`run_entry_init`) and the run.
        - It unwinds the exception runtime to its mark. For that, `rt_except.h` gains `__madc_cleanup_unwind_to`, its fifth conscious widening.
        - The entry commits and is counted at its link.
      - `cir_new_symbol_trap_fn` is the one per-symbol trap shape, shared with `--run-frozen`'s traps and gated by `check-one-symbol-trap.sh`.
    - **Slice 2** (`7d0e3de9e`): objects.
      - `CirBuilder::late_bound_object` is the test, and `var_storage_node` the one owner of the storage lvalue in code (eight arms).
      - The session resolves each cell import to a node-stable slot, and `bind_late_cells` binds it after each entry.
      - `check-var-emit-name-bypass.sh` gains the storage-lvalue ratchet.
    - **Slice 2b** (`1da63372e`): an inline body a stub waits for.
      - `Program::session_awaited` makes such a linkonce function a root in the entry that defines it, as Julia's late binding reaches it.
      - clang-repl-20 fails it ("Symbols not found: [ _ZN1S1fEv ]", tmp/repl/s5/d27i.repl).
    - The §41.3 rollback table's link-refused row became a static initializer taking the address of an undefined object (slice 1).
    - Tests: `test_repl_session`, the D27 function, object and C cases.
    - **Found on the way** (off the REPL's path, in `BUGS.md`):
      - B24 (silent): a weak function definition is emitted strong.
      - B25: a declared function returning a function pointer is prototyped `long long`.
      - B26 (silent): a new-expression skips default member initializers.
      - B27: a function template defined after its use is called by its bare name.
      - Also a `fulltest` ratchet that had been red since `1ad86f0db`, fixed in `df6343266`.
    - **Residuals (named):**
      - A template defined after its use stays on its bare name (B27). clang-repl fails the same use.
      - A block-scope function declaration is not scanned for stubs, so its entry is refused at link.
      - A reference, an array, a madc carrier or a thread-local object that nothing defines is refused at link.
      - The unwind destroys only what a `try` body registered. A plain scope's objects are not destroyed, as with `longjmp` and as with madc's uncaught throw.
  - **Order:** before D20. It changes what a refused-at-link entry is, and the §41.3 rollback table's link-refused row.

- **D28. A `var` holding a number takes arithmetic** (owner, 2026-09-26: "of course a var holding a number should support arithmetic ... overloaded operators"; the leanings below agreed).
  - **Where:** more operator entries in the carrier's table (`Program::add_array_methods()`, beside `==`, `!=`, `+=`), backed by `madarray_*` runtime functions (value-first.md's feeder-gap rule). Not a new mechanism.
  - **What is missing beyond the entries:**
    - The result is a new `var`. Check first that the external-operator lowering's by-value return slot, which already serves other non-trivial classes, carries a `var`. If it does not, fix that layer; never a text-returning workaround.
    - A number on the left (`1 + a`) needs free-operator entries.
    - `+=` adds for a number and still appends for text. Today `a += 1` is refused as well.
  - **Semantics:**
    - An integer with an integer is an integer, wrapping on overflow as Julia does.
    - An integer with a real is a real.
    - `/` on two integer-kind `var`s is real (`5 / 2` is `2.5`), as Julia, Python, JS and PHP give it. A plain `int` keeps C's division.
    - Two strings concatenate, matching `+=`. A string with a number is a run-time error, as `==`'s strict kind rule is.
    - Also unary `-`, `%`, and `<` `<=` `>` `>=`.
  - **Order:** before D20, as its own commit with a reducer: the REPL's default dialect (D4) hits it first. Closes BUGS.md B29.
  - **Built (2026-09-27, `589e29a28`):**
    - The rule is `madc::value::arithmetic` / `negate` / `compare`. The C++ operators on `madc::value` are hidden friends over it, and the `madarray_*` entries only shape operands.
    - Member rows (a `var` on the left) and free rows (a number or text on the left) come from one table. The member rows sit on the process-global carrier. The free rows live in each Program's overload set, so every Program registers them.
    - The external lane's by-value result slot already carried a `var`; the unary lane needed its slot (`89b85d0de`) and the carrier's admission.
    - Two more lanes changed to reach the rows: the free-operator lowering no longer claims a carrier on the left, and the value display reads a carrier result (`a + 1` shows `6`).
    - The bitwise operators, which a `var` does not define, stay refused (`testcarrierbuiltinrefused`).
    - Oracles: python3, php 8.3.6 and gcc on the distinctive lines of `testvararith`.

### Next

Phase 0 per §41: D18, then the classifier (§41.1, D11), the persistent-session proof (§41.2, D1), rollback (§41.3) and result capture (§41.4, D10).

D18 and the classifier are done. §41.2a slices 1, 2 and 2b are done: entries persist, an entry's statements run once, in source order, and they run under every standard (D3). The entry transaction's JIT half (§41.3) is done: a refused entry, whether its parse, its translation or its link refused it, leaves the live context as it was, with a diagnostic, and its definitions never come alive later. Slice 3 is done (2026-09-26): a class, its members and its instances can be spread over any number of entries, with one vtable, one type_info and one copy of every inline body for the whole session (§41.2a, "Built, slice 3"). §41.3's rollback is done (2026-09-26): a refused entry leaves nothing behind, in the Program or the live context, and `session_withheld` is deleted (§41.2a, "Built, §41.3's Program half"). Its named residuals stay open: a MIR fatal past the link check, and link diagnostics without a position. D27 is done (2026-09-26): a function or an object no entry defines yet is refused at its first use, as in Julia and clang-repl. A later definition is the one reached, and the failing use returns to the entry's boundary with the entry kept (§42 D27, "Built"). §41.4's result capture is done (2026-09-26): an entry without its final `;` shows its value in re-enterable syntax. That covers scalars, text, pointers, enums, structs, arrays, a madc `var` and the standard containers (§41.4a, "Built"). D28 is done (2026-09-27): a `var` holding a number takes arithmetic, with one rule in `madc::value` (§42 D28, "Built"). D20 is done (2026-09-27): `madc` with no program file is the REPL on a terminal, `-i` forces it, piped stdin is the program, and `madc -i file` runs the file, then the prompt has its names. The file and the session are one unit (§41.5a, "Built, slice 1" and "Built, slice 2"). D12 is done (2026-09-27): an entry's shown value is kept and named `ans`, `_`, `__`, `___` and `_N`, a scalar as a copy and an aggregate as the object, as Julia and IPython keep them. Its array results wait on B50 (§41.6a, "Built, slice 1" and "Built, slice 2"). G is fixed (a reference to an array shows its elements). The line editor (D23) and completion are designed (2026-09-27, §41.7a). Slices 1–4 are done: the editor, its history, Tab completing names, which is §37 item 7, and Tab completing members after `.`, `->` and `::` (§41.7a, "Built, slice 1" to "Built, slice 4"). Slice 4's other half, completing the `%` / `:` command names, comes with D24's commands. §37 item 8 is done (2026-09-27, §41.8a): the command front, `%type` and `%help` with command-name completion (slice 1), and `?name` / `%pinfo name` from the walk Tab reads (slice 2). §37 item 9, madcide's REPL pane on the same session, is designed against the code (§41.9a). §37 item 9 is done (2026-09-27/28): the session runs in a backend process behind `SessionClient`, dialect code drives it through `madc::session_*`, madcide's bottom panel has a REPL tab on it, and `madc` / `madc -i` run on the same backend, so a crash in an entry restarts the session instead of ending madc (§41.9a, "Built, slice 1" to "Built, slice 4"). §37 item 10, F5 into the session, is designed against the code (2026-09-28, §41.10a). §37 item 10 is done (2026-09-28): function keys, loading a buffer's text into a session with its main run, from C++ and the dialect, and madcide's F5, which runs the editor's buffer into the REPL tab's fresh session and leaves its names for the prompt (§41.10a, "Built, slice 1" to "Built, slice 3"). So §37's items are done. D12's arrays follow B50. Owner pause (2026-09-25): until the REPL makes real progress, defects found off its path go into `BUGS.md` instead of being fixed on the spot. The `fix-what-you-find.md` rule itself is unchanged. The pause is lifted (owner, 2026-09-28): with §37 complete, found defects are fixed again, and the `BUGS.md` list is burned down next.
