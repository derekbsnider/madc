# MIR source-level debugging support — guide for MIR developers

This document orients a MIR developer who wants to understand, review, or extend
the source-level-debug extensions on the `debug-support` branch of the
cyanogilvie/mir fork. It covers **only the debug-support changes** (not the
meson-subproject support that also lives on the branch).

## What it adds, and the design philosophy

The goal: let a **frontend** that emits MIR produce a debugger-usable view of
its JIT'd code (source stepping + variable inspection). It splits into two layers
across a deliberate seam.

**The MIR core stays format-agnostic.** `mir.c`/`mir-gen.c` know nothing about
DWARF, ELF, or any debug format; their whole contribution is three neutral,
source-language-agnostic primitives:

1. **Carry an opaque source location** (a `(file_id, line)` pair the frontend
   defines) from each insn through the generator to the final machine code, and
   hand back a per-function `code_offset -> (file_id, line)` map.
2. **Expose the size** of each generated function.
3. **Offer two "debuggable codegen" knobs** — disable inlining, and home every
   local in a stable stack slot — plus a way to query each local register's
   frame slot offset.

That's the whole core surface, and it ports to any debug format — line map →
`.debug_line` or CodeView lines, reg offsets → `DW_AT_location` or CodeView
locals, etc.

**A separate, optional emitter turns those primitives into a debug object.**
`mir-debug.{c,h}` (+ `mir-debug-gdb.c`) is a reusable DWARF/ELF builder plus GDB
JIT registration, generalized out of slimcc's hand-rolled emitter and then
renamed `mir-dwarf` → `mir-debug`: its API is debug-format-neutral (declarative
types, functions, frame-relative variable locations) and DWARF/ELF is merely its
current — and so far only — backend. It is pulled into `libmir.a` as on-demand
archive members, so a build that uses neither carries nothing. The emitter is
documented under "In-tree reference consumer: c2m" below; what a Mach-O or
CodeView backend would take is under "Future platform support".

The six commits (off `master`):

| commit | what |
|---|---|
| `44412895` | expose `MIR_func.code_len` |
| `04cd7223` | source locations on insns + per-function line map |
| `a9668098` | fix: don't leak a stale location onto function-entry insns (simplify) |
| `33251e7c` | fix: reset the current location at the start of each function's gen |
| `60e4a987` | `MIR_set_inline_permission` to disable call inlining |
| `fc290239` | `MIR_set_spill_all` debug codegen + per-function reg frame offsets |

For upstreaming these split cleanly into ~3 independent topic branches
(source-loc incl. the two fixes; disable-inlining; spill-all+reg-offsets);
`code_len` is a trivial prerequisite of the first.

## Public API surface (all in `mir.h`)

```c
/* --- source locations --- */
struct MIR_insn { ...; uint32_t file_id; uint32_t line; ...; };   /* +8 bytes/insn */
void MIR_set_source_loc (MIR_context_t ctx, uint32_t file_id, uint32_t line);
typedef struct MIR_line_map { uint32_t code_offset, file_id, line; } MIR_line_map_t;
struct MIR_func { ...; MIR_line_map_t *line_map; size_t line_map_len; ...; };

/* --- function code size --- */
struct MIR_func { ...; size_t code_len; ...; };

/* --- inlining control (default: on) --- */
int  MIR_get_inline_permission_p (MIR_context_t ctx);
void MIR_set_inline_permission   (MIR_context_t ctx, int enable_p);

/* --- spill-all debug codegen + reg locations (default: off) --- */
int  MIR_get_spill_all_p (MIR_context_t ctx);
void MIR_set_spill_all   (MIR_context_t ctx, int enable_p);
typedef struct MIR_reg_loc { uint32_t reg; int64_t fp_offset; } MIR_reg_loc_t;
struct MIR_func { ...; MIR_reg_loc_t *reg_locs; size_t reg_locs_len; ...; };
int  MIR_reg_frame_offset (MIR_func_t func, MIR_reg_t reg, int64_t *offset);
```

`file_id`/`line` are opaque to MIR (0/0 means "no location"). `line_map`,
`code_len`, `reg_locs` are filled by `MIR_gen` and freed with the function.

## How each piece works

### 1. Source locations → line map

- **Storage** (`mir.h`): two `uint32_t` on `MIR_insn`. Considered cheaper than a
  side table because the generator creates/destroys many insns and the
  propagation hook (below) is a single field copy.
- **Stamping** (`mir.c`): the context holds a "current location"
  (`curr_source_file_id`/`curr_source_line`); `create_insn` copies it onto every
  new insn. `MIR_set_source_loc` sets it. So a frontend calls
  `MIR_set_source_loc` before building a statement's insns and they inherit it.
- **Propagation through gen** (`mir-gen.c`): the chokepoint is
  `gen_add_insn_before`/`gen_add_insn_after` — nearly all generator-inserted
  insns go through these with an *anchor* insn; they copy `anchor->{file_id,line}`
  onto the new insn. `MIR_copy_insn` (`mir.c`, a `memcpy`) preserves it for free.
- **Recording** (`mir-gen.c` `gen_record_line` + the targets): in
  `target_translate`'s emit loop, just before appending an insn's bytes, the
  target calls `gen_record_line(gen_ctx, VARR_LENGTH(result_code), insn)`, which
  appends to a gen-context VARR (coalescing runs with the same location, skipping
  unlocated insns). `generate_func_code` then copies that VARR into
  `func->line_map`. **Per-arch:** wired for **x86_64** (`mir-gen-x86_64.c:2943`)
  and **aarch64** (`mir-gen-aarch64.c:2496`) only; ppc64/s390x/riscv64 produce an
  empty line map until the same one-line call is added to their emit loops.

Two bugs were fixed here, both about gen-created insns picking up a **stale**
current location instead of inheriting from an anchor:
- `simplify_func` (`mir.c`) inserts argument-extension insns at the function
  head; it now zeroes the current location first, and restamps per insn it
  processes, so they don't adopt a prior statement's line (the generated
  prologue anchors to the head insn, so a wrong line there mislabels the whole
  function entry).
- `generate_func_code` (`mir-gen.c`) calls `MIR_set_source_loc(ctx, 0, 0)` at the
  start of each function so insns created during gen that *don't* inherit from an
  anchor (prologue, spills) stay unlocated rather than picking up the previous
  function's last line — otherwise one function's lines bleed into another's
  code, which is very visible when stepping.

### 2. `code_len`

`MIR_gen` already computed each function's code length in `target_translate` and
discarded it. `generate_func_code` now stores it in `func->code_len`. Two lines.
Needed so a frontend can size the code region for symbol tables / `high_pc`.

### 3. Inlining control

`MIR_link` calls `process_inlines` for any function containing a call,
regardless of generator optimization level — so a small callee gets inlined into
its caller even at `-O0`. That defeats source debugging: the inlined body shows
the callee's lines inside the caller with no separate frame. The gate is a single
`if (!no_inline_p)` around the `process_inlines` call in `MIR_link` (`mir.c`),
controlled by the new context flag.

### 4. Spill-all + reg frame offsets (the variable-inspection enabler)

The key realization: MIR's normal register allocator keeps locals in **reused
hard registers even at `-O0`**, so a local has no stable, describable location.
For DWARF locations you need each local in a fixed memory slot.

- **Forcing it** (`mir-gen.c` `assign`): the allocator already special-cases
  address-taken regs by giving each its own stack slot via `get_new_stack_slot`.
  In spill-all mode that path is extended to *every* non-tied local reg. This is
  naive, libtcc-like codegen (slower, every value reloaded per insn through the
  existing spill machinery) but every value is now in memory at a fixed offset.
- **Capturing offsets** (`mir-gen.c` `reg_alloc`): right after `assign` (while
  `reg_renumber` is still valid, before `rewrite` consumes it), for each local
  reg homed in a slot it computes the offset with the existing per-target
  `target_get_stack_slot_offset(type, loc - MAX_HARD_REG - 1)` and pushes a
  `(reg, fp_offset)` into a gen VARR; `generate_func_code` transfers it to
  `func->reg_locs`. `MIR_reg_frame_offset` (`mir.c`) is a linear lookup.
- **Frame base**: the offsets are frame-pointer-relative when `keep_fp_p` is set.
  A frontend that `MIR_ALLOCA`s every local (slimcc) forces it implicitly, but a
  frontend that keeps scalars in pseudo-regs (c2mir) does not, and a simple
  non-vararg/non-alloca frame would otherwise omit FP and home its slots off SP —
  no register for `DW_AT_frame_base` to name. So **spill-all mode now keeps the
  frame pointer**: `target_machinize` sets `keep_fp_p |= MIR_get_spill_all_p(ctx)`
  on x86_64 (aarch64's `target_get_stack_slot_base_reg` is unconditionally FP, so
  it needs no change). The frontend pairs each offset with
  `DW_AT_frame_base = the host FP DWARF register`. `target_get_stack_slot_offset`
  exists for **all** targets, so reg offsets work everywhere `keep_fp_p` holds —
  unlike the line map, this part is arch-independent.

Gated on `spill_all_p` so non-debug compiles pay nothing.

## Invariants / gotchas for anyone modifying this

- The line map is built in **full-function** gen (`target_translate`), not the
  lazy BB-version path — fine for the eager `MIR_set_gen_interface`, but a lazy
  consumer (`MIR_set_lazy_gen_interface`) gets no line map.
- `reg_renumber` is only valid between `assign` and `rewrite`; capture there.
- Adding a new target requires: the `gen_record_line` call in its
  `target_translate` emit loop, and confirming `target_get_stack_slot_offset`
  returns FP-relative offsets under `keep_fp_p` — and that `keep_fp_p` is forced
  when `MIR_get_spill_all_p` holds (x86_64 does this in `target_machinize`;
  aarch64 always bases slots on FP so it is automatic).
- The `(file_id, line)` are entirely the frontend's namespace; MIR neither
  validates nor interprets them.

## Rough edges / open questions for upstream review

- **+8 bytes/insn unconditionally.** Could be gated behind a build flag or a
  context option if upstream prefers; a side table was rejected for hook
  simplicity but is an option.
- **Line-map recording only on x86_64/aarch64.** The other three targets need
  the one-line `gen_record_line` call to participate.
- **`reg_locs` captured whenever `spill_all_p`.** Cheap, but could be its own
  flag if a consumer wants spill-all without the table.
- **Spill-all is whole-function.** A finer "spill these named regs only" mode
  would give better debug code, at the cost of touching the allocator more.
- **No unwind info (DWARF CFI / Windows xdata) from MIR itself** — currently
  deliberate: frame unwinding relies on the real frame pointer (x29/rbp), which
  spill-all forces and both production targets keep. This is the main capability
  a non-gdb-on-ELF platform would need added; see "Future platform support".

## Testing

Validated end-to-end through the slimcc frontend + jitc (a Tcl extension):
real gdb sessions doing `break file:line`, `step`/`next`, `print`/`info locals`
on JIT'd code on x86_64; plus the existing MIR c-tests suite for non-regression
of normal codegen. There is no MIR-level unit test for the debug fields (they're
not observable from the `.mir`/c-tests harness); a small C harness calling
`MIR_gen` + asserting `line_map`/`reg_locs` contents would be the natural
addition for upstream.

## In-tree reference consumer: c2m

The bundled C frontend now consumes this API end-to-end — line stepping **and**
rich-typed variable inspection — so it doubles as the worked example for anyone
wiring up a frontend. The generic DWARF byte-pusher lives in `mir-debug.{c,h}`
(built into `libmir.a` as an on-demand archive member): a "dumb" builder where
the caller drives type-graph construction (`MIR_debug_base_type`/`pointer_type`/
`array_type`/`struct_type`+`add_member`/`add_bitfield`/`enum_type`+
`add_enumerator`/...) and the builder backpatches `DW_FORM_ref4` at emit time so
recursive types just work.

**Builder vs GDB-JIT registration are separate objects.** The process-global
`__jit_debug_descriptor` + `MIR_debug_gdb_register`/`unregister` live in
`mir-debug-gdb.c`, apart from the builder + `MIR_debug_emit` in `mir-debug.c`.
The descriptor is a single symbol gdb resolves by name, pulled into a build only
when something calls `MIR_debug_gdb_register` — so a minimal embedder that wants
no GDB-JIT support (or insists on its own descriptor) links just `mir-debug.o`
and never sees a second one.

**Registrations are bound to a context, freed by `MIR_finish`.**
`MIR_debug_gdb_register(ctx, buf, size)` tags the entry with `ctx` in a global
mutex-protected registry, and the first registration for a context installs an
unregister-sweep hook on it that `MIR_finish(ctx)` runs — dropping and freeing
every debug object for that context before its code is freed (the addresses
would otherwise go stale). Pass `ctx == NULL` for a manual lifetime: the returned
`MIR_debug_jit_t` must be handed to `MIR_debug_gdb_unregister` by hand. The hook
is reached through a function-pointer field on `MIR_context` (`gdb_jit_finish`,
installed via `_MIR_set_gdb_jit_finish`), so **mir.c references no
mir-debug-gdb symbol** and never force-links the descriptor. Both worked
examples now use MIR's descriptor: **c2m** binds its object to `main_ctx`;
**jitc** binds each `-g` cdef's object to that cdef's context, so its per-cdef
`MIR_finish` teardown unregisters it (jitc *deleted* its former hand-rolled
`__jit_debug_descriptor`/register/unregister/Tcl-mutex for this). slimcc itself
just builds the DWARF object via `mir-debug.o` (its own emitter was *deleted* in
favour of this builder) and leaves registration to its embedder.

Compile with `c2m -g <src> -eg`. `c2mir.c` stamps `MIR_set_source_loc` per
statement in `gen()`, and — while each function is compiled (types and decls
live) — builds the **rich C type graph** into a persistent `mir-debug` object
(interning aggregates/enums by tag node so self-references resolve) and
snapshots every named param/local with its MIR location. Two location shapes:

- a value-holding pseudo-reg (`decl->reg_p`): `DW_OP_fbreg(slot)`, no deref;
- a byte offset into the function's single frame `MIR_ALLOCA` block (everything
  else — aggregates, address-taken): `DW_OP_fbreg(fp slot) DW_OP_deref
  DW_OP_plus_uconst(offset)` — which is why `MIR_debug_add_var` takes a
  post-deref `member_offset`.

The driver (`c2mir-driver.c`) forces single-threaded `-O0` +
`MIR_set_inline_permission(0)` + `MIR_set_spill_all(1)`, and after `MIR_link`
calls `c2mir_get_debug_object` (which resolves the snapshot's regs to frame
offsets via `MIR_reg_frame_offset`, emits the ELF/DWARF, and is registered with
gdb). Result in gdb: char/short/int/long with correct width+sign, pointers
(and deref through JIT'd memory), enums by name, arrays, struct/union members,
by-value aggregate params, recursive types, and bitfields.

Two honest limits, both value-correct: narrow scalars live in wider promoted
reg slots (read low bytes, fine on little-endian); static/thread-local locals
and locals inside GNU statement-expressions aren't described. `mir-debug.c` is
kept parseable by c2m itself (named `dwsec_t` instead of `typeof`, the GDB-JIT
asm barrier `#ifndef __mirc__`) so it joins the self-bootstrap source set.

## Future platform support (macOS, Windows)

The whole design is currently DWARF/ELF + the GDB JIT interface. The `mir-dwarf`
→ `mir-debug` rename was the first step toward other targets; this section is the
forward-looking assessment of what they would actually take. The encouraging part
is that the layering already isolates the platform-specific parts:

| Layer | Where | Format-neutral? |
|---|---|---|
| **Debug primitives** — line map, `code_len`, spill-all + `MIR_reg_frame_offset` | MIR core (`mir.h`/`mir-gen.c`) | **Yes** — `(file_id,line)` opaque; reg→fp-offset universal |
| **Debug-content builder** — `MIR_debug_base_type`/`struct_type`/`add_var(fp_offset,deref,member)`… | `mir-debug.c` builder half | **Mostly** — declarative type/var model; DWARF-flavoured vocabulary only |
| **Container/format emit** — `emit_abbrev/info/line` bodies → ELF assembly | `mir-debug.c` emit half | **No** — DWARF bodies neutral, ELF wrapper specific |
| **Registration** — `__jit_debug_descriptor` protocol, context-bound lifetime | `mir-debug-gdb.c` + the `gdb_jit_finish` hook | **Protocol no, lifetime yes** |

The seams fall in the right places: `MIR_debug_emit` builds container-neutral
DWARF section bodies (`emit_abbrev/info/line` into `dwbuf_t`) and only its
trailing ~120 lines assemble them into ELF (`dwsec_t` + `Elf64_*`); and the
lifetime hook (`gdb_jit_finish`) is a generic `void(*)(MIR_context_t)` —
gdb-named but format-agnostic.

### macOS (Mach-O + lldb): a clean slot-in

lldb implements the **same GDB JIT interface** (it reads `__jit_debug_descriptor`
and materialises an in-memory object), so the protocol and the context-bound
lifetime carry over unchanged. The only new piece is **one container backend** —
a Mach-O assembler reusing the existing `emit_abbrev/info/line` bodies verbatim
(DWARF-in-Mach-O is the same DWARF; only the symbol table + load commands differ
from the `dwsec_t`/`Elf64` block). Frame-pointer unwinding works for lldb on
spill-all frames as it does for gdb, and Apple Silicon is aarch64 (already a
first-class reg-offset target). **No MIR-core change; a contained emit-backend
addition at the existing seam.**

### Windows (PE/COFF + CodeView/PDB + WinDbg/VS): core reusable, two new capabilities

The neutral core (line maps, code size, frame-relative locals) transfers. The
declarative builder can drive a CodeView emitter, though its location model
(`fp_offset/deref/member`) is DWARF-expression-shaped and would map — adequately —
onto CodeView frame-relative records. Beyond that, Windows forces two things the
shape does **not** yet have:

1. **Unwind info generation** (see below) — *mandatory*: Windows x64 requires
   `.pdata`/`.xdata` registered via `RtlAddFunctionTable` for **any** stack walk
   (debugger or SEH); without it the debugger can't unwind past a JIT'd frame.
2. **A non-GDB registration path** — there is no `__jit_debug_descriptor`
   equivalent; symbols reach the debugger via a different channel (a findable PDB,
   or the VS JIT-debugging APIs). A `mir-debug-win.c` would install its own cleanup
   through the same `_MIR_set_gdb_jit_finish` context hook (that half is reusable
   as-is); only the platform calls are new.

Plus a **CodeView/PDB emit backend**, which is a substantial format effort in its
own right (separate from unwind, and the larger of the two). These are additive at
the existing seams — a new gen output, a new registration backend, a new emitter —
not a redesign.

### The unwind-info work (the one missing *capability*)

MIR emits **zero** CFI/`.eh_frame`/`.pdata`/`.xdata` today (verified across
`mir.c`/`mir-gen*.c`). Adding it is the single most load-bearing gap, and
**spill-all already did the hard part**: `keep_fp_p` forces a frame pointer in
debug mode, so MIR only ever needs to describe the *canonical frame-pointer
prologue* — every unwind format becomes a small fixed **template** parameterized
by frame size + saved regs, not a general CFI compiler. Three additive pieces:

1. **A neutral per-function frame descriptor from `MIR_gen`** (~3–5 days, x86_64 +
   aarch64). A new output alongside `line_map`/`reg_locs`: frame size, FP reg,
   where the return address and callee-saved regs land, prologue boundary offsets.
   `MIR_gen` already computes all of it (prologue emission); this is a
   `gen_record_line`-style capture + accessor. The reusable core.
2. **DWARF CFI emit** (~2–3 days; Linux/macOS robustness, prerequisite). A CIE +
   one tiny fixed FP-based FDE per function in `mir-debug.c`. **No new
   registration** — gdb/lldb read `.eh_frame` straight from the already-registered
   object, so it slots into the existing emit+register path untouched. (On
   Linux/macOS, FP-walking already covers simple frames, so this is hardening.)
3. **Windows x64 `.pdata`/`.xdata` + `RtlAddFunctionTable`** (~3–5 days + Windows
   test iteration). The `UNWIND_INFO` template (`UWOP_PUSH_NONVOL` +
   `UWOP_SET_FPREG`, by frame size) + a `RUNTIME_FUNCTION` array + a
   `mir-debug-win.c` registration backend, cleaned up via the context hook
   (`RtlDeleteFunctionTable`). This is what *unblocks* Windows stack-walking; the
   real cost is test iteration, not code. (ARM64 Windows is a second xdata variant,
   +1–2 days.)

**Total ≈ 1.5–2.5 weeks** for unwind support that unblocks Windows stack-walking
and hardens Linux/macOS — dominated by the two emit backends and Windows testing,
not by MIR-core changes.

Two scope boundaries worth stating:

- **Unwind ≠ symbols.** This buys stack walking + frame identification. Windows
  *symbol* info (names/types/lines) is the CodeView/PDB effort above — a separate,
  larger workstream.
- **Debug-unwind is easy; production-Windows-unwind is not.** Everything here leans
  on `keep_fp_p`, which only holds in **spill-all (debug)** mode. Optimized JIT
  code on Windows would still need unwind info (Windows mandates it for all code),
  but for FP-omitted optimized frames that means general, prologue-aware emission —
  materially harder. The debug story is the easy case *because* spill-all
  guarantees the frame pointer.
