/* This file was added to the MIR fork as part of the MadC project.
   Copyright (C) 2019-2026 Derek Snider <derekbsnider@gmail.com>.
   Same license as the MIR project (see LICENSE).

   PE/COFF writers (madc fork, Windows release lane W3).

   #included by mir-debug.c when MIR_TARGET_WINDOWS_P: the alternative
   assembler behind the MIR_object seam, exactly as mir-macho.c is for
   Apple targets.  Everything upstream of it -- capture, sections,
   symbols, relocations -- is the ELF writer's own builder state; only
   the container format differs.  A cross build has no <windows.h>, so
   every constant is defined here (the mir-macho.c rule).

   Mapping from the ELF relocatable emitter (MIR_object_emit):

     .text          -> .text          (CODE, R+X, align 16)
     .mir.addrpool  -> .mir.addrpool  (DATA, R+W -- the addrpool-as-GOT
                       model; long name goes through the string table)
     .init_array    -> .CRT$XCU       (8-byte fn-pointer slots; a normal
                       mingw+UCRT link runs them before main -- W3.0(a2)
                       probe.  NOTE the CRT calls them with NO arguments
                       where the ELF loader passes argc/argv/envp; MIR
                       initializers take no parameters, so both calls
                       agree)
     .data          -> .data          (DATA, R+W)
     .bss           -> .bss           (UNINITIALIZED, R+W, no file bytes)

   Relocation model (every choice validated against a mingw-gcc 13 .o --
   the W3.0 oracle, container tmp/win/w3/oracle.o):

     COFF relocations carry no addend field: the addend lives in the
     relocated field's own bytes.
     ABS64 -> IMAGE_REL_AMD64_ADDR64 with the ELF addend written as the
              8-byte slot content.
     PC32  -> IMAGE_REL_AMD64_REL32 with addend + 4 in the 4-byte field:
              REL32 resolves S + A' - (P + 4) where ELF resolves
              S + A - P, so the two encodings differ by exactly the 4
              the ELF addend already folded in.  (A trailing imm32's
              extra -4 is already inside the ELF addend, so the one +4
              rule covers every field position.)
     The aarch64 kinds cannot occur: MIR_TARGET_WINDOWS validates as
     x86-64 only (mir-target.h).

   Weak symbols (MIR_item_set_binding linkonce -- template-instantiation
   merging) use the GNU shape from the weak.o oracle: the definition is
   emitted under a companion name (".weak.<name>.mir") and the visible
   symbol becomes IMAGE_SYM_CLASS_WEAK_EXTERNAL (undefined, value 0)
   whose aux record tags the companion with characteristics
   IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY.  The companion is STATIC where
   gas emits it EXTERNAL: a global companion would collide when two
   madc objects defining the same weak symbol meet in one external
   link, and the W3.1 reducer battery verifies ld resolves the static
   shape.  Relocations against the weak symbol point at the
   WEAK_EXTERNAL entry, preserving the ELF semantics (the linker's
   winning definition is the target).

   No .pdata/.xdata (SEH RUNTIME_FUNCTION unwind info) is emitted: this
   matches the JIT posture -- JIT frames carry no RUNTIME_FUNCTION
   entries either, and NON_SEH setjmp is the lane law precisely because
   nothing unwinds through MIR frames (W2.1).

   A debug builder is refused rather than dropped, exactly as the
   Mach-O .o writer refuses: silently discarding debug info the caller
   asked for is quiet degradation.  (DWARF-in-COFF is a later slice --
   mingw tooling consumes it fine.)

   Validation oracle (all on the build container): x86_64-w64-mingw32-gcc
   13 reference objects, x86_64-w64-mingw32-objdump, links by mingw ld,
   runs under wine AND real Windows via scripts/win_run.sh. */

/* ===== COFF constants (no <windows.h> on a cross host) ================== */

#define PE_FILE_MACHINE_AMD64 0x8664u

/* section characteristics */
#define PE_SCN_CNT_CODE 0x00000020u
#define PE_SCN_CNT_INIT_DATA 0x00000040u
#define PE_SCN_CNT_UNINIT_DATA 0x00000080u
#define PE_SCN_LNK_NRELOC_OVFL 0x01000000u
#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ 0x40000000u
#define PE_SCN_MEM_WRITE 0x80000000u

/* IMAGE_SCN_ALIGN_*: (log2(align) + 1) << 20, 1 .. 8192 bytes */
static uint32_t pe_align_flag (uint64_t align) {
  unsigned l2 = 0;
  if (align == 0) align = 1;
  if (align > 8192) align = 8192; /* the COFF flag ceiling */
  while (((uint64_t) 1 << l2) < align) l2++;
  return (uint32_t) (l2 + 1) << 20;
}

/* relocation types */
#define PE_REL_AMD64_ABSOLUTE 0x0000u
#define PE_REL_AMD64_ADDR64 0x0001u
#define PE_REL_AMD64_REL32 0x0004u

/* symbol storage classes / type */
#define PE_SYM_CLASS_EXTERNAL 2u
#define PE_SYM_CLASS_STATIC 3u
#define PE_SYM_CLASS_WEAK_EXTERNAL 105u
#define PE_SYM_TYPE_FUNCTION 0x20u
#define PE_WEAK_SEARCH_NOLIBRARY 1u

/* ===== the .o writer (COFF object) ====================================== */

/* The .o's sections in emission order (stable ordinals, the Mach-O .o
   writer's rule): .text / .mir.addrpool / .data always present, .CRT$XCU
   and .bss only when non-empty. */
enum {
  PE_OSEC_TEXT = 0,
  PE_OSEC_POOL,
  PE_OSEC_INIT,
  PE_OSEC_DATA,
  PE_OSEC_BSS,
  PE_OSEC_N
};

static int pe_osec_of (int sec) {
  return sec == MIR_OBJ_SEC_TEXT       ? PE_OSEC_TEXT
         : sec == MIR_OBJ_SEC_ADDRPOOL ? PE_OSEC_POOL
         : sec == MIR_OBJ_SEC_INITARR  ? PE_OSEC_INIT
         : sec == MIR_OBJ_SEC_DATA     ? PE_OSEC_DATA
         : sec == MIR_OBJ_SEC_BSS      ? PE_OSEC_BSS
                                       : -1;
}

static const char *pe_osec_name (int osec) {
  return osec == PE_OSEC_TEXT   ? ".text"
         : osec == PE_OSEC_POOL ? ".mir.addrpool"
         : osec == PE_OSEC_INIT ? ".CRT$XCU"
         : osec == PE_OSEC_DATA ? ".data"
                                : ".bss";
}

typedef struct {
  const uint8_t *body;
  uint64_t size, align;
  uint32_t chars;    /* section characteristics (align flag included) */
  uint32_t off;      /* PointerToRawData; 0 for .bss / empty */
  uint32_t reloff;   /* PointerToRelocations; 0 when none */
  uint32_t nreloc;   /* real relocation count (pre-overflow-clamp) */
  int ordinal;       /* 1-based SectionNumber; 0 = absent */
  int present_p, nobits_p;
  char namef[8];     /* the header's 8-byte name field (long names get
                        the "/<decimal strtab offset>" spelling) */
} pe_osec_t;

/* A COFF symbol-table record is 18 bytes; aux records occupy the same
   slots, so every index a relocation carries counts them. */
static void pe_sym_name (dwbuf_t *symtab, dwbuf_t *strtab, const char *name) {
  size_t len = strlen (name);
  if (len <= 8) {
    char f[8] = {0};
    memcpy (f, name, len);
    buf_bytes (symtab, f, 8);
  } else {
    buf_u32 (symtab, 0);                                /* zeroes = long name */
    buf_u32 (symtab, (uint32_t) (strtab->len + 4)); /* offset incl. size prefix */
    buf_str (strtab, name);
  }
}

static void pe_sym_record (dwbuf_t *symtab, dwbuf_t *strtab, const char *name, uint32_t value,
                           int sec_number, uint16_t type, uint8_t storage_class, uint8_t n_aux) {
  pe_sym_name (symtab, strtab, name);
  buf_u32 (symtab, value);
  buf_u16 (symtab, (uint16_t) (int16_t) sec_number);
  buf_u16 (symtab, type);
  buf_u8 (symtab, storage_class);
  buf_u8 (symtab, n_aux);
}

/* one relocation record: VirtualAddress, SymbolTableIndex, Type (10 bytes) */
static void pe_reloc (dwbuf_t *b, uint32_t addr, uint32_t symidx, uint16_t type) {
  buf_u32 (b, addr);
  buf_u32 (b, symidx);
  buf_u16 (b, type);
}

static int pe_emit_object (MIR_object_t obj, void **buf, size_t *size) {
  if (obj->debug != NULL || obj->dbg_raw_p) return -1; /* no DWARF-in-COFF yet: say so */

  size_t n = obj->n_syms;
  int rc = -1;
  unsigned char *p = NULL;
  uint32_t *final_idx = NULL;
  dwbuf_t symtab = {0}, strtab = {0}, rel[PE_OSEC_N];
  for (int s = 0; s < PE_OSEC_N; s++) rel[s] = (dwbuf_t) {0};

#define PE_ALIGN(v, a) (((v) + (uint64_t) (a) -1) & ~((uint64_t) (a) -1))
  /* ---- section table */
  pe_osec_t S[PE_OSEC_N];
  memset (S, 0, sizeof S);
  S[PE_OSEC_TEXT] = (pe_osec_t) {obj->text.p, obj->text.len, 16,
                                 PE_SCN_CNT_CODE | PE_SCN_MEM_EXECUTE | PE_SCN_MEM_READ,
                                 0, 0, 0, 0, 1, 0};
  S[PE_OSEC_POOL]
    = (pe_osec_t) {obj->pool.p, obj->pool.len, obj->pool_align > 8 ? obj->pool_align : 8,
                   PE_SCN_CNT_INIT_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE, 0, 0, 0, 0, 1, 0};
  S[PE_OSEC_INIT] = (pe_osec_t) {obj->initarr.p, obj->initarr.len, 8,
                                 PE_SCN_CNT_INIT_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE,
                                 0, 0, 0, 0, obj->initarr.len != 0, 0};
  S[PE_OSEC_DATA]
    = (pe_osec_t) {obj->data.p, obj->data.len, obj->data_align > 1 ? obj->data_align : 1,
                   PE_SCN_CNT_INIT_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE, 0, 0, 0, 0, 1, 0};
  S[PE_OSEC_BSS]
    = (pe_osec_t) {NULL, obj->bss_size, obj->bss_align > 1 ? obj->bss_align : 1,
                   PE_SCN_CNT_UNINIT_DATA | PE_SCN_MEM_READ | PE_SCN_MEM_WRITE,
                   0, 0, 0, 0, obj->bss_size != 0, 1};

  int nsects = 0;
  for (int s = 0; s < PE_OSEC_N; s++) {
    if (!S[s].present_p) continue;
    S[s].chars |= pe_align_flag (S[s].align);
    S[s].ordinal = ++nsects;
  }

  /* ---- symtab: builder order, aux-inclusive indexing.  Section symbols
     carry the section's own name + the conventional length/nreloc aux;
     defined globals are EXTERNAL, locals STATIC, weak globals become the
     WEAK_EXTERNAL pair described in the header comment. */
  final_idx = calloc (n ? n : 1, sizeof (uint32_t));
  if (final_idx == NULL) goto done;
  uint32_t n_records = 0;
  /* section-symbol aux records need each section's final nreloc, which is
     known only after the reloc pass -- record patch positions instead. */
  size_t sec_aux_pos[PE_OSEC_N];
  int sec_aux_osec[PE_OSEC_N], n_sec_aux = 0;
  for (size_t i = 0; i < n; i++) {
    objsym_t *sym = &obj->syms[i];
    if (sym->name == NULL && !sym->section_p) continue; /* anonymous: unreferenceable */
    int osec = sym->defined_p ? pe_osec_of (sym->sec) : -1;
    if (sym->defined_p && (osec < 0 || !S[osec].present_p)) goto done; /* stale sec id */
    if (sym->section_p) {
      final_idx[i] = n_records;
      pe_sym_record (&symtab, &strtab, pe_osec_name (osec), 0, S[osec].ordinal, 0,
                     PE_SYM_CLASS_STATIC, 1);
      sec_aux_pos[n_sec_aux] = symtab.len; /* aux written below, patched later */
      sec_aux_osec[n_sec_aux++] = osec;
      buf_u32 (&symtab, (uint32_t) S[osec].size); /* Length */
      buf_u16 (&symtab, 0);                       /* NumberOfRelocations: patched */
      buf_u16 (&symtab, 0);                       /* NumberOfLinenumbers */
      buf_u32 (&symtab, 0);                       /* CheckSum */
      buf_u16 (&symtab, 0);                       /* Number (COMDAT assoc) */
      buf_u8 (&symtab, 0);                        /* Selection */
      buf_u8 (&symtab, 0);                        /* pad */
      buf_u16 (&symtab, 0);                       /* pad */
      n_records += 2;
      continue;
    }
    uint16_t type = sym->func_p ? PE_SYM_TYPE_FUNCTION : 0;
    if (sym->defined_p && sym->weak_p && !sym->local_p) {
      /* companion STATIC definition + the WEAK_EXTERNAL that tags it */
      size_t cl = strlen (sym->name) + sizeof ".weak..mir";
      char *cn = malloc (cl);
      if (cn == NULL) goto done;
      snprintf (cn, cl, ".weak.%s.mir", sym->name);
      uint32_t companion = n_records;
      pe_sym_record (&symtab, &strtab, cn, (uint32_t) sym->value, S[osec].ordinal, type,
                     PE_SYM_CLASS_STATIC, 0);
      free (cn);
      n_records++;
      final_idx[i] = n_records;
      pe_sym_record (&symtab, &strtab, sym->name, 0, 0, type, PE_SYM_CLASS_WEAK_EXTERNAL, 1);
      buf_u32 (&symtab, companion);              /* TagIndex */
      buf_u32 (&symtab, PE_WEAK_SEARCH_NOLIBRARY);
      buf_u32 (&symtab, 0);                      /* 10 unused bytes */
      buf_u32 (&symtab, 0);
      buf_u16 (&symtab, 0);
      n_records += 2;
      continue;
    }
    final_idx[i] = n_records++;
    if (sym->defined_p)
      pe_sym_record (&symtab, &strtab, sym->name, (uint32_t) sym->value, S[osec].ordinal, type,
                     sym->local_p ? PE_SYM_CLASS_STATIC : PE_SYM_CLASS_EXTERNAL, 0);
    else
      pe_sym_record (&symtab, &strtab, sym->name, 0, 0, type, PE_SYM_CLASS_EXTERNAL, 0);
  }

  /* ---- relocation arrays (ascending by offset within each section is the
     builder's own append order) */
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if (r->sym < 0 || (size_t) r->sym >= n) goto done;
    objsym_t *sym = &obj->syms[r->sym];
    if (sym->name == NULL && !sym->section_p) goto done; /* nothing to point at */
    int osec = pe_osec_of (r->sec);
    if (osec < 0 || !S[osec].present_p || S[osec].nobits_p) goto done;
    if (r->kind == MIR_OBJ_RELOC_ABS64)
      pe_reloc (&rel[osec], (uint32_t) r->offset, final_idx[r->sym], PE_REL_AMD64_ADDR64);
    else if (r->kind == MIR_OBJ_RELOC_PC32)
      pe_reloc (&rel[osec], (uint32_t) r->offset, final_idx[r->sym], PE_REL_AMD64_REL32);
    else
      goto done; /* aarch64 kinds cannot reach a WINDOWS (x86-64-only) build */
    S[osec].nreloc++;
  }

  /* NumberOfRelocations is 16-bit: past 0xffff the spec's overflow shape
     applies -- flag the section LNK_NRELOC_OVFL, clamp the header count to
     0xffff, and PREPEND one ABSOLUTE record whose VirtualAddress is the
     real count including itself (the binutils convention).  Spec-checked;
     no oracle reaches 64k relocations, so this path is belt-and-braces. */
  for (int s = 0; s < PE_OSEC_N; s++) {
    if (S[s].nreloc < 0xffffu) continue;
    dwbuf_t ovfl = {0};
    pe_reloc (&ovfl, S[s].nreloc + 1, 0, PE_REL_AMD64_ABSOLUTE);
    buf_bytes (&ovfl, rel[s].p, rel[s].len);
    free (rel[s].p);
    rel[s] = ovfl;
    S[s].chars |= PE_SCN_LNK_NRELOC_OVFL;
  }

  /* patch the section-symbol aux records now the counts are final */
  for (int a = 0; a < n_sec_aux; a++) {
    uint16_t c = S[sec_aux_osec[a]].nreloc > 0xffffu ? 0xffffu
                                                     : (uint16_t) S[sec_aux_osec[a]].nreloc;
    memcpy (symtab.p + sec_aux_pos[a] + 4, &c, 2);
  }

  /* ---- section-header name fields: long names go through the string
     table, so they must be appended BEFORE the layout freezes strtab.len
     (symbol-name offsets already recorded stay valid -- appends never
     move earlier content) */
  for (int s = 0; s < PE_OSEC_N; s++) {
    if (!S[s].present_p) continue;
    const char *nm = pe_osec_name (s);
    size_t nl = strlen (nm);
    if (nl <= 8) {
      memcpy (S[s].namef, nm, nl);
    } else {
      char t[16];
      int tl = snprintf (t, sizeof t, "/%u", (unsigned) (strtab.len + 4));
      if (tl < 0 || tl > 8) goto done; /* a >7-digit offset cannot spell in 8 bytes */
      memcpy (S[s].namef, t, (size_t) tl);
      buf_str (&strtab, nm);
    }
  }

  /* ---- file layout: header, section headers, bodies (4-aligned), then
     relocation arrays, symbol table, string table */
  uint64_t cur = 20 + (uint64_t) nsects * 40;
  for (int s = 0; s < PE_OSEC_N; s++) {
    if (!S[s].present_p || S[s].nobits_p || S[s].size == 0) continue;
    cur = PE_ALIGN (cur, 4);
    S[s].off = (uint32_t) cur;
    cur += S[s].size;
  }
  for (int s = 0; s < PE_OSEC_N; s++) {
    if (rel[s].len == 0) continue;
    S[s].reloff = (uint32_t) cur;
    cur += rel[s].len;
  }
  uint64_t symoff = cur;
  uint64_t stroff = symoff + symtab.len;
  uint64_t total = stroff + 4 + strtab.len;

  /* ---- assemble */
  p = calloc (1, (size_t) total);
  if (p == NULL) goto done;
  {
    dwbuf_t h = {0};
    buf_u16 (&h, PE_FILE_MACHINE_AMD64);
    buf_u16 (&h, (uint16_t) nsects);
    buf_u32 (&h, 0); /* TimeDateStamp: deterministic output */
    buf_u32 (&h, (uint32_t) symoff);
    buf_u32 (&h, n_records);
    buf_u16 (&h, 0); /* SizeOfOptionalHeader */
    buf_u16 (&h, 0); /* Characteristics */
    for (int s = 0; s < PE_OSEC_N; s++) {
      if (!S[s].present_p) continue;
      buf_bytes (&h, S[s].namef, 8);
      buf_u32 (&h, 0); /* VirtualSize (objects: 0) */
      buf_u32 (&h, 0); /* VirtualAddress */
      buf_u32 (&h, (uint32_t) S[s].size); /* SizeOfRawData (.bss: its vm size) */
      buf_u32 (&h, S[s].nobits_p ? 0 : S[s].off);
      buf_u32 (&h, S[s].reloff);
      buf_u32 (&h, 0); /* PointerToLinenumbers */
      buf_u16 (&h, S[s].nreloc > 0xffffu ? 0xffffu : (uint16_t) S[s].nreloc);
      buf_u16 (&h, 0); /* NumberOfLinenumbers */
      buf_u32 (&h, S[s].chars);
    }
    int ok = h.len == 20 + (uint64_t) nsects * 40; /* sizing drift = a writer bug */
    if (ok) memcpy (p, h.p, h.len);
    free (h.p);
    if (!ok) goto done;
  }
  for (int s = 0; s < PE_OSEC_N; s++)
    if (S[s].present_p && !S[s].nobits_p && S[s].size != 0 && S[s].body != NULL)
      memcpy (p + S[s].off, S[s].body, (size_t) S[s].size);
  /* the relocations' in-field addends (COFF has no addend field) */
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    uint8_t *slot = p + S[pe_osec_of (r->sec)].off + r->offset;
    if (r->kind == MIR_OBJ_RELOC_ABS64) {
      uint64_t a = (uint64_t) r->addend;
      memcpy (slot, &a, 8);
    } else { /* PC32, the only other kind that survived the pass above */
      int64_t a = r->addend + 4;
      if (a < INT32_MIN || a > INT32_MAX) goto done;
      int32_t a32 = (int32_t) a;
      memcpy (slot, &a32, 4);
    }
  }
  for (int s = 0; s < PE_OSEC_N; s++)
    if (rel[s].len != 0) memcpy (p + S[s].reloff, rel[s].p, rel[s].len);
  memcpy (p + symoff, symtab.p, symtab.len);
  {
    uint32_t strsize = (uint32_t) (4 + strtab.len);
    memcpy (p + stroff, &strsize, 4);
    if (strtab.len != 0) memcpy (p + stroff + 4, strtab.p, strtab.len);
  }

  *buf = p;
  *size = (size_t) total;
  p = NULL;
  rc = 0;
#undef PE_ALIGN

done:
  free (p);
  free (final_idx);
  free (symtab.p);
  free (strtab.p);
  for (int s = 0; s < PE_OSEC_N; s++) free (rel[s].p);
  return rc;
}

/* ===== the .o reader (COFF -> the neutral input view) ====================
   The exact inverse of the writer above, and no more: this reads MIR's OWN
   COFF shape (the same scope statement the ELF and Mach-O fronts make --
   objects from other compilers are out of scope).  Symbols keep their RAW
   record indices -- aux records occupy index slots in COFF, so the view
   carries inert placeholders for them and relocations index the view
   directly.  The WEAK_EXTERNAL + STATIC-companion pair decodes back to one
   weak DEFINED symbol at the companion's location; in-field addends decode
   back to explicit ones (REL32's +4 removed). */

static uint32_t pe_rd32 (const uint8_t *p) {
  uint32_t v;
  memcpy (&v, p, 4);
  return v;
}

static uint16_t pe_rd16 (const uint8_t *p) {
  uint16_t v;
  memcpy (&v, p, 2);
  return v;
}

/* section-header name -> MIR_OBJ_SEC_*, or -1 when it is not one of ours */
static int pe_sec_of_name (const char *nm) {
  for (int s = 0; s < PE_OSEC_N; s++)
    if (strcmp (nm, pe_osec_name (s)) == 0)
      return s == PE_OSEC_TEXT   ? MIR_OBJ_SEC_TEXT
             : s == PE_OSEC_POOL ? MIR_OBJ_SEC_ADDRPOOL
             : s == PE_OSEC_INIT ? MIR_OBJ_SEC_INITARR
             : s == PE_OSEC_DATA ? MIR_OBJ_SEC_DATA
                                 : MIR_OBJ_SEC_BSS;
  return -1;
}

static int objin_from_pe (objin_t *v, const void *vbuf, size_t size, char *err_msg,
                          size_t err_len) {
  const uint8_t *buf = vbuf;
  uint32_t reloff[5] = {0, 0, 0, 0, 0}, nreloc[5] = {0, 0, 0, 0, 0};
  int ovfl[5] = {0, 0, 0, 0, 0}, seen[5] = {0, 0, 0, 0, 0};
  int sec_of_ord[256]; /* 1-based SectionNumber -> MIR_OBJ_SEC_* */

  memset (v, 0, sizeof (*v));
  for (int s = 0; s < 5; s++) v->sec[s].align = 1;
  for (int i = 0; i < 256; i++) sec_of_ord[i] = -1;
  if (buf == NULL || size < 20) {
    OBJLOAD_ERR ("truncated or empty object");
    return -1;
  }
  if (pe_rd16 (buf) != PE_FILE_MACHINE_AMD64) {
    OBJLOAD_ERR ("object's machine 0x%x is not x86-64 COFF (0x%x)", (unsigned) pe_rd16 (buf),
                 (unsigned) PE_FILE_MACHINE_AMD64);
    return -1;
  }
  uint32_t nsects = pe_rd16 (buf + 2);
  uint64_t symoff = pe_rd32 (buf + 8);
  uint64_t n_records = pe_rd32 (buf + 12);
  if (pe_rd16 (buf + 16) != 0) {
    OBJLOAD_ERR ("relocatable COFF with an optional header (not this emitter's shape)");
    return -1;
  }
  if (20 + (uint64_t) nsects * 40 > size || symoff + n_records * 18 > size) {
    OBJLOAD_ERR ("section or symbol table lies outside the file");
    return -1;
  }
  /* the string table sits right after the symbol table: u32 total size */
  uint64_t stroff = symoff + n_records * 18;
  const char *strtab = NULL;
  uint64_t strsize = 0;
  if (stroff + 4 <= size) {
    strsize = pe_rd32 (buf + stroff);
    if (strsize < 4 || stroff + strsize > size) {
      OBJLOAD_ERR ("malformed string table");
      return -1;
    }
    strtab = (const char *) buf + stroff;
  }

  /* ---- section headers, mapped by name */
  char secnm[256]; /* long names resolve through the string table */
  for (uint32_t s = 0; s < nsects; s++) {
    const uint8_t *sh = buf + 20 + (uint64_t) s * 40;
    if (sh[0] == '/') { /* "/<decimal offset>" */
      char digits[8];
      memcpy (digits, sh + 1, 7);
      digits[7] = 0;
      uint64_t noff = strtoull (digits, NULL, 10);
      if (strtab == NULL || noff < 4 || noff >= strsize) {
        OBJLOAD_ERR ("section %u long name out of bounds", s);
        return -1;
      }
      snprintf (secnm, sizeof secnm, "%s", strtab + noff);
    } else {
      memcpy (secnm, sh, 8);
      secnm[8] = 0;
    }
    int sec = pe_sec_of_name (secnm);
    if (sec < 0) {
      OBJLOAD_ERR ("unsupported section %s", secnm);
      return -1;
    }
    if (seen[sec]) {
      OBJLOAD_ERR ("duplicate section %s", secnm);
      return -1;
    }
    seen[sec] = 1;
    uint32_t ssize = pe_rd32 (sh + 16), soff = pe_rd32 (sh + 20);
    uint32_t chars = pe_rd32 (sh + 36);
    unsigned al = (chars >> 20) & 0xf;
    v->sec[sec].size = ssize;
    if (al != 0) v->sec[sec].align = (uint64_t) 1 << (al - 1);
    int nobits_p = (chars & PE_SCN_CNT_UNINIT_DATA) != 0;
    if (!nobits_p && ssize != 0) {
      if (soff > size || ssize > size - soff) {
        OBJLOAD_ERR ("section %s body lies outside the file", secnm);
        return -1;
      }
      v->sec[sec].body = buf + soff;
    }
    reloff[sec] = pe_rd32 (sh + 24);
    nreloc[sec] = pe_rd16 (sh + 32);
    ovfl[sec] = (chars & PE_SCN_LNK_NRELOC_OVFL) != 0;
    if (s + 1 < 256) sec_of_ord[s + 1] = sec;
  }
  if (v->sec[MIR_OBJ_SEC_TEXT].align > 16) {
    OBJLOAD_ERR ("text alignment %llu unsupported (the emitter's is 16)",
                 (unsigned long long) v->sec[MIR_OBJ_SEC_TEXT].align);
    return -1;
  }
  if (v->sec[MIR_OBJ_SEC_INITARR].size % 8 != 0) {
    OBJLOAD_ERR (".CRT$XCU is not 8-byte slots");
    return -1;
  }

  /* ---- symbols: RAW record order IS the view's order (aux slots become
     inert local placeholders no relocation may reference).  String-table
     names are borrowed; INLINE names (8-byte field, not NUL-terminated at
     8 chars) are copied into the view's own arena -- sized up front so the
     pointers never move. */
  char *arena = NULL;
  if (n_records != 0) {
    v->syms = calloc ((size_t) n_records, sizeof (objin_sym_t));
    v->namebuf = malloc ((size_t) n_records * 9);
    if (v->syms == NULL || v->namebuf == NULL) {
      OBJLOAD_ERR ("out of memory");
      goto fail;
    }
    arena = v->namebuf;
  }
  for (uint64_t i = 0; i < n_records; i++) {
    const uint8_t *sr = buf + symoff + i * 18;
    uint32_t value = pe_rd32 (sr + 8);
    int sec_number = (int16_t) pe_rd16 (sr + 12);
    uint16_t type = pe_rd16 (sr + 14);
    uint8_t storage_class = sr[16], n_aux = sr[17];
    objin_sym_t *d = &v->syms[v->n_syms++];
    d->sec = MIR_OBJ_SEC_UNDEF;
    d->dbg = -1;
    const char *nm;
    if (pe_rd32 (sr) == 0) { /* long name via the string table */
      uint32_t noff = pe_rd32 (sr + 4);
      if (strtab == NULL || noff < 4 || noff >= strsize) {
        OBJLOAD_ERR ("symbol %llu name out of bounds", (unsigned long long) i);
        goto fail;
      }
      nm = strtab + noff;
    } else {
      memcpy (arena, sr, 8);
      arena[8] = 0;
      nm = arena;
      arena += 9;
    }
    if (storage_class == PE_SYM_CLASS_WEAK_EXTERNAL) {
      /* aux TagIndex names the STATIC companion carrying the definition */
      if (n_aux < 1 || i + 1 >= n_records) {
        OBJLOAD_ERR ("weak external '%s' has no aux record", nm);
        goto fail;
      }
      uint32_t tag = pe_rd32 (buf + symoff + (i + 1) * 18);
      if (tag + 1 > i) { /* the companion precedes its weak symbol */
        OBJLOAD_ERR ("weak external '%s' tags a bad companion %u", nm, (unsigned) tag);
        goto fail;
      }
      objin_sym_t *c = &v->syms[tag];
      if (c->sec == MIR_OBJ_SEC_UNDEF) {
        OBJLOAD_ERR ("weak external '%s' tags an undefined companion", nm);
        goto fail;
      }
      d->name = nm;
      d->sec = c->sec;
      d->value = c->value;
      d->func_p = c->func_p;
      d->weak_p = 1;
    } else if (sec_number == 0) {
      if (storage_class == PE_SYM_CLASS_EXTERNAL) {
        d->name = nm;
        d->undef_p = 1;
      } else {
        d->local_p = 1; /* an inert record (a file symbol etc.) */
      }
    } else if (sec_number > 0 && sec_number < 256 && sec_of_ord[sec_number] >= 0) {
      int sec = sec_of_ord[sec_number];
      if (storage_class == PE_SYM_CLASS_STATIC && value == 0 && pe_sec_of_name (nm) == sec) {
        d->sec = sec; /* the section's own symbol (a C name cannot start '.') */
        d->section_p = 1;
        d->local_p = 1;
      } else if (storage_class == PE_SYM_CLASS_STATIC || storage_class == PE_SYM_CLASS_EXTERNAL) {
        if (value > v->sec[sec].size) {
          OBJLOAD_ERR ("symbol '%s' lies outside its section", nm);
          goto fail;
        }
        d->name = nm;
        d->sec = sec;
        d->value = value;
        d->func_p = (type & 0xf0) == PE_SYM_TYPE_FUNCTION;
        d->local_p = storage_class == PE_SYM_CLASS_STATIC;
      } else {
        OBJLOAD_ERR ("symbol '%s' has unsupported storage class %u", nm,
                     (unsigned) storage_class);
        goto fail;
      }
    } else {
      OBJLOAD_ERR ("symbol '%s' is not defined in one of this object's sections", nm);
      goto fail;
    }
    /* aux records occupy index slots: keep them as inert local placeholders */
    for (uint8_t a = 0; a < n_aux && i + 1 < n_records; a++) {
      i++;
      objin_sym_t *x = &v->syms[v->n_syms++];
      x->sec = MIR_OBJ_SEC_UNDEF;
      x->dbg = -1;
      x->local_p = 1;
    }
  }

  /* ---- relocations, per section */
  for (int sec = 0; sec < 5; sec++) {
    uint64_t count = nreloc[sec];
    uint64_t roff = reloff[sec];
    if (count == 0) continue;
    if (roff + count * 10 > size) {
      OBJLOAD_ERR ("section %d relocations lie outside the file", sec);
      goto fail;
    }
    if (ovfl[sec]) { /* real count in the first record's VirtualAddress */
      count = pe_rd32 (buf + roff);
      if (count == 0 || roff + count * 10 > size) {
        OBJLOAD_ERR ("section %d overflow relocation count is malformed", sec);
        goto fail;
      }
      roff += 10;
      count -= 1; /* the binutils convention counts the overflow record */
    }
    if (v->sec[sec].body == NULL) {
      OBJLOAD_ERR ("section %d carries relocations but no body", sec);
      goto fail;
    }
    objin_rel_t *nv = realloc (v->rels, (v->n_rels + count) * sizeof (objin_rel_t));
    if (nv == NULL) {
      OBJLOAD_ERR ("out of memory");
      goto fail;
    }
    v->rels = nv;
    for (uint64_t k = 0; k < count; k++) {
      const uint8_t *re = buf + roff + k * 10;
      uint32_t r_addr = pe_rd32 (re), symnum = pe_rd32 (re + 4);
      uint16_t rtype = pe_rd16 (re + 8);
      if (symnum >= v->n_syms) {
        OBJLOAD_ERR ("relocation %llu in section %d against a bad symbol index",
                     (unsigned long long) k, sec);
        goto fail;
      }
      int kind = rtype == PE_REL_AMD64_ADDR64  ? MIR_OBJ_RELOC_ABS64
                 : rtype == PE_REL_AMD64_REL32 ? MIR_OBJ_RELOC_PC32
                                               : -1;
      if (kind < 0) {
        OBJLOAD_ERR ("unsupported relocation type %u in section %d", (unsigned) rtype, sec);
        goto fail;
      }
      uint32_t slot = kind == MIR_OBJ_RELOC_ABS64 ? 8 : 4;
      if (r_addr > v->sec[sec].size || v->sec[sec].size - r_addr < slot) {
        OBJLOAD_ERR ("relocation %llu in section %d lies outside the section",
                     (unsigned long long) k, sec);
        goto fail;
      }
      objin_rel_t *d = &v->rels[v->n_rels++];
      d->sec = sec;
      d->offset = r_addr;
      d->sym = symnum;
      d->kind = kind;
      if (kind == MIR_OBJ_RELOC_ABS64) {
        uint64_t a;
        memcpy (&a, v->sec[sec].body + r_addr, 8);
        d->addend = (int64_t) a;
      } else {
        int32_t a32;
        memcpy (&a32, v->sec[sec].body + r_addr, 4);
        d->addend = (int64_t) a32 - 4; /* REL32's base is P + 4, ELF's is P */
      }
    }
  }
  return 0;

fail:
  objin_free (v); /* the name arena is the view's own (namebuf) */
  return -1;
}
