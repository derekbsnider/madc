/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef C2MIR_H

#define C2MIR_H

#include "mir.h"

#define COMMAND_LINE_SOURCE_NAME "<command-line>"
#define STDIN_SOURCE_NAME "<stdin>"

struct c2mir_macro_command {
  int def_p;              /* #define or #undef */
  const char *name, *def; /* def is used only when def_p is true */
};

struct c2mir_options {
  FILE *message_file;
  int debug_p, verbose_p, ignore_warnings_p, no_prepro_p, prepro_only_p;
  int syntax_only_p, pedantic_p, asm_p, object_p;
  int debug_info_p; /* stamp source locations for gdb debug info (see mir-debug.h) */
  size_t module_num;
  FILE *prepro_output_file; /* non-null for prepro_only_p */
  const char *output_file_name;
  size_t macro_commands_num, include_dirs_num;
  struct c2mir_macro_command *macro_commands;
  const char **include_dirs;
  int external_tree_p;  /* non-zero: caller owns AST nodes, c2mir won't free them */
};

void c2mir_init (MIR_context_t ctx);
void c2mir_finish (MIR_context_t ctx);
int c2mir_compile (MIR_context_t ctx, struct c2mir_options *ops, int (*getc_func) (void *),
                   void *getc_data, const char *source_name, FILE *output_file);

/* For a debug_info_p compile: the source files referenced by the stamped line
   numbers, in file-id order (index i has file id i+1).  Returns the count and
   sets *names to the (compiler-owned) array.  Valid until c2mir_finish.  Used
   by the driver to build the DWARF file table.  Single-threaded compiles only. */
size_t c2mir_get_source_files (MIR_context_t ctx, const char ***names);

/* For a debug_info_p compile, after MIR_link/gen: build the in-memory GDB-JIT
   DWARF object (function symbols, .debug_line, and rich-typed variable DIEs for
   the snapshotted locals/params) covering all generated functions.  On success
   returns 0 and sets *buf (malloc'd; hand to MIR_debug_gdb_register, which takes
   ownership) and *size; returns nonzero if there is nothing to emit or the host
   is unsupported.  Single-threaded compiles only. */
int c2mir_get_debug_object (MIR_context_t ctx, void **buf, size_t *size);

#endif
