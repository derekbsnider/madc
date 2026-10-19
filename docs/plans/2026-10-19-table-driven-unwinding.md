# Table-driven unwinding (B205) — plan

Owner go 2026-10-19 ("yes, build it... carefully and mindfully"), after the
frame-registration design measured +153 Ir per call (6.4x on a 36-instruction
function) and was rejected ("7X slower seems completely unacceptable"). That
design is kept as `git stash` "B205 frame-registration design (rejected ...)".
Revised the same day to DWARF CFI and the platform unwinder ("okay, let's do
CFI then"), after the owner's direction that the language layer never knows
the CPU architecture and that every MIR target is supported.

## The defect

A throw that passes through a function with no `try` of its own skips that
function's destructors (B205): the SJLJ raise longjmps over every frame, and a
c2mir `cleanup` attribute runs on normal scope exits only. Today only objects
constructed inside a LEXICAL try body run on a throw, through a runtime
registration (`emit_try_body_cleanup_push`) that costs a malloc, a push and a
remove on every normal path.

## Canon (recon 2026-10-19)

- g++ and clang: zero instructions on the normal path. Each function carries a
  call-site table (LSDA) naming the cleanups live at each call, and DWARF CFI
  (`.eh_frame`) describing its frame; the language runtime asks the PLATFORM
  unwinder (`_Unwind_*`: libgcc, libunwind, the Windows unwinder) to walk, and
  runs the cleanups, then lands in the handler. The architecture lives in the
  compiler's backend (CFI) and the platform unwinder, never in the runtime.
- gcc's own CFI (`tmp/tsprobe/cfi_probe.c`, `gcc -S -O1
  -fno-omit-frame-pointer`): x86-64 `CFA = rsp+8` at entry, `rbp` saved at
  CFA-16, then `CFA = rbp+16`, callee-saved registers as CFA offsets; aarch64
  `CFA = sp+0` at entry, `x29`/`x30` and callee-saved registers as CFA offsets.
- gcc and clang **C** with `-fexceptions` run a `cleanup` attribute's handler
  while an exception unwinds through the frame (`tmp/b205/cleanup_eh_c.c`:
  `cleanup 7`, `caught 7` with both; nothing without `-fexceptions`). So the
  deepest layer is c2mir's `cleanup` attribute, not the CIR builder: C++
  objects reach c2mir as `cleanup` variables.
- When no handler exists anywhere, g++ terminates WITHOUT running destructors
  (two-phase search). madc runs every registered cleanup first — a second
  divergence this work fixes for the no-`try` case.

## Constraints

- No architecture code outside MIR. `src/rt/` is strict C11 with no builtins
  and no asm: it is dual-built (host compiler into libmadc, c2mir into the AOT
  ledger — `scripts/ledger_sources.txt` lists `rt_except.c`), so it cannot read
  registers; the platform unwinder does.
- Every MIR target: x86-64, aarch64, ppc64, riscv64, s390x.

## What MIR already provides

- Every function with memory locals has a constant entry ALLOCA (c2mir's
  frame block) and so keeps a frame pointer on every target: rbp (x86-64), x29
  (aarch64), r31 (ppc64), s0 (riscv64), r11 (s390x). Only x86-64 omits it, in
  functions with no ALLOCA and no stack-arg call (`prohibit_omitting_fp`).
- Each target's prologue (`target_make_prolog_epilog`) knows exactly where it
  saves the return address, the caller's frame pointer and each callee-saved
  register — the facts CFI states.
- Constant ALLOCA sizes round to 16 on every target (each `out_insn`); the
  result sits at SP (x86-64, aarch64, riscv64) or above the new stack header
  (ppc64: header + parameter area; s390x: 160 + parameter + ld areas).
- Per-instruction data already rides the generator into a code-offset table
  (`file_id`/`line` -> `MIR_func.line_map`, `gen_record_line`). A call is ONE
  machine instruction on every target, so its return address is the offset
  after `out_insn`.
- Code is freed only with its whole MIR context (no module unload).
- AOT: the object capture (`MIR_object_*`: data, symbols, relocations,
  `MIR_object_add_init`) is format-neutral; ELF, PE and Mach-O writers lay it out.
- `.debug_frame` templates exist for x86-64 and aarch64 only, for gdb.

## Design

1. **MIR regions (generic: `mir.h` / `mir.c` / `mir-gen.c`)**
   - `MIR_insn.unwind_region` (uint32, 0 = none), stamped like the source
     location: `MIR_set_unwind_region (ctx, region)`.
   - `MIR_new_unwind_region (ctx, func_item, parent, offset, cleanup_item)` ->
     region id: one cleanup variable at `frame_base + offset`, run by
     `cleanup_item (address)`, nested in `parent`.
   - `MIR_set_func_unwind_base (ctx, func_item, reg, align)`: the frame base
     is the function's constant entry ALLOCA (`reg`), rounded up to `align`.
   - The generator fills `MIR_func.unwind`: the call-site table {return
     offset, region} for calls in a region, and the frame base as a CFA
     offset (`base_cfa_offset`) — each target reports its FP's CFA offset and
     its ALLOCA result bias, the generic code adds the entry ALLOCA bytes.
   - Inliner: a callee that declares regions is not inlined; insns copied from
     any other callee take the call's region.
   - Binary and text MIR I/O carry regions, the base and each insn's region.
2. **MIR CFI (generic encoder + one description per target)**
   - Each target's prologue states its frame as data: the CFA rule after the
     prologue (FP + k, or SP + k for a frameless x86-64 body), where the
     return address and every saved register sit (CFA offsets, DWARF register
     numbers), and the code offset where the body begins.
   - `mir-gen.c` encodes a CIE per target (its entry rule: x86-64 `rsp+8`,
     aarch64/ppc64/riscv64 `sp+0`, s390x `r15+160`) and an FDE per function:
     the entry rule, one row at the body start; epilogues are not described
     (walks start at call sites).
   - JIT: generated FDEs are registered with the platform unwinder —
     `__register_frame` (libgcc: a terminated table; libunwind: one FDE per
     call), `RtlAddFunctionTable` with x64 unwind codes on Windows — and
     deregistered with the context.
   - The existing `.debug_frame` templates are replaced by the real CFI.
3. **c2mir** (option `unwind_regions_p`, set by madc for languages with
   exceptions — C++ and the dialect; C stays off, as gcc without
   `-fexceptions`)
   - one region per automatic `cleanup` variable; it becomes live after the
     declaration's initializer (gcc's rule) or, for an object the front end
     constructs by a later statement, at `__builtin_cleanup_arm (&var)` — a
     no-code marker (the constructor is outside the region, as in g++).
   - each normal-exit cleanup call (`build_cleanup_stmt`) runs in its
     variable's PARENT region; blocks and `for` statements end the regions
     their declarations open.
   - with the option on, a scalar cleanup variable lives in the frame block.
4. **AOT images**: the writers carry the CFI and the region tables —
   ELF `.eh_frame` (+ `.eh_frame_hdr`, `PT_GNU_EH_FRAME` in executables; PC
   relocations in objects), Mach-O `__TEXT,__eh_frame`, PE `.pdata`/`.xdata`
   (the exception directory) — and an init entry registers the image's region
   tables with the runtime. Linux executables gain `libgcc_s.so.1`.
5. **Runtime (`src/rt/rt_except.c`, strict C11)**
   - the walk: `_Unwind_Backtrace`; per frame `_Unwind_GetIP` /
     `_Unwind_GetCFA`; a frame whose IP is a MIR call site in a region runs
     that region's cleanups innermost first, each at `CFA + base_cfa_offset +
     offset`; it stops at the frame holding the target try context (running
     only regions nested inside its try guard). On Windows the same loop over
     `RtlLookupFunctionEntry` / `RtlVirtualUnwind`, behind one function.
   - no try at all: print and abort WITHOUT running cleanups (canon).
   - lookup: tables registered by an AOT image's init, plus a resolver the
     JIT host installs (per MIR context, sorted index, mutex).
   - an "unwinding" query, so a cleanup that must behave differently during a
     throw (the scope block) asks, as a destructor asks
     `std::uncaught_exceptions()`.
6. **Consumers converted (one mechanism, the runtime cleanup stack deleted)**
   - CIR try-body registration (`emit_try_body_cleanup_push`): deleted.
   - catch end on a throw out of a handler: the catch guard's own cleanup.
   - block-scope static's guard abort: a guard local disarmed on success.
   - MT-5 scope block: a cleanup local; abandons quietly while unwinding.
   - REPL entry boundary: walks before its longjmp (today it runs dtors AFTER
     the jump, on dead stack).

Normal-path cost: none. The emitted machine code returns to the pre-B205
shape (verified by an instruction-count A/B against `tmp/b205/runcost.cpp`).

## Thread-safety contract

C++ standard-library convention. CFI and AOT tables are registered during
code generation or image initialization and immutable after; the platform
unwinder's registry carries its own lock. The JIT resolver's index is per MIR
context, built and read under one mutex (throw path only). The walk's state is
on the throwing thread's stack; the "unwinding" flag is per-context state,
saved and restored with the rest of the exception state at task switches.

## Order (each step its own commit, Tier 1 + Tier 2; inert until step 6)

1. MIR regions: carrier, API, I/O, inliner rule, call-site table, CFA-relative
   frame base on all five targets (unit-tested through MIR directly).
2. MIR CFI: encoder, the five target descriptions, JIT registration (Linux,
   macOS); unit test walks generated frames with `_Unwind_Backtrace`.
   Non-x86 targets are tested under qemu-user with cross toolchains.
3. c2mir: regions from `cleanup` variables, `__builtin_cleanup_arm`, option.
4. AOT: CFI and region tables in ELF, Mach-O and PE images; Windows unwind
   codes and JIT registration.
5. Runtime walker + registry + resolver (no consumer yet).
6. Switch: c2mir option on, CIR arms objects and drops the registration,
   consumers converted, runtime cleanup stack deleted, REPL boundary.
   `tests/testthrow_through_frame` (11 cases, g++/clang oracle) is the gate,
   plus JIT/exe/obj on every existing exception test.

## Known gaps (stated, not hidden)

- Two-phase search across tries (no matching handler anywhere -> terminate
  without unwinding) needs each try's handler list in its context; the
  no-try case lands here, the rest is the next step.
- Epilogues carry no CFI rows: a sampling profiler stopped inside one sees a
  stale rule (walks from call sites are exact).
- Partially constructed arrays and temporaries are covered exactly as far as
  they are `cleanup` variables in the emitted tree.
- Exceptions crossing into C++ host code (libstdc++ landing pads) need a
  personality routine; the CFI is the prerequisite, not the whole of it.
