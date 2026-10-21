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
- Recon 2026-10-20 (`lsda_probe.c`, `alloca_cfa_probe.c`): gcc 13 and clang
  18 C with `-fexceptions` give a function with a `cleanup` variable
  `.cfi_personality 0x9b, DW.ref.__gcc_personality_v0` and `.cfi_lsda 0x1b`
  into `.gcc_except_table`, and a function without one neither. In a function
  whose SP moves after the prologue (a VLA), clang defines the CFA on the
  frame pointer once it is set on all five targets: x86-64 `rbp+16`, aarch64
  `x29+16`, ppc64 `r31+144`, riscv64 `s0+0`, s390x `r11+352`. MIR's five
  prologues already do the same.

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
- ALLOCA sizes round to 16 on every target (each `out_insn` for a constant;
  the register form's own rounding -- s390x's rounded to 8 until the regions
  made it 16); the result sits at SP (x86-64, aarch64, riscv64) or above the
  new stack header (ppc64: header + parameter area; s390x: 160 + parameter +
  ld areas).
- Per-instruction data already rides the generator into a code-offset table
  (`file_id`/`line` -> `MIR_func.line_map`, `gen_record_line`). A call insn's
  pattern may put insns around the machine call (an address load, ppc64's
  TOC reload), so a call site is recorded as the insn's whole code range --
  the LSDA call-site table's shape.
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
   - The generator fills `MIR_func.unwind`: the call-site table {start, len,
     region} for calls in a region (a return address RA matches when start <
     RA <= start + len), and the frame base as a register + offset
     (`base_reg`, a DWARF number, and `base_offset`; DWARF's `breg` form):
     the body's CFA rule (each target's CFI moves the CFA onto its frame
     pointer once it is set, as gcc and clang do) plus the base's CFA offset —
     each target's prologue reports where it leaves SP relative to the CFA
     and its ALLOCA result bias (`gen_unwind_frame`), the generic code
     subtracts the entry ALLOCA bytes. A personality reads the register from
     its own frame's context (`_Unwind_GetGR`), so it never needs the CFA,
     which the unwinder reports one frame late.
   - Inliner: a callee that declares regions is not inlined; insns copied from
     any other callee take the call's region.
   - Binary and text MIR I/O carry regions, the base and each insn's region.
2. **MIR CFI (generic encoder + one description per target)**
   - Each target's prologue states its frame as data, the way gcc's `.cfi_*`
     directives do: a mark is the code after one prologue insn, and from a
     mark on a CFA rule (SP + k once the frame is allocated, FP + k once the
     frame pointer is set) or a save rule (the return address or a register,
     at a CFA offset, DWARF numbers) holds. A callee-saved register keeps its
     value until the body, so its rule starts at the body's mark. A deleted
     mark insn hands the mark to its predecessor (a split puts its
     replacement before it).
   - `mir-gen.c` encodes a CIE per target (its entry rule: x86-64 `rsp+8`,
     aarch64/ppc64/riscv64 `sp+0`, s390x `r15+160`) and an FDE per function:
     one row per mark, in code order; epilogues are not described (walks start
     at call sites).
   - A function with regions names the front end's personality routine and
     carries its region table as the FDE's LSDA (CIE augmentation `zPLR`), as
     gcc and clang do for a C function with a `cleanup` variable under
     `-fexceptions` (`.cfi_personality __gcc_personality_v0`, `.cfi_lsda` into
     `.gcc_except_table`; a function without one gets neither). The platform
     unwinder calls that personality only for frames that name it, so a table
     is never read by a runtime it does not belong to; the personality reads
     it with `_Unwind_GetLanguageSpecificData`, the call-site offsets against
     `_Unwind_GetRegionStart` -- no registry, in the JIT or an image.
     `MIR_set_func_unwind_personality (ctx, func_item, item)` names it (c2mir:
     an option, set by madc). The table is MIR's own (no landing pads; the
     cleanup is a function called with the variable's address). One encoder
     writes it for the JIT and the image writers: a version byte, the pointer
     encoding of the cleanups (`DW_EH_PE_absptr` in the JIT, pc-relative in
     images), `base_reg` (uleb), `base_offset` (sleb), `base_align` (uleb),
     the call sites {start, len, region} and the regions {parent, offset,
     cleanup pointer}.
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
   (the exception directory) — each FDE pointing at its function's region
   table (an ELF/Mach-O except-table section; on Windows the UNWIND_INFO's
   language-specific data). Linux executables already import
   `libgcc_s.so.1` (the probed C++ NEEDED set).
   - Canon (gcc 13 / clang 18 `-fexceptions`, recon 2026-10-20). A `.o`
     carries `.eh_frame` (SHF_ALLOC, align 8) with PC32 relocations:
     pc_begin to `.text`+off, the CIE's personality to a `DW.ref` data word
     (encoding 0x9b, indirect pcrel sdata4; the word holds an ABS64 to the
     personality), the FDE's LSDA to `.gcc_except_table`+off (0x1b). Every
     function gets an FDE. There is one "zR" CIE plus one "zPLR" CIE per
     personality, and no terminator (crtend's). The executable puts
     `.eh_frame_hdr` (v1; 0x1b / 0x03 / 0x3b; a sorted table),
     `.eh_frame` + terminator and `.gcc_except_table` in a read-only
     segment, with `PT_GNU_EH_FRAME` over the header.
   - Two object section kinds, `.eh_frame` and `.gcc_except_table`, through
     every consumer of the builder: the `.o` writer, the merge reader
     (concatenation is sound — CIE pointers are section-relative), the
     in-process loader (registers its `.eh_frame` as the JIT does), the
     executable writer. Section kinds are counted by `MIR_OBJ_SEC_NUM`, never
     a literal.
   - `.mir.addrpool` is MIR's GOT. The personality's `DW.ref` word is a pool
     slot, and so is each cleanup the region table names: in an image its
     pointers are 0x9b into those slots. The table therefore needs no
     dynamic relocation and stays read-only, even for an imported cleanup.
     The JIT keeps absptr. The table's header byte names the encoding, and
     the runtime reads both.
   - ONE CIE/FDE + region-table encoder for the JIT registration and the
     object capture (gcc's `output_call_frame_info` shape), parametrized by
     the address model (JIT: absolute values; object: PC32 relocations).
   - Order: ELF (x86-64, aarch64) first; Mach-O (`__TEXT,__eh_frame`,
     `__gcc_except_tab`) next; PE `.pdata`/`.xdata` with the Windows runtime
     switch. Until a container carries the sections, its capture emits none.
5. **Runtime (`src/rt/rt_except.c`, strict C11)**
   - the walk is the platform unwinder's forced unwind to the SJLJ try
     (revised 2026-10-20 from an `_Unwind_Backtrace` loop, which cannot tell
     madc's tables from a g++ frame's): glibc's thread-cancellation shape,
     the canon for running DWARF cleanups down to a `setjmp` point. A throw
     calls `_Unwind_ForcedUnwind` on an `_Unwind_Exception` inside the madc
     exception with a stop function; the unwinder calls the stop function
     for every frame and the personality, `__madc_personality`, for every
     frame that names it. A two-phase `_Unwind_RaiseException` cannot serve:
     tries are SJLJ, and some sit in host C frames with no personality (the
     task runtime's `task_run_body`, the REPL entry boundary), which no
     search phase can find.
     - stop function: once the context's `_Unwind_GetCFA` (the SP of the
       frame it describes) is above the target try context -- the frame
       holding the try has been passed -- or at the end of the stack, pop
       the try and longjmp into it (glibc's `unwind_stop` compares the
       `jmp_buf`'s SP the same way; the try context is a local of the try's
       frame, so its own address serves, with no `jmp_buf` internals).
     - personality, per frame: its region table
       (`_Unwind_GetLanguageSpecificData`), the call site at `_Unwind_GetIP`
       - `_Unwind_GetRegionStart`, the frame base at `_Unwind_GetGR
       (base_reg) + base_offset`; the call site's region chain runs
       innermost first, each cleanup called with `base + offset`, stopping
       at the target try's guard (the region whose cleanup is the try's
       leave and whose variable holds that try context) -- objects the try
       body made go, objects outside it stay.
     - the unwind's header (the ABI's `_Unwind_Exception`, with the target
       try) is heap-allocated and freed by the stop function as it lands: a
       g++ landing pad resets SP to its own frame and its `_Unwind_Resume`
       reads the header back from storage below it (g++ heap-allocates its
       exceptions; glibc keeps its in the thread descriptor).
     - a foreign exception (a host C++ throw crossing MIR frames): the
       personality's cleanup phase runs every region, as g++ runs a frame's
       destructors for any exception. A g++ host frame between a madc throw
       and its try runs its own destructors through its own personality --
       the SJLJ longjmp used to skip them.
     - Windows keeps the SJLJ raise until its step (MIR frames there have no
       unwind tables yet); then the language-specific handler in each
       UNWIND_INFO, over the same table.
   - no try at all: print and abort WITHOUT unwinding or running cleanups
     (canon: g++'s search phase finds no handler and terminates).
   - lookup: the platform unwinder's own FDE lookup, which hands the
     personality its LSDA (revised 2026-10-20 from a runtime registry + JIT
     resolver: the canon shape needs neither, nor their lock).
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

C++ standard-library convention. CFI and region tables are written during
code generation or image loading and immutable after; the platform
unwinder's registry carries its own lock, and the walk reads the region
tables through it (no runtime index of its own). The walk's state is
on the throwing thread's stack; the "unwinding" flag is per-context state,
saved and restored with the rest of the exception state at task switches.

## Order (each step its own commit, Tier 1 + Tier 2; inert until step 7)

1. MIR regions: carrier, API, I/O, inliner rule, call-site table, CFA-relative
   frame base on all five targets (unit-tested through MIR directly).
2. MIR CFI: encoder, the five target descriptions, JIT registration (Linux,
   macOS); unit test walks generated frames with `_Unwind_Backtrace`.
   Non-x86 targets are tested under qemu-user with cross toolchains.
3. c2mir: regions from `cleanup` variables, `__builtin_cleanup_arm`, option.
4. Region tables as LSDAs: the base as `breg`, the personality API, the one
   encoder, the JIT FDE names the personality and points at the table; the
   unit test raises through generated frames with a test personality.
5. Runtime personality + the forced-unwind raise: every throw (not on
   Windows) reaches its try through `_Unwind_ForcedUnwind` and the stop
   function -- behaviour-preserving while no frame names the personality, so
   the whole exception suite proves the landing; a unit test runs the
   personality end to end through c2mir-compiled frames in madc's try shape.
6. AOT: CFI and region tables in ELF, Mach-O and PE images; Windows unwind
   codes and JIT registration.
   - 6.0 `MIR_OBJ_SEC_NUM`: per-kind tables replace the literal 5s and the
     per-kind ternary chains (builder, input view, loader, merge reader, ELF
     writers, Mach-O / PE fronts). Behaviour-preserving.
   - 6.1 ELF: the two kinds, the one encoder, the capture's FDEs + tables,
     `.o` writer, merge, loader registration, executable `.eh_frame_hdr` +
     `PT_GNU_EH_FRAME`, runtime reads 0x9b. Unit tests: an object loaded
     in-process and a merged one run the real personality (cleanups 112,
     11, 10, caught 12); an emitted executable's raise reaches a
     c2mir-compiled personality through `PT_GNU_EH_FRAME`.
   - 6.2 Mach-O (canon: clang 18 objects, ld64.lld 18 and Apple ld images,
     recon 2026-10-20). The capture runs for Apple targets.
     - `.o`: `__TEXT,__eh_frame` (S_COALESCED | no_toc | strip_static_syms |
       live_support, align 8) and `__TEXT,__gcc_except_tab` (S_REGULAR).
       Mach-O spells a pc-relative data field two ways, as clang does: in
       `__eh_frame` a field through a `__mir_addrpool` slot holding symbol S
       (the CIE's personality) is a GOT reference to S (X86_64_RELOC_GOT
       with the +4 in the field; ARM64_RELOC_POINTER_TO_GOT), because ld64
       reads a 0x9b personality only in that form; every other field (a
       table's cleanup slots included: ld64 never reads an LSDA) is a
       SUBTRACTOR (its own section's start label) + UNSIGNED (target) pair
       with the addend in the field. An FDE's LSDA field targets its table's
       own `lexc_<offset>` label with no addend, as clang's targets its
       GCC_except_tableN: against the section label plus the table's offset
       Apple ld 1115 writes the LSDA at the label MINUS the offset (measured
       on arm64; lld handles both). The reader inverts all three: a GOT
       reference becomes PC32 to the object's slot for S, a label is its
       section's symbol at its offset.
     - Measured on macOS 14 arm64 and x86_64 (Rosetta), with a c2mir probe:
       madc's executable, an executable from merged madc objects, and Apple
       ld over madc objects each unwind two region-bearing frames, the
       personality reading both tables and calling each cleanup through its
       slot.
     - Executable: `__gcc_except_tab` and `__eh_frame` (no terminator) in
       `__TEXT` after `__text`, the fields resolved at emit, plus
       `__unwind_info` (version 1), which the macOS unwinder reads first:
       regular second-level pages of {function offset, encoding}, each
       function with an FDE in DWARF mode (x86_64 0x04000000 | FDE offset,
       arm64 0x03000000 | FDE offset; the personality and LSDA come from the
       FDE, so the LSDA index is empty), a function without one 0, and a
       sentinel index entry at the end of `__text`. An FDE past the 24-bit
       offset field is DWARF mode with offset 0 (the unwinder searches).
     - JIT: unchanged (libunwind's `__register_frame` takes one FDE).
     - Tests: the executable and merged-object cases of
       tests/unit/test_mir_unwind.cpp run on macOS (x86_64, arm64); the
       machogate legs check the `.o` and image structure with llvm-objdump and
       ld64.lld on the container; Apple ld links a `.o` on the Macs.
   - 6.3 PE + the Windows runtime (canon recon 2026-10-21: mingw gcc 13 and
     clang 18 `x86_64-w64-mingw32`, mingw libgcc's unwind-seh.o / unwind-c.o).
     - Canon. Every function has a RUNTIME_FUNCTION in `.pdata` ({begin, end,
       UNWIND_INFO} as ADDR32NB image-relative words) and an UNWIND_INFO in
       `.xdata` (version 1; prologue codes in reverse prologue order; the
       frame register and its offset, at most 15 x 16 = 240 bytes above the
       RSP of the fixed allocation, established after it). A function with
       handlers sets EHANDLER|UHANDLER and names a language-specific handler
       (`__gcc_personality_seh0`) by RVA; its LSDA is the handler data that
       follows the codes, offsets relative to the function start (no
       `.gcc_except_table`). The image's exception directory (data
       directory 3) covers `.pdata`. libgcc's `_GCC_specific_handler` bridges
       the dispatcher to a DWARF-style personality, but `_Unwind_GetGR`
       aborts past register 1, `_Unwind_GetCFA` is the frame's RSP, and its
       forced unwind calls a stop function only in frames that carry its own
       handler.
     - MIR's Windows x86-64 frame becomes the canonical one. Today the
       prologue stores rbp below RSP (`mov %rbp,-8(%rsp)` -- Windows has no
       red zone), sets rbp at the frame's TOP before the allocation, and
       saves callee-saved registers at its bottom: no frame over 240 bytes
       has a valid description. The Windows prologue is `push %rbp; sub $N,
       %rsp; lea FO(%rsp),%rbp` (frame register rbp at offset FO = min(N,
       240) -- see the canon recon below -- so the establisher frame is
       rbp - FO), the saves at the establisher frame + k (SAVE_NONVOL /
       SAVE_XMM128), inline page probes for N >= 4096, and the epilogue `lea
       (N - FO)(%rbp),%rsp; pop %rbp; ret` (a form the unwinder recognizes).
       Every rbp-relative displacement MIR assigned against the top --
       stack slots, incoming arguments, the varargs spill, `fp + disp`
       address arithmetic -- moves by the one frame delta N - FO when the
       prologue is made (none for a frame of 240 bytes or less).
     - Unwind codes come from the same prologue facts as the DWARF CFI (the
       marks `gen_cfi_*` record): one encoder for the JIT and the images.
     - JIT: each function's UNWIND_INFO, LSDA and a handler thunk (`jmp
       *slot` to the handler) sit in its code allocation (every RVA is
       32-bit from the base, and code memory may be far from madc's image);
       `RtlAddFunctionTable` registers it, `RtlDeleteFunctionTable` drops it.
       A frame with no regions still gets codes: `RtlUnwindEx` across a JIT
       frame with none faulted (win_ucrt_gate.sh).
     - PE images and COFF objects: `.pdata`, `.xdata`, an image-relative
       relocation kind (ADDR32NB), the exception directory; the handler
       through an in-image stub, as ld writes one for an imported
       personality. The synthesized entry stub gets its entry too.
     - Runtime: madc's own language-specific handler, `__madc_seh_handler`,
       runs the same region table: the call site from `ControlPc` - the
       function start, the frame base from the establisher frame. The throw
       on Windows is an unwinding `longjmp` (`_setjmp` with the try frame's
       establisher frame; ucrt's `longjmp` calls `RtlUnwindEx`, which calls
       each frame's handler with EXCEPTION_UNWINDING and the target frame's
       with EXCEPTION_TARGET_UNWIND): madc's handler runs a frame's regions
       when a madc throw is in flight (a thread-local the throw sets) or for
       a GCC unwind code (a host C++ exception crossing MIR frames), and in
       the target frame stops at the try's guard. A plain C `longjmp` runs
       no cleanups (gcc's handler ignores STATUS_LONGJUMP too). Host g++
       frames between a madc throw and its try skip their destructors on
       Windows, as today.
     - Order: probe `RtlUnwindEx`'s target-frame and handler-call semantics
       on genuine Windows (a mingw C probe with a hand-written `.seh_handler`)
       first; then the MIR frame (behaviour-preserving, the wine and genuine
       Windows suites as its oracle); the codes + JIT registration
       (testunwind_backtrace's win64 skip lifts); the PE/COFF sections; the
       handler + the throw switch with step 7.
     - Probe 2026-10-21 (`tmp/castattr/sehprobe.c` + `sehprobe_asm.S`, the
       proposed frame with N = 1024 and a `.seh_handler`), genuine Windows
       and wine alike: a `longjmp` across the frame calls its handler with
       code 0x80000026 (STATUS_LONGJUMP), flags 0x02, establisher frame ==
       rbp; a `longjmp` to a `_setjmp(buf, rbp)` in the frame calls it with
       flags 0x22 (UNWINDING | TARGET_UNWIND), the same establisher frame,
       and lands. The design holds as written.
     - Canon recon 2026-10-21 (frame register offset, probes): clang sets the
       frame register ABOVE the bottom -- `push %rbp; sub $N,%rsp; lea
       K(%rsp),%rbp; .seh_setframe %rbp, K` (K = 32 or 128) -- and passes the
       establisher frame, `lea -K(%rbp)`, to `_setjmp`; gcc does the same
       with K = 128 and, in a function without alloca, `push %rbp; mov
       %rsp,%rbp; push ...; sub` (pushes after the frame register are sound
       only while SP is fixed). MIR's frame register offset is therefore
       FO = min(N, 240): a frame of 240 bytes or less keeps today's rbp (the
       top), so nothing in its body moves; a larger frame's FP-relative
       displacements move by N - FO. The establisher frame a try passes to
       `_setjmp` is rbp - FO (a MIR operation, as clang's frame address).
       Both canons probe a frame or an alloca of 4096 bytes or more through
       `___chkstk_ms`; MIR links nothing, so it probes inline (the prologue
       loads every page below SP before the one `sub`; an alloca walks SP
       down a page at a time). Without the probes a native Windows process
       faulted on its first store into a larger area (wine commits the stack
       up front): `tests/teststackprobe`.
7. Switch: c2mir option on, CIR arms objects and drops the registration,
   consumers converted, runtime cleanup stack deleted, REPL boundary.
   `tests/testthrow_through_frame` (11 cases, g++/clang oracle) is the gate,
   plus JIT/exe/obj on every existing exception test.

## Known gaps (stated, not hidden)

- Two-phase search across tries (no matching handler anywhere -> terminate
  without unwinding) needs each try's handler list in its context; the
  no-try case lands here, the rest is the next step.
- Epilogues carry no CFI rows: a sampling profiler stopped inside one sees a
  stale rule (walks from call sites are exact).
- Each generated function is registered as its own table. libgcc 13+ keeps
  registered tables in a b-tree; older libgcc (Ubuntu 22.04's gcc 11) keeps a
  list that every frame lookup walks, so a throw costs O(registered functions)
  per frame there. To measure on 22.04; the remedy, if it shows, is one table
  per generation batch.
- Partially constructed arrays and temporaries are covered exactly as far as
  they are `cleanup` variables in the emitted tree.
- A host C++ exception crossing MIR frames runs their cleanups but no madc
  `try` catches it (tries match madc exceptions only); a madc exception
  crossing g++ host frames runs their destructors, and of their handlers
  only a `catch (...)` takes it (its class is madc's, which
  `__gxx_personality_v0` treats as foreign).
