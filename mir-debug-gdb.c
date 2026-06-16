/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* GDB JIT interface for the mir-debug emitter -- the process-global
   __jit_debug_descriptor and the register/unregister calls that publish a
   debug object built by MIR_debug_emit (see mir-debug.h).

   This lives in its own translation unit, separate from the DWARF builder in
   mir-debug.c, on purpose: the descriptor is a single process-global symbol
   that gdb looks up by name, so an embedder that already owns its own
   __jit_debug_descriptor can link the builder + MIR_debug_emit without pulling
   in -- and clashing with -- a second descriptor.  Only consumers that call
   MIR_debug_gdb_register pull this object in.

   Registrations may be bound to a MIR_context_t: MIR_finish then unregisters
   them automatically (the JIT code they describe is freed there, so the debug
   info becomes stale).  The binding is installed through a hook on the context
   (_MIR_set_gdb_jit_finish), so mir.c itself never references this file. */

#include "mir-debug.h"
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

typedef enum { JIT_NOACTION = 0, JIT_REGISTER_FN, JIT_UNREGISTER_FN } jit_actions_t;
struct jit_code_entry {
  struct jit_code_entry *next_entry, *prev_entry;
  const char *symfile_addr;
  uint64_t symfile_size;
};
struct jit_descriptor {
  uint32_t version, action_flag;
  struct jit_code_entry *relevant_entry, *first_entry;
};
/* gdb sets a breakpoint here; must not be inlined or elided.  The empty asm is
   an optimization barrier for that; MIR's own c2m (which builds this file in its
   self-bootstrap) neither supports inline asm nor performs the elision, so it is
   skipped there. */
void __attribute__ ((noinline)) __jit_debug_register_code (void) {
#ifndef __mirc__
  __asm__ __volatile__ ("");
#endif
}
struct jit_descriptor __jit_debug_descriptor = {1, 0, NULL, NULL};

/* One registration.  The jit_code_entry must be first so a MIR_debug_jit_t can
   be used directly as the descriptor's list node.  ctx (if non-NULL) ties the
   entry to a context for MIR_finish-time cleanup; reg_* threads all live
   entries for the context sweep. */
struct MIR_debug_jit_entry {
  struct jit_code_entry e;
  MIR_context_t ctx;
  struct MIR_debug_jit_entry *reg_next, *reg_prev;
};

static pthread_mutex_t mir_debug_jit_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct MIR_debug_jit_entry *mir_debug_all_regs; /* every live entry, mutex-protected */

static void mir_debug_gdb_ctx_finish (MIR_context_t ctx);

/* Link e into the gdb descriptor and tell gdb (mutex held). */
static void descr_register (struct MIR_debug_jit_entry *r) {
  r->e.prev_entry = NULL;
  r->e.next_entry = __jit_debug_descriptor.first_entry;
  if (__jit_debug_descriptor.first_entry != NULL) __jit_debug_descriptor.first_entry->prev_entry = &r->e;
  __jit_debug_descriptor.first_entry = &r->e;
  __jit_debug_descriptor.relevant_entry = &r->e;
  __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
  __jit_debug_register_code ();
}

/* Unlink e from the gdb descriptor and tell gdb (mutex held). */
static void descr_unregister (struct MIR_debug_jit_entry *r) {
  if (r->e.prev_entry != NULL)
    r->e.prev_entry->next_entry = r->e.next_entry;
  else
    __jit_debug_descriptor.first_entry = r->e.next_entry;
  if (r->e.next_entry != NULL) r->e.next_entry->prev_entry = r->e.prev_entry;
  __jit_debug_descriptor.relevant_entry = &r->e;
  __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
  __jit_debug_register_code ();
}

/* Remove r from the global registry list (mutex held). */
static void reg_unlink (struct MIR_debug_jit_entry *r) {
  if (r->reg_prev != NULL)
    r->reg_prev->reg_next = r->reg_next;
  else
    mir_debug_all_regs = r->reg_next;
  if (r->reg_next != NULL) r->reg_next->reg_prev = r->reg_prev;
}

MIR_debug_jit_t MIR_debug_gdb_register (MIR_context_t ctx, void *buf, size_t size) {
  if (buf == NULL) return NULL;
  struct MIR_debug_jit_entry *r = calloc (1, sizeof (struct MIR_debug_jit_entry));
  if (r == NULL) return NULL;
  r->e.symfile_addr = buf;
  r->e.symfile_size = (uint64_t) size;
  r->ctx = ctx;
  pthread_mutex_lock (&mir_debug_jit_mutex);
  descr_register (r);
  r->reg_prev = NULL;
  r->reg_next = mir_debug_all_regs;
  if (mir_debug_all_regs != NULL) mir_debug_all_regs->reg_prev = r;
  mir_debug_all_regs = r;
  pthread_mutex_unlock (&mir_debug_jit_mutex);
  /* Bind to the context so MIR_finish drops it (idempotent install). */
  if (ctx != NULL) _MIR_set_gdb_jit_finish (ctx, mir_debug_gdb_ctx_finish);
  return (MIR_debug_jit_t) r;
}

void MIR_debug_gdb_unregister (MIR_debug_jit_t entry) {
  struct MIR_debug_jit_entry *r = (struct MIR_debug_jit_entry *) entry;
  if (r == NULL) return;
  pthread_mutex_lock (&mir_debug_jit_mutex);
  descr_unregister (r);
  reg_unlink (r);
  pthread_mutex_unlock (&mir_debug_jit_mutex);
  free ((void *) r->e.symfile_addr);
  free (r);
}

/* Installed on a context by MIR_debug_gdb_register; MIR_finish calls it to
   unregister every debug object bound to that context. */
static void mir_debug_gdb_ctx_finish (MIR_context_t ctx) {
  pthread_mutex_lock (&mir_debug_jit_mutex);
  struct MIR_debug_jit_entry *r = mir_debug_all_regs, *next;
  for (; r != NULL; r = next) {
    next = r->reg_next;
    if (r->ctx != ctx) continue;
    descr_unregister (r);
    reg_unlink (r);
    free ((void *) r->e.symfile_addr);
    free (r);
  }
  pthread_mutex_unlock (&mir_debug_jit_mutex);
}
