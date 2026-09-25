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

The old BASIC-style stored-program idea is explicitly deferred from this proposal.

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
> - **Profile selection:** needs a flag threaded through `run_tui` → `IdeSession::open` → `init_view_es`, plus `learn.layout` / `learn.menu` files.
> - **Toolbar:** no face has one.

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

Implement the earlier Thonny-inspired stepper:

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

- BASIC-style stored-program/list/edit/run workspace at the REPL prompt;
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
| madcide REPL panel, teaching profile | **Clean insertion points** | A panel kind beside Terminal (`compose_chrome_pane`, a `term_pump`-style pump); a profile flag into `init_view_es` plus `learn.*` files. |
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
  - a discarded `if constexpr` branch that omits its final `;`;
  - an `enum {…}` definition's missing `;`. File mode accepts it too; it is fixed next, in its own commit.

First slice: §37 items 1–6 in the CLI interactive session only (D20: `madc`, `madc -i`). Items 7–10 depend on the completion service, the madcide panel, F-keys and a surviving program session, and follow in that order.

## 42. Decisions (owner, 2026-09-25)

**The rule:** Julia + IPython behaviour first, Clang-Repl second.
- When Julia and IPython disagree, **Julia decides language semantics**: binding, redefinition, value display, interrupts.
- **IPython decides the toolbox**: command names and what they do, history, introspection, numbered I/O.
- A precedent that C syntax cannot host is adapted, and the adaptation is stated.

These decisions supersede the plan text they name.

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

### Next

Phase 0 per §41: D18, then the classifier (§41.1, D11), the persistent-session proof (§41.2, D1), rollback (§41.3) and result capture (§41.4, D10).
