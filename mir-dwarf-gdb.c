/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* GDB JIT interface for the mir-dwarf emitter -- the process-global
   __jit_debug_descriptor and the register/unregister calls that publish a
   debug object built by MIR_dwarf_emit (see mir-dwarf.h).

   This lives in its own translation unit, separate from the DWARF builder in
   mir-dwarf.c, on purpose: the descriptor is a single process-global symbol
   that gdb looks up by name, so an embedder that already owns its own
   __jit_debug_descriptor (e.g. a Tcl extension driving its own JIT lifetime)
   can link the builder + MIR_dwarf_emit without pulling in -- and clashing
   with -- a second descriptor.  Only consumers that call MIR_dwarf_gdb_register
   pull this object in. */

#include "mir-dwarf.h"
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

static pthread_mutex_t mir_dwarf_jit_mutex = PTHREAD_MUTEX_INITIALIZER;

MIR_dwarf_jit_t MIR_dwarf_gdb_register (void *buf, size_t size) {
  if (buf == NULL) return NULL;
  struct jit_code_entry *e = calloc (1, sizeof (struct jit_code_entry));
  if (e == NULL) return NULL;
  e->symfile_addr = buf;
  e->symfile_size = (uint64_t) size;
  pthread_mutex_lock (&mir_dwarf_jit_mutex);
  e->prev_entry = NULL;
  e->next_entry = __jit_debug_descriptor.first_entry;
  if (__jit_debug_descriptor.first_entry != NULL) __jit_debug_descriptor.first_entry->prev_entry = e;
  __jit_debug_descriptor.first_entry = e;
  __jit_debug_descriptor.relevant_entry = e;
  __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
  __jit_debug_register_code ();
  pthread_mutex_unlock (&mir_dwarf_jit_mutex);
  return (MIR_dwarf_jit_t) e;
}

void MIR_dwarf_gdb_unregister (MIR_dwarf_jit_t entry) {
  struct jit_code_entry *e = (struct jit_code_entry *) entry;
  if (e == NULL) return;
  pthread_mutex_lock (&mir_dwarf_jit_mutex);
  if (e->prev_entry != NULL)
    e->prev_entry->next_entry = e->next_entry;
  else
    __jit_debug_descriptor.first_entry = e->next_entry;
  if (e->next_entry != NULL) e->next_entry->prev_entry = e->prev_entry;
  __jit_debug_descriptor.relevant_entry = e;
  __jit_debug_descriptor.action_flag = JIT_UNREGISTER_FN;
  __jit_debug_register_code ();
  pthread_mutex_unlock (&mir_dwarf_jit_mutex);
  free ((void *) e->symfile_addr);
  free (e);
}
