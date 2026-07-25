/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Mach-O64 executable writer (madc fork, Mach-O/ARM64 track axis B).

   #included by mir-debug.c when MIR_TARGET_APPLE_P: the alternative
   assembler behind the MIR_object seam.  Everything upstream of it --
   capture, sections, symbols, relocations -- is the ELF writer's own
   builder state; only the container format differs.  Linux has no
   mach-o/loader.h, so every constant is defined here.

   Mapping from the ELF executable emitter (MIR_object_emit_executable):

     .text          -> __TEXT,__text            (after header + load cmds)
     .mir.addrpool  -> __DATA_CONST,__mir_addrpool  (S_REGULAR -- the
                       addrpool-as-GOT model needs no stubs, no lazy
                       binding, no indirect symbol table; dyld reads the
                       rebase/bind streams)
     .init_array    -> __DATA_CONST,__mod_init_func (S_MOD_INIT_FUNC_
                       POINTERS; dyld runs entries after fixups)
     .data          -> __DATA,__data
     .bss           -> __DATA,__bss              (S_ZEROFILL vm tail)
     _start stub    -> GONE: LC_MAIN's entryoff points at the entry
                       symbol; dyld's libdyld glue passes argc/argv/envp
                       and exits with main's return
     PC32 / aarch64 page pairs -> resolved at emit (bias-invariant --
                       dyld slides are page-multiple), same field patchers
     internal ABS64 -> link vaddr BAKED into the file bytes + a rebase
                       entry (dyld ADDS the slide to the stored value)
     import ABS64   -> zero slot + bind entry (two-level, libSystem
                       ordinal, `_'-prefixed name)
     Full RELRO     -> __DATA_CONST (dyld maps it read-only after fixups)

   The image is always PIE (MH_PIE -- mandatory on arm64), always links
   /usr/lib/libSystem.B.dylib (macOS mandates dynamic libSystem), and
   always carries a linker-signed ad-hoc code signature (CodeDirectory
   with SHA-256 page hashes, no certificate -- Apple Silicon refuses
   unsigned binaries; ad-hoc signing is pure hashing).  shared_p is
   refused loudly: there is no dylib emission by design.

   Validation oracle (all-Linux): clang-18 -target *-apple-macos12 +
   ld64.lld-18 reference binaries; llvm-otool/readobj/nm-18. */

/* ===== big-endian helpers (code-signature blobs are network order) ====== */

static void machob_be32 (dwbuf_t *b, uint32_t v) {
  buf_u8 (b, (uint8_t) (v >> 24));
  buf_u8 (b, (uint8_t) (v >> 16));
  buf_u8 (b, (uint8_t) (v >> 8));
  buf_u8 (b, (uint8_t) v);
}

static void machob_be64 (dwbuf_t *b, uint64_t v) {
  machob_be32 (b, (uint32_t) (v >> 32));
  machob_be32 (b, (uint32_t) v);
}

/* ===== SHA-256 (FIPS 180-4) -- signature page hashes + deterministic UUID.
   Self-contained: the emitter takes no crypto dependency (the
   no-external-toolchain law).  Verified against the standard "abc"
   test vector by the fork's object tests. ====== */

typedef struct {
  uint32_t h[8];
  uint64_t nbytes;
  uint8_t blk[64];
  size_t blk_len;
} macho_sha256_t;

static const uint32_t macho_sha_k[64]
  = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
     0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
     0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
     0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
     0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
     0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
     0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
     0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
     0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
     0xc67178f2};

#define MACHO_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void macho_sha256_block (macho_sha256_t *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16)
           | ((uint32_t) p[i * 4 + 2] << 8) | (uint32_t) p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = MACHO_ROTR (w[i - 15], 7) ^ MACHO_ROTR (w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = MACHO_ROTR (w[i - 2], 17) ^ MACHO_ROTR (w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
  e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = MACHO_ROTR (e, 6) ^ MACHO_ROTR (e, 11) ^ MACHO_ROTR (e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + macho_sha_k[i] + w[i];
    uint32_t s0 = MACHO_ROTR (a, 2) ^ MACHO_ROTR (a, 13) ^ MACHO_ROTR (a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void macho_sha256_init (macho_sha256_t *s) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy (s->h, h0, sizeof h0);
  s->nbytes = 0;
  s->blk_len = 0;
}

static void macho_sha256_update (macho_sha256_t *s, const void *data, size_t len) {
  const uint8_t *p = data;
  s->nbytes += len;
  if (s->blk_len != 0) {
    size_t take = 64 - s->blk_len;
    if (take > len) take = len;
    memcpy (s->blk + s->blk_len, p, take);
    s->blk_len += take;
    p += take;
    len -= take;
    if (s->blk_len == 64) {
      macho_sha256_block (s, s->blk);
      s->blk_len = 0;
    }
  }
  for (; len >= 64; p += 64, len -= 64) macho_sha256_block (s, p);
  if (len != 0) {
    memcpy (s->blk, p, len);
    s->blk_len = len;
  }
}

static void macho_sha256_final (macho_sha256_t *s, uint8_t out[32]) {
  uint64_t bits = s->nbytes * 8;
  uint8_t pad = 0x80, zero = 0;
  macho_sha256_update (s, &pad, 1);
  while (s->blk_len != 56) macho_sha256_update (s, &zero, 1);
  for (int i = 7; i >= 0; i--) {
    uint8_t b = (uint8_t) (bits >> (i * 8));
    macho_sha256_update (s, &b, 1);
  }
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t) (s->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t) (s->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t) (s->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t) s->h[i];
  }
}

/* ===== Mach-O constants (no mach-o/loader.h on a Linux host) ============ */

#define MACHO_MH_MAGIC_64 0xfeedfacfu
#define MACHO_MH_EXECUTE 2u
#define MACHO_MH_NOUNDEFS 0x1u
#define MACHO_MH_DYLDLINK 0x4u
#define MACHO_MH_TWOLEVEL 0x80u
#define MACHO_MH_PIE 0x200000u

#define MACHO_CPU_TYPE_X86_64 0x01000007u
#define MACHO_CPU_SUBTYPE_X86_64_ALL 3u
#define MACHO_CPU_TYPE_ARM64 0x0100000cu
#define MACHO_CPU_SUBTYPE_ARM64_ALL 0u

#define MACHO_LC_REQ_DYLD 0x80000000u
#define MACHO_LC_SEGMENT_64 0x19u
#define MACHO_LC_SYMTAB 0x2u
#define MACHO_LC_DYSYMTAB 0xbu
#define MACHO_LC_LOAD_DYLIB 0xcu
#define MACHO_LC_LOAD_DYLINKER 0xeu
#define MACHO_LC_UUID 0x1bu
#define MACHO_LC_CODE_SIGNATURE 0x1du
#define MACHO_LC_FUNCTION_STARTS 0x26u
#define MACHO_LC_DATA_IN_CODE 0x29u
#define MACHO_LC_DYLD_INFO_ONLY (0x22u | MACHO_LC_REQ_DYLD)
#define MACHO_LC_MAIN (0x28u | MACHO_LC_REQ_DYLD)
#define MACHO_LC_BUILD_VERSION 0x32u

#define MACHO_VM_PROT_READ 1u
#define MACHO_VM_PROT_WRITE 2u
#define MACHO_VM_PROT_EXECUTE 4u

#define MACHO_S_REGULAR 0x0u
#define MACHO_S_ZEROFILL 0x1u
#define MACHO_S_MOD_INIT_FUNC_POINTERS 0x9u
#define MACHO_S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define MACHO_S_ATTR_SOME_INSTRUCTIONS 0x00000400u

#define MACHO_N_UNDF 0x0u
#define MACHO_N_SECT 0xeu
#define MACHO_N_EXT 0x1u
#define MACHO_N_WEAK_DEF 0x0080u

#define MACHO_PLATFORM_MACOS 1u
#define MACHO_MINOS_12_0 0x000c0000u /* 12.0.0 as xxxx.yy.zz nibbles */

#define MACHO_REBASE_TYPE_POINTER 1u
#define MACHO_REBASE_OPCODE_SET_TYPE_IMM 0x10u
#define MACHO_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20u
#define MACHO_REBASE_OPCODE_DO_REBASE_IMM_TIMES 0x50u

#define MACHO_BIND_TYPE_POINTER 1u
#define MACHO_BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10u
#define MACHO_BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40u
#define MACHO_BIND_OPCODE_SET_TYPE_IMM 0x50u
#define MACHO_BIND_OPCODE_SET_ADDEND_SLEB 0x60u
#define MACHO_BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70u
#define MACHO_BIND_OPCODE_DO_BIND 0x90u

#define MACHO_CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0u
#define MACHO_CSMAGIC_CODEDIRECTORY 0xfade0c02u
#define MACHO_CSSLOT_CODEDIRECTORY 0u
#define MACHO_CS_ADHOC 0x00000002u
#define MACHO_CS_LINKER_SIGNED 0x00020000u
#define MACHO_CS_HASHTYPE_SHA256 2u
#define MACHO_CS_EXECSEG_MAIN_BINARY 0x1u
#define MACHO_CS_PAGE_SIZE 4096u /* signing pages are 4K regardless of VM page size */

/* segment / vm layout */
#define MACHO_BASE 0x100000000ull /* __PAGEZERO covers [0, MACHO_BASE) */
#if MIR_TARGET_IS_AARCH64
#define MACHO_PAGE 0x4000ull /* arm64 mandates 16K segment alignment */
#else
#define MACHO_PAGE 0x1000ull
#endif

/* fixed LC_SEGMENT_64 order -> dyld segment indexes for rebase/bind */
enum {
  MACHO_SEG_PAGEZERO = 0,
  MACHO_SEG_TEXT = 1,
  MACHO_SEG_DATA_CONST = 2,
  MACHO_SEG_DATA = 3,
  MACHO_SEG_LINKEDIT = 4,
};

/* ===== small writers ==================================================== */

/* 16-byte space-free segment/section name field */
static void machob_name16 (dwbuf_t *b, const char *name) {
  char nm[16];
  size_t l = strlen (name);
  if (l > sizeof nm) l = sizeof nm;
  memset (nm, 0, sizeof nm);
  memcpy (nm, name, l);
  buf_bytes (b, nm, sizeof nm);
}

/* LC_SEGMENT_64 header (72 bytes) */
static void machob_segment (dwbuf_t *b, const char *name, uint64_t vmaddr, uint64_t vmsize,
                            uint64_t fileoff, uint64_t filesize, uint32_t prot, uint32_t nsects) {
  buf_u32 (b, MACHO_LC_SEGMENT_64);
  buf_u32 (b, 72 + nsects * 80);
  machob_name16 (b, name);
  buf_u64 (b, vmaddr);
  buf_u64 (b, vmsize);
  buf_u64 (b, fileoff);
  buf_u64 (b, filesize);
  buf_u32 (b, prot); /* maxprot */
  buf_u32 (b, prot); /* initprot */
  buf_u32 (b, nsects);
  buf_u32 (b, 0); /* flags */
}

/* section_64 (80 bytes) */
static void machob_section (dwbuf_t *b, const char *sect, const char *seg, uint64_t addr,
                            uint64_t size, uint32_t offset, uint32_t align_log2, uint32_t flags) {
  machob_name16 (b, sect);
  machob_name16 (b, seg);
  buf_u64 (b, addr);
  buf_u64 (b, size);
  buf_u32 (b, offset);
  buf_u32 (b, align_log2);
  buf_u32 (b, 0); /* reloff */
  buf_u32 (b, 0); /* nreloc */
  buf_u32 (b, flags);
  buf_u32 (b, 0); /* reserved1 */
  buf_u32 (b, 0); /* reserved2 */
  buf_u32 (b, 0); /* reserved3 */
}

/* string-carrying load command (LC_LOAD_DYLINKER / LC_LOAD_DYLIB) sizes are
   8-padded; the fixed part is passed pre-written by the caller */
static uint32_t machob_lc_str_size (uint32_t fixed, const char *s) {
  uint32_t n = fixed + (uint32_t) strlen (s) + 1;
  return (n + 7u) & ~7u;
}

typedef struct {
  int seg;
  uint64_t off; /* offset within the segment */
} macho_rebase_t;

typedef struct {
  int seg;
  uint64_t off;
  const char *name; /* import name, no `_' prefix yet */
  int64_t addend;
} macho_bind_t;

static int macho_rebase_cmp (const void *a, const void *b) {
  const macho_rebase_t *x = a, *y = b;
  if (x->seg != y->seg) return x->seg - y->seg;
  return x->off < y->off ? -1 : x->off > y->off ? 1 : 0;
}

static int macho_bind_cmp (const void *a, const void *b) {
  const macho_bind_t *x = a, *y = b;
  if (x->seg != y->seg) return x->seg - y->seg;
  return x->off < y->off ? -1 : x->off > y->off ? 1 : 0;
}

static int macho_u64_cmp (const void *a, const void *b) {
  uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* ===== the writer ======================================================= */

static int macho_emit_executable (MIR_object_t obj, const MIR_object_exec_params *params,
                                  void **buf, size_t *size) {
  if (params->shared_p) return -1; /* no dylib emission by design (no libmadc.dylib) */
  const char *entry_nm = params->entry != NULL ? params->entry : "main";
  const char *ident = params->identifier != NULL ? params->identifier : "mir.image";
  size_t n = obj->n_syms;

  /* the entry symbol must be a defined text function */
  size_t entry_i = n;
  for (size_t i = 0; i < n; i++)
    if (obj->syms[i].name != NULL && obj->syms[i].defined_p && obj->syms[i].sec == MIR_OBJ_SEC_TEXT
        && strcmp (obj->syms[i].name, entry_nm) == 0) {
      entry_i = i;
      break;
    }
  if (entry_i == n) return -1;

  /* ---- relocation census: field kinds resolve at emit; internal ABS64
     becomes a rebase, import ABS64 becomes a bind.  A dynamic slot in
     __TEXT has no Mach-O story (and the PIC capture never makes one) --
     refuse loudly, ELF DT_TEXTREL parity. */
  size_t n_rebase = 0, n_bind = 0;
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if (obj_kind_rtype (r->kind) < 0 || r->sym < 0 || (size_t) r->sym >= n) return -1;
    if (r->kind != MIR_OBJ_RELOC_ABS64) {
      if (!obj->syms[r->sym].defined_p) return -1; /* field kind against an import */
      continue;
    }
    if (r->sec == MIR_OBJ_SEC_TEXT) return -1; /* dynamic slot in text */
    if (obj->syms[r->sym].defined_p)
      n_rebase++;
    else
      n_bind++;
  }

  /* ---- load-command sizing (needed before any file offset exists) */
  uint32_t n_init_sects = obj->initarr.len != 0 ? 1 : 0;
  uint32_t n_data_sects = 1 /* __data (kept even when empty: stable numbering) */
                          + (obj->bss_size != 0 ? 1 : 0);
  uint32_t dylinker_size = machob_lc_str_size (12, "/usr/lib/dyld");
  uint32_t libsystem_size = machob_lc_str_size (24, "/usr/lib/libSystem.B.dylib");
  uint32_t sizeofcmds = (72 + 0)                          /* __PAGEZERO */
                        + (72 + 80)                       /* __TEXT + __text */
                        + (72 + (1 + n_init_sects) * 80)  /* __DATA_CONST */
                        + (72 + n_data_sects * 80)        /* __DATA */
                        + (72 + 0)                        /* __LINKEDIT */
                        + 48                              /* LC_DYLD_INFO_ONLY */
                        + 24                              /* LC_SYMTAB */
                        + 80                              /* LC_DYSYMTAB */
                        + dylinker_size                   /* LC_LOAD_DYLINKER */
                        + 24                              /* LC_UUID */
                        + 24                              /* LC_BUILD_VERSION */
                        + 24                              /* LC_MAIN */
                        + libsystem_size                  /* LC_LOAD_DYLIB libSystem */
                        + 16                              /* LC_FUNCTION_STARTS */
                        + 16                              /* LC_DATA_IN_CODE */
                        + 16;                             /* LC_CODE_SIGNATURE */
  uint32_t ncmds = 16; /* 5 segments + the 11 fixed commands above */
  for (size_t i = 0; i < params->n_needed; i++) {
    sizeofcmds += machob_lc_str_size (24, params->needed[i]);
    ncmds++;
  }

#define MACHO_ALIGN(v, a) (((v) + (uint64_t) (a) -1) & ~((uint64_t) (a) -1))
  /* ---- layout: identity fileoff <-> vaddr-MACHO_BASE mapping */
  uint64_t text_off = MACHO_ALIGN (32 + sizeofcmds, 16); /* keeps SSE pool alignment */
  uint64_t text_end = text_off + obj->text.len;
  uint64_t text_seg_size = MACHO_ALIGN (text_end, MACHO_PAGE);

  size_t pool_align = obj->pool_align > 8 ? obj->pool_align : 8;
  if (pool_align > MACHO_PAGE) return -1; /* congruence bound, ELF parity */
  uint64_t dc_off = text_seg_size; /* page-aligned segment start = pool start */
  uint64_t initarr_off = MACHO_ALIGN (dc_off + obj->pool.len, 8);
  uint64_t dc_end = initarr_off + obj->initarr.len;
  uint64_t dc_seg_size = MACHO_ALIGN (dc_end - dc_off, MACHO_PAGE);
  if (dc_seg_size == 0) dc_seg_size = MACHO_PAGE; /* keep fixed segment indexes */

  size_t data_align = obj->data_align > 8 ? obj->data_align : 8;
  size_t bss_align = obj->bss_align > 8 ? obj->bss_align : 8;
  if (data_align > MACHO_PAGE || bss_align > MACHO_PAGE) return -1;
  uint64_t data_off = dc_off + dc_seg_size;
  uint64_t data_file_size = MACHO_ALIGN (obj->data.len, MACHO_PAGE);
  if (data_file_size == 0) data_file_size = MACHO_PAGE;
  uint64_t bss_vaddr = MACHO_ALIGN (MACHO_BASE + data_off + obj->data.len, bss_align);
  uint64_t data_vm_size
    = MACHO_ALIGN (bss_vaddr + obj->bss_size - (MACHO_BASE + data_off), MACHO_PAGE);
  if (data_vm_size < data_file_size) data_vm_size = data_file_size;
  uint64_t le_off = data_off + data_file_size; /* __LINKEDIT file start */

  uint64_t text_vaddr = MACHO_BASE + text_off; /* builder .text offset 0 */
  uint64_t data_vaddr = MACHO_BASE + data_off;
  uint64_t pool_vaddr = MACHO_BASE + dc_off;
  uint64_t initarr_vaddr = MACHO_BASE + initarr_off;

#define MACHO_SEC_VADDR(sec) \
  ((sec) == MIR_OBJ_SEC_TEXT      ? text_vaddr \
   : (sec) == MIR_OBJ_SEC_DATA    ? data_vaddr \
   : (sec) == MIR_OBJ_SEC_BSS     ? bss_vaddr \
   : (sec) == MIR_OBJ_SEC_INITARR ? initarr_vaddr \
                                  : pool_vaddr)
  /* slot file offset for a reloc-carrying section (never TEXT/BSS here) */
#define MACHO_SLOT_OFF(sec) \
  ((sec) == MIR_OBJ_SEC_DATA ? data_off : (sec) == MIR_OBJ_SEC_INITARR ? initarr_off : dc_off)
  /* dyld segment index + segment file base for a slot's section */
#define MACHO_SLOT_SEG(sec) ((sec) == MIR_OBJ_SEC_DATA ? MACHO_SEG_DATA : MACHO_SEG_DATA_CONST)
#define MACHO_SLOT_SEG_OFF(sec) ((sec) == MIR_OBJ_SEC_DATA ? data_off : dc_off)

  /* ---- collect rebase / bind entries (sorted for the opcode streams) */
  int rc = -1;
  unsigned char *p = NULL;
  dwbuf_t rebase = {0}, bind = {0}, funcstarts = {0}, symtab = {0}, strtab = {0}, sig = {0};
  uint64_t *fstarts = NULL;
  macho_rebase_t *rebases = calloc (n_rebase ? n_rebase : 1, sizeof (macho_rebase_t));
  macho_bind_t *binds = calloc (n_bind ? n_bind : 1, sizeof (macho_bind_t));
  if (rebases == NULL || binds == NULL) goto done;
  {
    size_t ri = 0, bi = 0;
    for (size_t i = 0; i < obj->n_rels; i++) {
      objreloc_t *r = &obj->rels[i];
      if (r->kind != MIR_OBJ_RELOC_ABS64) continue;
      uint64_t seg_rel = MACHO_SLOT_OFF (r->sec) + r->offset - MACHO_SLOT_SEG_OFF (r->sec);
      if (obj->syms[r->sym].defined_p)
        rebases[ri++] = (macho_rebase_t){MACHO_SLOT_SEG (r->sec), seg_rel};
      else
        binds[bi++] = (macho_bind_t){MACHO_SLOT_SEG (r->sec), seg_rel, obj->syms[r->sym].name,
                                     r->addend};
    }
    qsort (rebases, n_rebase, sizeof (macho_rebase_t), macho_rebase_cmp);
    qsort (binds, n_bind, sizeof (macho_bind_t), macho_bind_cmp);
  }

  /* ---- rebase opcode stream */
  if (n_rebase != 0) {
    buf_u8 (&rebase, MACHO_REBASE_OPCODE_SET_TYPE_IMM | MACHO_REBASE_TYPE_POINTER);
    for (size_t i = 0; i < n_rebase; i++) {
      buf_u8 (&rebase,
              (uint8_t) (MACHO_REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | rebases[i].seg));
      buf_uleb (&rebase, rebases[i].off);
      buf_u8 (&rebase, MACHO_REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1);
    }
    buf_u8 (&rebase, 0); /* REBASE_OPCODE_DONE */
    while (rebase.len % 8 != 0) buf_u8 (&rebase, 0);
  }

  /* ---- bind opcode stream (all imports resolve against libSystem --
     ordinal 1; extra LC_LOAD_DYLIBs are load-time deps only) */
  if (n_bind != 0) {
    int64_t cur_addend = 0;
    buf_u8 (&bind, MACHO_BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1);
    buf_u8 (&bind, MACHO_BIND_OPCODE_SET_TYPE_IMM | MACHO_BIND_TYPE_POINTER);
    for (size_t i = 0; i < n_bind; i++) {
      buf_u8 (&bind, MACHO_BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0);
      buf_u8 (&bind, '_'); /* the C-symbol underscore prefix */
      buf_str (&bind, binds[i].name);
      if (binds[i].addend != cur_addend) {
        buf_u8 (&bind, MACHO_BIND_OPCODE_SET_ADDEND_SLEB);
        buf_sleb (&bind, binds[i].addend);
        cur_addend = binds[i].addend;
      }
      buf_u8 (&bind, (uint8_t) (MACHO_BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | binds[i].seg));
      buf_uleb (&bind, binds[i].off);
      buf_u8 (&bind, MACHO_BIND_OPCODE_DO_BIND);
    }
    buf_u8 (&bind, 0); /* BIND_OPCODE_DONE */
    while (bind.len % 8 != 0) buf_u8 (&bind, 0);
  }

  /* ---- LC_FUNCTION_STARTS: sorted uleb deltas of defined text function
     offsets from the __TEXT segment start (= vmaddr base, fileoff 0) */
  {
    size_t n_f = 0;
    fstarts = calloc (n ? n : 1, sizeof (uint64_t));
    if (fstarts == NULL) goto done;
    for (size_t i = 0; i < n; i++)
      if (obj->syms[i].defined_p && !obj->syms[i].section_p && obj->syms[i].func_p
          && obj->syms[i].sec == MIR_OBJ_SEC_TEXT)
        fstarts[n_f++] = text_off + obj->syms[i].value;
    qsort (fstarts, n_f, sizeof (uint64_t), macho_u64_cmp);
    uint64_t prev = 0;
    for (size_t i = 0; i < n_f; i++) {
      if (i != 0 && fstarts[i] == prev) continue; /* aliases collapse */
      buf_uleb (&funcstarts, fstarts[i] - prev);
      prev = fstarts[i];
    }
    buf_u8 (&funcstarts, 0); /* terminator */
    while (funcstarts.len % 8 != 0) buf_u8 (&funcstarts, 0);
  }

  /* ---- symtab: nlist_64 in local / extdef / undef order (LC_DYSYMTAB
     ranges), `_' prefix on every name; ELF section symbols have no
     Mach-O counterpart and are skipped (relocs against them were baked) */
  uint32_t n_local = 0, n_extdef = 0, n_undef = 0;
  buf_u8 (&strtab, 0);
  for (int pass = 0; pass < 3; pass++)
    for (size_t i = 0; i < n; i++) {
      objsym_t *s = &obj->syms[i];
      if (s->name == NULL || s->section_p) continue;
      int cls = !s->defined_p ? 2 : s->local_p ? 0 : 1;
      if (cls != pass) continue;
      uint32_t strx = (uint32_t) strtab.len;
      buf_u8 (&strtab, '_');
      buf_str (&strtab, s->name);
      buf_u32 (&symtab, strx);                                          /* n_strx */
      if (s->defined_p) {
        buf_u8 (&symtab, (uint8_t) (MACHO_N_SECT | (s->local_p ? 0 : MACHO_N_EXT)));
        /* n_sect is the 1-based section ordinal in LC order:
           __text=1, __mir_addrpool=2, then __mod_init_func, __data, __bss */
        uint8_t n_sect = s->sec == MIR_OBJ_SEC_TEXT      ? 1
                         : s->sec == MIR_OBJ_SEC_ADDRPOOL ? 2
                         : s->sec == MIR_OBJ_SEC_INITARR  ? 3
                         : s->sec == MIR_OBJ_SEC_DATA     ? (uint8_t) (2 + n_init_sects + 1)
                                                          : (uint8_t) (2 + n_init_sects + 2);
        buf_u8 (&symtab, n_sect);
        buf_u16 (&symtab, s->weak_p ? MACHO_N_WEAK_DEF : 0);            /* n_desc */
        buf_u64 (&symtab, MACHO_SEC_VADDR (s->sec) + s->value);         /* n_value */
        if (pass == 0)
          n_local++;
        else
          n_extdef++;
      } else {
        buf_u8 (&symtab, MACHO_N_UNDF | MACHO_N_EXT);
        buf_u8 (&symtab, 0);                                            /* NO_SECT */
        buf_u16 (&symtab, 1 << 8);                                      /* libSystem ordinal */
        buf_u64 (&symtab, 0);
        n_undef++;
      }
    }
  while (strtab.len % 8 != 0) buf_u8 (&strtab, 0);

  /* ---- __LINKEDIT layout (ascending dataoffs; the signature is LAST and
     the file ends exactly at its end -- codesign requirement) */
  uint64_t rebase_le = 0;
  uint64_t bind_le = rebase_le + rebase.len;
  uint64_t fstarts_le = bind_le + bind.len;
  uint64_t dic_le = fstarts_le + funcstarts.len; /* LC_DATA_IN_CODE, empty */
  uint64_t symtab_le = dic_le;
  uint64_t strtab_le = symtab_le + symtab.len;
  uint64_t sig_le = MACHO_ALIGN (strtab_le + strtab.len, 16);
  uint64_t sig_off = le_off + sig_le; /* file offset; == codeLimit */

  /* ---- ad-hoc code signature sizing (content hashed after assembly) */
  uint32_t ident_len = (uint32_t) strlen (ident) + 1;
  uint32_t n_code_slots = (uint32_t) ((sig_off + MACHO_CS_PAGE_SIZE - 1) / MACHO_CS_PAGE_SIZE);
  uint32_t cd_len = 88 + ident_len + n_code_slots * 32;
  uint32_t sb_len = 20 + cd_len;
  uint64_t le_size = sig_le + sb_len;
  uint64_t total = le_off + le_size;

  /* ---- assemble */
  p = calloc (1, (size_t) total);
  if (p == NULL) goto done;
  if (obj->text.len != 0) memcpy (p + text_off, obj->text.p, obj->text.len);
  if (obj->pool.len != 0) memcpy (p + dc_off, obj->pool.p, obj->pool.len);
  /* initarr file bytes: zero, written by the relocation pass below */
  if (obj->data.len != 0) memcpy (p + data_off, obj->data.p, obj->data.len);
  memcpy (p + le_off + rebase_le, rebase.p, rebase.len);
  memcpy (p + le_off + bind_le, bind.p, bind.len);
  memcpy (p + le_off + fstarts_le, funcstarts.p, funcstarts.len);
  memcpy (p + le_off + symtab_le, symtab.p, symtab.len);
  memcpy (p + le_off + strtab_le, strtab.p, strtab.len);

  /* ---- relocation pass: field kinds patch in place (bias-invariant);
     internal ABS64 bakes the link vaddr (the rebase stream tells dyld to
     ADD the slide to it); import slots stay zero for the binds */
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    objsym_t *s = &obj->syms[r->sym];
    uint64_t slot_off = (r->sec == MIR_OBJ_SEC_TEXT ? text_off : MACHO_SLOT_OFF (r->sec))
                        + r->offset;
    if (r->kind != MIR_OBJ_RELOC_ABS64) {
      uint64_t v = MACHO_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend;
      if (obj_apply_field_reloc (r->kind, p + slot_off, v, MACHO_BASE + slot_off) != 0)
        goto done; /* range overflow: not an emitter-layout reality */
    } else if (s->defined_p) {
      uint64_t v = MACHO_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend;
      memcpy (p + slot_off, &v, 8);
    }
  }

  /* ---- mach header + load commands */
  {
    dwbuf_t lc = {0};
    buf_u32 (&lc, MACHO_MH_MAGIC_64);
#if MIR_TARGET_IS_AARCH64
    buf_u32 (&lc, MACHO_CPU_TYPE_ARM64);
    buf_u32 (&lc, MACHO_CPU_SUBTYPE_ARM64_ALL);
#else
    buf_u32 (&lc, MACHO_CPU_TYPE_X86_64);
    buf_u32 (&lc, MACHO_CPU_SUBTYPE_X86_64_ALL);
#endif
    buf_u32 (&lc, MACHO_MH_EXECUTE);
    buf_u32 (&lc, ncmds);
    buf_u32 (&lc, sizeofcmds);
    buf_u32 (&lc, MACHO_MH_NOUNDEFS | MACHO_MH_DYLDLINK | MACHO_MH_TWOLEVEL | MACHO_MH_PIE);
    buf_u32 (&lc, 0); /* reserved */

    machob_segment (&lc, "__PAGEZERO", 0, MACHO_BASE, 0, 0, 0, 0);

    machob_segment (&lc, "__TEXT", MACHO_BASE, text_seg_size, 0, text_seg_size,
                    MACHO_VM_PROT_READ | MACHO_VM_PROT_EXECUTE, 1);
    machob_section (&lc, "__text", "__TEXT", text_vaddr, obj->text.len, (uint32_t) text_off, 4,
                    MACHO_S_REGULAR | MACHO_S_ATTR_PURE_INSTRUCTIONS
                      | MACHO_S_ATTR_SOME_INSTRUCTIONS);

    machob_segment (&lc, "__DATA_CONST", MACHO_BASE + dc_off, dc_seg_size, dc_off, dc_seg_size,
                    MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, 1 + n_init_sects);
    {
      unsigned pl2 = 0;
      while ((1u << pl2) < pool_align) pl2++;
      machob_section (&lc, "__mir_addrpool", "__DATA_CONST", pool_vaddr, obj->pool.len,
                      (uint32_t) dc_off, pl2, MACHO_S_REGULAR);
    }
    if (n_init_sects != 0)
      machob_section (&lc, "__mod_init_func", "__DATA_CONST", initarr_vaddr, obj->initarr.len,
                      (uint32_t) initarr_off, 3, MACHO_S_MOD_INIT_FUNC_POINTERS);

    machob_segment (&lc, "__DATA", data_vaddr, data_vm_size, data_off, data_file_size,
                    MACHO_VM_PROT_READ | MACHO_VM_PROT_WRITE, n_data_sects);
    {
      unsigned dl2 = 0, bl2 = 0;
      while ((1u << dl2) < data_align) dl2++;
      while ((1u << bl2) < bss_align) bl2++;
      machob_section (&lc, "__data", "__DATA", data_vaddr, obj->data.len, (uint32_t) data_off,
                      dl2, MACHO_S_REGULAR);
      if (obj->bss_size != 0)
        machob_section (&lc, "__bss", "__DATA", bss_vaddr, obj->bss_size, 0, bl2,
                        MACHO_S_ZEROFILL);
    }

    machob_segment (&lc, "__LINKEDIT", MACHO_BASE + le_off, MACHO_ALIGN (le_size, MACHO_PAGE),
                    le_off, le_size, MACHO_VM_PROT_READ, 0);

    /* LC_DYLD_INFO_ONLY */
    buf_u32 (&lc, MACHO_LC_DYLD_INFO_ONLY);
    buf_u32 (&lc, 48);
    buf_u32 (&lc, rebase.len != 0 ? (uint32_t) (le_off + rebase_le) : 0);
    buf_u32 (&lc, (uint32_t) rebase.len);
    buf_u32 (&lc, bind.len != 0 ? (uint32_t) (le_off + bind_le) : 0);
    buf_u32 (&lc, (uint32_t) bind.len);
    buf_u32 (&lc, 0); /* weak_bind */
    buf_u32 (&lc, 0);
    buf_u32 (&lc, 0); /* lazy_bind: none -- everything binds eagerly */
    buf_u32 (&lc, 0);
    buf_u32 (&lc, 0); /* export trie: executables export nothing */
    buf_u32 (&lc, 0);

    /* LC_SYMTAB */
    buf_u32 (&lc, MACHO_LC_SYMTAB);
    buf_u32 (&lc, 24);
    buf_u32 (&lc, (uint32_t) (le_off + symtab_le));
    buf_u32 (&lc, n_local + n_extdef + n_undef);
    buf_u32 (&lc, (uint32_t) (le_off + strtab_le));
    buf_u32 (&lc, (uint32_t) strtab.len);

    /* LC_DYSYMTAB (no indirect symbol table: no stub/GOT-typed sections) */
    buf_u32 (&lc, MACHO_LC_DYSYMTAB);
    buf_u32 (&lc, 80);
    buf_u32 (&lc, 0);                    /* ilocalsym */
    buf_u32 (&lc, n_local);              /* nlocalsym */
    buf_u32 (&lc, n_local);              /* iextdefsym */
    buf_u32 (&lc, n_extdef);             /* nextdefsym */
    buf_u32 (&lc, n_local + n_extdef);   /* iundefsym */
    buf_u32 (&lc, n_undef);              /* nundefsym */
    for (int i = 0; i < 12; i++) buf_u32 (&lc, 0); /* toc..locrel: none */

    /* LC_LOAD_DYLINKER */
    buf_u32 (&lc, MACHO_LC_LOAD_DYLINKER);
    buf_u32 (&lc, dylinker_size);
    buf_u32 (&lc, 12); /* name offset */
    {
      size_t base_len = lc.len - 12;
      buf_str (&lc, "/usr/lib/dyld");
      while (lc.len - base_len != dylinker_size) buf_u8 (&lc, 0);
    }

    /* LC_UUID (deterministic content hash, patched after assembly) */
    size_t uuid_pos = lc.len + 8;
    buf_u32 (&lc, MACHO_LC_UUID);
    buf_u32 (&lc, 24);
    buf_zeros (&lc, 16);

    /* LC_BUILD_VERSION */
    buf_u32 (&lc, MACHO_LC_BUILD_VERSION);
    buf_u32 (&lc, 24);
    buf_u32 (&lc, MACHO_PLATFORM_MACOS);
    buf_u32 (&lc, MACHO_MINOS_12_0); /* minos */
    buf_u32 (&lc, MACHO_MINOS_12_0); /* sdk */
    buf_u32 (&lc, 0);                /* ntools */

    /* LC_MAIN */
    buf_u32 (&lc, MACHO_LC_MAIN);
    buf_u32 (&lc, 24);
    buf_u64 (&lc, text_off + obj->syms[entry_i].value); /* entryoff */
    buf_u64 (&lc, 0);                                   /* stacksize: default */

    /* LC_LOAD_DYLIB libSystem (+ params->needed extras -- load-time deps;
       every bind still resolves against libSystem, ordinal 1) */
    {
      const char *libs[1] = {"/usr/lib/libSystem.B.dylib"};
      for (size_t i = 0; i < 1 + params->n_needed; i++) {
        const char *nm = i == 0 ? libs[0] : params->needed[i - 1];
        uint32_t sz = machob_lc_str_size (24, nm);
        size_t base_len = lc.len;
        buf_u32 (&lc, MACHO_LC_LOAD_DYLIB);
        buf_u32 (&lc, sz);
        buf_u32 (&lc, 24);         /* name offset */
        buf_u32 (&lc, 2);          /* timestamp */
        buf_u32 (&lc, 0x05270000); /* current_version 1319.0.0 */
        buf_u32 (&lc, 0x00010000); /* compatibility_version 1.0.0 */
        buf_str (&lc, nm);
        while (lc.len - base_len != sz) buf_u8 (&lc, 0);
      }
    }

    /* LC_FUNCTION_STARTS */
    buf_u32 (&lc, MACHO_LC_FUNCTION_STARTS);
    buf_u32 (&lc, 16);
    buf_u32 (&lc, (uint32_t) (le_off + fstarts_le));
    buf_u32 (&lc, (uint32_t) funcstarts.len);

    /* LC_DATA_IN_CODE (empty; tooling parity) */
    buf_u32 (&lc, MACHO_LC_DATA_IN_CODE);
    buf_u32 (&lc, 16);
    buf_u32 (&lc, (uint32_t) (le_off + dic_le));
    buf_u32 (&lc, 0);

    /* LC_CODE_SIGNATURE */
    buf_u32 (&lc, MACHO_LC_CODE_SIGNATURE);
    buf_u32 (&lc, 16);
    buf_u32 (&lc, (uint32_t) sig_off);
    buf_u32 (&lc, sb_len);

    if (lc.len != 32 + (size_t) sizeofcmds) { /* sizing drift = a writer bug: fail loudly */
      free (lc.p);
      goto done;
    }
    memcpy (p, lc.p, lc.len);
    free (lc.p);

    /* deterministic LC_UUID: SHA-256 of the image up to the signature
       (UUID field currently zero), truncated, RFC-4122 version/variant
       bits -- reproducible builds without an entropy source */
    {
      macho_sha256_t sh;
      uint8_t digest[32];
      macho_sha256_init (&sh);
      macho_sha256_update (&sh, p, (size_t) sig_off);
      macho_sha256_final (&sh, digest);
      digest[6] = (uint8_t) ((digest[6] & 0x0f) | 0x40);
      digest[8] = (uint8_t) ((digest[8] & 0x3f) | 0x80);
      memcpy (p + uuid_pos, digest, 16);
    }
  }

  /* ---- linker-signed ad-hoc code signature: SuperBlob{CodeDirectory
     v0x20400, SHA-256 4K page hashes over [0, codeLimit)}.  Hashed LAST:
     everything before sig_off (including the UUID) is final. */
  {
    machob_be32 (&sig, MACHO_CSMAGIC_EMBEDDED_SIGNATURE);
    machob_be32 (&sig, sb_len);
    machob_be32 (&sig, 1); /* one blob */
    machob_be32 (&sig, MACHO_CSSLOT_CODEDIRECTORY);
    machob_be32 (&sig, 20); /* CodeDirectory offset in the superblob */
    machob_be32 (&sig, MACHO_CSMAGIC_CODEDIRECTORY);
    machob_be32 (&sig, cd_len);
    machob_be32 (&sig, 0x20400); /* version (execSeg fields) */
    machob_be32 (&sig, MACHO_CS_ADHOC | MACHO_CS_LINKER_SIGNED);
    machob_be32 (&sig, 88 + ident_len); /* hashOffset (slot 0; no special slots) */
    machob_be32 (&sig, 88);             /* identOffset */
    machob_be32 (&sig, 0);              /* nSpecialSlots */
    machob_be32 (&sig, n_code_slots);
    machob_be32 (&sig, (uint32_t) sig_off); /* codeLimit */
    buf_u8 (&sig, 32);                      /* hashSize */
    buf_u8 (&sig, MACHO_CS_HASHTYPE_SHA256);
    buf_u8 (&sig, 0);  /* platform */
    buf_u8 (&sig, 12); /* pageSize log2 */
    machob_be32 (&sig, 0); /* spare2 */
    machob_be32 (&sig, 0); /* scatterOffset */
    machob_be32 (&sig, 0); /* teamOffset */
    machob_be32 (&sig, 0); /* spare3 */
    machob_be64 (&sig, 0); /* codeLimit64 */
    machob_be64 (&sig, 0);             /* execSegBase: __TEXT fileoff */
    machob_be64 (&sig, text_seg_size); /* execSegLimit */
    machob_be64 (&sig, MACHO_CS_EXECSEG_MAIN_BINARY);
    buf_bytes (&sig, ident, ident_len);
    for (uint32_t i = 0; i < n_code_slots; i++) {
      macho_sha256_t sh;
      uint8_t digest[32];
      uint64_t start = (uint64_t) i * MACHO_CS_PAGE_SIZE;
      uint64_t len = sig_off - start < MACHO_CS_PAGE_SIZE ? sig_off - start : MACHO_CS_PAGE_SIZE;
      macho_sha256_init (&sh);
      macho_sha256_update (&sh, p + start, (size_t) len);
      macho_sha256_final (&sh, digest);
      buf_bytes (&sig, digest, 32);
    }
    if (sig.len != sb_len) goto done; /* blob sizing drift = a writer bug */
    memcpy (p + sig_off, sig.p, sig.len);
  }

  *buf = p;
  *size = (size_t) total;
  p = NULL;
  rc = 0;
#undef MACHO_ALIGN
#undef MACHO_SEC_VADDR
#undef MACHO_SLOT_OFF
#undef MACHO_SLOT_SEG
#undef MACHO_SLOT_SEG_OFF

done:
  free (p);
  free (rebases);
  free (binds);
  free (fstarts);
  free (rebase.p);
  free (bind.p);
  free (funcstarts.p);
  free (symtab.p);
  free (strtab.p);
  free (sig.p);
  return rc;
}
