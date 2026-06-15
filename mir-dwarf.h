/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* mir-dwarf: a small, frontend-agnostic emitter that turns information a MIR
   frontend already has -- generated function addresses/sizes, the per-function
   source line map (MIR_func.line_map), and a description of local variables and
   their C types -- into an in-memory ELF object carrying a symbol table and
   DWARF (.debug_line + .debug_info), and registers it with an attached debugger
   through the GDB JIT interface.  This gives source-level debugging (named
   frames, break file:line, step/next, print/info locals) of JIT'd code in gdb
   on ELF platforms, without the frontend writing any ELF/DWARF bytes itself.

   See c2mir for a reference consumer.  Typical use:

     MIR_dwarf_t d = MIR_dwarf_init ();
     uint32_t f = MIR_dwarf_add_file (d, "/path/to/source.c");   // file_id you
                                                                 // stamp on insns
     MIR_dwarf_type_t ti = MIR_dwarf_base_type (d, "int", MIR_DWARF_ENC_SIGNED, 4);
     for each generated function fn:
       MIR_dwarf_add_func (d, fn->name, fn->machine_code, fn->code_len,
                           fn->line_map, fn->line_map_len);
       for each local var v with reg r and type t:
         int64_t off; MIR_reg_frame_offset (fn, r, &off);
         MIR_dwarf_add_var (d, v->name, v->is_param, t, off, deref_p, 0);
     void *buf; size_t size;
     MIR_dwarf_emit (d, &buf, &size);
     MIR_dwarf_jit_t h = MIR_dwarf_gdb_register (buf, size);  // takes buf
     MIR_dwarf_destroy (d);
     ... run the code ...
     MIR_dwarf_gdb_unregister (h);  // before freeing the code

   The frontend should generate debuggable code: optimize level 0,
   MIR_set_inline_permission(ctx,0), and -- for variable inspection --
   MIR_set_spill_all(ctx,1) so every local has a stable frame slot.  */

#ifndef MIR_DWARF_H
#define MIR_DWARF_H

#include <stddef.h>
#include <stdint.h>
#include "mir.h" /* for MIR_line_map_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Base-type encodings (a subset of DWARF DW_ATE_*). */
typedef enum {
  MIR_DWARF_ENC_SIGNED = 1,
  MIR_DWARF_ENC_UNSIGNED,
  MIR_DWARF_ENC_FLOAT,
  MIR_DWARF_ENC_BOOL,
  MIR_DWARF_ENC_SIGNED_CHAR,
  MIR_DWARF_ENC_UNSIGNED_CHAR,
} MIR_dwarf_encoding_t;

typedef struct MIR_dwarf *MIR_dwarf_t;
typedef uint32_t MIR_dwarf_type_t; /* opaque type handle; 0 == void / no type */

/* Create a builder for the host architecture, or NULL if the host is not a
   supported little-endian ELF target. */
extern MIR_dwarf_t MIR_dwarf_init (void);
extern void MIR_dwarf_destroy (MIR_dwarf_t d);

/* Register a source file path; returns a 1-based id to use as the file_id you
   stamp on insns via MIR_set_source_loc (0 means "no file"). */
extern uint32_t MIR_dwarf_add_file (MIR_dwarf_t d, const char *path);

/* --- Type graph ---------------------------------------------------------
   The caller drives construction (walking its own type representation and
   deduplicating by its own type identity); the builder records handles and
   resolves cross references at emit time, so forward/recursive references are
   fine.  For a recursive aggregate, reserve its handle first (the *_type call
   returns it) and add members afterwards -- a member can reference the
   aggregate's own handle.  */
extern MIR_dwarf_type_t MIR_dwarf_base_type (MIR_dwarf_t d, const char *name,
                                             MIR_dwarf_encoding_t enc, int byte_size);
extern MIR_dwarf_type_t MIR_dwarf_pointer_type (MIR_dwarf_t d, MIR_dwarf_type_t pointee);
extern MIR_dwarf_type_t MIR_dwarf_array_type (MIR_dwarf_t d, MIR_dwarf_type_t elem,
                                              int64_t count /* <0 == unknown */);
extern MIR_dwarf_type_t MIR_dwarf_typedef_type (MIR_dwarf_t d, const char *name,
                                                MIR_dwarf_type_t ref);
extern MIR_dwarf_type_t MIR_dwarf_struct_type (MIR_dwarf_t d, const char *name /* NULL == anon */,
                                               int64_t byte_size, int is_union);
extern void MIR_dwarf_add_member (MIR_dwarf_t d, MIR_dwarf_type_t agg, const char *name,
                                  MIR_dwarf_type_t type, int64_t byte_offset);
extern void MIR_dwarf_add_bitfield (MIR_dwarf_t d, MIR_dwarf_type_t agg, const char *name,
                                    MIR_dwarf_type_t type, int64_t bit_offset, int bit_size);
extern MIR_dwarf_type_t MIR_dwarf_enum_type (MIR_dwarf_t d, const char *name, int64_t byte_size);
extern void MIR_dwarf_add_enumerator (MIR_dwarf_t d, MIR_dwarf_type_t en, const char *name,
                                      int64_t value);
extern MIR_dwarf_type_t MIR_dwarf_func_type (MIR_dwarf_t d, MIR_dwarf_type_t ret_type);
extern void MIR_dwarf_add_param_type (MIR_dwarf_t d, MIR_dwarf_type_t fn, MIR_dwarf_type_t type);

/* --- Functions and their variables --------------------------------------
   addr/size/line_map come straight from a generated MIR_func (machine_code,
   code_len, line_map / line_map_len).  Variables are attached to the most
   recently added function.  */
extern void MIR_dwarf_add_func (MIR_dwarf_t d, const char *name, const void *addr, size_t size,
                                const MIR_line_map_t *line_map, size_t line_map_len);
/* Location of the variable, built as the DWARF expression
     DW_OP_fbreg(fp_offset) [DW_OP_deref] [DW_OP_plus_uconst(member_offset)].
   fp_offset: the frame-pointer-relative slot, from MIR_reg_frame_offset.
   deref_p: nonzero if that slot holds an *address* (an ALLOCA-style frontend)
   rather than the value itself.  member_offset: a constant added after the
   optional deref -- e.g. a frontend that homes all locals in one frame block
   and indexes each by a byte offset passes the block-pointer slot with
   deref_p=1 and the per-variable offset here (0 when unused). */
extern void MIR_dwarf_add_var (MIR_dwarf_t d, const char *name, int is_param,
                               MIR_dwarf_type_t type, int64_t fp_offset, int deref_p,
                               uint64_t member_offset);

/* Produce the in-memory ELF object.  On success returns 0 and sets *buf
   (malloc'd; free with free(), or hand to MIR_dwarf_gdb_register which takes
   ownership) and *size.  Returns nonzero on failure / unsupported host. */
extern int MIR_dwarf_emit (MIR_dwarf_t d, void **buf, size_t *size);

/* --- GDB JIT interface --------------------------------------------------
   The emitter owns the process-global __jit_debug_descriptor.  Register the
   object so an attached gdb reads it; the returned handle keeps the buffer
   alive (ownership transfers) until unregistered.  Unregister before the code
   the object describes is freed. */
typedef struct MIR_dwarf_jit_entry *MIR_dwarf_jit_t;
extern MIR_dwarf_jit_t MIR_dwarf_gdb_register (void *buf, size_t size);
extern void MIR_dwarf_gdb_unregister (MIR_dwarf_jit_t entry);

#ifdef __cplusplus
}
#endif

#endif /* MIR_DWARF_H */
