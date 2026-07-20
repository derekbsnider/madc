/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_GEN_H

#define MIR_GEN_H

#include "mir.h"

#ifndef MIR_NO_GEN_DEBUG
#define MIR_NO_GEN_DEBUG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern void MIR_gen_init (MIR_context_t ctx);
extern void MIR_gen_set_debug_file (MIR_context_t ctx, FILE *f);
extern void MIR_gen_set_debug_level (MIR_context_t ctx, int debug_level);
extern void MIR_gen_set_optimize_level (MIR_context_t ctx, unsigned int level);
extern void *MIR_gen (MIR_context_t ctx, MIR_item_t func_item);
extern void MIR_gen_get_code (MIR_context_t ctx, MIR_item_t func_item,
                              const uint8_t **code_ptr, size_t *code_size);
/* AOT object mode: with the mode on, MIR_gen captures machine code and
   relocations instead of publishing executable code (x86-64 ELF only).
   After load + link (a resolver may map unresolvable imports to any non-NULL
   sentinel; nothing of it is consumed) and generating every function,
   MIR_gen_object_emit assembles the relocatable object: 0 on success, *buf
   malloc'd and caller-owned.  See mir-debug.h (MIR_object_*) for the
   builder these feed. */
extern void MIR_gen_set_object_mode (MIR_context_t ctx, int on_p);
extern int MIR_gen_object_emit (MIR_context_t ctx, void **buf, size_t *size);
extern void MIR_set_gen_interface (MIR_context_t ctx, MIR_item_t func_item);
extern void MIR_set_lazy_gen_interface (MIR_context_t ctx, MIR_item_t func_item);
extern void MIR_set_lazy_bb_gen_interface (MIR_context_t ctx, MIR_item_t func_item);
extern void MIR_gen_finish (MIR_context_t ctx);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef MIR_GEN_H */
