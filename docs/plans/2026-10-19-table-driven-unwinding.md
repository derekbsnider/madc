# Table-driven unwinding (B205) — plan

Owner go 2026-10-19 ("yes, build it... carefully and mindfully"), after the
frame-registration design measured +153 Ir per call (6.4x on a 36-instruction
function) and was rejected ("7X slower seems completely unacceptable"). That
design is kept as `git stash` "B205 frame-registration design (rejected ...)".

## The defect

A throw that passes through a function with no `try` of its own skips that
function's destructors (B205): the SJLJ raise longjmps over every frame, and a
c2mir `cleanup` attribute runs on normal scope exits only. Today only objects
constructed inside a LEXICAL try body run on a throw, through a runtime
registration (`emit_try_body_cleanup_push`) that costs a malloc, a push and a
remove on every normal path.

## Canon (recon 2026-10-19)

- g++ and clang: zero instructions on the normal path. Each function carries a
  call-site table (LSDA) naming the cleanups live at each call; a throw walks
  the stack (CFI) and runs them, then lands in the handler.
- gcc and clang **C** with `-fexceptions` run a `cleanup` attribute's handler
  while an exception unwinds through the frame (`tmp/b205/cleanup_eh_c.c`:
  `cleanup 7`, `caught 7` with both; nothing without `-fexceptions`). So the
  deepest layer is c2mir's `cleanup` attribute, not the CIR builder: C++
  objects reach c2mir as `cleanup` variables.
- When no handler exists anywhere, g++ terminates WITHOUT running destructors
  (two-phase search). madc runs every registered cleanup first — a second
  divergence this work fixes for the no-`try` case.

## What MIR already provides

- rbp (x86-64) and x29 (aarch64) are fixed registers, never allocated
  (`mir-x86_64.h:49`, `mir-aarch64.h:50`). Every function with memory locals
  has a constant entry ALLOCA (c2mir's frame block, `c2mir.c:20357`) and so
  keeps a frame pointer; both targets store the standard record
  {caller FP, return address} at FP.
- aarch64 gives every non-leaf function a frame record (`mir-gen-aarch64.c`
  prologue). x86-64 omits the frame pointer in functions with no ALLOCA; such a
  frame's size is `block_size + service_area_size`, plus the call's stack-arg
  area (`mir-gen-x86_64.c:727`), and rbp passes through it untouched.
- Constant ALLOCA sizes round to 16 on both targets (`out_insn`).
- Per-instruction data already rides the generator into a code-offset table:
  `file_id`/`line` -> `MIR_func.line_map` (`gen_record_line`, called from each
  target's emit loop beside `out_insn`). A call is ONE machine instruction on
  both targets, so its return address is the offset after `out_insn`.
- Code is freed only with its whole MIR context (no module unload).
- AOT: `MIR_ref_data_item` gives data -> code ABS64 relocations in every image
  writer; `MIR_object_add_init` adds an init-array entry.

## Design

1. **MIR (generic, `mir.h` / `mir.c` / `mir-gen.c`)**
   - `MIR_insn.unwind_region` (uint32, 0 = none), stamped like the source
     location: `MIR_set_unwind_region (ctx, region)`. `MIR_gen` stamps 0 on the
     insns it creates.
   - `MIR_new_unwind_region (ctx, func_item, parent, offset, cleanup_item)` ->
     region id: one cleanup variable at `frame_base + offset`, run by
     `cleanup_item (address)`, nested in `parent`.
   - `MIR_set_func_unwind_base (ctx, func_item, reg, align)`: the register the
     region offsets are relative to — defined by the function's constant
     entry ALLOCA, optionally rounded up to `align` (c2mir's over-aligned frame).
   - The generator fills `MIR_func.unwind` (like `line_map`): the frame kind
     (FP or SP), the SP frame size, the base's FP offset (each target reports
     its post-prologue SP offset — the one target-specific number), and the
     call-site table {return offset, region, stack-arg bytes}.
   - Inliner: a callee that declares regions is not inlined; insns copied from
     any other callee take the call's region.
2. **c2mir** (option `unwind_regions_p`, set by madc for languages with
   exceptions — C++ and the dialect; C stays off, as gcc without
   `-fexceptions`)
   - one region per automatic `cleanup` variable; it becomes live after the
     declaration's initializer (gcc's rule) or, for an object the front end
     constructs by a later statement, at `__builtin_cleanup_arm (&var)` — a
     no-code marker (the constructor is outside the region, as in g++).
   - each normal-exit cleanup call (`build_cleanup_stmt`) runs in its
     variable's PARENT region.
3. **Runtime (`src/rt/rt_except.c`)**
   - a walker over MIR frames: from the raise's own frame pointer up the chain;
     per frame look up the return address, run the live regions' cleanups
     innermost first; stop at the frame holding the target try context
     (running only regions nested inside its try guard) or at the first frame
     that is not MIR code.
   - no try at all: print and abort WITHOUT running cleanups (canon).
   - lookup: tables registered by an AOT image's init, plus a resolver the
     JIT host installs (per MIR context, sorted index, mutex).
   - an "unwinding" query, so a cleanup that must behave differently during a
     throw (the scope block) asks, as a destructor asks
     `std::uncaught_exceptions()`.
4. **Consumers converted (one mechanism, the runtime cleanup stack deleted)**
   - CIR try-body registration (`emit_try_body_cleanup_push`): deleted.
   - catch end on a throw out of a handler: the catch guard's own cleanup.
   - block-scope static's guard abort: a guard local disarmed on success.
   - MT-5 scope block: a cleanup local; abandons quietly while unwinding.
   - REPL entry boundary: walks before its longjmp (today it runs dtors AFTER
     the jump, on dead stack).
5. **AOT**: the writer emits each function's unwind record as data (code and
   cleanup addresses as relocations) and an init entry that registers the
   image's table.

Normal-path cost: none. The emitted machine code returns to the pre-B205
shape (verified by an instruction-count A/B against `tmp/b205/runcost.cpp`).

## Thread-safety contract

C++ standard-library convention. AOT tables are registered during image
initialization and immutable after. The JIT resolver's index is per MIR
context, built and read under one mutex (throw path only). The walk's state is
on the throwing thread's stack; the "unwinding" flag is per-context state,
saved and restored with the rest of the exception state at task switches.

## Order (each step its own commit, Tier 1 + Tier 2; inert until step 5)

1. MIR: region carrier + API + JIT unwind info + inliner rule (unit-tested
   through MIR directly).
2. c2mir: regions from `cleanup` variables, `__builtin_cleanup_arm`, option.
3. MIR AOT: unwind records + init registration in ELF, PE, Mach-O images.
4. Runtime walker + registry + resolver (no consumer yet).
5. Switch: c2mir option on, CIR arms objects and drops the registration,
   consumers converted, runtime cleanup stack deleted, REPL boundary.
   `tests/testthrow_through_frame` (11 cases, g++/clang oracle) is the gate,
   plus JIT/exe/obj on every existing exception test.

## Known gaps (stated, not hidden)

- A non-MIR frame between the throw and the try (a callback through `qsort`,
  host code) stops the walk; frames above it keep the old behaviour. Same for
  `--emit=c11` output compiled by gcc: its frames carry no madc tables.
  Follow-on: register MIR CFI with the platform unwinder and run madc tables
  from a personality / forced-unwind stop function — which also lets
  libstdc++ exceptions pass through madc frames.
- Two-phase search across tries (no matching handler anywhere -> terminate
  without unwinding) needs each try's handler list in its context; the
  no-try case lands here, the rest is the next step.
- Partially constructed arrays and temporaries are covered exactly as far as
  they are `cleanup` variables in the emitted tree.
