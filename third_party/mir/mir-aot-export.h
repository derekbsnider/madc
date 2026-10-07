/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* AOT object mode: generated code calls some builtins through dotted import
   items ("mir.va_arg", "mir.va_block_arg", "mir.arg_memcpy"), which become
   undefined symbols in an emitted object; libmir exports them under those
   names so a program using them links and loads.  MIR_AOT_SYM gives the C
   symbol for an asm label: the platform's C label prefix -- none on ELF and
   PE, '_' on Mach-O (__USER_LABEL_PREFIX__) -- then the name.  Mach-O has no
   alias attribute, so an Apple export is a wrapper function under this label. */

#ifndef MIR_AOT_EXPORT_H
#define MIR_AOT_EXPORT_H

#define MIR_AOT_STR2(s) #s
#define MIR_AOT_STR(s) MIR_AOT_STR2 (s)
#define MIR_AOT_SYM(name) MIR_AOT_STR (__USER_LABEL_PREFIX__) name

#endif /* #ifndef MIR_AOT_EXPORT_H */
