# madc REPL + the teaching IDE — plan (2026-09-21)

**Owner statement of intent (2026-09-21):**

> A beginner-friendly C/C++ IDE with a real REPL and optional live teacher
> connection.

madcide is **not** becoming another VSCode — VSCode exists. The model is
**Thonny**: a simple, interactive *teaching* IDE, but for C, C++ and the madc
dialect instead of Python. The REPL and step-by-step execution are the primary
concerns; help text is a later concern.

This document is recon + plan. It does not authorize code.

---

## 1. Why this gap is real

Python has Thonny. Julia has a REPL that people cite as a reason to use the
language. C and C++ have neither, and the substitutes are worse than nothing
for a beginner:

- **Cling / clang-repl** exist but are not teaching tools: they target expert
  users, and they fake persistence by rebuilding a translation unit per line.
- **gdb** is a debugger for people who already understand the machine. Its
  granularity is a *line table* — it cannot show you the inside of an
  expression.
- **Online compilers** (godbolt, replit) have no persistent state and no
  stepping.

So the beginner's loop in C/C++ today is: write a whole program, compile it,
get a diagnostic written for a compiler engineer, run it, get a segfault with
no explanation. Every one of those steps is a place where a learner forms a
*wrong* mental model and nothing corrects it.

**The specific thing C/C++ beginners get wrong is memory** — pointers,
lifetimes, what "uninitialized" means, what a dangling pointer is, what the
stack is. That is the difference from Thonny worth designing around: Thonny's
visualizations are *reference*-centric because Python has no addresses. Ours
must be **memory**-centric. See §5.

---

## 2. What we already have

This is the most important section, because most of the substrate exists and
was built for other reasons. **Do not re-plan these.**

| capability | where it already lives |
|---|---|
| **The running madc IS the compiler** | owner law; madcide never execs a madc — the live parse handle is the compilation |
| **Persistent eval with contexts** | `madc::eval_*` in `src/ns_madc.cpp` — `eval_expression`, `eval_bool`, `eval_double`, `eval_expression_int`, each with a `_ctx` variant. *Contexts already exist*: persistent state is not from scratch |
| **Interpreter tier is already sanctioned** | `docs/adr/0001-cir-c2mir-backend.md` reserves direct-MIR-via-API for runtime internals **and the interpreter / REPL / debug tier**, emitted into the same MIR module. `MIR_interp` is used today by the unit harness |
| **Sub-expression source mapping** | `.claude/rules/mc11-ir.md` (SET IN STONE): every `cir_node` carries its originating tokens, the `TokenBase` parse subtree, and file/line/col. This is the prerequisite for expression-level stepping, and it is an invariant, not a feature |
| **Statement↔byte correlation maps** | shipped v0.100.0 for viewsync (source ↔ MC11 caret tracking) |
| **Views** | the same buffer re-represented as source / MC11 / C11 / C++ *in place*, split-tree editor region |
| **Event-sourced journal** | append-only, replay to any point, checkpoint, compaction, `event:N` history View |
| **Multi-client session** | a window IS a client; N clients on one document; presence carets through one anchor registry; rendered in terminal AND web |
| **Transports + permission tiers** | `api` transport with tiers, headless `--serve`, `ws`, MCP seat, LSP face, VS Code extension, attach relay, session discovery |
| **Error-tolerant parsing** | `feature/error-tolerant-parse-claude` — structured diagnostics, contained-rejection shape, resync at a region's close, stream-truth brace debt. **Banked work, unmerged** |
| **UI level enum** | `ui::NONE < ui::LINE < ui::TUI < ui::WEB < ui::GUI < ui::GFX2D < ui::GFX3D` (`include/madc/bits/ui_enums`) — the REPL is a `ui::LINE` client, which already has a seat (the ex/edlin mode, V2.5) |
| **Key/command registry** | commands and menus are data (`profiles/default.menu`); key bindings are profile data, never hardcoded |
| **Roadmap slot** | Track 1.3 already lists "reimplementing eval/exec + REPL on MIR" as remaining work on the central workstream |

**The live teacher connection is ~80% built.** It is the v0.100.0 Nexus arc
with a permission tier selected. A teacher attaching to a student's session =
attach relay + session discovery + presence carets + a tier (observe /
suggest / drive). What is missing is the *front door* (how a teacher is
invited, named, and trusted), not the machinery.

---

## 3. Thonny — what to take, what to leave

**Take:**

- **Expression-level stepping.** Thonny's signature move: stepping does not
  advance a line, it evaluates a *subexpression* and replaces it with its
  value in place. `foo(x + 5) * 2` lights up `x + 5`, then `foo(...)`, then
  the multiply. This is the single highest-value feature and we have the
  prerequisite (§2, mc11-ir).
- **Frames as visible things.** "Step into" presents the callee's frame as a
  nested window rather than a stack list. Beginners learn what a call *is*.
- **Zero setup.** Thonny bundles its Python. madc is a single binary with a
  frozen header forest — we are already in a better position: no toolchain
  install, no include paths, no build file.
- **Plain-language diagnostics layered OVER, not replacing, the real one.**
  Thonny's assistant explains the error underneath. Never hide the real
  compiler text — the learner must eventually read it.
- **A small, non-configurable default UI.** Complexity is opt-in.

**Leave:**

- Thonny's reference/heap diagram as-is (Python semantics; see §5).
- Its assumption that execution is interpretation with no build step. For us
  the *compile* is itself a teaching surface (§5).

---

## 4. Julia — what to take

- **Leading-character prompt modes.** `?` help, `;` shell, `]` package. One
  keystroke switches mode and the prompt changes colour/text to say so. This
  is the best single UI idea to steal: discoverable, modeless-feeling,
  zero menu. Proposed madc set:
  - `madc>` — the language
  - `?` — help (signature + types always; prose when a provider has it)
  - `;` — shell
  - `:` — compiler/session commands (`:std c17`, `:strict`, `:views`, `:reset`)
- **Automatic display of the last result**, with a trailing `;` suppressing
  it; `ans` holds the previous value.
- **Errors never kill the session.**
- **History + reverse search** (ctrl-r), persistent across sessions.
- **Introspection as ordinary commands**, not debugger-only: `typeof`,
  "which overload would this call resolve to", "where is this declared".
- Tab completion over identifiers, members, namespaces, overloads, headers.

---

## 5. What we must invent (this is the original part)

Thonny and Julia do not answer these, because Python and Julia do not have
them. This is where the C/C++ teaching IDE earns its existence.

**5.1 A memory view, not a reference view.** Stack frames with real addresses,
locals with their storage, pointers drawn as arrows to what they point *at*,
and — critically — the three states Python has no name for: **uninitialized**,
**dangling**, **freed**. A beginner who can *see* that `int x;` contains
garbage, and that `free(p)` leaves `p` pointing at memory that is no longer
theirs, has learned the thing C is hardest at teaching.

**5.2 The compile is a teaching surface.** We have Views. Step once and show
the statement in **source**, in **MC11**, and in the **C11 lowering** at the
same time. "What does `for (auto &x : v)` actually do?" stops being a lecture
and becomes a pane. No other teaching IDE can do this because no other one
carries a dual-level IR.

**5.3 Undefined behaviour as a teaching event.** When the interpreter tier can
see UB (signed overflow, OOB index, use-after-free, reading uninitialized
memory), it should *stop and explain*, not produce a plausible wrong answer.
This is the interpreter's advantage over the JIT and is worth building for.

**5.4 Declaration-vs-execution split.** In Python everything is a statement.
In C a REPL must accept a `struct` definition, a function definition, a
`#include`, and an expression at the same prompt, and know which is which.
This is a real design problem — see §7.

---

## 6. Settled decisions (defaults, not questions)

1. **The stepper runs on the interpreter tier (`MIR_interp`), not the JIT.**
   Already sanctioned by ADR 0001. Stepping JIT output means breakpoints and
   ptrace; stepping MIR means one instruction at a time with state in hand.
2. **One IR, two executors — never two semantics.** A gate runs the same
   program through the JIT and the interpreter and requires identical
   observable output. Without it we have built the parallel implementation the
   rules forbid, and a teaching tool that lies.
3. **Interactive leniency is a MODE on the `--std=` enum, never a parser
   special-case.** The worst teaching outcome is "it worked in the REPL but
   fails in a file." The relaxations must be a declared, listable set, and the
   REPL must be able to say *"I allowed this; ISO C would not."*
4. **The REPL is a `ui::LINE` client of the existing session**, not a separate
   program. It therefore inherits multi-client, transports, the journal and
   the teacher connection for free.
5. **Help = declaration first, prose second.** Signature/types/header/module
   always available offline from what the compiler already knows; man pages
   bound as a lazy module (host's, not shipped — per-page licensing); dialect
   prose authored beside its declaration and *generated* into man pages, with
   a fulltest gate that an undocumented dialect public fails the build.
   (Deferred — owner: help text is a later concern.)
6. **Automatic printing goes through the `var` carrier**, which is the
   value-first direction anyway.

---

## 7. Open questions (owner input wanted)

1. **What is a REPL "line" in C?** Options: every input is a top-level
   declaration unless it parses as an expression; or an explicit `:let` for
   definitions. Julia and Python dodge this. Proposal: infer, and show the
   learner which way it was read.
2. **Redefinition.** `int x = 1;` then `int x = 2;` — C says error, a REPL
   says obviously fine. Proposal: allow, mark the shadowing visibly, and make
   strict mode refuse.
3. **How far does a step go?** Statement, expression, or MIR instruction?
   Proposal: expression by default, with instruction-level available in an
   advanced tier.
4. **Teacher-connection trust model.** Invite code? Session discovery on the
   LAN? Explicit approval per attach? The tiers exist; the social protocol
   does not.

---

## 8. Staged plan

Each stage is independently useful and independently gated. No stage begins
before the C-compatibility prerequisite (below) is met.

**Stage 0 — prerequisite (in flight).** madc compiles MIR's own C source.
A teaching tool built on a compiler that miscompiles teaches wrong lessons;
a beginner who hits *our* bug instead of their own learns nothing true.
Tracked by `scripts/c2mir_tests_lane.sh` (298/359 at b3dc26b79) and c2mir.c
itself (119 errors). Merge `feature/error-tolerant-parse-claude` here too —
the REPL needs its contained-rejection shape.

**Stage 1 — the REPL loop (`ui::LINE` client).** Prompt modes, persistent
context over `madc::eval_*_ctx`, automatic result display through `var`,
history + reverse search, error recovery that does not poison the session.
*Gate:* a scripted session of N inputs replays byte-identically; a malformed
input leaves the session usable.

**Stage 2 — introspection.** Tab completion from the parser's own lookup +
the forest decl-index; `typeof`; overload resolution explanation; jump to
declaration (the LSP face already computes it).
*Gate:* completion/type answers match what the compiler resolved.

**Stage 3 — the stepper.** Interpreter tier, expression granularity via the
retained parse subtree, frames as nested views, step in source/MC11/C11.
*Gate:* JIT-vs-interpreter equivalence over the whole suite; a stepping
session over a known program produces an expected step trace.

**Stage 4 — the memory view + UB reporting.** §5.1 and §5.3.
*Gate:* reducers for uninitialized / dangling / freed / OOB each produce the
teaching event, with negative controls that correct programs do not.

**Stage 5 — the teaching front door.** The Thonny-shaped default UI, plain
language over real diagnostics, and the teacher-connection social protocol on
top of the existing tiers.

---

## 9. Invariants this must satisfy

From `docs/plans/madc-vision-and-invariants.md` and the rules:

- No hardcoded standards or targets; every REPL relaxation is gated through
  the `--std=`/`LanguageStd` enum and the feature registry (I1–I8).
- One IR, one emitter; the REPL and stepper are *readers* of MC11-IR, never a
  second tree.
- No parallel implementations: the interpreter tier is a second *executor*,
  gated for equivalence, not a second semantics.
- Enums, not strings, for every discriminator (prompt mode, step granularity,
  teaching-event kind) — in the engine AND in dialect code.
- Every new capability states its thread-safety contract.
- madcide ships no application code the engine should own.

---

## 10. Pointers

- `docs/adr/0001-cir-c2mir-backend.md` — the interpreter/REPL/debug tier
- `docs/plans/ROADMAP.md` Track 1.3 — "reimplementing eval/exec + REPL on MIR"
- `docs/plans/madc-ide.md` — the existing IDE plan
- `.claude/rules/mc11-ir.md` — why sub-expression stepping is possible
- `feature/error-tolerant-parse-claude` — banked error recovery
- `src/ns_madc.cpp` — `madc::eval_*` and contexts
