/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* mir-debug: a small, frontend-agnostic emitter that turns information a MIR
   frontend already has -- generated function addresses/sizes, the per-function
   source line map (MIR_func.line_map), and a description of local variables and
   their C types -- into an in-memory ELF object carrying a symbol table and
   DWARF (.debug_line + .debug_info), and registers it with an attached debugger
   through the GDB JIT interface.  This gives source-level debugging (named
   frames, break file:line, step/next, print/info locals) of JIT'd code in gdb
   on ELF platforms, without the frontend writing any ELF/DWARF bytes itself.

   See c2mir for a reference consumer.  Typical use:

     MIR_debug_t d = MIR_debug_init ();
     uint32_t f = MIR_debug_add_file (d, "/path/to/source.c");   // file_id you
                                                                 // stamp on insns
     MIR_debug_type_t ti = MIR_debug_base_type (d, "int", MIR_DEBUG_ENC_SIGNED, 4);
     for each generated function fn:
       MIR_debug_add_func (d, fn->name, fn->machine_code, fn->code_len,
                           fn->line_map, fn->line_map_len);
       for each local var v with reg r and type t:
         int64_t off; MIR_reg_frame_offset (fn, r, &off);
         MIR_debug_add_var (d, v->name, v->is_param, t, off, deref_p, 0);
     void *buf; size_t size;
     MIR_debug_emit (d, &buf, &size);
     MIR_debug_gdb_register (ctx, buf, size);  // takes buf; freed by MIR_finish(ctx)
     MIR_debug_destroy (d);
     ... run the code ...
     // MIR_finish (ctx) unregisters and frees the debug object

   The frontend should generate debuggable code: optimize level 0,
   MIR_set_inline_permission(ctx,0), and -- for variable inspection --
   MIR_set_spill_all(ctx,1) so every local has a stable frame slot.  */

#ifndef MIR_DEBUG_H
#define MIR_DEBUG_H

#include <stddef.h>
#include <stdint.h>
#include "mir.h" /* for MIR_line_map_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Base-type encodings (a subset of DWARF DW_ATE_*). */
typedef enum {
  MIR_DEBUG_ENC_SIGNED = 1,
  MIR_DEBUG_ENC_UNSIGNED,
  MIR_DEBUG_ENC_FLOAT,
  MIR_DEBUG_ENC_BOOL,
  MIR_DEBUG_ENC_SIGNED_CHAR,
  MIR_DEBUG_ENC_UNSIGNED_CHAR,
} MIR_debug_encoding_t;

typedef struct MIR_debug *MIR_debug_t;
typedef uint32_t MIR_debug_type_t; /* opaque type handle; 0 == void / no type */

/* Create a builder for the host architecture, or NULL if the host is not a
   supported little-endian ELF target. */
extern MIR_debug_t MIR_debug_init (void);
extern void MIR_debug_destroy (MIR_debug_t d);

/* Register a source file path; returns a 1-based id to use as the file_id you
   stamp on insns via MIR_set_source_loc (0 means "no file"). */
extern uint32_t MIR_debug_add_file (MIR_debug_t d, const char *path);

/* --- Type graph ---------------------------------------------------------
   The caller drives construction (walking its own type representation and
   deduplicating by its own type identity); the builder records handles and
   resolves cross references at emit time, so forward/recursive references are
   fine.  For a recursive aggregate, reserve its handle first (the *_type call
   returns it) and add members afterwards -- a member can reference the
   aggregate's own handle.  */
extern MIR_debug_type_t MIR_debug_base_type (MIR_debug_t d, const char *name,
                                             MIR_debug_encoding_t enc, int byte_size);
extern MIR_debug_type_t MIR_debug_pointer_type (MIR_debug_t d, MIR_debug_type_t pointee);
extern MIR_debug_type_t MIR_debug_array_type (MIR_debug_t d, MIR_debug_type_t elem,
                                              int64_t count /* <0 == unknown */);
extern MIR_debug_type_t MIR_debug_typedef_type (MIR_debug_t d, const char *name,
                                                MIR_debug_type_t ref);
extern MIR_debug_type_t MIR_debug_struct_type (MIR_debug_t d, const char *name /* NULL == anon */,
                                               int64_t byte_size, int is_union);
extern void MIR_debug_add_member (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                                  MIR_debug_type_t type, int64_t byte_offset);
extern void MIR_debug_add_bitfield (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                                    MIR_debug_type_t type, int64_t bit_offset, int bit_size);
extern MIR_debug_type_t MIR_debug_enum_type (MIR_debug_t d, const char *name, int64_t byte_size);
extern void MIR_debug_add_enumerator (MIR_debug_t d, MIR_debug_type_t en, const char *name,
                                      int64_t value);
extern MIR_debug_type_t MIR_debug_func_type (MIR_debug_t d, MIR_debug_type_t ret_type);
extern void MIR_debug_add_param_type (MIR_debug_t d, MIR_debug_type_t fn, MIR_debug_type_t type);

/* --- Functions and their variables --------------------------------------
   addr/size/line_map come straight from a generated MIR_func (machine_code,
   code_len, line_map / line_map_len).  Variables are attached to the most
   recently added function.  */
extern void MIR_debug_add_func (MIR_debug_t d, const char *name, const void *addr, size_t size,
                                const MIR_line_map_t *line_map, size_t line_map_len);
/* Location of the variable, built as the DWARF expression
     DW_OP_fbreg(fp_offset) [DW_OP_deref] [DW_OP_plus_uconst(member_offset)].
   fp_offset: the frame-pointer-relative slot, from MIR_reg_frame_offset.
   deref_p: nonzero if that slot holds an *address* (an ALLOCA-style frontend)
   rather than the value itself.  member_offset: a constant added after the
   optional deref -- e.g. a frontend that homes all locals in one frame block
   and indexes each by a byte offset passes the block-pointer slot with
   deref_p=1 and the per-variable offset here (0 when unused). */
extern void MIR_debug_add_var (MIR_debug_t d, const char *name, int is_param,
                               MIR_debug_type_t type, int64_t fp_offset, int deref_p,
                               uint64_t member_offset);

/* Produce the in-memory ELF object.  On success returns 0 and sets *buf
   (malloc'd; free with free(), or hand to MIR_debug_gdb_register which takes
   ownership) and *size.  Returns nonzero on failure / unsupported host. */
extern int MIR_debug_emit (MIR_debug_t d, void **buf, size_t *size);

/* --- GDB JIT interface --------------------------------------------------
   Defined in mir-debug-gdb.c, which owns the process-global
   __jit_debug_descriptor.  Register a buffer (from MIR_debug_emit, ownership
   transfers) so an attached gdb reads it.

   ctx ties the registration to a MIR context: MIR_finish(ctx) then unregisters
   and frees it automatically, since the JIT code it describes is freed there.
   That is the normal path -- bind to the context that generated the code and
   never unregister by hand.  Pass ctx == NULL for a manual lifetime, where the
   returned handle must be given to MIR_debug_gdb_unregister before the code is
   freed.  (The returned handle may be unregistered early in either case.) */
typedef struct MIR_debug_jit_entry *MIR_debug_jit_t;
extern MIR_debug_jit_t MIR_debug_gdb_register (MIR_context_t ctx, void *buf, size_t size);
extern void MIR_debug_gdb_unregister (MIR_debug_jit_t entry);

/* --- AOT relocatable-object builder --------------------------------------
   The second consumer of this file's ELF emission core (one writer serves
   both the GDB-JIT debug object above and the AOT `.o`).  mir-gen.c's object
   mode (MIR_gen_set_object_mode) captures machine code, data, symbols and
   relocations here instead of publishing to executable memory;
   MIR_object_emit then assembles an on-disk relocatable ELF object
   (ET_REL, x86-64 first).  Unlike the debug object -- which describes
   already-resolved JIT addresses -- everything here is section-relative and
   unresolved: .text/.data/.mir.addrpool carry the real bytes, their .rela.*
   siblings carry the relocations the system linker applies. */

typedef struct MIR_object *MIR_object_t;

/* Section identifiers (fixed section-header indexes 1..4 in the emitted
   object; .init_array follows at 5 when non-empty).  MIR_OBJ_SEC_UNDEF
   marks an imported (undefined) symbol.
   ADDRPOOL (".mir.addrpool", R6 PIC) is the GOT-shaped address-slot
   section: every 8-byte address the generated code materializes or calls
   through -- const-pool entries and switch tables -- lives here instead of
   .text, reached by rip-relative PC32 references (x86-64) or adrp+ldr/add
   page pairs (aarch64), so .text carries no relocations at all (no
   DT_TEXTREL in executables/shared objects).
   INITARR (".init_array", SHT_INIT_ARRAY -- the platform section, so an
   external linker collects it natively) holds 8-byte function-pointer
   slots, one per registered module initializer (MIR_object_add_init);
   each slot is zero in the file and covered by an ABS64 relocation. */
enum {
  MIR_OBJ_SEC_UNDEF = -1,
  MIR_OBJ_SEC_TEXT = 0,
  MIR_OBJ_SEC_DATA = 1,
  MIR_OBJ_SEC_BSS = 2,
  MIR_OBJ_SEC_ADDRPOOL = 3,
  MIR_OBJ_SEC_INITARR = 4,
};

/* Relocation kinds (mapped to the arch-specific ELF type at emit). */
enum {
  MIR_OBJ_RELOC_ABS64 = 0, /* absolute 64-bit address: R_X86_64_64 /
                              R_AARCH64_ABS64 */
  MIR_OBJ_RELOC_PC32 = 1,  /* rip-relative 32-bit: R_X86_64_PC32 (S+A-P).
                              Never dynamic: resolved by whoever fixes the
                              section layout (external linker for a .o, the
                              executable emitter at emit time, the in-process
                              loader at map time). */
  /* aarch64 insn-field kinds: the adrp+ldr/add pool pairs the aarch64
     capture emits against .mir.addrpool.  Like PC32 they are never dynamic
     (page distances slide with the whole image), and each patches a bitfield
     of one 4-byte insn. */
  MIR_OBJ_RELOC_AARCH64_ADR_PG_HI21 = 2, /* adrp: R_AARCH64_ADR_PREL_PG_HI21,
                                            Page(S+A)-Page(P) into immhi:immlo */
  MIR_OBJ_RELOC_AARCH64_LDST64_LO12 = 3, /* ldr Xt,[Xn,#lo12]:
                                            R_AARCH64_LDST64_ABS_LO12_NC,
                                            (S+A)[11:3] into imm12 */
  MIR_OBJ_RELOC_AARCH64_ADD_LO12 = 4,    /* add Xd,Xn,#lo12:
                                            R_AARCH64_ADD_ABS_LO12_NC,
                                            (S+A)[11:0] into imm12 */
};

/* Create a builder, or NULL if the build's target is not a supported ELF
   object target (x86-64 and aarch64 have the reloc mapping). */
extern MIR_object_t MIR_object_create (void);
extern void MIR_object_destroy (MIR_object_t obj);

/* Append len code bytes to .text (start padded to 16-byte alignment);
   returns the .text offset the bytes landed at. */
extern size_t MIR_object_text_append (MIR_object_t obj, const void *bytes, size_t len);
/* Append len bytes to .data at the given power-of-2 alignment (bytes == NULL
   appends zero fill); returns the .data offset. */
extern size_t MIR_object_data_append (MIR_object_t obj, const void *bytes, size_t len,
                                      size_t align);
/* Reserve len bytes in .bss at the given power-of-2 alignment; returns the
   .bss offset. */
extern size_t MIR_object_bss_reserve (MIR_object_t obj, size_t len, size_t align);
/* Append len bytes to .mir.addrpool at the given power-of-2 alignment
   (bytes == NULL appends zero fill); returns the pool offset.  Slots that
   hold link-time addresses should be left zero and covered by an ABS64
   relocation in MIR_OBJ_SEC_ADDRPOOL. */
extern size_t MIR_object_addrpool_append (MIR_object_t obj, const void *bytes, size_t len,
                                          size_t align);

/* Define (sec >= 0, at section offset value with the given size) or import
   (sec == MIR_OBJ_SEC_UNDEF) a symbol.  func_p selects STT_FUNC vs
   STT_OBJECT; binding is STB_GLOBAL unless local_p (STB_LOCAL) or weak_p
   (STB_WEAK).  Returns a stable symbol id for MIR_object_add_reloc, or -1 on
   failure. */
extern int MIR_object_add_symbol (MIR_object_t obj, const char *name, int sec, uint64_t value,
                                  uint64_t size, int func_p, int local_p, int weak_p);
/* Turn an existing symbol (typically created as MIR_OBJ_SEC_UNDEF when it was
   first referenced by a relocation) into a definition.  Referencing before
   defining is the normal capture order for forward calls. */
extern void MIR_object_symbol_define (MIR_object_t obj, int sym_id, int sec, uint64_t value,
                                      uint64_t size, int func_p, int local_p, int weak_p);
/* The STT_SECTION symbol id for sec (created on first use) -- the target for
   section-relative relocations (label addresses, lref data). */
extern int MIR_object_section_symbol (MIR_object_t obj, int sec);
/* Nonzero if the symbol has been defined (not just referenced). */
extern int MIR_object_symbol_defined_p (MIR_object_t obj, int sym_id);
/* Add a relocation of the given kind in section sec (TEXT, DATA or ADDRPOOL)
   at offset, against sym_id with the given addend.  The relocated slot's
   bytes should be left zero: RELA addends carry the whole value. */
extern void MIR_object_add_reloc (MIR_object_t obj, int sec, uint64_t offset, int sym_id,
                                  int64_t addend, int kind);

/* Look up a symbol by name.  Returns nonzero and fills sec/value/size when a
   DEFINED symbol with that name exists (value is its section offset). */
extern int MIR_object_find_symbol (MIR_object_t obj, const char *name, int *sec, uint64_t *value,
                                   uint64_t *size);

/* Register a defined text function as a module initializer: appends one
   8-byte slot to .init_array, covered by an ABS64 relocation against the
   function's symbol (locals qualify -- per-TU initializers are typically
   static).  The array reaches every consumer: the .o carries the section +
   .rela.init_array (external linkers collect it natively), merges
   concatenate it, executables/shared objects emit DT_INIT_ARRAY/
   DT_INIT_ARRAYSZ over it, and the in-process loader exposes it through
   MIR_object_loaded_init_array.  Entries are called (int argc, char **argv,
   char **envp), the platform init-array contract.  Returns 0, or -1 when
   the name is not a defined .text symbol. */
extern int MIR_object_add_init (MIR_object_t obj, const char *name);

/* Attach a debug builder (AOT R5): MIR_object_emit and
   MIR_object_emit_executable will generate .debug_abbrev/.debug_info/
   .debug_line (and .debug_frame on x86-64) from it into the artifact.
   Function addresses in the builder must be .text SECTION OFFSETS (cast to
   the addr pointer; offset 0 is valid), typically taken from
   MIR_object_find_symbol.  The .o carries R_X86_64_64 relocations against
   the .text section symbol for every code address, so an external linker
   relocates the DWARF correctly; executables and shared objects get the
   final link-time vaddrs baked in (gdb rebases ET_DYN itself).  The builder
   is borrowed -- it must stay alive across the emit calls -- and may not be
   the target of MIR_debug_emit afterwards (address interpretation differs). */
extern void MIR_object_set_debug (MIR_object_t obj, MIR_debug_t d);

/* Assemble the relocatable object.  On success returns 0 and sets *buf
   (malloc'd, caller-owned; write it out and free it) and *size.  Returns
   nonzero on failure / unsupported host. */
extern int MIR_object_emit (MIR_object_t obj, void **buf, size_t *size);

/* Direct executable emission (no external toolchain): assemble the captured
   object as a complete ET_EXEC dynamic executable.  MIR synthesizes _start
   (SysV stack -> __libc_start_main (entry, argc, argv)) -- no crt1.o -- and
   the whole dynamic-linking apparatus (PT_INTERP, .dynamic with the given
   DT_NEEDED list, .dynsym/.dynstr/.hash, .rela.dyn).  Internal references
   are resolved at emit (fixed load base); imported symbols become R_X86_64_64
   slot relocations the dynamic loader fills eagerly at load -- MIR's call
   model already routes imports through address slots, so no PLT/GOT is
   built (.mir.addrpool in the R+W segment IS the GOT; .text is clean of
   relocations, so no DT_TEXTREL -- the tag would only reappear, loudly, if
   a future capture put a dynamic slot back in text).  x86-64 Linux only.

   With shared_p set the same emitter produces an ET_DYN shared object
   (dlopen/DT_NEEDED-consumable): load base 0, no PT_INTERP/_start/entry;
   defined global symbols are exported through .dynsym/.hash; internal
   references cannot resolve at emit (unknown load bias) so EVERY relocation
   lands in .rela.dyn -- R_X86_64_RELATIVE for internal targets (-Bsymbolic
   semantics: internal references never interpose), R_X86_64_64 for imports.

   With pie_p set (executables only; shared_p wins if both) the executable
   becomes a position-independent ET_DYN: shared-object base-0 layout and
   RELATIVE treatment of internal address slots, but keeping PT_INTERP,
   _start, e_entry, and the import-only .dynsym of an executable (defined
   globals are NOT exported), plus DT_FLAGS_1 = DF_1_PIE so tooling
   classifies it as a PIE.  The stub's entry reference is rip-relative, so
   the loader may place the image at any bias (ASLR).

   Every image is Full RELRO and NX, unconditionally: .mir.addrpool (the
   GOT) and .dynamic lead the R+W segment under a page-padded PT_GNU_RELRO
   (the loader mprotects them read-only after relocation), DT_FLAGS /
   DT_FLAGS_1 carry BIND_NOW / NOW (a statement of fact -- no lazy binding
   exists to disable), and PT_GNU_STACK marks the stack non-executable.

   Module initializers ride the builder's .init_array (MIR_object_add_init,
   or merged in from input objects): it lands inside the RELRO lead region
   -- relocated, then protected, then read -- and a non-empty array emits
   DT_INIT_ARRAY / DT_INIT_ARRAYSZ.  Shared objects get their entries run
   by ld.so at load.  Executables rely on glibc >= 2.34: the _start stub
   passes init = NULL to __libc_start_main, which then walks the main map's
   own dynamic segment (csu/libc-start.c call_init) -- older glibc would
   silently skip the array.  There is no DT_INIT: the array is the one
   init model. */
typedef struct MIR_object_exec_params {
  const char *interp;        /* PT_INTERP path; NULL = /lib64/ld-linux-x86-64.so.2 (executables only) */
  const char *const *needed; /* DT_NEEDED sonames, emitted in order */
  size_t n_needed;
  const char *entry;   /* defined text symbol __libc_start_main receives; NULL = "main"
                          (executables only) */
  const char *runpath; /* DT_RUNPATH library search path; NULL = omit */
  int shared_p;        /* nonzero: emit an ET_DYN shared object instead of ET_EXEC */
  int pie_p;           /* nonzero: emit the executable as a position-independent
                          ET_DYN (PIE) instead of fixed-base ET_EXEC; ignored when
                          shared_p is set */
  const char *identifier; /* Apple targets: the code-signature identifier
                             (conventionally the output basename); NULL =
                             "mir.image".  Ignored for ELF targets.  Callers
                             must zero-initialize this struct so newly added
                             tail fields default off. */
  /* Extra read-only carrier section (Apple targets): when extra_data is
     non-NULL and extra_size nonzero, the writer lays one additional
     LC_SEGMENT_64 named extra_segname holding a single section
     extra_sectname with exactly these bytes between the data segments and
     __LINKEDIT -- covered by the emit-time code signature, so the caller
     never rewrites a signed file (the `-sectcreate' shape).  Both names
     are required, at most 16 bytes each (Mach-O name fields).  ELF targets
     ignore these fields: the ELF carrier for trailing payloads is a blob
     appended after the write (ELF tolerates trailing bytes; Mach-O does
     not once signed). */
  const char *extra_segname;  /* e.g. "__MADC"; NULL = no extra section */
  const char *extra_sectname; /* e.g. "__forest" */
  const void *extra_data;
  size_t extra_size;
} MIR_object_exec_params;

/* Apple targets (MIR_TARGET_APPLE_P builds): MIR_object_emit_executable
   assembles a Mach-O64 MH_EXECUTE image instead of ELF -- PIE, linked
   against /usr/lib/libSystem.B.dylib (params->needed become additional
   LC_LOAD_DYLIBs; every import still binds against libSystem), entered
   through LC_MAIN (no _start stub -- dyld's libdyld glue calls the entry
   symbol with argc/argv and exits with its return), internal address
   slots baked + rebase opcodes, imports as `_'-prefixed dyld bind
   opcodes, and a linker-signed ad-hoc code signature (SHA-256 page
   hashes, no certificate -- mandatory on arm64).  params->interp and
   runpath are ignored; shared_p is refused (no dylib emission). */
extern int MIR_object_emit_executable (MIR_object_t obj, const MIR_object_exec_params *params,
                                       void **buf, size_t *size);

/* --- In-process loader for MIR-emitted relocatable objects ---------------
   The .o-as-precompiled-cache lane: load an ET_REL object previously
   produced by MIR_object_emit back into THIS process and prepare it for
   execution -- .text/.data/.mir.addrpool copied into a fresh anonymous
   mapping (.bss zero-filled), the emitter's relocation subset applied
   (R_X86_64_64 slots + R_X86_64_PC32 text->pool references),
   .text then remapped read+execute.  Undefined symbols resolve through the
   emitter's own dotted mir.* builtin exports first (they are this library's
   AOT contract), then through the caller's resolver (return NULL for "not
   found"; the loader fails after consulting it for EVERY undefined symbol,
   so a logging resolver sees the complete miss list).  Not a general ELF
   loader: objects from other compilers are out of scope.  On failure
   returns NULL and, when err_msg != NULL, writes a diagnostic there. */
typedef struct MIR_object_loaded *MIR_object_loaded_t;
typedef void *(*MIR_object_resolver_t) (const char *name, void *env);
/* --- Merge reader (multi-object linking) ---------------------------------
   Parse one MIR-emitted ET_REL image and APPEND it into a builder: sections
   concatenate at their alignments (.init_array included -- every input
   TU's initializers ride the merged array, in input order), symbols unify
   by name (an UNDEF resolves against a definition from either side; a
   strong definition replaces a weak one; two strong definitions are a loud
   error; locals never unify), relocations are rebased.  .debug_* sections concatenate too (multi-CU
   output): their relocations -- code-address slots against .text, and the
   cross-debug-section offsets (CU abbrev offset, stmt_list, FDE CIE
   pointers) against the debug sections' own symbols -- rebase with the
   same rules and resolve at the final emit.  An input whose debug info
   predates those relocations (bare offsets) is refused when appended at a
   nonzero debug base: re-emit the cache.  Repeated reads over one builder
   ARE the link; the merged builder feeds the single-object consumers
   unchanged (MIR_object_emit = the ld -r shape -- still externally
   linkable and re-mergeable, MIR_object_emit_executable, emit +
   MIR_object_load for an in-process run).  Returns 0 on success, -1 on
   error with err_msg filled.  (The historical 1 = merged-but-debug-dropped
   result is gone -- nothing is dropped anymore.) */
extern int MIR_object_read (MIR_object_t obj, const void *buf, size_t size, char *err_msg,
                            size_t err_len);

/* Iterate the names of the builder's UNDEFINED (imported) symbols:
   idx 0, 1, ... until NULL. */
extern const char *MIR_object_undef_name (MIR_object_t obj, size_t idx);

extern MIR_object_loaded_t MIR_object_load (const void *buf, size_t size,
                                            MIR_object_resolver_t resolver, void *env,
                                            char *err_msg, size_t err_len);
/* Address of a defined global (or weak) symbol in the loaded image, or NULL. */
extern void *MIR_object_loaded_sym (MIR_object_loaded_t lo, const char *name);
/* The loaded image's init array: relocated function pointers, in object
   order.  Returns the first entry's address (NULL when the image has none)
   and sets *n to the entry count.  The caller runs them -- each as
   (int argc, char **argv, char **envp) -- before entering the image's
   code; the loader itself never calls them. */
extern void *const *MIR_object_loaded_init_array (MIR_object_loaded_t lo, size_t *n);
/* Unmap the image and free the handle.  Must not be called while code from
   the image can still run -- including atexit handlers it registered. */
extern void MIR_object_loaded_unload (MIR_object_loaded_t lo);

#ifdef __cplusplus
}
#endif

#endif /* MIR_DEBUG_H */
