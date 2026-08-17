/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Local ELF64 ABI definitions for hosts without <elf.h> (macOS, Windows).

   The object layer in mir-debug.c reads and writes ELF as a byte-level
   container format -- it never depends on the HOST being an ELF platform
   (exactly as mir-macho.c writes Mach-O from Linux with its own local
   MACHO_* definitions).  This header supplies the subset of the SysV gABI
   plus x86-64 / AArch64 psABI definitions that mir-debug.c uses, with the
   canonical values, so the full object layer (ELF .o emit, executable
   assembly, read-back, in-process load) is available on every host.
   Included by mir-debug.c only when <elf.h> is absent -- an ELF host's
   system header always wins.  Little-endian 64-bit classes only, matching
   the object layer's supported targets. */

#ifndef MIR_ELF_DEFS_H
#define MIR_ELF_DEFS_H

#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint16_t Elf64_Section;

#define EI_NIDENT 16

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff;
  Elf64_Off e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum;
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  Elf64_Word sh_name;
  Elf64_Word sh_type;
  Elf64_Xword sh_flags;
  Elf64_Addr sh_addr;
  Elf64_Off sh_offset;
  Elf64_Xword sh_size;
  Elf64_Word sh_link;
  Elf64_Word sh_info;
  Elf64_Xword sh_addralign;
  Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
  Elf64_Word st_name;
  unsigned char st_info;
  unsigned char st_other;
  Elf64_Section st_shndx;
  Elf64_Addr st_value;
  Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset;
  Elf64_Addr p_vaddr;
  Elf64_Addr p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr d_ptr;
  } d_un;
} Elf64_Dyn;

/* e_ident indexes and values */
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ELFOSABI_SYSV 0

/* e_type */
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3

/* e_machine */
#define EM_PPC64 21
#define EM_S390 22
#define EM_X86_64 62
#define EM_AARCH64 183
#define EM_RISCV 243

/* special section indexes */
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

/* sh_type */
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOBITS 8
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14

/* sh_flags */
#define SHF_WRITE (1 << 0)
#define SHF_ALLOC (1 << 1)
#define SHF_EXECINSTR (1 << 2)
#define SHF_INFO_LINK (1 << 6)

/* symbol binding / type / visibility */
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STV_DEFAULT 0
#define ELF64_ST_BIND(val) (((unsigned char) (val)) >> 4)
#define ELF64_ST_TYPE(val) ((val) & 0xf)
#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

/* relocation info */
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define ELF64_R_INFO(sym, type) ((((Elf64_Xword) (sym)) << 32) + (type))

/* p_type / p_flags */
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552
#define PF_X (1 << 0)
#define PF_W (1 << 1)
#define PF_R (1 << 2)

/* d_tag */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_DEBUG 21
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_FLAGS_1 0x6ffffffb
#define DF_BIND_NOW 0x00000008
#define DF_1_NOW 0x00000001
#define DF_1_PIE 0x08000000

/* x86-64 psABI relocations */
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_RELATIVE 8
#define R_X86_64_32 10

/* AArch64 psABI relocations */
#define R_AARCH64_ABS64 257
#define R_AARCH64_ABS32 258
#define R_AARCH64_PREL32 261
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_RELATIVE 1027

#endif /* MIR_ELF_DEFS_H */
