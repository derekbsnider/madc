/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Generic GDB-JIT DWARF debug-object emitter -- see mir-debug.h. */

#include "mir-debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__has_include)
#if __has_include(<elf.h>)
#include <elf.h>
#define MIR_DEBUG_HAVE_ELF 1
#endif
#if __has_include(<sys/mman.h>)
#include <sys/mman.h>
#include <unistd.h>
#define MIR_DEBUG_HAVE_MMAP 1 /* the in-process object loader (MIR_object_load) */
#endif
#endif

/* ===== growable byte buffer + LEB128 ===================================== */
typedef struct {
  unsigned char *p;
  size_t len, cap;
} dwbuf_t;

static int buf_reserve (dwbuf_t *b, size_t n) {
  if (b->len + n <= b->cap) return 0;
  size_t c = b->cap ? b->cap * 2 : 256;
  while (c < b->len + n) c *= 2;
  void *np = realloc (b->p, c);
  if (np == NULL) return -1;
  b->p = np;
  b->cap = c;
  return 0;
}
static void buf_bytes (dwbuf_t *b, const void *d, size_t n) {
  if (buf_reserve (b, n)) return;
  memcpy (b->p + b->len, d, n);
  b->len += n;
}
static void buf_u8 (dwbuf_t *b, uint8_t v) { buf_bytes (b, &v, 1); }
static void buf_u16 (dwbuf_t *b, uint16_t v) { buf_bytes (b, &v, 2); }
static void buf_u32 (dwbuf_t *b, uint32_t v) { buf_bytes (b, &v, 4); }
static void buf_u64 (dwbuf_t *b, uint64_t v) { buf_bytes (b, &v, 8); }
static void buf_str (dwbuf_t *b, const char *s) { buf_bytes (b, s ? s : "", (s ? strlen (s) : 0) + 1); }
static void buf_uleb (dwbuf_t *b, uint64_t v) {
  do {
    uint8_t x = v & 0x7f;
    v >>= 7;
    if (v) x |= 0x80;
    buf_u8 (b, x);
  } while (v);
}
static void buf_sleb (dwbuf_t *b, int64_t v) {
  for (;;) {
    uint8_t x = v & 0x7f;
    v >>= 7; /* arithmetic */
    int done = (v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40));
    if (!done) x |= 0x80;
    buf_u8 (b, x);
    if (done) break;
  }
}

/* ===== DWARF constants =================================================== */
enum {
  DW_TAG_array_type = 0x01,
  DW_TAG_enumeration_type = 0x04,
  DW_TAG_formal_parameter = 0x05,
  DW_TAG_member = 0x0d,
  DW_TAG_pointer_type = 0x0f,
  DW_TAG_compile_unit = 0x11,
  DW_TAG_structure_type = 0x13,
  DW_TAG_subroutine_type = 0x15,
  DW_TAG_typedef = 0x16,
  DW_TAG_union_type = 0x17,
  DW_TAG_unspecified_type = 0x3b,
  DW_TAG_subrange_type = 0x21,
  DW_TAG_base_type = 0x24,
  DW_TAG_subprogram = 0x2e,
  DW_TAG_variable = 0x34,
  DW_TAG_enumerator = 0x28,
  DW_CHILDREN_no = 0,
  DW_CHILDREN_yes = 1,
  DW_AT_location = 0x02,
  DW_AT_name = 0x03,
  DW_AT_byte_size = 0x0b,
  DW_AT_bit_size = 0x0d,
  DW_AT_stmt_list = 0x10,
  DW_AT_low_pc = 0x11,
  DW_AT_high_pc = 0x12,
  DW_AT_language = 0x13,
  DW_AT_comp_dir = 0x1b,
  DW_AT_const_value = 0x1c,
  DW_AT_producer = 0x25,
  DW_AT_count = 0x37,
  DW_AT_data_member_location = 0x38,
  DW_AT_data_bit_offset = 0x6b,
  DW_AT_encoding = 0x3e,
  DW_AT_external = 0x3f,
  DW_AT_frame_base = 0x40,
  DW_AT_type = 0x49,
  DW_FORM_addr = 0x01,
  DW_FORM_data2 = 0x05,
  DW_FORM_data1 = 0x0b,
  DW_FORM_sdata = 0x0d,
  DW_FORM_udata = 0x0f,
  DW_FORM_string = 0x08,
  DW_FORM_flag = 0x0c,
  DW_FORM_ref4 = 0x13,
  DW_FORM_sec_offset = 0x17,
  DW_FORM_exprloc = 0x18,
  DW_ATE_boolean = 0x02,
  DW_ATE_float = 0x04,
  DW_ATE_signed = 0x05,
  DW_ATE_signed_char = 0x06,
  DW_ATE_unsigned = 0x07,
  DW_ATE_unsigned_char = 0x08,
  DW_OP_deref = 0x06,
  DW_OP_plus_uconst = 0x23,
  DW_OP_fbreg = 0x91,
  DW_OP_reg6 = 0x56,  /* x86_64 rbp */
  DW_OP_reg29 = 0x6d, /* aarch64 x29 */
  DW_LANG_C99 = 0x0c,
  DW_LNS_copy = 1,
  DW_LNS_advance_pc = 2,
  DW_LNS_advance_line = 3,
  DW_LNS_set_file = 4,
  DW_LNS_set_prologue_end = 10,
  DW_LNE_end_sequence = 1,
  DW_LNE_set_address = 2,
};

/* abbrev codes */
enum {
  A_CU = 1, A_SUBPROG, A_BASE, A_PTR, A_VAR, A_PARAM, A_VOIDT,
  A_STRUCT, A_UNION, A_MEMBER, A_MEMBERBF, A_ARRAY, A_SUBRANGE, A_SUBRANGE0,
  A_ENUM, A_ENUMERATOR, A_SUBR, A_FPARAM, A_TYPEDEF,
};

#if defined(__aarch64__)
#define MIR_DEBUG_FP_OP DW_OP_reg29
#define MIR_DEBUG_EM EM_AARCH64
#elif defined(__x86_64__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_X86_64
#elif defined(__riscv) && __riscv_xlen == 64
#define MIR_DEBUG_FP_OP DW_OP_reg6 /* not validated; see frontend note */
#define MIR_DEBUG_EM EM_RISCV
#elif defined(__powerpc64__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_PPC64
#elif defined(__s390x__)
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM EM_S390
#else
#define MIR_DEBUG_FP_OP DW_OP_reg6
#define MIR_DEBUG_EM 0
#endif

/* ===== builder state ===================================================== */
enum { DT_VOID, DT_BASE, DT_PTR, DT_ARRAY, DT_STRUCT, DT_UNION, DT_ENUM, DT_TYPEDEF, DT_FUNC };

typedef struct {
  int kind, enc;
  int64_t size, count;
  uint32_t ref;     /* ptr/array/typedef/func-return target */
  char *name;
  int first_member, last_member; /* members (struct/union), enumerators (enum), params (func) */
} dwtype_t;

typedef struct {
  char *name;
  uint32_t type;
  int64_t off;        /* member byte offset, or enumerator value */
  int bit_size, bit_off;
  int is_enumerator;  /* off is a value, type unused */
  int next;
} dwmember_t;

typedef struct {
  char *name;
  const void *addr;
  size_t size;
  MIR_line_map_t *line_map;
  size_t line_map_len;
  int first_var, last_var;
} dwfunc_t;

typedef struct {
  char *name;
  int is_param, deref_p;
  uint32_t type;
  int64_t fp_offset;
  uint64_t member_offset; /* added (DW_OP_plus_uconst) after the optional deref */
  int next;
} dwvar_t;

#define DEFVEC(T, field)                                                         \
  T *field;                                                                      \
  int n_##field, c_##field
struct MIR_debug {
  DEFVEC (char *, files);
  DEFVEC (dwtype_t, types);
  DEFVEC (dwmember_t, members);
  DEFVEC (dwfunc_t, funcs);
  DEFVEC (dwvar_t, vars);
  int cur_func;
};
#undef DEFVEC

#define VEC_PUSH(d, field, val)                                                  \
  do {                                                                           \
    if ((d)->n_##field == (d)->c_##field) {                                      \
      (d)->c_##field = (d)->c_##field ? (d)->c_##field * 2 : 16;                 \
      (d)->field = realloc ((d)->field, (size_t) (d)->c_##field * sizeof (*(d)->field)); \
    }                                                                            \
    (d)->field[(d)->n_##field++] = (val);                                        \
  } while (0)

static char *dw_strdup (const char *s) {
  if (s == NULL) return NULL;
  size_t n = strlen (s) + 1;
  char *r = malloc (n);
  if (r != NULL) memcpy (r, s, n);
  return r;
}

MIR_debug_t MIR_debug_init (void) {
  if (MIR_DEBUG_EM == 0) return NULL;
  MIR_debug_t d = calloc (1, sizeof (struct MIR_debug));
  if (d == NULL) return NULL;
  /* type 0 == void */
  dwtype_t v = {.kind = DT_VOID, .ref = 0, .count = -1, .first_member = -1, .last_member = -1,
                .name = dw_strdup ("void")};
  VEC_PUSH (d, types, v);
  return d;
}

void MIR_debug_destroy (MIR_debug_t d) {
  if (d == NULL) return;
  for (int i = 0; i < d->n_files; i++) free (d->files[i]);
  free (d->files);
  for (int i = 0; i < d->n_types; i++) free (d->types[i].name);
  free (d->types);
  for (int i = 0; i < d->n_members; i++) free (d->members[i].name);
  free (d->members);
  for (int i = 0; i < d->n_funcs; i++) {
    free (d->funcs[i].name);
    free (d->funcs[i].line_map);
  }
  free (d->funcs);
  for (int i = 0; i < d->n_vars; i++) free (d->vars[i].name);
  free (d->vars);
  free (d);
}

uint32_t MIR_debug_add_file (MIR_debug_t d, const char *path) {
  for (int i = 0; i < d->n_files; i++)
    if (strcmp (d->files[i], path) == 0) return (uint32_t) (i + 1);
  char *s = dw_strdup (path);
  VEC_PUSH (d, files, s);
  return (uint32_t) d->n_files;
}

static uint32_t new_type (MIR_debug_t d, int kind) {
  dwtype_t t = {.kind = kind, .ref = 0, .count = -1, .first_member = -1, .last_member = -1};
  VEC_PUSH (d, types, t);
  return (uint32_t) (d->n_types - 1);
}

MIR_debug_type_t MIR_debug_base_type (MIR_debug_t d, const char *name, MIR_debug_encoding_t enc,
                                      int byte_size) {
  static const int map[] = {0, DW_ATE_signed, DW_ATE_unsigned, DW_ATE_float, DW_ATE_boolean,
                            DW_ATE_signed_char, DW_ATE_unsigned_char};
  uint32_t i = new_type (d, DT_BASE);
  d->types[i].enc = (enc >= 1 && enc <= 6) ? map[enc] : DW_ATE_signed;
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_pointer_type (MIR_debug_t d, MIR_debug_type_t pointee) {
  uint32_t i = new_type (d, DT_PTR);
  d->types[i].ref = pointee;
  d->types[i].size = sizeof (void *);
  return i;
}
MIR_debug_type_t MIR_debug_array_type (MIR_debug_t d, MIR_debug_type_t elem, int64_t count) {
  uint32_t i = new_type (d, DT_ARRAY);
  d->types[i].ref = elem;
  d->types[i].count = count;
  return i;
}
MIR_debug_type_t MIR_debug_typedef_type (MIR_debug_t d, const char *name, MIR_debug_type_t ref) {
  uint32_t i = new_type (d, DT_TYPEDEF);
  d->types[i].ref = ref;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_struct_type (MIR_debug_t d, const char *name, int64_t byte_size,
                                        int is_union) {
  uint32_t i = new_type (d, is_union ? DT_UNION : DT_STRUCT);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_enum_type (MIR_debug_t d, const char *name, int64_t byte_size) {
  uint32_t i = new_type (d, DT_ENUM);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_debug_type_t MIR_debug_func_type (MIR_debug_t d, MIR_debug_type_t ret_type) {
  uint32_t i = new_type (d, DT_FUNC);
  d->types[i].ref = ret_type;
  return i;
}

static void append_member (MIR_debug_t d, MIR_debug_type_t agg, dwmember_t m) {
  m.next = -1;
  VEC_PUSH (d, members, m);
  int idx = d->n_members - 1;
  if (d->types[agg].first_member < 0)
    d->types[agg].first_member = idx;
  else
    d->members[d->types[agg].last_member].next = idx;
  d->types[agg].last_member = idx;
}

void MIR_debug_add_member (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                           MIR_debug_type_t type, int64_t byte_offset) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .off = byte_offset};
  append_member (d, agg, m);
}
void MIR_debug_add_bitfield (MIR_debug_t d, MIR_debug_type_t agg, const char *name,
                             MIR_debug_type_t type, int64_t bit_offset, int bit_size) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .bit_off = (int) bit_offset,
                  .bit_size = bit_size};
  append_member (d, agg, m);
}
void MIR_debug_add_enumerator (MIR_debug_t d, MIR_debug_type_t en, const char *name, int64_t value) {
  dwmember_t m = {.name = dw_strdup (name), .off = value, .is_enumerator = 1};
  append_member (d, en, m);
}
void MIR_debug_add_param_type (MIR_debug_t d, MIR_debug_type_t fn, MIR_debug_type_t type) {
  dwmember_t m = {.type = type};
  append_member (d, fn, m);
}

void MIR_debug_add_func (MIR_debug_t d, const char *name, const void *addr, size_t size,
                         const MIR_line_map_t *line_map, size_t line_map_len) {
  MIR_line_map_t *lm = NULL;
  if (line_map != NULL && line_map_len != 0) {
    lm = malloc (line_map_len * sizeof (MIR_line_map_t));
    if (lm != NULL) memcpy (lm, line_map, line_map_len * sizeof (MIR_line_map_t));
  }
  dwfunc_t f = {.name = dw_strdup (name), .addr = addr, .size = size, .line_map = lm,
                .line_map_len = lm ? line_map_len : 0, .first_var = -1, .last_var = -1};
  VEC_PUSH (d, funcs, f);
  d->cur_func = d->n_funcs - 1;
}

void MIR_debug_add_var (MIR_debug_t d, const char *name, int is_param, MIR_debug_type_t type,
                        int64_t fp_offset, int deref_p, uint64_t member_offset) {
  if (d->n_funcs == 0) return;
  dwvar_t v = {.name = dw_strdup (name), .is_param = is_param, .type = type, .fp_offset = fp_offset,
               .member_offset = member_offset, .deref_p = deref_p, .next = -1};
  VEC_PUSH (d, vars, v);
  int idx = d->n_vars - 1;
  dwfunc_t *f = &d->funcs[d->cur_func];
  if (f->first_var < 0)
    f->first_var = idx;
  else
    d->vars[f->last_var].next = idx;
  f->last_var = idx;
}

/* ===== DWARF emission ==================================================== */

/* How the generators interpret dwfunc_t.addr and where address values land.
   The GDB-JIT path (MIR_debug_emit) uses absolute JIT addresses; the AOT
   paths (MIR_object_set_debug) store .text section offsets and bias them to
   the artifact's address space -- 0 for the .o (the slots then carry section
   offsets and get .rela.debug_* relocations), the final link-time text vaddr
   for executables / shared objects (gdb rebases ET_DYN itself). */
typedef struct {
  uint64_t bias;            /* added to every emitted address value */
  const uint8_t *code_base; /* readable code bytes in offset mode (the frame
                               emitter's prologue detection); NULL: fn->addr
                               is itself the readable pointer */
  int offset_mode_p;        /* fn->addr holds a .text offset; offset 0 is a
                               valid function, not the "no code" sentinel */
  /* When non-NULL, called for every 8-byte code-address slot: sec is
     0 .debug_info / 1 .debug_line / 2 .debug_frame, pos the slot's offset in
     that section, value the pre-bias address value.  The .o emitter turns
     these into relocations against the .text section symbol. */
  void (*addr_slot) (void *env, int sec, size_t pos, uint64_t value);
  /* When non-NULL, called for every 4-byte cross-debug-section offset slot
     (CU header abbrev offset, DW_AT_stmt_list, .debug_frame FDE CIE
     pointers): src/tgt use the sec id space above plus 3 = .debug_abbrev,
     value the offset within the target section.  The .o emitter turns these
     into R_X86_64_32 relocations against the target debug section's symbol
     -- without them the offsets are valid only while one CU sits at
     section offset 0, which is exactly what breaks multi-object links. */
  void (*sec_ref) (void *env, int src, size_t pos, int tgt, uint32_t value);
  void *env;
} dwgen_t;

static void dw_addr (const dwgen_t *g, dwbuf_t *b, int sec, uint64_t value) {
  if (g->addr_slot != NULL) g->addr_slot (g->env, sec, b->len, value);
  buf_u64 (b, g->bias + value);
}

static void dw_secref (const dwgen_t *g, dwbuf_t *b, int src, int tgt, uint32_t value) {
  if (g->sec_ref != NULL) g->sec_ref (g->env, src, b->len, tgt, value);
  buf_u32 (b, value);
}

static int dw_func_skip_p (const dwgen_t *g, const dwfunc_t *fn) {
  return !g->offset_mode_p && fn->addr == NULL;
}

/* [text_lo, text_lo + text_size) over the described functions, in the addr
   space fn->addr uses (absolute or section offsets). */
static void dw_text_range (MIR_debug_t d, const dwgen_t *g, uint64_t *text_lo,
                           uint64_t *text_size) {
  uint64_t lo = UINT64_MAX, hi = 0;
  for (int i = 0; i < d->n_funcs; i++) {
    if (dw_func_skip_p (g, &d->funcs[i])) continue;
    uint64_t a = (uint64_t) (uintptr_t) d->funcs[i].addr;
    uint64_t e = a + (d->funcs[i].size ? d->funcs[i].size : 1);
    if (a < lo) lo = a;
    if (e > hi) hi = e;
  }
  if (lo == UINT64_MAX) lo = hi = 0;
  *text_lo = lo;
  *text_size = hi > lo ? hi - lo : 0;
}

static void ab_hdr (dwbuf_t *b, int code, int tag, int children) {
  buf_uleb (b, code);
  buf_uleb (b, tag);
  buf_u8 (b, children);
}
static void ab_attr (dwbuf_t *b, int at, int form) {
  buf_uleb (b, at);
  buf_uleb (b, form);
}

static void emit_abbrev (dwbuf_t *b) {
  ab_hdr (b, A_CU, DW_TAG_compile_unit, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_producer, DW_FORM_string);
  ab_attr (b, DW_AT_language, DW_FORM_data2);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_comp_dir, DW_FORM_string);
  ab_attr (b, DW_AT_low_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_high_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_stmt_list, DW_FORM_sec_offset);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBPROG, DW_TAG_subprogram, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_low_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_high_pc, DW_FORM_addr);
  ab_attr (b, DW_AT_frame_base, DW_FORM_exprloc);
  ab_attr (b, DW_AT_external, DW_FORM_flag);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_BASE, DW_TAG_base_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_encoding, DW_FORM_data1);
  ab_attr (b, DW_AT_byte_size, DW_FORM_data1);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_PTR, DW_TAG_pointer_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_byte_size, DW_FORM_data1);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  for (int v = 0; v < 2; v++) {
    ab_hdr (b, v ? A_PARAM : A_VAR, v ? DW_TAG_formal_parameter : DW_TAG_variable, DW_CHILDREN_no);
    ab_attr (b, DW_AT_name, DW_FORM_string);
    ab_attr (b, DW_AT_type, DW_FORM_ref4);
    ab_attr (b, DW_AT_location, DW_FORM_exprloc);
    buf_uleb (b, 0); buf_uleb (b, 0);
  }

  ab_hdr (b, A_VOIDT, DW_TAG_unspecified_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  buf_uleb (b, 0); buf_uleb (b, 0);

  for (int u = 0; u < 2; u++) {
    ab_hdr (b, u ? A_UNION : A_STRUCT, u ? DW_TAG_union_type : DW_TAG_structure_type,
            DW_CHILDREN_yes);
    ab_attr (b, DW_AT_name, DW_FORM_string);
    ab_attr (b, DW_AT_byte_size, DW_FORM_udata);
    buf_uleb (b, 0); buf_uleb (b, 0);
  }

  ab_hdr (b, A_MEMBER, DW_TAG_member, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  ab_attr (b, DW_AT_data_member_location, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_MEMBERBF, DW_TAG_member, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  ab_attr (b, DW_AT_data_bit_offset, DW_FORM_udata);
  ab_attr (b, DW_AT_bit_size, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ARRAY, DW_TAG_array_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBRANGE, DW_TAG_subrange_type, DW_CHILDREN_no);
  ab_attr (b, DW_AT_count, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBRANGE0, DW_TAG_subrange_type, DW_CHILDREN_no);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ENUM, DW_TAG_enumeration_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_byte_size, DW_FORM_udata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_ENUMERATOR, DW_TAG_enumerator, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_const_value, DW_FORM_sdata);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_SUBR, DW_TAG_subroutine_type, DW_CHILDREN_yes);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_FPARAM, DW_TAG_formal_parameter, DW_CHILDREN_no);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  ab_hdr (b, A_TYPEDEF, DW_TAG_typedef, DW_CHILDREN_no);
  ab_attr (b, DW_AT_name, DW_FORM_string);
  ab_attr (b, DW_AT_type, DW_FORM_ref4);
  buf_uleb (b, 0); buf_uleb (b, 0);

  buf_uleb (b, 0); /* end of table */
}

typedef struct {
  size_t pos;
  uint32_t target;
} tfix_t;

static void emit_info (MIR_debug_t d, dwbuf_t *b, const dwgen_t *g, uint64_t text_lo,
                       uint64_t text_size, const char *cu_name) {
  size_t unit_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_len = b->len;
  buf_u16 (b, 4);
  dw_secref (g, b, 0, 3, 0); /* abbrev offset (this emission's table is at 0) */
  buf_u8 (b, 8);
  buf_uleb (b, A_CU);
  buf_str (b, "mir-debug");
  buf_u16 (b, DW_LANG_C99);
  buf_str (b, cu_name ? cu_name : "");
  buf_str (b, "");
  dw_addr (g, b, 0, text_lo);
  dw_addr (g, b, 0, text_lo + text_size);
  dw_secref (g, b, 0, 1, 0); /* stmt_list (this emission's line program is at 0) */

  uint32_t *toff = d->n_types ? calloc ((size_t) d->n_types, sizeof (uint32_t)) : NULL;
  tfix_t *fix = NULL;
  int nfix = 0, cfix = 0;
#define REF(target_)                                                                          \
  do {                                                                                        \
    if (nfix == cfix) { cfix = cfix ? cfix * 2 : 64; fix = realloc (fix, (size_t) cfix * sizeof (tfix_t)); } \
    fix[nfix].pos = b->len; fix[nfix].target = (target_); nfix++; buf_u32 (b, 0);             \
  } while (0)

  for (int t = 0; t < d->n_types; t++) {
    dwtype_t *dt = &d->types[t];
    toff[t] = (uint32_t) (b->len - unit_len_pos);
    switch (dt->kind) {
    case DT_BASE:
      buf_uleb (b, A_BASE); buf_str (b, dt->name); buf_u8 (b, (uint8_t) dt->enc);
      buf_u8 (b, (uint8_t) dt->size);
      break;
    case DT_PTR:
      buf_uleb (b, A_PTR); buf_u8 (b, (uint8_t) (dt->size ? dt->size : sizeof (void *)));
      REF (dt->ref);
      break;
    case DT_TYPEDEF:
      buf_uleb (b, A_TYPEDEF); buf_str (b, dt->name); REF (dt->ref);
      break;
    case DT_STRUCT: case DT_UNION:
      buf_uleb (b, dt->kind == DT_UNION ? A_UNION : A_STRUCT);
      buf_str (b, dt->name);
      buf_uleb (b, (uint64_t) dt->size);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        dwmember_t *dm = &d->members[m];
        if (dm->bit_size > 0) {
          buf_uleb (b, A_MEMBERBF); buf_str (b, dm->name); REF (dm->type);
          buf_uleb (b, (uint64_t) dm->bit_off); buf_uleb (b, (uint64_t) dm->bit_size);
        } else {
          buf_uleb (b, A_MEMBER); buf_str (b, dm->name); REF (dm->type);
          buf_uleb (b, (uint64_t) dm->off);
        }
      }
      buf_u8 (b, 0);
      break;
    case DT_ARRAY:
      buf_uleb (b, A_ARRAY); REF (dt->ref);
      if (dt->count >= 0) { buf_uleb (b, A_SUBRANGE); buf_uleb (b, (uint64_t) dt->count); }
      else buf_uleb (b, A_SUBRANGE0);
      buf_u8 (b, 0);
      break;
    case DT_ENUM:
      buf_uleb (b, A_ENUM); buf_str (b, dt->name); buf_uleb (b, (uint64_t) dt->size);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        buf_uleb (b, A_ENUMERATOR); buf_str (b, d->members[m].name); buf_sleb (b, d->members[m].off);
      }
      buf_u8 (b, 0);
      break;
    case DT_FUNC:
      buf_uleb (b, A_SUBR); REF (dt->ref);
      for (int m = dt->first_member; m >= 0; m = d->members[m].next) {
        buf_uleb (b, A_FPARAM); REF (d->members[m].type);
      }
      buf_u8 (b, 0);
      break;
    default: /* DT_VOID */
      buf_uleb (b, A_VOIDT); buf_str (b, dt->name ? dt->name : "void");
      break;
    }
  }
  for (int f = 0; f < nfix; f++)
    if (toff != NULL) memcpy (b->p + fix[f].pos, &toff[fix[f].target], 4);
  free (fix);

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (dw_func_skip_p (g, fn)) continue;
    buf_uleb (b, A_SUBPROG);
    buf_str (b, fn->name);
    dw_addr (g, b, 0, (uint64_t) (uintptr_t) fn->addr);
    dw_addr (g, b, 0, (uint64_t) (uintptr_t) fn->addr + fn->size);
    buf_u8 (b, 1); buf_u8 (b, MIR_DEBUG_FP_OP);
    buf_u8 (b, 1);
    for (int v = fn->first_var; v >= 0; v = d->vars[v].next) {
      dwvar_t *dv = &d->vars[v];
      buf_uleb (b, dv->is_param ? A_PARAM : A_VAR);
      buf_str (b, dv->name);
      buf_u32 (b, toff ? toff[dv->type] : 0);
      dwbuf_t e = {0};
      buf_u8 (&e, DW_OP_fbreg);
      buf_sleb (&e, dv->fp_offset);
      if (dv->deref_p) buf_u8 (&e, DW_OP_deref);
      if (dv->member_offset != 0) {
        buf_u8 (&e, DW_OP_plus_uconst);
        buf_uleb (&e, dv->member_offset);
      }
      buf_uleb (b, e.len);
      buf_bytes (b, e.p, e.len);
      free (e.p);
    }
    buf_u8 (b, 0);
  }
  buf_u8 (b, 0);
  free (toff);
  uint32_t unit_len = (uint32_t) (b->len - after_len);
  memcpy (b->p + unit_len_pos, &unit_len, 4);
#undef REF
}

static void emit_line (MIR_debug_t d, dwbuf_t *b, const dwgen_t *g) {
  size_t unit_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_len = b->len;
  buf_u16 (b, 4);
  size_t hdr_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_hdr_len = b->len;
  buf_u8 (b, 1);
  buf_u8 (b, 1);
  buf_u8 (b, 1);
  buf_u8 (b, (uint8_t) (int8_t) -5);
  buf_u8 (b, 14);
  buf_u8 (b, 13);
  static const uint8_t std_lens[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
  buf_bytes (b, std_lens, sizeof std_lens);
  buf_u8 (b, 0); /* include_directories */
  for (int i = 0; i < d->n_files; i++) {
    buf_str (b, d->files[i]);
    buf_uleb (b, 0); buf_uleb (b, 0); buf_uleb (b, 0);
  }
  buf_u8 (b, 0);
  uint32_t hdr_len = (uint32_t) (b->len - after_hdr_len);
  memcpy (b->p + hdr_len_pos, &hdr_len, 4);

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (dw_func_skip_p (g, fn) || fn->line_map == NULL || fn->line_map_len == 0) continue;
    buf_u8 (b, 0); buf_uleb (b, 9); buf_u8 (b, DW_LNE_set_address);
    dw_addr (g, b, 1, (uint64_t) (uintptr_t) fn->addr);
    uint32_t cur_off = 0, cur_line = 1, cur_file = 1;
    /* entry-point row carrying the first statement's line + prologue_end */
    {
      MIR_line_map_t *e0 = &fn->line_map[0];
      uint32_t f0 = e0->file_id ? e0->file_id : 1;
      if (f0 != cur_file) { buf_u8 (b, DW_LNS_set_file); buf_uleb (b, f0); cur_file = f0; }
      buf_u8 (b, DW_LNS_advance_line); buf_sleb (b, (int64_t) e0->line - (int64_t) cur_line);
      cur_line = e0->line;
      buf_u8 (b, DW_LNS_copy);
    }
    int prologue_marked = 0;
    for (size_t j = 0; j < fn->line_map_len; j++) {
      MIR_line_map_t *e = &fn->line_map[j];
      uint32_t file = e->file_id ? e->file_id : 1;
      if (file != cur_file) { buf_u8 (b, DW_LNS_set_file); buf_uleb (b, file); cur_file = file; }
      if (e->line != cur_line) {
        buf_u8 (b, DW_LNS_advance_line); buf_sleb (b, (int64_t) e->line - (int64_t) cur_line);
        cur_line = e->line;
      }
      if (e->code_offset != cur_off) {
        buf_u8 (b, DW_LNS_advance_pc); buf_uleb (b, (uint64_t) (e->code_offset - cur_off));
        cur_off = e->code_offset;
      }
      if (!prologue_marked && e->code_offset != 0) { buf_u8 (b, DW_LNS_set_prologue_end); prologue_marked = 1; }
      buf_u8 (b, DW_LNS_copy);
    }
    if (fn->size > cur_off) { buf_u8 (b, DW_LNS_advance_pc); buf_uleb (b, (uint64_t) (fn->size - cur_off)); }
    buf_u8 (b, 0); buf_uleb (b, 1); buf_u8 (b, DW_LNE_end_sequence);
  }
  uint32_t unit_len = (uint32_t) (b->len - after_len);
  memcpy (b->p + unit_len_pos, &unit_len, 4);
}

#ifdef MIR_DEBUG_HAVE_ELF
/* One ELF section descriptor for the output object assembler below.  A named
   type (rather than an anonymous struct + typeof) so the file stays compilable
   by C frontends without the GNU typeof extension -- e.g. MIR's own c2m, which
   builds mir-debug.c as part of its self-bootstrap. */
typedef struct {
  const char *name;
  uint32_t type, link, info, align;
  uint64_t flags, addr, entsize;
  const dwbuf_t *body;
  uint64_t size;
} dwsec_t;

#define DWSEC_MAX 24

/* Assemble an ELF object from section descriptors S[0..ns-1] (S[0] must be
   the zeroed null section).  Appends the .shstrtab section itself, so the
   caller must leave one free slot (ns + 1 <= DWSEC_MAX).  On success returns
   0 and sets *buf (malloc'd, caller-owned) and *size.  This is the one ELF
   writer core -- it serves both the GDB-JIT debug object (MIR_debug_emit) and
   the AOT relocatable object (MIR_object_emit). */
static int elf_assemble (dwsec_t *S, int ns, uint16_t e_machine, void **buf, size_t *size) {
  dwbuf_t shstr = {0};
  uint32_t name_off[DWSEC_MAX];

  if (ns + 1 > DWSEC_MAX) return -1;
  buf_u8 (&shstr, 0);
  name_off[0] = 0;
  for (int i = 1; i < ns; i++) {
    name_off[i] = (uint32_t) shstr.len;
    buf_str (&shstr, S[i].name);
  }
  int i_shstr = ns;
  name_off[i_shstr] = (uint32_t) shstr.len;
  buf_str (&shstr, ".shstrtab");
  S[ns++] = (dwsec_t) {".shstrtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &shstr, shstr.len};

  size_t off = sizeof (Elf64_Ehdr);
  uint64_t sec_off[DWSEC_MAX] = {0};
  for (int i = 1; i < ns; i++) {
    if (S[i].type == SHT_NOBITS) {
      sec_off[i] = off;
      continue;
    }
    size_t align = S[i].align > 8 ? S[i].align : 8;
    off = (off + align - 1) & ~(align - 1);
    sec_off[i] = off;
    off += S[i].size;
  }
  off = (off + 7) & ~(size_t) 7;
  size_t shoff = off, total = shoff + (size_t) ns * sizeof (Elf64_Shdr);

  unsigned char *p = calloc (1, total);
  if (p == NULL) {
    free (shstr.p);
    return -1;
  }
  Elf64_Ehdr *eh = (Elf64_Ehdr *) p;
  eh->e_ident[EI_MAG0] = ELFMAG0;
  eh->e_ident[EI_MAG1] = ELFMAG1;
  eh->e_ident[EI_MAG2] = ELFMAG2;
  eh->e_ident[EI_MAG3] = ELFMAG3;
  eh->e_ident[EI_CLASS] = ELFCLASS64;
  eh->e_ident[EI_DATA] = ELFDATA2LSB;
  eh->e_ident[EI_VERSION] = EV_CURRENT;
  eh->e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh->e_type = ET_REL;
  eh->e_machine = e_machine;
  eh->e_version = EV_CURRENT;
  eh->e_shoff = shoff;
  eh->e_ehsize = sizeof (Elf64_Ehdr);
  eh->e_shentsize = sizeof (Elf64_Shdr);
  eh->e_shnum = (Elf64_Half) ns;
  eh->e_shstrndx = (Elf64_Half) i_shstr;

  Elf64_Shdr *sh = (Elf64_Shdr *) (p + shoff);
  for (int i = 1; i < ns; i++) {
    if (S[i].type != SHT_NOBITS && S[i].body != NULL && S[i].size)
      memcpy (p + sec_off[i], S[i].body->p, S[i].size);
    sh[i].sh_name = name_off[i];
    sh[i].sh_type = S[i].type;
    sh[i].sh_flags = S[i].flags;
    sh[i].sh_addr = S[i].addr;
    sh[i].sh_offset = (S[i].type == SHT_NOBITS) ? 0 : sec_off[i];
    sh[i].sh_size = S[i].size;
    sh[i].sh_link = S[i].link;
    sh[i].sh_info = S[i].info;
    sh[i].sh_addralign = S[i].align;
    sh[i].sh_entsize = S[i].entsize;
  }
  free (shstr.p);
  *buf = p;
  *size = total;
  return 0;
}

/* ===== .debug_frame (CFI) ================================================ */
/* Authoritative unwind info for the JIT frames (x86-64 only for now).  gdb's
   heuristic prologue analysis does not recognize MIR's FP prologue
   (mov %rbp,-8(%rsp); lea -8(%rsp),%rbp -- no push), so without CFI a
   backtrace cannot leave frame 0 even though the in-memory FP chain is
   canonical (saved rbp at [rbp], return address at [rbp+8]).  Debug codegen
   (spill-all) forces keep_fp_p, so every function with a prologue gets the
   same fixed shape and needs only a template FDE: CFA=rsp+8 at entry, rbp
   saved at CFA-16 after insn 1 (+5), CFA=rbp+16 after insn 2 (+10).  A
   function whose entry bytes do not match the signature (a prologue-less
   leaf) keeps the CIE default row, which is exact for it.  The 1-2 insn
   epilogue window where the restored rbp mislabels the CFA is accepted
   debug-tier imprecision. */
enum {
  DW_CFA_nop = 0x00,
  DW_CFA_def_cfa = 0x0c, /* ULEB reg, ULEB offset */
  DW_CFA_advance_loc = 0x40, /* low 6 bits = code delta */
  DW_CFA_offset = 0x80, /* low 6 bits = reg; ULEB factored offset */
};

static void buf_patch_u32 (dwbuf_t *b, size_t pos, uint32_t v) { memcpy (b->p + pos, &v, 4); }

static void emit_frame (MIR_debug_t d, dwbuf_t *b, const dwgen_t *g) {
#if defined(__x86_64__)
  static const uint8_t fp_prologue[] = {0x48, 0x89, 0x6c, 0x24, 0xf8,  /* mov %rbp,-0x8(%rsp) */
                                        0x48, 0x8d, 0x6c, 0x24, 0xf8}; /* lea -0x8(%rsp),%rbp */
  /* CIE (DWARF32 .debug_frame v1): CFA = rsp+8, return address at CFA-8 */
  size_t cie_off = b->len, len_pos = b->len;
  buf_u32 (b, 0); /* length, backpatched */
  buf_u32 (b, 0xffffffff); /* CIE id */
  buf_u8 (b, 1); /* version */
  buf_u8 (b, 0); /* augmentation "" */
  buf_uleb (b, 1); /* code alignment factor */
  buf_sleb (b, -8); /* data alignment factor */
  buf_u8 (b, 16); /* return address register (rip) */
  buf_u8 (b, DW_CFA_def_cfa);
  buf_uleb (b, 7); /* rsp */
  buf_uleb (b, 8); /* CFA = rsp+8 */
  buf_u8 (b, DW_CFA_offset | 16);
  buf_uleb (b, 1); /* ra at CFA + 1*(-8) */
  while ((b->len - len_pos) % 8 != 0) buf_u8 (b, DW_CFA_nop);
  buf_patch_u32 (b, len_pos, (uint32_t) (b->len - len_pos - 4));

  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    if (dw_func_skip_p (g, fn) || fn->size == 0) continue;
    size_t fde_len_pos = b->len;
    buf_u32 (b, 0); /* length, backpatched */
    dw_secref (g, b, 2, 2, (uint32_t) cie_off); /* CIE pointer (section offset) */
    dw_addr (g, b, 2, (uint64_t) (uintptr_t) fn->addr); /* initial location */
    buf_u64 (b, (uint64_t) fn->size); /* address range */
    /* prologue detection peeks at the machine code: in offset mode fn->addr
       is a .text offset, readable only through the builder's code bytes */
    const uint8_t *fbytes = g->code_base != NULL ? g->code_base + (uintptr_t) fn->addr
                                                 : (const uint8_t *) fn->addr;
    if (fn->size >= sizeof (fp_prologue)
        && memcmp (fbytes, fp_prologue, sizeof (fp_prologue)) == 0) {
      buf_u8 (b, DW_CFA_advance_loc | 5);
      buf_u8 (b, DW_CFA_offset | 6); /* rbp saved... */
      buf_uleb (b, 2); /* ...at CFA + 2*(-8) */
      buf_u8 (b, DW_CFA_advance_loc | 5);
      buf_u8 (b, DW_CFA_def_cfa);
      buf_uleb (b, 6); /* rbp */
      buf_uleb (b, 16); /* CFA = rbp+16 */
    }
    while ((b->len - fde_len_pos) % 8 != 0) buf_u8 (b, DW_CFA_nop);
    buf_patch_u32 (b, fde_len_pos, (uint32_t) (b->len - fde_len_pos - 4));
  }
#else
  (void) d;
  (void) b;
  (void) g;
#endif
}

int MIR_debug_emit (MIR_debug_t d, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (d == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM == 0) return -1;

  /* GDB-JIT mode: absolute JIT addresses, fn->addr readable directly;
     cross-section offsets stay in place (single emission, all at 0) */
  dwgen_t g = {0, NULL, 0, NULL, NULL, NULL};
  uint64_t text_base, text_size;
  dw_text_range (d, &g, &text_base, &text_size);

  /* section bodies */
  dwbuf_t strtab = {0}, symtab = {0}, abbrev = {0}, info = {0}, line = {0}, frame = {0};
  buf_u8 (&strtab, 0);
  Elf64_Sym z = {0};
  buf_bytes (&symtab, &z, sizeof z);
  for (int i = 0; i < d->n_funcs; i++) {
    dwfunc_t *fn = &d->funcs[i];
    Elf64_Sym s = {0};
    s.st_name = (Elf64_Word) strtab.len;
    buf_str (&strtab, fn->name ? fn->name : "");
    s.st_info = ELF64_ST_INFO (STB_GLOBAL, STT_FUNC);
    s.st_other = STV_DEFAULT;
    if (fn->addr != NULL) {
      s.st_shndx = 1; /* .text */
      s.st_value = (Elf64_Addr) ((uintptr_t) fn->addr - text_base);
    } else {
      s.st_shndx = SHN_UNDEF;
    }
    s.st_size = (Elf64_Xword) fn->size;
    buf_bytes (&symtab, &s, sizeof s);
  }
  const char *cu_name = d->n_files ? d->files[0] : "";
  emit_abbrev (&abbrev);
  emit_info (d, &info, &g, text_base, text_size, cu_name);
  emit_line (d, &line, &g);
  emit_frame (d, &frame, &g);

  dwsec_t S[DWSEC_MAX];
  int ns = 0;
  S[ns++] = (dwsec_t){0};
  S[ns++] = (dwsec_t){".text", SHT_NOBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, text_base, 0,
                      NULL, text_size};
  int i_symtab = ns;
  S[ns++] = (dwsec_t){".symtab", SHT_SYMTAB, 0, 1, 8, 0, 0, sizeof (Elf64_Sym), &symtab,
                      symtab.len};
  int i_strtab = ns;
  S[ns++] = (dwsec_t){".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &strtab, strtab.len};
  S[i_symtab].link = (uint32_t) i_strtab;
  S[ns++] = (dwsec_t){".debug_abbrev", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &abbrev, abbrev.len};
  S[ns++] = (dwsec_t){".debug_info", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &info, info.len};
  S[ns++] = (dwsec_t){".debug_line", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &line, line.len};
  if (frame.len != 0)
    S[ns++] = (dwsec_t){".debug_frame", SHT_PROGBITS, 0, 0, 8, 0, 0, 0, &frame, frame.len};
  int rc = elf_assemble (S, ns, MIR_DEBUG_EM, buf, size);
  free (strtab.p); free (symtab.p); free (abbrev.p); free (info.p); free (line.p); free (frame.p);
  return rc;
}

/* ===== AOT relocatable-object builder (MIR_object_*) ===================== */
/* The second consumer of the ELF core above (see mir-debug.h).  Everything is
   section-relative and unresolved; the system linker applies the .rela.*
   entries.  Fixed section-header indexes: 1 .text, 2 .data, 3 .bss (then
   .symtab/.strtab and any .rela.* / .shstrtab). */

typedef struct {
  char *name; /* copied; NULL for section symbols and anonymous items */
  int sec;    /* MIR_OBJ_SEC_* or MIR_OBJ_SEC_UNDEF */
  uint64_t value, size;
  unsigned char func_p, local_p, weak_p, section_p, defined_p;
} objsym_t;

typedef struct {
  int sec; /* MIR_OBJ_SEC_TEXT or MIR_OBJ_SEC_DATA */
  uint64_t offset;
  int sym;
  int64_t addend;
  int kind;
} objreloc_t;

struct MIR_object {
  dwbuf_t text, data, pool; /* pool = .mir.addrpool, the GOT-shaped
                               address-slot section (R6 PIC) */
  uint64_t bss_size;
  size_t data_align, bss_align, pool_align;
  objsym_t *syms;
  size_t n_syms, cap_syms;
  objreloc_t *rels;
  size_t n_rels, cap_rels;
  int sec_sym[4];    /* section-symbol ids for TEXT/DATA/BSS/ADDRPOOL; -1 until created */
  MIR_debug_t debug; /* borrowed builder with .text-offset func addrs (R5);
                        NULL = no debug sections in the artifacts */
};

static void buf_zeros (dwbuf_t *b, size_t n) {
  if (buf_reserve (b, n)) return;
  memset (b->p + b->len, 0, n);
  b->len += n;
}

static char *obj_strdup (const char *s) {
  size_t n = strlen (s) + 1;
  char *p = malloc (n);
  if (p != NULL) memcpy (p, s, n);
  return p;
}

MIR_object_t MIR_object_create (void) {
  if (MIR_DEBUG_EM != EM_X86_64) return NULL; /* x86-64 first: the reloc mapping */
  MIR_object_t obj = calloc (1, sizeof (struct MIR_object));
  if (obj == NULL) return NULL;
  obj->data_align = obj->bss_align = obj->pool_align = 1;
  obj->sec_sym[0] = obj->sec_sym[1] = obj->sec_sym[2] = obj->sec_sym[3] = -1;
  return obj;
}

void MIR_object_destroy (MIR_object_t obj) {
  if (obj == NULL) return;
  for (size_t i = 0; i < obj->n_syms; i++) free (obj->syms[i].name);
  free (obj->syms);
  free (obj->rels);
  free (obj->text.p);
  free (obj->data.p);
  free (obj->pool.p);
  free (obj);
}

size_t MIR_object_text_append (MIR_object_t obj, const void *bytes, size_t len) {
  while (obj->text.len % 16 != 0) buf_u8 (&obj->text, 0);
  size_t off = obj->text.len;
  buf_bytes (&obj->text, bytes, len);
  return off;
}

size_t MIR_object_data_append (MIR_object_t obj, const void *bytes, size_t len, size_t align) {
  if (align > obj->data_align) obj->data_align = align;
  if (align > 1)
    while (obj->data.len % align != 0) buf_u8 (&obj->data, 0);
  size_t off = obj->data.len;
  if (bytes == NULL)
    buf_zeros (&obj->data, len);
  else
    buf_bytes (&obj->data, bytes, len);
  return off;
}

size_t MIR_object_bss_reserve (MIR_object_t obj, size_t len, size_t align) {
  if (align > obj->bss_align) obj->bss_align = align;
  if (align > 1) obj->bss_size = (obj->bss_size + align - 1) & ~((uint64_t) align - 1);
  size_t off = (size_t) obj->bss_size;
  obj->bss_size += len;
  return off;
}

size_t MIR_object_addrpool_append (MIR_object_t obj, const void *bytes, size_t len, size_t align) {
  if (align > obj->pool_align) obj->pool_align = align;
  if (align > 1)
    while (obj->pool.len % align != 0) buf_u8 (&obj->pool, 0);
  size_t off = obj->pool.len;
  if (bytes == NULL)
    buf_zeros (&obj->pool, len);
  else
    buf_bytes (&obj->pool, bytes, len);
  return off;
}

int MIR_object_add_symbol (MIR_object_t obj, const char *name, int sec, uint64_t value,
                           uint64_t size, int func_p, int local_p, int weak_p) {
  if (obj->n_syms >= obj->cap_syms) {
    size_t c = obj->cap_syms ? obj->cap_syms * 2 : 64;
    void *np = realloc (obj->syms, c * sizeof (objsym_t));
    if (np == NULL) return -1;
    obj->syms = np;
    obj->cap_syms = c;
  }
  objsym_t *s = &obj->syms[obj->n_syms];
  memset (s, 0, sizeof *s);
  if (name != NULL && (s->name = obj_strdup (name)) == NULL) return -1;
  s->sec = sec;
  s->value = value;
  s->size = size;
  s->func_p = func_p != 0;
  s->local_p = local_p != 0;
  s->weak_p = weak_p != 0;
  s->defined_p = sec != MIR_OBJ_SEC_UNDEF;
  return (int) obj->n_syms++;
}

void MIR_object_symbol_define (MIR_object_t obj, int sym_id, int sec, uint64_t value,
                               uint64_t size, int func_p, int local_p, int weak_p) {
  if (sym_id < 0 || (size_t) sym_id >= obj->n_syms) return;
  objsym_t *s = &obj->syms[sym_id];
  s->sec = sec;
  s->value = value;
  s->size = size;
  s->func_p = func_p != 0;
  s->local_p = local_p != 0;
  s->weak_p = weak_p != 0;
  s->defined_p = sec != MIR_OBJ_SEC_UNDEF;
}

int MIR_object_section_symbol (MIR_object_t obj, int sec) {
  if (sec < MIR_OBJ_SEC_TEXT || sec > MIR_OBJ_SEC_ADDRPOOL) return -1;
  if (obj->sec_sym[sec] < 0) {
    int id = MIR_object_add_symbol (obj, NULL, sec, 0, 0, 0, 1, 0);
    if (id >= 0) obj->syms[id].section_p = 1;
    obj->sec_sym[sec] = id;
  }
  return obj->sec_sym[sec];
}

int MIR_object_symbol_defined_p (MIR_object_t obj, int sym_id) {
  if (sym_id < 0 || (size_t) sym_id >= obj->n_syms) return 0;
  return obj->syms[sym_id].defined_p;
}

void MIR_object_add_reloc (MIR_object_t obj, int sec, uint64_t offset, int sym_id, int64_t addend,
                           int kind) {
  if (obj->n_rels >= obj->cap_rels) {
    size_t c = obj->cap_rels ? obj->cap_rels * 2 : 64;
    void *np = realloc (obj->rels, c * sizeof (objreloc_t));
    if (np == NULL) return;
    obj->rels = np;
    obj->cap_rels = c;
  }
  obj->rels[obj->n_rels++] = (objreloc_t) {sec, offset, sym_id, addend, kind};
}

int MIR_object_find_symbol (MIR_object_t obj, const char *name, int *sec, uint64_t *value,
                            uint64_t *size) {
  if (obj == NULL || name == NULL) return 0;
  for (size_t i = 0; i < obj->n_syms; i++) {
    objsym_t *s = &obj->syms[i];
    if (!s->defined_p || s->name == NULL || strcmp (s->name, name) != 0) continue;
    if (sec != NULL) *sec = s->sec;
    if (value != NULL) *value = s->value;
    if (size != NULL) *size = s->size;
    return 1;
  }
  return 0;
}

void MIR_object_set_debug (MIR_object_t obj, MIR_debug_t d) {
  if (obj != NULL) obj->debug = d;
}

/* Address-slot recorder for the .o's DWARF: collects every 8-byte code
   address the generators write, so the emitter below can zero the slots and
   emit .rela.debug_* relocations against the .text section symbol. */
typedef struct {
  int sec; /* 0 .debug_info / 1 .debug_line / 2 .debug_frame */
  size_t pos;
  uint64_t value;
} dwslot_t;

typedef struct {
  dwslot_t *v;
  int n, cap;
} dwslots_t;

/* 4-byte cross-debug-section offset slots (abbrev offset, stmt_list, CIE
   pointers) -- relocated against the TARGET debug section's symbol so both
   external linkers and MIR_object_read can concatenate debug sections. */
typedef struct {
  int src, tgt; /* dwgen sec ids: 0 info / 1 line / 2 frame / 3 abbrev */
  size_t pos;
  uint32_t value;
} dwsref_t;

typedef struct {
  dwslots_t slots;
  struct {
    dwsref_t *v;
    int n, cap;
  } refs;
} dwcap_t;

static void dwslot_record (void *env, int sec, size_t pos, uint64_t value) {
  dwslots_t *s = &((dwcap_t *) env)->slots;
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->v = realloc (s->v, (size_t) s->cap * sizeof (dwslot_t));
  }
  if (s->v == NULL) return; /* OOM: slots lost; the emit below degrades */
  s->v[s->n].sec = sec;
  s->v[s->n].pos = pos;
  s->v[s->n].value = value;
  s->n++;
}

static void dwsref_record (void *env, int src, size_t pos, int tgt, uint32_t value) {
  dwcap_t *c = env;
  if (c->refs.n == c->refs.cap) {
    c->refs.cap = c->refs.cap ? c->refs.cap * 2 : 16;
    c->refs.v = realloc (c->refs.v, (size_t) c->refs.cap * sizeof (dwsref_t));
  }
  if (c->refs.v == NULL) return;
  c->refs.v[c->refs.n].src = src;
  c->refs.v[c->refs.n].tgt = tgt;
  c->refs.v[c->refs.n].pos = pos;
  c->refs.v[c->refs.n].value = value;
  c->refs.n++;
}

int MIR_object_emit (MIR_object_t obj, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (obj == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM != EM_X86_64) return -1;

  /* DWARF relocations bind to the .text section symbol -- make sure it
     exists before the symbol count is snapshotted below. */
  if (obj->debug != NULL) MIR_object_section_symbol (obj, MIR_OBJ_SEC_TEXT);

  /* Final symtab order: null, locals (incl. section symbols), then
     globals/weak.  Relocations hold stable ids; map them here. */
  size_t n = obj->n_syms;
  uint32_t *final_idx = calloc (n ? n : 1, sizeof (uint32_t));
  if (final_idx == NULL) return -1;
  dwbuf_t strtab = {0}, symtab = {0}, rela_text = {0}, rela_data = {0}, rela_pool = {0};
  buf_u8 (&strtab, 0);
  Elf64_Sym z = {0};
  buf_bytes (&symtab, &z, sizeof z);
  static const int sec_shndx[4] = {1, 2, 3, 4};

  /* DWARF emission runs BEFORE the symtab build: the debug sections get
     STT_SECTION symbols of their own (targets of the cross-debug-section
     relocations), so their planned section indexes -- which depend on
     which .rela.* alloc sections exist and whether .debug_frame is empty
     -- must be known while locals are written. */
  dwbuf_t dw_abbrev = {0}, dw_info = {0}, dw_line = {0}, dw_frame = {0};
  dwcap_t cap = {0};
  if (obj->debug != NULL) {
    dwgen_t g = {0, obj->text.p, 1, dwslot_record, dwsref_record, &cap};
    uint64_t text_lo, text_size;
    dw_text_range (obj->debug, &g, &text_lo, &text_size);
    emit_abbrev (&dw_abbrev);
    emit_info (obj->debug, &dw_info, &g, text_lo, text_size,
               obj->debug->n_files ? obj->debug->files[0] : "");
    emit_line (obj->debug, &dw_line, &g);
    emit_frame (obj->debug, &dw_frame, &g);
  }
  /* Section order below: null, .text, .data, .bss, .mir.addrpool, .symtab,
     .strtab, [.rela.text], [.rela.data], [.rela.mir.addrpool], then the
     debug sections.  Presence of each .rela.* mirrors the emission loop's
     classification exactly. */
  int have_rt = 0, have_rd = 0, have_rp = 0;
  for (size_t i = 0; i < obj->n_rels; i++) {
    int rs = obj->rels[i].sec;
    if (rs == MIR_OBJ_SEC_TEXT)
      have_rt = 1;
    else if (rs == MIR_OBJ_SEC_DATA)
      have_rd = 1;
    else
      have_rp = 1;
  }
  int dbg_shndx[4] = {0, 0, 0, 0};      /* dwgen ids: info/line/frame/abbrev */
  uint32_t dbg_secsym[4] = {0, 0, 0, 0}; /* their final symtab indexes */
  if (obj->debug != NULL) {
    int nx = 7 + have_rt + have_rd + have_rp;
    dbg_shndx[3] = nx++; /* .debug_abbrev */
    dbg_shndx[0] = nx++; /* .debug_info */
    dbg_shndx[1] = nx++; /* .debug_line */
    if (dw_frame.len != 0) dbg_shndx[2] = nx;
  }

  uint32_t next = 1, n_locals = 0;
  static const int dbg_sym_order[4] = {3, 0, 1, 2}; /* section-index order */
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) /* debug section symbols: locals, after the objsym locals */
      for (int k = 0; k < 4; k++) {
        int d = dbg_sym_order[k];
        if (dbg_shndx[d] == 0) continue;
        Elf64_Sym es = {0};
        es.st_info = ELF64_ST_INFO (STB_LOCAL, STT_SECTION);
        es.st_shndx = (Elf64_Section) dbg_shndx[d];
        buf_bytes (&symtab, &es, sizeof es);
        dbg_secsym[d] = next++;
        n_locals++;
      }
    for (size_t i = 0; i < n; i++) {
      objsym_t *s = &obj->syms[i];
      int local = s->local_p || s->section_p;
      if ((pass == 0) != (local != 0)) continue;
      Elf64_Sym es = {0};
      if (s->name != NULL) {
        es.st_name = (Elf64_Word) strtab.len;
        buf_str (&strtab, s->name);
      }
      unsigned char bind = local ? STB_LOCAL : s->weak_p ? STB_WEAK : STB_GLOBAL;
      unsigned char type = s->section_p ? STT_SECTION
                           : !s->defined_p ? STT_NOTYPE
                           : s->func_p ? STT_FUNC
                                       : STT_OBJECT;
      es.st_info = ELF64_ST_INFO (bind, type);
      es.st_other = STV_DEFAULT;
      es.st_shndx = s->defined_p ? (Elf64_Section) sec_shndx[s->sec] : SHN_UNDEF;
      es.st_value = s->defined_p ? s->value : 0;
      es.st_size = s->size;
      buf_bytes (&symtab, &es, sizeof es);
      if (pass == 0) n_locals++;
      final_idx[i] = next++;
    }
  }

  int bad_p = 0;
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if ((r->kind != MIR_OBJ_RELOC_ABS64 && r->kind != MIR_OBJ_RELOC_PC32) || r->sym < 0
        || (size_t) r->sym >= n
        || (r->sec != MIR_OBJ_SEC_TEXT && r->sec != MIR_OBJ_SEC_DATA
            && r->sec != MIR_OBJ_SEC_ADDRPOOL)) {
      bad_p = 1;
      break;
    }
    Elf64_Rela er;
    er.r_offset = r->offset;
    er.r_info = ELF64_R_INFO ((uint64_t) final_idx[r->sym],
                              r->kind == MIR_OBJ_RELOC_PC32 ? R_X86_64_PC32 : R_X86_64_64);
    er.r_addend = r->addend;
    buf_bytes (r->sec == MIR_OBJ_SEC_TEXT   ? &rela_text
               : r->sec == MIR_OBJ_SEC_DATA ? &rela_data
                                            : &rela_pool,
               &er, sizeof er);
  }

  /* DWARF relocations (R5 + multi-object): each 8-byte code-address slot
     zeroed and relocated against the .text section symbol, each 4-byte
     cross-debug-section offset (CU abbrev offset, stmt_list, FDE CIE
     pointers) zeroed and R_X86_64_32-relocated against the target debug
     section's symbol -- so an external linker's OR MIR_object_read's
     placement fixes the debug info exactly like the code, multi-CU
     included. */
  dwbuf_t rela_dw_info = {0}, rela_dw_line = {0}, rela_dw_frame = {0};
  if (obj->debug != NULL && !bad_p) {
    uint32_t text_sym_idx = final_idx[obj->sec_sym[MIR_OBJ_SEC_TEXT]];
    for (int i = 0; i < cap.slots.n; i++) {
      dwslot_t *sl = &cap.slots.v[i];
      dwbuf_t *tgt = sl->sec == 0 ? &dw_info : sl->sec == 1 ? &dw_line : &dw_frame;
      dwbuf_t *rel = sl->sec == 0 ? &rela_dw_info : sl->sec == 1 ? &rela_dw_line : &rela_dw_frame;
      memset (tgt->p + sl->pos, 0, 8);
      Elf64_Rela er;
      er.r_offset = sl->pos;
      er.r_info = ELF64_R_INFO ((uint64_t) text_sym_idx, R_X86_64_64);
      er.r_addend = (int64_t) sl->value;
      buf_bytes (rel, &er, sizeof er);
    }
    for (int i = 0; i < cap.refs.n; i++) {
      dwsref_t *rf = &cap.refs.v[i];
      dwbuf_t *tgt = rf->src == 0 ? &dw_info : rf->src == 1 ? &dw_line : &dw_frame;
      dwbuf_t *rel = rf->src == 0 ? &rela_dw_info : rf->src == 1 ? &rela_dw_line : &rela_dw_frame;
      memset (tgt->p + rf->pos, 0, 4);
      Elf64_Rela er;
      er.r_offset = rf->pos;
      er.r_info = ELF64_R_INFO ((uint64_t) dbg_secsym[rf->tgt], R_X86_64_32);
      er.r_addend = (int64_t) rf->value;
      buf_bytes (rel, &er, sizeof er);
    }
  }
  free (cap.slots.v);
  free (cap.refs.v);

  int rc = -1;
  if (!bad_p) {
    dwsec_t S[DWSEC_MAX];
    int ns = 0;
    S[ns++] = (dwsec_t) {0};
    S[ns++] = (dwsec_t) {".text", SHT_PROGBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, 0, 0,
                         &obj->text, obj->text.len};
    S[ns++] = (dwsec_t) {".data", SHT_PROGBITS, 0, 0,
                         (uint32_t) (obj->data_align > 8 ? obj->data_align : 8),
                         SHF_ALLOC | SHF_WRITE, 0, 0, &obj->data, obj->data.len};
    S[ns++] = (dwsec_t) {".bss", SHT_NOBITS, 0, 0,
                         (uint32_t) (obj->bss_align > 8 ? obj->bss_align : 8),
                         SHF_ALLOC | SHF_WRITE, 0, 0, NULL, obj->bss_size};
    S[ns++] = (dwsec_t) {".mir.addrpool", SHT_PROGBITS, 0, 0,
                         (uint32_t) (obj->pool_align > 8 ? obj->pool_align : 8),
                         SHF_ALLOC | SHF_WRITE, 0, 0, &obj->pool, obj->pool.len};
    int i_symtab = ns;
    S[ns++] = (dwsec_t) {".symtab", SHT_SYMTAB, 0, n_locals + 1, 8, 0, 0, sizeof (Elf64_Sym),
                         &symtab, symtab.len};
    int i_strtab = ns;
    S[ns++] = (dwsec_t) {".strtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &strtab, strtab.len};
    S[i_symtab].link = (uint32_t) i_strtab;
    if (rela_text.len != 0)
      S[ns++] = (dwsec_t) {".rela.text", SHT_RELA, (uint32_t) i_symtab, 1, 8, SHF_INFO_LINK, 0,
                           sizeof (Elf64_Rela), &rela_text, rela_text.len};
    if (rela_data.len != 0)
      S[ns++] = (dwsec_t) {".rela.data", SHT_RELA, (uint32_t) i_symtab, 2, 8, SHF_INFO_LINK, 0,
                           sizeof (Elf64_Rela), &rela_data, rela_data.len};
    if (rela_pool.len != 0)
      S[ns++] = (dwsec_t) {".rela.mir.addrpool", SHT_RELA, (uint32_t) i_symtab, 4, 8,
                           SHF_INFO_LINK, 0, sizeof (Elf64_Rela), &rela_pool, rela_pool.len};
    /* the planned indexes behind dbg_secsym[] must match the table being
       built -- divergence is an emitter-maintenance bug; refuse rather
       than mis-relocate */
    int plan_ok = obj->debug == NULL || ns == dbg_shndx[3];
    if (obj->debug != NULL && plan_ok) {
      S[ns++] = (dwsec_t) {".debug_abbrev", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &dw_abbrev,
                           dw_abbrev.len};
      uint32_t i_dw_info = (uint32_t) ns;
      S[ns++] = (dwsec_t) {".debug_info", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &dw_info, dw_info.len};
      uint32_t i_dw_line = (uint32_t) ns;
      S[ns++] = (dwsec_t) {".debug_line", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, &dw_line, dw_line.len};
      uint32_t i_dw_frame = 0;
      if (dw_frame.len != 0) {
        i_dw_frame = (uint32_t) ns;
        S[ns++]
          = (dwsec_t) {".debug_frame", SHT_PROGBITS, 0, 0, 8, 0, 0, 0, &dw_frame, dw_frame.len};
      }
      if (rela_dw_info.len != 0)
        S[ns++] = (dwsec_t) {".rela.debug_info", SHT_RELA, (uint32_t) i_symtab, i_dw_info, 8,
                             SHF_INFO_LINK, 0, sizeof (Elf64_Rela), &rela_dw_info,
                             rela_dw_info.len};
      if (rela_dw_line.len != 0)
        S[ns++] = (dwsec_t) {".rela.debug_line", SHT_RELA, (uint32_t) i_symtab, i_dw_line, 8,
                             SHF_INFO_LINK, 0, sizeof (Elf64_Rela), &rela_dw_line,
                             rela_dw_line.len};
      if (rela_dw_frame.len != 0)
        S[ns++] = (dwsec_t) {".rela.debug_frame", SHT_RELA, (uint32_t) i_symtab, i_dw_frame, 8,
                             SHF_INFO_LINK, 0, sizeof (Elf64_Rela), &rela_dw_frame,
                             rela_dw_frame.len};
    }
    /* non-executable stack marker (its absence makes ld assume an executable
       stack and warn) */
    S[ns++] = (dwsec_t) {".note.GNU-stack", SHT_PROGBITS, 0, 0, 1, 0, 0, 0, NULL, 0};
    if (plan_ok) rc = elf_assemble (S, ns, EM_X86_64, buf, size);
  }
  free (final_idx);
  free (strtab.p);
  free (symtab.p);
  free (rela_text.p);
  free (rela_data.p);
  free (rela_pool.p);
  free (dw_abbrev.p);
  free (dw_info.p);
  free (dw_line.p);
  free (dw_frame.p);
  free (rela_dw_info.p);
  free (rela_dw_line.p);
  free (rela_dw_frame.p);
  return rc;
}

/* ===== AOT executable emitter (ET_EXEC/ET_DYN, x86-64 Linux) ============= */
/* The third consumer of the object builder: a complete dynamic executable
   with no external toolchain.  This lays out segments/vaddrs by hand -- a
   different concern from elf_assemble's link-view section table (ET_REL,
   no program headers), so it is a sibling stage over the SAME builder data,
   not a parallel builder.  Model (from madc-master's proven in-house writer,
   simplified by MIR's exact reloc ledgers):
     - fixed load base 0x400000, identity file-offset<->vaddr mapping;
     - two PT_LOADs (R+X: headers/interp/hash/dynsym/dynstr/rela.dyn/text;
       R+W: addrpool/.dynamic then a page pad, then data/bss tail) +
       PT_PHDR + PT_INTERP + PT_DYNAMIC (PHDR/INTERP first, gABI order;
       PT_PHDR is what the loader derives a PIE's load bias from) +
       PT_GNU_STACK (non-exec) + PT_GNU_RELRO (Full RELRO: the pool -- the
       GOT -- and .dynamic lead the R+W segment and are mprotected
       read-only after relocation; BIND_NOW is already the only semantics,
       there is no lazy binding to disable);
     - internal relocations resolve at emit; imports become eager
       R_X86_64_64 slot relocations in .rela.dyn (no PLT/GOT: MIR calls
       already go through address slots) => DT_TEXTREL until the PIC rung;
     - synthesized _start: SysV stack -> __libc_start_main (entry, argc,
       argv, init=0, fini=0, rtld_fini) through its own reloc slot.
   shared_p flips the same layout to an ET_DYN shared object: base 0 (the
   loader picks the bias, so .dynamic/.rela.dyn/.dynsym values stay link-time
   vaddrs the loader rebases), no PT_INTERP/_start/entry, defined globals
   exported in .dynsym, and EVERY relocation kept dynamic -- internal targets
   as R_X86_64_RELATIVE (bias + link vaddr; -Bsymbolic semantics), imports as
   R_X86_64_64.  DT_INIT (params->init, when defined) covers load-time
   initializers for dlopen'd modules that have no main to call them.
   pie_p combines the two: an executable (PT_INTERP/_start/entry, import-only
   dynsym) on the shared object's base-0 ET_DYN layout with RELATIVE internal
   slots, tagged DT_FLAGS_1 = DF_1_PIE.  The stub is bias-clean (rip-relative
   entry lea + slot call), so PIE needs no extra codegen -- the R6 PIC capture
   already keeps .text relocation-free. */

#define OBJX_BASE_ADDR 0x400000ull
#define OBJX_PAGE 0x1000ull
#ifndef DF_1_PIE /* pre-2.27 glibc elf.h */
#define DF_1_PIE 0x08000000
#endif

/* _start: xor ebp; mov rdx->r9; pop rsi (argc); mov rsp->rdx (argv);
   align rsp; push rax; push rsp; r8=0 (fini); ecx=0 (init);
   lea entry(%rip),%rdi (@0x14, disp @0x17 -- rip-relative so the ONE stub
   serves ET_EXEC and PIE alike); call *disp32(%rip) (@0x1b, disp @0x1d)
   through the 8-byte __libc_start_main slot in the R+W address-pool region
   (PIC: a dynamic slot in text would fault the loader's write -- there is
   no DT_TEXTREL to make it remap); hlt; int3 padding to 48. */
static const uint8_t objx_start_stub[]
  = {0x31, 0xed, 0x49, 0x89, 0xd1, 0x5e, 0x48, 0x89, 0xe2, 0x48, 0x83,
     0xe4, 0xf0, 0x50, 0x54, 0x45, 0x31, 0xc0, 0x31, 0xc9, 0x48, 0x8d,
     0x3d, 0x00, 0x00, 0x00, 0x00, 0xff, 0x15, 0x00, 0x00, 0x00, 0x00,
     0xf4, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
     0xcc, 0xcc, 0xcc, 0xcc};
#define OBJX_STUB_ENTRY_DISP 23 /* disp32 of lea entry(%rip),%rdi */
#define OBJX_STUB_CALL_DISP 29  /* disp32 of call *(%rip) */
#define OBJX_STUB_SIZE 48
/* the 48-byte stub keeps the captured text's 16-byte alignment on its own
   (SSE pool constants: movaps/xorpd fault otherwise) */
#define OBJX_TEXT_BIAS OBJX_STUB_SIZE

static uint32_t objx_elf_hash (const char *name) {
  uint32_t h = 0, g;
  for (; *name; ++name) {
    h = (h << 4) + (uint8_t) *name;
    g = h & 0xf0000000;
    if (g) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

int MIR_object_emit_executable (MIR_object_t obj, const MIR_object_exec_params *params,
                                void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (obj == NULL || params == NULL || buf == NULL || size == NULL || MIR_DEBUG_EM != EM_X86_64)
    return -1;
  int shared_p = params->shared_p != 0;
  int pie_p = !shared_p && params->pie_p != 0;
  /* pic_image_p: the load bias is unknown (ET_DYN) -- base 0, internal
     ABS64 slots become R_X86_64_RELATIVE for the loader to rebase */
  int pic_image_p = shared_p || pie_p;
  uint64_t base = pic_image_p ? 0 : OBJX_BASE_ADDR;
  const char *interp = params->interp != NULL ? params->interp : "/lib64/ld-linux-x86-64.so.2";
  const char *entry_nm = params->entry != NULL ? params->entry : "main";
  static const Elf64_Section objx_shndx[4] = {6, 9, 10, 7}; /* text/data/bss/pool shdr indexes */

  /* the entry symbol must be a defined text function (executables only) */
  size_t n = obj->n_syms;
  size_t entry_i = n;
  if (!shared_p) {
    for (size_t i = 0; i < n; i++)
      if (obj->syms[i].name != NULL && obj->syms[i].defined_p
          && obj->syms[i].sec == MIR_OBJ_SEC_TEXT && strcmp (obj->syms[i].name, entry_nm) == 0) {
        entry_i = i;
        break;
      }
    if (entry_i == n) return -1;
  }
  /* optional DT_INIT symbol; a module without one is valid -- omit the tag */
  size_t init_i = n;
  if (params->init != NULL)
    for (size_t i = 0; i < n; i++)
      if (obj->syms[i].name != NULL && obj->syms[i].defined_p
          && obj->syms[i].sec == MIR_OBJ_SEC_TEXT && strcmp (obj->syms[i].name, params->init) == 0) {
        init_i = i;
        break;
      }

  /* dynsym = every undefined (imported) symbol, plus __libc_start_main for
     the _start slot if the module does not already import it (executables);
     shared objects additionally export their defined globals (st_value gets
     the resolved link vaddr patched in after layout) */
  uint32_t *dyn_idx = calloc (n ? n : 1, sizeof (uint32_t)); /* 0 = not imported */
  if (dyn_idx == NULL) return -1;
  dwbuf_t dynstr = {0}, dynsym = {0}, hash = {0}, symtab = {0}, strtab = {0}, shstr = {0};
  dwbuf_t dw_abbrev = {0}, dw_info = {0}, dw_line = {0}, dw_frame = {0}; /* R5 DWARF */
  int rc = -1;
  unsigned char *p = NULL;
  uint32_t *lib_name_off = NULL;
  buf_u8 (&dynstr, 0);
  Elf64_Sym z = {0};
  buf_bytes (&dynsym, &z, sizeof z);
  lib_name_off = calloc (params->n_needed ? params->n_needed : 1, sizeof (uint32_t));
  if (lib_name_off == NULL) goto done;
  for (size_t i = 0; i < params->n_needed; i++) {
    lib_name_off[i] = (uint32_t) dynstr.len;
    buf_str (&dynstr, params->needed[i]);
  }
  uint32_t runpath_off = 0;
  if (params->runpath != NULL) {
    runpath_off = (uint32_t) dynstr.len;
    buf_str (&dynstr, params->runpath);
  }
  uint32_t ndyn = 1, lsm_idx = 0;
  const char **dyn_names = calloc (n + 1, sizeof (char *));
  if (dyn_names == NULL) goto done;
  for (size_t i = 0; i < n; i++) {
    objsym_t *s = &obj->syms[i];
    if (s->name == NULL) continue;
    if (s->defined_p && (!shared_p || s->local_p || s->section_p)) continue;
    Elf64_Sym es = {0};
    es.st_name = (Elf64_Word) dynstr.len;
    buf_str (&dynstr, s->name);
    if (s->defined_p) { /* shared-object export; st_value patched after layout */
      es.st_info = ELF64_ST_INFO (s->weak_p ? STB_WEAK : STB_GLOBAL,
                                  s->func_p ? STT_FUNC : STT_OBJECT);
      es.st_shndx = objx_shndx[s->sec];
      es.st_size = s->size;
    } else {
      es.st_info = ELF64_ST_INFO (STB_GLOBAL, s->func_p ? STT_FUNC : STT_NOTYPE);
    }
    buf_bytes (&dynsym, &es, sizeof es);
    dyn_names[ndyn] = s->name;
    dyn_idx[i] = ndyn++;
    if (!shared_p && strcmp (s->name, "__libc_start_main") == 0) lsm_idx = dyn_idx[i];
  }
  if (!shared_p && lsm_idx == 0) {
    Elf64_Sym es = {0};
    es.st_name = (Elf64_Word) dynstr.len;
    buf_str (&dynstr, "__libc_start_main");
    es.st_info = ELF64_ST_INFO (STB_GLOBAL, STT_FUNC);
    buf_bytes (&dynsym, &es, sizeof es);
    dyn_names[ndyn] = "__libc_start_main";
    lsm_idx = ndyn++;
  }

  { /* SysV .hash over the dynsyms */
    uint32_t nbucket = ndyn, nchain = ndyn;
    uint32_t *buckets = calloc (nbucket, sizeof (uint32_t));
    uint32_t *chains = calloc (nchain, sizeof (uint32_t));
    if (buckets == NULL || chains == NULL) {
      free (buckets);
      free (chains);
      free ((void *) dyn_names);
      goto done;
    }
    for (uint32_t i = 1; i < ndyn; i++) {
      uint32_t h = objx_elf_hash (dyn_names[i]) % nbucket;
      chains[i] = buckets[h];
      buckets[h] = i;
    }
    buf_u32 (&hash, nbucket);
    buf_u32 (&hash, nchain);
    for (uint32_t i = 0; i < nbucket; i++) buf_u32 (&hash, buckets[i]);
    for (uint32_t i = 0; i < nchain; i++) buf_u32 (&hash, chains[i]);
    free (buckets);
    free (chains);
  }
  free ((void *) dyn_names);

  /* executables: import ABS64 relocations survive to .rela.dyn, internal
     ones resolve below; shared objects: the load bias is unknown, so EVERY
     ABS64 stays dynamic.  PC32 references are bias-invariant and resolve at
     emit in both modes -- never dynamic.  A dynamic slot inside .text would
     mean DT_TEXTREL: the PIC capture keeps text clean, but track it loudly
     rather than assume. */
  size_t n_dyn_rel = shared_p ? 0 : 1; /* the __libc_start_main slot */
  int textrel_p = 0;
  for (size_t i = 0; i < obj->n_rels; i++) {
    objreloc_t *r = &obj->rels[i];
    if ((r->kind != MIR_OBJ_RELOC_ABS64 && r->kind != MIR_OBJ_RELOC_PC32) || r->sym < 0
        || (size_t) r->sym >= n)
      goto done;
    if (r->kind == MIR_OBJ_RELOC_PC32) {
      if (!obj->syms[r->sym].defined_p) goto done; /* PC32 against an import: no such producer */
      continue;
    }
    if (pic_image_p || !obj->syms[r->sym].defined_p) {
      n_dyn_rel++;
      if (r->sec == MIR_OBJ_SEC_TEXT) textrel_p = 1;
    }
  }

  /* ---- layout: identity file-offset<->vaddr mapping from OBJX_BASE_ADDR */
#define OBJX_ALIGN(v, a) (((v) + (uint64_t) (a) -1) & ~((uint64_t) (a) -1))
  uint64_t off = sizeof (Elf64_Ehdr);
  uint64_t phdr_off = off;
  /* executables: PHDR, INTERP, LOAD RX, LOAD RW, DYNAMIC, GNU_STACK,
     GNU_RELRO (gABI order -- PT_PHDR/PT_INTERP precede the loadable
     segments; the loader derives a PIE's load bias from PT_PHDR:
     l_addr = phdr runtime addr - p_vaddr, so without it a PIE's rebasing
     silently uses bias 0 and ld.so faults on its own unrebased pointers).
     shared objects: LOAD, LOAD, DYNAMIC, GNU_STACK, GNU_RELRO (dlopen
     takes l_addr from the mapping itself). */
  const int n_phdrs = shared_p ? 5 : 7;
  off += (uint64_t) n_phdrs * sizeof (Elf64_Phdr);
  uint64_t interp_off = off, interp_size = shared_p ? 0 : strlen (interp) + 1;
  off = OBJX_ALIGN (interp_off + interp_size, 8);
  uint64_t hash_off = off;
  off = OBJX_ALIGN (hash_off + hash.len, 8);
  uint64_t dynsym_off = off;
  off = OBJX_ALIGN (dynsym_off + dynsym.len, 1);
  uint64_t dynstr_off = off;
  off = OBJX_ALIGN (dynstr_off + dynstr.len, 8);
  uint64_t rela_off = off, rela_size = (uint64_t) n_dyn_rel * sizeof (Elf64_Rela);
  off = OBJX_ALIGN (rela_off + rela_size, 16);
  /* no _start stub/slot in a .so; text_off's 16-alignment already preserves
     the captured code's SSE-constant alignment on its own */
  uint64_t text_bias = shared_p ? 0 : OBJX_TEXT_BIAS;
  uint64_t text_off = off, text_size = text_bias + obj->text.len;
  off = OBJX_ALIGN (text_off + text_size, OBJX_PAGE);
  uint64_t rx_filesz = text_off + text_size; /* the R+X segment: file start..text end */
  uint64_t rw_off = off;                     /* page-aligned R+W segment start */
  /* RELRO: the protected pieces LEAD the R+W segment -- .mir.addrpool (the
     GOT, incl. the _start __libc_start_main slot) then .dynamic -- and the
     boundary is page-padded (glibc's _dl_protect_relro protects only whole
     pages, and a page shared with .data could not be protected), so
     PT_GNU_RELRO below covers them exactly.  .data/.bss follow, writable. */
  size_t pool_align = obj->pool_align > 8 ? obj->pool_align : 8;
  if (pool_align > OBJX_PAGE) goto done; /* congruence bound; nothing emits >4K aligns */
  uint64_t pool_off = rw_off; /* page-aligned, so any <=4K pool_align holds */
  uint64_t lsm_off = 0, pool_view_size = obj->pool.len;
  off = pool_off + obj->pool.len;
  if (!shared_p) { /* the _start __libc_start_main slot rides with the pool */
    lsm_off = OBJX_ALIGN (off, 8);
    off = lsm_off + 8;
    pool_view_size = off - pool_off;
  }
  off = OBJX_ALIGN (off, 8);
  uint64_t dynamic_off = off;
  /* NEEDED*n [+RUNPATH] [+INIT] + STRTAB/STRSZ/SYMTAB/SYMENT/HASH
     + RELA/RELASZ/RELAENT + FLAGS (BIND_NOW) + FLAGS_1 (NOW [| PIE])
     [+DEBUG -- executables only, gcc/ld parity: ld.so writes the r_debug
     rendezvous address into the slot (before RELRO protection), which is
     how gdb's probes-based solib interface locates the link map]
     [+TEXTREL -- only if a dynamic slot targets text; the PIC capture
     keeps text clean] + NULL */
  uint64_t n_dyntags = params->n_needed + (params->runpath != NULL ? 1 : 0) + (init_i < n ? 1 : 0)
                       + 10 + (shared_p ? 0 : 1) + (textrel_p ? 1 : 0) + 1;
  uint64_t dynamic_size = n_dyntags * sizeof (Elf64_Dyn);
  uint64_t relro_end = OBJX_ALIGN (dynamic_off + dynamic_size, OBJX_PAGE);
  size_t data_align = obj->data_align > 8 ? obj->data_align : 8;
  if (data_align > OBJX_PAGE) goto done;
  uint64_t data_off = relro_end; /* page boundary, so any <=4K data_align holds */
  off = data_off + obj->data.len;
  uint64_t rw_filesz = off - rw_off;
  size_t bss_align = obj->bss_align > 8 ? obj->bss_align : 8;
  uint64_t bss_vaddr = OBJX_ALIGN (base + off, bss_align);
  uint64_t rw_memsz = bss_vaddr + obj->bss_size - (base + rw_off);
  uint64_t text_vaddr = base + text_off;
  uint64_t data_vaddr = base + data_off;
  uint64_t pool_vaddr = base + pool_off;
  uint64_t code_vaddr = text_vaddr + text_bias; /* the builder's .text offset 0 */

  /* section vaddr for a defined symbol/section id */
#define OBJX_SEC_VADDR(sec) \
  ((sec) == MIR_OBJ_SEC_TEXT   ? code_vaddr \
   : (sec) == MIR_OBJ_SEC_DATA ? data_vaddr \
   : (sec) == MIR_OBJ_SEC_BSS  ? bss_vaddr \
                               : pool_vaddr)

  /* ---- DWARF (R5): the builder's .text offsets biased to the final
     link-time vaddrs -- ET_EXEC's identity layout makes them the runtime
     addresses; for ET_DYN gdb applies the load bias itself.  Non-alloc file
     content, laid out after the load segments below. */
  if (obj->debug != NULL) {
    dwgen_t g = {code_vaddr, obj->text.p, 1, NULL, NULL, NULL};
    uint64_t text_lo, dbg_text_size;
    dw_text_range (obj->debug, &g, &text_lo, &dbg_text_size);
    emit_abbrev (&dw_abbrev);
    emit_info (obj->debug, &dw_info, &g, text_lo, dbg_text_size,
               obj->debug->n_files ? obj->debug->files[0] : "");
    emit_line (obj->debug, &dw_line, &g);
    emit_frame (obj->debug, &dw_frame, &g);
  }

  /* ---- .symtab / .strtab (debug view: resolved link-time vaddrs) */
  buf_u8 (&strtab, 0);
  buf_bytes (&symtab, &z, sizeof z);
  uint32_t n_locals = 0, next = 1;
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < n; i++) {
      objsym_t *s = &obj->syms[i];
      int local = s->local_p || s->section_p;
      if ((pass == 0) != (local != 0)) continue;
      Elf64_Sym es = {0};
      if (s->name != NULL) {
        es.st_name = (Elf64_Word) strtab.len;
        buf_str (&strtab, s->name);
      }
      unsigned char bind = local ? STB_LOCAL : s->weak_p ? STB_WEAK : STB_GLOBAL;
      unsigned char type = s->section_p ? STT_SECTION
                           : !s->defined_p ? STT_NOTYPE
                           : s->func_p ? STT_FUNC
                                       : STT_OBJECT;
      es.st_info = ELF64_ST_INFO (bind, type);
      es.st_shndx = s->defined_p ? objx_shndx[s->sec] : SHN_UNDEF;
      es.st_value = s->defined_p ? OBJX_SEC_VADDR (s->sec) + s->value : 0;
      es.st_size = s->size;
      buf_bytes (&symtab, &es, sizeof es);
      if (pass == 0) n_locals++;
      next++;
    }
  (void) next;

  /* ---- assemble the image */
  int n_secs = 14; /* fixed table below; + up to 4 appended .debug_* */
  uint32_t shname[18];
  static const char *objx_sec_names[18]
    = {"",      ".interp", ".hash",    ".dynsym", ".dynstr", ".rela.dyn", ".text",
       ".mir.addrpool", ".dynamic", ".data", ".bss", ".symtab", ".strtab", ".shstrtab",
       ".debug_abbrev", ".debug_info", ".debug_line", ".debug_frame"};
  int n_dbg_secs = obj->debug != NULL ? (dw_frame.len != 0 ? 4 : 3) : 0;
  n_secs += n_dbg_secs;
  buf_u8 (&shstr, 0);
  shname[0] = 0;
  for (int i = 1; i < n_secs; i++) {
    shname[i] = (uint32_t) shstr.len;
    buf_str (&shstr, objx_sec_names[i]);
  }
  /* debug sections: non-alloc file content between the load image and the
     symtab (indexes 14.. so the fixed table above keeps its numbering) */
  uint64_t dw_abbrev_off = OBJX_ALIGN (off, 8);
  uint64_t dw_info_off = dw_abbrev_off + dw_abbrev.len;
  uint64_t dw_line_off = dw_info_off + dw_info.len;
  uint64_t dw_frame_off = OBJX_ALIGN (dw_line_off + dw_line.len, 8);
  if (n_dbg_secs != 0) off = dw_frame_off + dw_frame.len;
  uint64_t symtab_off = OBJX_ALIGN (off, 8);
  uint64_t strtab_off = symtab_off + symtab.len;
  uint64_t shstr_off = strtab_off + strtab.len;
  uint64_t shoff = OBJX_ALIGN (shstr_off + shstr.len, 8);
  uint64_t total = shoff + (uint64_t) n_secs * sizeof (Elf64_Shdr);

  p = calloc (1, (size_t) total);
  if (p == NULL) goto done;
  memcpy (p + hash_off, hash.p, hash.len);
  memcpy (p + dynsym_off, dynsym.p, dynsym.len);
  memcpy (p + dynstr_off, dynstr.p, dynstr.len);
  if (shared_p) { /* exported definitions: patch the resolved link-time vaddrs */
    Elf64_Sym *ds = (Elf64_Sym *) (p + dynsym_off);
    for (size_t i = 0; i < n; i++)
      if (dyn_idx[i] != 0 && obj->syms[i].defined_p)
        ds[dyn_idx[i]].st_value = OBJX_SEC_VADDR (obj->syms[i].sec) + obj->syms[i].value;
  } else {
    memcpy (p + interp_off, interp, interp_size);
    memcpy (p + text_off, objx_start_stub, OBJX_STUB_SIZE);
    /* both stub references are rip-relative link-vaddr distances --
       bias-invariant, so ET_EXEC and PIE patch identically */
    uint64_t entry_vaddr = code_vaddr + obj->syms[entry_i].value;
    uint32_t edisp
      = (uint32_t) (entry_vaddr - (text_vaddr + OBJX_STUB_ENTRY_DISP + 4));
    memcpy (p + text_off + OBJX_STUB_ENTRY_DISP, &edisp, 4);
    /* call *disp32(%rip) into the R+W __libc_start_main slot */
    uint32_t disp
      = (uint32_t) ((base + lsm_off) - (text_vaddr + OBJX_STUB_CALL_DISP + 4));
    memcpy (p + text_off + OBJX_STUB_CALL_DISP, &disp, 4);
  }
  if (obj->text.len != 0) memcpy (p + text_off + text_bias, obj->text.p, obj->text.len);
  if (obj->data.len != 0) memcpy (p + data_off, obj->data.p, obj->data.len);
  if (obj->pool.len != 0) memcpy (p + pool_off, obj->pool.p, obj->pool.len);
  if (n_dbg_secs != 0) {
    if (dw_abbrev.len != 0) memcpy (p + dw_abbrev_off, dw_abbrev.p, dw_abbrev.len);
    if (dw_info.len != 0) memcpy (p + dw_info_off, dw_info.p, dw_info.len);
    if (dw_line.len != 0) memcpy (p + dw_line_off, dw_line.p, dw_line.len);
    if (dw_frame.len != 0) memcpy (p + dw_frame_off, dw_frame.p, dw_frame.len);
  }

  /* relocations: PC32 references resolve at emit in both modes (the whole
     image slides by one bias, so PC distances are final); executables
     resolve internal ABS64 targets in place and send imports to .rela.dyn;
     shared objects send every ABS64 to .rela.dyn -- internal targets as
     R_X86_64_RELATIVE (loader writes bias + addend) */
  {
    Elf64_Rela *er = (Elf64_Rela *) (p + rela_off);
    if (!shared_p) {
      er->r_offset = base + lsm_off;
      er->r_info = ELF64_R_INFO ((uint64_t) lsm_idx, R_X86_64_64);
      er->r_addend = 0;
      er++;
    }
    for (size_t i = 0; i < obj->n_rels; i++) {
      objreloc_t *r = &obj->rels[i];
      uint64_t slot_off = (r->sec == MIR_OBJ_SEC_TEXT       ? text_off + text_bias
                           : r->sec == MIR_OBJ_SEC_DATA     ? data_off
                           : r->sec == MIR_OBJ_SEC_ADDRPOOL ? pool_off
                                                            : 0)
                          + r->offset;
      objsym_t *s = &obj->syms[r->sym];
      if (r->kind == MIR_OBJ_RELOC_PC32) {
        int64_t d = (int64_t) (OBJX_SEC_VADDR (s->sec) + s->value) + r->addend
                    - (int64_t) (base + slot_off);
        if (d < INT32_MIN || d > INT32_MAX) {
          free (p);
          p = NULL;
          goto done; /* image far larger than 2GB: not an emitter reality */
        }
        int32_t d32 = (int32_t) d;
        memcpy (p + slot_off, &d32, 4);
      } else if (s->defined_p && pic_image_p) {
        er->r_offset = base + slot_off;
        er->r_info = ELF64_R_INFO (0, R_X86_64_RELATIVE);
        er->r_addend = (int64_t) (OBJX_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend);
        er++;
      } else if (s->defined_p) {
        uint64_t v = OBJX_SEC_VADDR (s->sec) + s->value + (uint64_t) r->addend;
        memcpy (p + slot_off, &v, 8);
      } else {
        er->r_offset = base + slot_off;
        er->r_info = ELF64_R_INFO ((uint64_t) dyn_idx[r->sym], R_X86_64_64);
        er->r_addend = r->addend;
        er++;
      }
    }
  }

  { /* .dynamic */
    Elf64_Dyn *d = (Elf64_Dyn *) (p + dynamic_off);
    for (size_t i = 0; i < params->n_needed; i++) {
      d->d_tag = DT_NEEDED;
      d->d_un.d_val = lib_name_off[i];
      d++;
    }
#define OBJX_DYN(tag, val)  \
  do {                      \
    d->d_tag = (tag);       \
    d->d_un.d_val = (val);  \
    d++;                    \
  } while (0)
    if (params->runpath != NULL) OBJX_DYN (DT_RUNPATH, runpath_off);
    if (init_i < n) OBJX_DYN (DT_INIT, code_vaddr + obj->syms[init_i].value);
    OBJX_DYN (DT_STRTAB, base + dynstr_off);
    OBJX_DYN (DT_STRSZ, dynstr.len);
    OBJX_DYN (DT_SYMTAB, base + dynsym_off);
    OBJX_DYN (DT_SYMENT, sizeof (Elf64_Sym));
    OBJX_DYN (DT_HASH, base + hash_off);
    OBJX_DYN (DT_RELA, base + rela_off);
    OBJX_DYN (DT_RELASZ, rela_size);
    OBJX_DYN (DT_RELAENT, sizeof (Elf64_Rela));
    /* no lazy binding exists (no PLT/DT_JMPREL): BIND_NOW is a statement
       of fact, and with PT_GNU_RELRO it classifies as Full RELRO */
    OBJX_DYN (DT_FLAGS, DF_BIND_NOW);
    OBJX_DYN (DT_FLAGS_1, DF_1_NOW | (pie_p ? DF_1_PIE : 0));
    if (!shared_p) OBJX_DYN (DT_DEBUG, 0);
    if (textrel_p) OBJX_DYN (DT_TEXTREL, 0);
    OBJX_DYN (DT_NULL, 0);
#undef OBJX_DYN
  }

  memcpy (p + symtab_off, symtab.p, symtab.len);
  memcpy (p + strtab_off, strtab.p, strtab.len);
  memcpy (p + shstr_off, shstr.p, shstr.len);

  { /* ELF header */
    Elf64_Ehdr *eh = (Elf64_Ehdr *) p;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_ident[EI_OSABI] = ELFOSABI_SYSV;
    eh->e_type = pic_image_p ? ET_DYN : ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    /* the _start stub heads .text; for PIE this is the link-time vaddr
       (base 0) and the loader rebases it with the load bias */
    eh->e_entry = shared_p ? 0 : text_vaddr;
    eh->e_phoff = phdr_off;
    eh->e_shoff = shoff;
    eh->e_ehsize = sizeof (Elf64_Ehdr);
    eh->e_phentsize = sizeof (Elf64_Phdr);
    eh->e_phnum = (Elf64_Half) n_phdrs;
    eh->e_shentsize = sizeof (Elf64_Shdr);
    eh->e_shnum = (Elf64_Half) n_secs;
    eh->e_shstrndx = 13;
  }
  { /* program headers: [PHDR, INTERP,] LOAD R+X, LOAD R+W, DYNAMIC */
    Elf64_Phdr *ph = (Elf64_Phdr *) (p + phdr_off);
    if (!shared_p) {
      ph->p_type = PT_PHDR;
      ph->p_flags = PF_R;
      ph->p_offset = phdr_off;
      ph->p_vaddr = ph->p_paddr = base + phdr_off;
      ph->p_filesz = ph->p_memsz = (uint64_t) n_phdrs * sizeof (Elf64_Phdr);
      ph->p_align = 8;
      ph++;
      ph->p_type = PT_INTERP;
      ph->p_flags = PF_R;
      ph->p_offset = interp_off;
      ph->p_vaddr = ph->p_paddr = base + interp_off;
      ph->p_filesz = ph->p_memsz = interp_size;
      ph->p_align = 1;
      ph++;
    }
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr = ph->p_paddr = base;
    ph->p_filesz = ph->p_memsz = rx_filesz;
    ph->p_align = OBJX_PAGE;
    ph++;
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_W;
    ph->p_offset = rw_off;
    ph->p_vaddr = ph->p_paddr = base + rw_off;
    ph->p_filesz = rw_filesz;
    ph->p_memsz = rw_memsz;
    ph->p_align = OBJX_PAGE;
    ph++;
    ph->p_type = PT_DYNAMIC;
    ph->p_flags = PF_R | PF_W;
    ph->p_offset = dynamic_off;
    ph->p_vaddr = ph->p_paddr = base + dynamic_off;
    ph->p_filesz = ph->p_memsz = dynamic_size;
    ph->p_align = 8;
    ph++;
    /* an ABSENT PT_GNU_STACK means an EXECUTABLE stack on x86-64 Linux
       (kernel compat default); MIR emits no stack trampolines, so non-exec
       is unconditionally correct (offset/vaddr/sizes stay 0 -- calloc'd) */
    ph->p_type = PT_GNU_STACK;
    ph->p_flags = PF_R | PF_W;
    ph->p_align = 0x10;
    ph++;
    /* ld.so mprotects [rw_off, relro_end) read-only after relocation and
       before DT_INIT; the pool (the GOT) and .dynamic lead the segment so
       the cover is exact, and nothing writes them after relocation (no
       DT_DEBUG/DT_PLTGOT emitted; pool content is runtime-read-only) */
    ph->p_type = PT_GNU_RELRO;
    ph->p_flags = PF_R;
    ph->p_offset = rw_off;
    ph->p_vaddr = ph->p_paddr = base + rw_off;
    ph->p_filesz = ph->p_memsz = relro_end - rw_off;
    ph->p_align = 1;
  }
  { /* section headers (readelf/gdb view; the loader uses only phdrs) */
    Elf64_Shdr *sh = (Elf64_Shdr *) (p + shoff);
    /* named type: c2m's self-bootstrap builds this file without GNU typeof */
    struct objx_shrow {
      uint32_t type, link, info, align;
      uint64_t flags, off, vaddr_p, sz, entsize;
    } T[18] = {
      {0, 0, 0, 0, 0, 0, 0, 0, 0},
      {SHT_PROGBITS, 0, 0, 1, SHF_ALLOC, interp_off, 1, interp_size, 0},
      {SHT_HASH, 3, 0, 8, SHF_ALLOC, hash_off, 1, hash.len, 4},
      {SHT_DYNSYM, 4, 1, 8, SHF_ALLOC, dynsym_off, 1, dynsym.len, sizeof (Elf64_Sym)},
      {SHT_STRTAB, 0, 0, 1, SHF_ALLOC, dynstr_off, 1, dynstr.len, 0},
      {SHT_RELA, 3, 0, 8, SHF_ALLOC, rela_off, 1, rela_size, sizeof (Elf64_Rela)},
      {SHT_PROGBITS, 0, 0, 16, SHF_ALLOC | SHF_EXECINSTR, text_off, 1, text_size, 0},
      {SHT_PROGBITS, 0, 0, (uint32_t) pool_align, SHF_ALLOC | SHF_WRITE, pool_off, 1,
       pool_view_size, 0},
      {SHT_DYNAMIC, 4, 0, 8, SHF_ALLOC | SHF_WRITE, dynamic_off, 1, dynamic_size,
       sizeof (Elf64_Dyn)},
      {SHT_PROGBITS, 0, 0, (uint32_t) data_align, SHF_ALLOC | SHF_WRITE, data_off, 1,
       obj->data.len, 0},
      {SHT_NOBITS, 0, 0, (uint32_t) bss_align, SHF_ALLOC | SHF_WRITE, off, 2, obj->bss_size, 0},
      {SHT_SYMTAB, 12, n_locals + 1, 8, 0, symtab_off, 0, symtab.len, sizeof (Elf64_Sym)},
      {SHT_STRTAB, 0, 0, 1, 0, strtab_off, 0, strtab.len, 0},
      {SHT_STRTAB, 0, 0, 1, 0, shstr_off, 0, shstr.len, 0},
    };
    if (n_dbg_secs != 0) { /* appended after the fixed table (e_shstrndx stays 13) */
      T[14] = (struct objx_shrow) {SHT_PROGBITS, 0, 0, 1, 0, dw_abbrev_off, 0, dw_abbrev.len, 0};
      T[15] = (struct objx_shrow) {SHT_PROGBITS, 0, 0, 1, 0, dw_info_off, 0, dw_info.len, 0};
      T[16] = (struct objx_shrow) {SHT_PROGBITS, 0, 0, 1, 0, dw_line_off, 0, dw_line.len, 0};
      if (dw_frame.len != 0)
        T[17] = (struct objx_shrow) {SHT_PROGBITS, 0, 0, 8, 0, dw_frame_off, 0, dw_frame.len, 0};
    }
    for (int i = 1; i < n_secs; i++) {
      sh[i].sh_name = shname[i];
      sh[i].sh_type = T[i].type;
      sh[i].sh_flags = T[i].flags;
      sh[i].sh_addr = T[i].vaddr_p == 1 ? base + T[i].off
                      : T[i].vaddr_p == 2 ? bss_vaddr
                                          : 0;
      sh[i].sh_offset = T[i].off;
      sh[i].sh_size = T[i].sz;
      sh[i].sh_link = T[i].link;
      sh[i].sh_info = T[i].info;
      sh[i].sh_addralign = T[i].align;
      sh[i].sh_entsize = T[i].entsize;
    }
  }
  *buf = p;
  *size = (size_t) total;
  p = NULL;
  rc = 0;
#undef OBJX_SEC_VADDR
#undef OBJX_ALIGN

done:
  free (p);
  free (lib_name_off);
  free (dyn_idx);
  free (dynstr.p);
  free (dynsym.p);
  free (hash.p);
  free (symtab.p);
  free (strtab.p);
  free (shstr.p);
  free (dw_abbrev.p);
  free (dw_info.p);
  free (dw_line.p);
  free (dw_frame.p);
  return rc;
}

/* ===== In-process ET_REL loader (MIR_object_load) ======================== */
/* The third consumer of the ELF core: parse back the exact subset
   MIR_object_emit writes (.text/.data/.bss + .rela.text/.rela.data, RELA
   addends carry the whole value, R_X86_64_64 only), place it in one
   anonymous mapping with page-aligned regions (so .text can drop to R+X
   after the relocation pass), resolve imports, and export the defined
   globals for lookup.  See mir-debug.h for the contract. */

typedef struct {
  char *name;
  void *addr;
} objload_sym_t;

struct MIR_object_loaded {
  uint8_t *map;
  size_t map_size;
  objload_sym_t *syms; /* defined global/weak symbols, sorted by name */
  size_t n_syms;
};

#if defined(__ELF__) && defined(__GNUC__) && defined(__x86_64__)
/* The dotted AOT builtin exports (asm-aliased in mir-x86_64.c /
   mir-gen-x86_64.c) -- extern-declared by asm label because '.' is not a C
   identifier character, and weak so that linking mir-debug alone (the
   GDB-JIT-only embedder case) stays possible: absent aliases resolve to
   NULL and the name simply falls through to the caller's resolver. */
extern char mir_objload_va_arg asm ("mir.va_arg") __attribute__ ((weak));
extern char mir_objload_va_block_arg asm ("mir.va_block_arg") __attribute__ ((weak));
extern char mir_objload_arg_memcpy asm ("mir.arg_memcpy") __attribute__ ((weak));
extern char mir_objload_ui2f asm ("mir.ui2f") __attribute__ ((weak));
extern char mir_objload_ui2d asm ("mir.ui2d") __attribute__ ((weak));
extern char mir_objload_ui2ld asm ("mir.ui2ld") __attribute__ ((weak));
extern char mir_objload_ld2i asm ("mir.ld2i") __attribute__ ((weak));

static void *objload_builtin (const char *name) {
  static const struct {
    const char *name;
    char *addr;
  } tab[] = {
    {"mir.va_arg", &mir_objload_va_arg},
    {"mir.va_block_arg", &mir_objload_va_block_arg},
    {"mir.arg_memcpy", &mir_objload_arg_memcpy},
    {"mir.ui2f", &mir_objload_ui2f},
    {"mir.ui2d", &mir_objload_ui2d},
    {"mir.ui2ld", &mir_objload_ui2ld},
    {"mir.ld2i", &mir_objload_ld2i},
  };
  for (size_t i = 0; i < sizeof (tab) / sizeof (tab[0]); i++)
    if (strcmp (name, tab[i].name) == 0) return tab[i].addr;
  return NULL;
}
#else
static void *objload_builtin (const char *name MIR_UNUSED) { return NULL; }
#endif

static int objload_sym_cmp (const void *a, const void *b) {
  return strcmp (((const objload_sym_t *) a)->name, ((const objload_sym_t *) b)->name);
}

#define OBJLOAD_ERR(...) \
  do { \
    if (err_msg != NULL && err_len != 0) snprintf (err_msg, err_len, __VA_ARGS__); \
  } while (0)

/* Shared ELF scan front for the emitter's ET_REL subset -- one parse for the
   in-process loader (MIR_object_load) and the merge reader (MIR_object_read).
   Validates the container, identifies the fixed alloc-section set (by flags
   + the .mir.addrpool name), and locates the symbol table.  Anything alloc
   beyond the four known sections is emitter growth these consumers do not
   understand -- fail loudly rather than drop it. */
typedef struct {
  const uint8_t *buf;
  size_t size;
  const Elf64_Ehdr *eh;
  const Elf64_Shdr *sh;
  int i_text, i_data, i_bss, i_pool, i_symtab;
  const Elf64_Sym *syms;
  size_t n_file_syms;
  const char *strtab;
  size_t strtab_size;
  int has_debug_p; /* any non-alloc .debug_* section present */
} objx_scan_t;

static int objx_scan (objx_scan_t *sc, const void *vbuf, size_t size, char *err_msg,
                      size_t err_len) {
  const uint8_t *buf = vbuf;

  memset (sc, 0, sizeof (*sc));
  sc->i_text = sc->i_data = sc->i_bss = sc->i_pool = sc->i_symtab = -1;
  if (MIR_DEBUG_EM != EM_X86_64) {
    OBJLOAD_ERR ("MIR objects are x86-64 only");
    return -1;
  }
  if (buf == NULL || size < sizeof (Elf64_Ehdr)) {
    OBJLOAD_ERR ("truncated or empty object");
    return -1;
  }
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *) buf;
  if (memcmp (eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64
      || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
    OBJLOAD_ERR ("not a 64-bit little-endian ELF file");
    return -1;
  }
  if (eh->e_type != ET_REL) {
    OBJLOAD_ERR ("not a relocatable object (ET_REL)");
    return -1;
  }
  if (eh->e_machine != EM_X86_64) {
    OBJLOAD_ERR ("not an x86-64 object");
    return -1;
  }
  if (eh->e_shentsize != sizeof (Elf64_Shdr) || eh->e_shnum == 0 || eh->e_shoff > size
      || (uint64_t) eh->e_shnum * sizeof (Elf64_Shdr) > size - eh->e_shoff
      || eh->e_shstrndx >= eh->e_shnum) {
    OBJLOAD_ERR ("malformed section header table");
    return -1;
  }
  const Elf64_Shdr *sh = (const Elf64_Shdr *) (buf + eh->e_shoff);

  /* Every non-NOBITS section body a consumer touches must lie inside the
     file; validate once up front. */
  for (int i = 0; i < eh->e_shnum; i++)
    if (sh[i].sh_type != SHT_NOBITS
        && (sh[i].sh_offset > size || sh[i].sh_size > size - sh[i].sh_offset)) {
      OBJLOAD_ERR ("section %d body out of file bounds", i);
      return -1;
    }
  const char *shstr = (const char *) (buf + sh[eh->e_shstrndx].sh_offset);
  size_t shstr_size = sh[eh->e_shstrndx].sh_size;

  /* Identify the fixed section set.  .mir.addrpool is picked out by name
     (its flags alone would classify it as data); the rest of the alloc
     sections are classified by their flags (EXECINSTR -> text, NOBITS ->
     bss, else data).  Non-alloc sections (.symtab, .rela.*, .note.GNU-stack,
     .debug_*) ride along or are consumed by name. */
  for (int i = 1; i < eh->e_shnum; i++) {
    if (sh[i].sh_name >= shstr_size) {
      OBJLOAD_ERR ("section %d name out of bounds", i);
      return -1;
    }
    if (sh[i].sh_type == SHT_SYMTAB) {
      if (sc->i_symtab >= 0) {
        OBJLOAD_ERR ("multiple symbol tables");
        return -1;
      }
      sc->i_symtab = i;
    }
    if ((sh[i].sh_flags & SHF_ALLOC) == 0) {
      if (strncmp (shstr + sh[i].sh_name, ".debug_", 7) == 0) sc->has_debug_p = 1;
      continue;
    }
    int *slot = strcmp (shstr + sh[i].sh_name, ".mir.addrpool") == 0 ? &sc->i_pool
                : (sh[i].sh_flags & SHF_EXECINSTR) != 0              ? &sc->i_text
                : sh[i].sh_type == SHT_NOBITS                        ? &sc->i_bss
                                                                     : &sc->i_data;
    if (*slot >= 0) {
      OBJLOAD_ERR ("unsupported extra alloc section %s", shstr + sh[i].sh_name);
      return -1;
    }
    *slot = i;
  }

  if (sc->i_symtab >= 0) {
    if (sh[sc->i_symtab].sh_entsize != sizeof (Elf64_Sym)
        || sh[sc->i_symtab].sh_link >= eh->e_shnum
        || sh[sh[sc->i_symtab].sh_link].sh_type != SHT_STRTAB) {
      OBJLOAD_ERR ("malformed symbol table");
      return -1;
    }
    sc->syms = (const Elf64_Sym *) (buf + sh[sc->i_symtab].sh_offset);
    sc->n_file_syms = sh[sc->i_symtab].sh_size / sizeof (Elf64_Sym);
    sc->strtab = (const char *) (buf + sh[sh[sc->i_symtab].sh_link].sh_offset);
    sc->strtab_size = sh[sh[sc->i_symtab].sh_link].sh_size;
  }
  sc->buf = buf;
  sc->size = size;
  sc->eh = eh;
  sc->sh = sh;
  return 0;
}

MIR_object_loaded_t MIR_object_load (const void *vbuf, size_t size,
                                     MIR_object_resolver_t resolver, void *env, char *err_msg,
                                     size_t err_len) {
#if !defined(MIR_DEBUG_HAVE_MMAP)
  (void) vbuf;
  (void) size;
  (void) resolver;
  (void) env;
  OBJLOAD_ERR ("in-process object loading is unsupported on this host (no mmap)");
  return NULL;
#else
  objx_scan_t sc;
  if (objx_scan (&sc, vbuf, size, err_msg, err_len) != 0) return NULL;
  const uint8_t *buf = sc.buf;
  const Elf64_Ehdr *eh = sc.eh;
  const Elf64_Shdr *sh = sc.sh;
  int i_text = sc.i_text, i_data = sc.i_data, i_bss = sc.i_bss, i_pool = sc.i_pool,
      i_symtab = sc.i_symtab;
  const Elf64_Sym *syms = sc.syms;
  size_t n_file_syms = sc.n_file_syms;
  const char *strtab = sc.strtab;
  size_t strtab_size = sc.strtab_size;

  /* One mapping, page-aligned regions: [text][data][bss][pool].  Page
     alignment dominates the emitter's section alignments (16 / data_align)
     and lets the text region change protection independently. */
  long ps = sysconf (_SC_PAGESIZE);
  size_t pg = ps > 0 ? (size_t) ps : 4096;
#define OBJLOAD_PGALIGN(x) (((x) + pg - 1) & ~(pg - 1))
  size_t text_size = i_text >= 0 ? sh[i_text].sh_size : 0;
  size_t data_size = i_data >= 0 ? sh[i_data].sh_size : 0;
  size_t bss_size = i_bss >= 0 ? sh[i_bss].sh_size : 0;
  size_t pool_size = i_pool >= 0 ? sh[i_pool].sh_size : 0;
  if ((i_text >= 0 && sh[i_text].sh_addralign > pg) || (i_data >= 0 && sh[i_data].sh_addralign > pg)
      || (i_bss >= 0 && sh[i_bss].sh_addralign > pg)
      || (i_pool >= 0 && sh[i_pool].sh_addralign > pg)) {
    OBJLOAD_ERR ("section alignment exceeds the page size");
    return NULL;
  }
  size_t data_off = OBJLOAD_PGALIGN (text_size);
  size_t bss_off = data_off + OBJLOAD_PGALIGN (data_size);
  size_t pool_off = bss_off + OBJLOAD_PGALIGN (bss_size);
  size_t map_size = pool_off + OBJLOAD_PGALIGN (pool_size);
  if (map_size == 0) map_size = pg;
  uint8_t *map
    = mmap (NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED) {
    OBJLOAD_ERR ("mmap of %zu bytes failed", map_size);
    return NULL;
  }
  if (text_size != 0) memcpy (map, buf + sh[i_text].sh_offset, text_size);
  if (data_size != 0) memcpy (map + data_off, buf + sh[i_data].sh_offset, data_size);
  if (pool_size != 0) memcpy (map + pool_off, buf + sh[i_pool].sh_offset, pool_size);
  /* .bss is zero by mmap. */

  /* Region base for a symbol defined in file section index shndx. */
#define OBJLOAD_REGION(shndx, out_base, out_size) \
  ((int) (shndx) == i_text  ? (*(out_base) = map, *(out_size) = text_size, 1) \
   : (int) (shndx) == i_data ? (*(out_base) = map + data_off, *(out_size) = data_size, 1) \
   : (int) (shndx) == i_bss  ? (*(out_base) = map + bss_off, *(out_size) = bss_size, 1) \
   : (int) (shndx) == i_pool ? (*(out_base) = map + pool_off, *(out_size) = pool_size, 1) \
                             : 0)

  /* Relocation pass over every SHT_RELA section targeting text or data.
     Unresolved imports are counted through to the end -- the resolver is
     consulted for every name, so a logging resolver reports the full miss
     list -- then the load fails as a whole. */
  size_t unresolved = 0;
  char first_unresolved[128] = "";
  for (int ri = 1; ri < eh->e_shnum; ri++) {
    if (sh[ri].sh_type != SHT_RELA) continue;
    if (sh[ri].sh_info >= eh->e_shnum) {
      OBJLOAD_ERR ("relocation section %d targets an out-of-range section", ri);
      goto fail_unmap;
    }
    /* Relocations against sections the loader does not map -- the .debug_*
       sections of a -g object -- are irrelevant in-process (GDB-JIT is the
       in-process debug story); skip them.  Only an unmapped ALLOC target is
       an error. */
    if ((sh[sh[ri].sh_info].sh_flags & SHF_ALLOC) == 0) continue;
    uint8_t *tgt_base;
    size_t tgt_size;
    if (!OBJLOAD_REGION (sh[ri].sh_info, &tgt_base, &tgt_size)) {
      OBJLOAD_ERR ("relocation section %d targets an unsupported section", ri);
      goto fail_unmap;
    }
    if (sh[ri].sh_entsize != sizeof (Elf64_Rela) || (int) sh[ri].sh_link != i_symtab
        || syms == NULL) {
      OBJLOAD_ERR ("malformed relocation section %d", ri);
      goto fail_unmap;
    }
    const Elf64_Rela *ra = (const Elf64_Rela *) (buf + sh[ri].sh_offset);
    size_t n_rel = sh[ri].sh_size / sizeof (Elf64_Rela);
    for (size_t k = 0; k < n_rel; k++) {
      unsigned rtype = (unsigned) ELF64_R_TYPE (ra[k].r_info);
      if (rtype != R_X86_64_64 && rtype != R_X86_64_PC32) {
        OBJLOAD_ERR ("unsupported relocation type %u (loader handles the emitter's "
                     "R_X86_64_64/R_X86_64_PC32 subset)",
                     rtype);
        goto fail_unmap;
      }
      size_t slot_size = rtype == R_X86_64_PC32 ? 4 : 8;
      size_t si = ELF64_R_SYM (ra[k].r_info);
      if (si >= n_file_syms || ra[k].r_offset > tgt_size
          || tgt_size - ra[k].r_offset < slot_size) {
        OBJLOAD_ERR ("malformed relocation %zu in section %d", k, ri);
        goto fail_unmap;
      }
      const Elf64_Sym *s = syms + si;
      uint64_t sval;
      if (s->st_shndx == SHN_UNDEF) {
        if (s->st_name >= strtab_size) {
          OBJLOAD_ERR ("symbol %zu name out of bounds", si);
          goto fail_unmap;
        }
        const char *nm = strtab + s->st_name;
        void *a = objload_builtin (nm);
        if (a == NULL && resolver != NULL) a = resolver (nm, env);
        if (a == NULL) {
          if (unresolved++ == 0) snprintf (first_unresolved, sizeof (first_unresolved), "%s", nm);
          continue;
        }
        sval = (uint64_t) a;
      } else {
        uint8_t *rbase;
        size_t rsize;
        if (!OBJLOAD_REGION (s->st_shndx, &rbase, &rsize)) {
          OBJLOAD_ERR ("relocation against a symbol in an unsupported section (%u)",
                       (unsigned) s->st_shndx);
          goto fail_unmap;
        }
        sval = (uint64_t) rbase + s->st_value;
      }
      if (rtype == R_X86_64_PC32) {
        int64_t d = (int64_t) sval + ra[k].r_addend
                    - (int64_t) (uint64_t) (tgt_base + ra[k].r_offset);
        if (d < INT32_MIN || d > INT32_MAX) {
          OBJLOAD_ERR ("PC32 relocation %zu in section %d out of 32-bit range", k, ri);
          goto fail_unmap;
        }
        int32_t d32 = (int32_t) d;
        memcpy (tgt_base + ra[k].r_offset, &d32, 4);
      } else {
        uint64_t v = sval + (uint64_t) ra[k].r_addend;
        memcpy (tgt_base + ra[k].r_offset, &v, 8);
      }
    }
  }
  if (unresolved != 0) {
    OBJLOAD_ERR ("%zu unresolved symbol(s), first: %s", unresolved, first_unresolved);
    goto fail_unmap;
  }

  if (text_size != 0 && mprotect (map, OBJLOAD_PGALIGN (text_size), PROT_READ | PROT_EXEC) != 0) {
    OBJLOAD_ERR ("mprotect of the text region failed");
    goto fail_unmap;
  }

  /* Export table: defined global/weak named symbols, sorted for bsearch. */
  MIR_object_loaded_t lo = calloc (1, sizeof (struct MIR_object_loaded));
  if (lo == NULL) goto fail_unmap;
  lo->map = map;
  lo->map_size = map_size;
  size_t n_exp = 0;
  for (size_t si = 0; si < n_file_syms; si++) {
    unsigned bind = ELF64_ST_BIND (syms[si].st_info);
    if (syms[si].st_name != 0 && syms[si].st_name < strtab_size
        && (bind == STB_GLOBAL || bind == STB_WEAK) && syms[si].st_shndx != SHN_UNDEF)
      n_exp++;
  }
  if (n_exp != 0 && (lo->syms = calloc (n_exp, sizeof (objload_sym_t))) != NULL) {
    for (size_t si = 0; si < n_file_syms; si++) {
      unsigned bind = ELF64_ST_BIND (syms[si].st_info);
      uint8_t *rbase;
      size_t rsize;
      if (syms[si].st_name == 0 || syms[si].st_name >= strtab_size
          || (bind != STB_GLOBAL && bind != STB_WEAK) || syms[si].st_shndx == SHN_UNDEF
          || !OBJLOAD_REGION (syms[si].st_shndx, &rbase, &rsize))
        continue;
      char *nm = obj_strdup (strtab + syms[si].st_name);
      if (nm == NULL) continue;
      lo->syms[lo->n_syms].name = nm;
      lo->syms[lo->n_syms].addr = rbase + syms[si].st_value;
      lo->n_syms++;
    }
    qsort (lo->syms, lo->n_syms, sizeof (objload_sym_t), objload_sym_cmp);
  }
  return lo;

fail_unmap:
  munmap (map, map_size);
  return NULL;
#undef OBJLOAD_REGION
#undef OBJLOAD_PGALIGN
#endif /* MIR_DEBUG_HAVE_MMAP */
}

void *MIR_object_loaded_sym (MIR_object_loaded_t lo, const char *name) {
  if (lo == NULL || name == NULL || lo->n_syms == 0) return NULL;
  objload_sym_t key;
  key.name = (char *) name;
  objload_sym_t *f = bsearch (&key, lo->syms, lo->n_syms, sizeof (objload_sym_t), objload_sym_cmp);
  return f == NULL ? NULL : f->addr;
}

void MIR_object_loaded_unload (MIR_object_loaded_t lo) {
  if (lo == NULL) return;
#if defined(MIR_DEBUG_HAVE_MMAP)
  if (lo->map != NULL) munmap (lo->map, lo->map_size);
#endif
  for (size_t i = 0; i < lo->n_syms; i++) free (lo->syms[i].name);
  free (lo->syms);
  free (lo);
}

/* ===== Merge reader (multi-object linking) =============================== */
/* MIR_object_read parses one MIR-emitted ET_REL image (through the same
   objx_scan front the loader uses) and APPENDS it into a builder: sections
   concatenate at their alignments, symbols unify by name, relocations are
   rebased.  Repeated reads over one builder ARE the link -- the merged
   builder then feeds the existing single-object consumers unchanged
   (MIR_object_emit for an ld -r shape, MIR_object_emit_executable for
   executables/shared objects, emit + MIR_object_load for in-process runs;
   internal cross-object references resolve at that final emit). */

static int objx_shndx_sec (const objx_scan_t *sc, int shndx) {
  return shndx == sc->i_text   ? MIR_OBJ_SEC_TEXT
         : shndx == sc->i_data ? MIR_OBJ_SEC_DATA
         : shndx == sc->i_bss  ? MIR_OBJ_SEC_BSS
         : shndx == sc->i_pool ? MIR_OBJ_SEC_ADDRPOOL
                               : -1;
}

/* Name -> dst symbol id for unification: the builder's pre-read non-local
   named symbols sorted for bsearch; symbols appended DURING the read live
   past first_new and are scanned linearly (a per-object tail, so no
   re-sort per add). */
typedef struct {
  const char *name;
  int id;
} objx_nmid_t;

static int objx_nmid_cmp (const void *a, const void *b) {
  return strcmp (((const objx_nmid_t *) a)->name, ((const objx_nmid_t *) b)->name);
}

static int objx_named_lookup (MIR_object_t obj, const objx_nmid_t *idx, size_t n_idx,
                              size_t first_new, const char *nm) {
  objx_nmid_t key;
  key.name = nm;
  key.id = 0;
  objx_nmid_t *f = bsearch (&key, idx, n_idx, sizeof key, objx_nmid_cmp);
  if (f != NULL) return f->id;
  for (size_t i = first_new; i < obj->n_syms; i++) {
    objsym_t *s = &obj->syms[i];
    if (s->name != NULL && !s->local_p && !s->section_p && strcmp (s->name, nm) == 0)
      return (int) i;
  }
  return -1;
}

int MIR_object_read (MIR_object_t obj, const void *vbuf, size_t size, char *err_msg,
                     size_t err_len) {
  objx_scan_t sc;
  int rc = -1;
  objx_nmid_t *idx = NULL;
  int *map = NULL;
  signed char *map_sec = NULL;

  if (obj == NULL) {
    OBJLOAD_ERR ("no object builder");
    return -1;
  }
  if (objx_scan (&sc, vbuf, size, err_msg, err_len) != 0) return -1;
  const Elf64_Shdr *sh = sc.sh;

  /* Alloc-section alignments: powers of two; .text is fixed at the
     emitter's 16 (MIR_object_text_append aligns to exactly that). */
  uint64_t al[4] = {1, 1, 1, 1};
  const int isec[4] = {sc.i_text, sc.i_data, sc.i_bss, sc.i_pool};
  for (int s = 0; s < 4; s++) {
    if (isec[s] < 0) continue;
    uint64_t a = sh[isec[s]].sh_addralign;
    if (a == 0) a = 1;
    if ((a & (a - 1)) != 0) {
      OBJLOAD_ERR ("section alignment %llu is not a power of two", (unsigned long long) a);
      return -1;
    }
    al[s] = a;
  }
  if (al[MIR_OBJ_SEC_TEXT] > 16) {
    OBJLOAD_ERR ("text alignment %llu unsupported (the emitter's is 16)",
                 (unsigned long long) al[MIR_OBJ_SEC_TEXT]);
    return -1;
  }

  /* Append the section bodies; the returned offsets are this object's
     rebase deltas.  An absent/empty section keeps a harmless base (no
     symbol or relocation can reference into it). */
  uint64_t base[4];
  base[MIR_OBJ_SEC_TEXT]
    = sc.i_text >= 0 && sh[sc.i_text].sh_size != 0
        ? MIR_object_text_append (obj, sc.buf + sh[sc.i_text].sh_offset, sh[sc.i_text].sh_size)
        : obj->text.len;
  base[MIR_OBJ_SEC_DATA]
    = sc.i_data >= 0 && sh[sc.i_data].sh_size != 0
        ? MIR_object_data_append (obj, sc.buf + sh[sc.i_data].sh_offset, sh[sc.i_data].sh_size,
                                  (size_t) al[MIR_OBJ_SEC_DATA])
        : obj->data.len;
  base[MIR_OBJ_SEC_BSS] = sc.i_bss >= 0 && sh[sc.i_bss].sh_size != 0
                            ? MIR_object_bss_reserve (obj, sh[sc.i_bss].sh_size,
                                                      (size_t) al[MIR_OBJ_SEC_BSS])
                            : obj->bss_size;
  base[MIR_OBJ_SEC_ADDRPOOL]
    = sc.i_pool >= 0 && sh[sc.i_pool].sh_size != 0
        ? MIR_object_addrpool_append (obj, sc.buf + sh[sc.i_pool].sh_offset,
                                      sh[sc.i_pool].sh_size, (size_t) al[MIR_OBJ_SEC_ADDRPOOL])
        : obj->pool.len;

  /* Sorted unification index over the PRE-read builder symbols. */
  size_t first_new = obj->n_syms, n_idx = 0;
  idx = malloc ((first_new ? first_new : 1) * sizeof (objx_nmid_t));
  map = malloc ((sc.n_file_syms ? sc.n_file_syms : 1) * sizeof (int));
  map_sec = malloc (sc.n_file_syms ? sc.n_file_syms : 1);
  if (idx == NULL || map == NULL || map_sec == NULL) {
    OBJLOAD_ERR ("out of memory");
    goto done;
  }
  for (size_t i = 0; i < first_new; i++) {
    objsym_t *s = &obj->syms[i];
    if (s->name == NULL || s->local_p || s->section_p) continue;
    idx[n_idx].name = s->name;
    idx[n_idx].id = (int) i;
    n_idx++;
  }
  qsort (idx, n_idx, sizeof (objx_nmid_t), objx_nmid_cmp);

  /* Symbols: file index -> dst id (map), with the section id recorded for
     section symbols (map_sec) -- their relocation addends absorb the
     rebase, a named symbol's dst value already carries it. */
  for (size_t i = 1; i < sc.n_file_syms; i++) {
    const Elf64_Sym *s = &sc.syms[i];
    unsigned st = ELF64_ST_TYPE (s->st_info), sb = ELF64_ST_BIND (s->st_info);
    map[i] = -1;
    map_sec[i] = -1;
    if (st == STT_SECTION) {
      int sec = objx_shndx_sec (&sc, (int) s->st_shndx);
      if (sec < 0) continue; /* a dropped (debug) section's symbol: only an
                                error if a kept relocation references it */
      map[i] = MIR_object_section_symbol (obj, sec);
      map_sec[i] = (signed char) sec;
      if (map[i] < 0) {
        OBJLOAD_ERR ("out of memory");
        goto done;
      }
      continue;
    }
    if (st != STT_NOTYPE && st != STT_OBJECT && st != STT_FUNC) {
      OBJLOAD_ERR ("unsupported symbol type %u (symbol %zu)", st, i);
      goto done;
    }
    if (s->st_shndx == SHN_ABS || s->st_shndx == SHN_COMMON) {
      OBJLOAD_ERR ("unsupported ABS/COMMON symbol %zu", i);
      goto done;
    }
    const char *nm = NULL;
    if (s->st_name != 0) {
      if (s->st_name >= sc.strtab_size) {
        OBJLOAD_ERR ("symbol %zu name out of bounds", i);
        goto done;
      }
      nm = sc.strtab + s->st_name;
    }
    int func_p = st == STT_FUNC;
    if (s->st_shndx == SHN_UNDEF) {
      if (nm == NULL) {
        OBJLOAD_ERR ("unnamed undefined symbol %zu", i);
        goto done;
      }
      int ex = objx_named_lookup (obj, idx, n_idx, first_new, nm);
      map[i] = ex >= 0 ? ex
                       : MIR_object_add_symbol (obj, nm, MIR_OBJ_SEC_UNDEF, 0, 0, func_p, 0, 0);
      if (map[i] < 0) {
        OBJLOAD_ERR ("out of memory");
        goto done;
      }
      continue;
    }
    int sec = objx_shndx_sec (&sc, (int) s->st_shndx);
    if (sec < 0) {
      OBJLOAD_ERR ("symbol %zu defined in an unsupported section (%u)", i,
                   (unsigned) s->st_shndx);
      goto done;
    }
    uint64_t val = s->st_value + base[sec];
    if (sb == STB_LOCAL) { /* locals never unify; duplicate names are fine */
      map[i] = MIR_object_add_symbol (obj, nm, sec, val, s->st_size, func_p, 1, 0);
      if (map[i] < 0) {
        OBJLOAD_ERR ("out of memory");
        goto done;
      }
      continue;
    }
    int weak_p = sb == STB_WEAK;
    int ex = nm != NULL ? objx_named_lookup (obj, idx, n_idx, first_new, nm) : -1;
    if (ex < 0) {
      map[i] = MIR_object_add_symbol (obj, nm, sec, val, s->st_size, func_p, 0, weak_p);
      if (map[i] < 0) {
        OBJLOAD_ERR ("out of memory");
        goto done;
      }
      continue;
    }
    objsym_t *es = &obj->syms[ex];
    if (!es->defined_p) {
      MIR_object_symbol_define (obj, ex, sec, val, s->st_size, func_p, 0, weak_p);
      map[i] = ex;
    } else if (weak_p) {
      map[i] = ex; /* an existing definition (strong or first weak) wins */
    } else if (es->weak_p) {
      /* a strong definition replaces a weak one, in place */
      es->sec = sec;
      es->value = val;
      es->size = s->st_size;
      es->func_p = func_p != 0;
      es->weak_p = 0;
      es->defined_p = 1;
      map[i] = ex;
    } else {
      OBJLOAD_ERR ("duplicate symbol '%s'", nm);
      goto done;
    }
  }

  /* Relocations: kept for the alloc sections (offset and section-symbol
     addends rebased); .rela.debug_* drops with its sections. */
  for (int ri = 1; ri < sc.eh->e_shnum; ri++) {
    if (sh[ri].sh_type != SHT_RELA) continue;
    if (sh[ri].sh_info >= sc.eh->e_shnum) {
      OBJLOAD_ERR ("relocation section %d targets an out-of-range section", ri);
      goto done;
    }
    int tsec = objx_shndx_sec (&sc, (int) sh[ri].sh_info);
    if (tsec < 0) continue; /* .rela.debug_*: dropped with the debug sections */
    if (tsec == MIR_OBJ_SEC_BSS) {
      OBJLOAD_ERR ("relocation section %d targets .bss", ri);
      goto done;
    }
    if (sh[ri].sh_entsize != sizeof (Elf64_Rela) || (int) sh[ri].sh_link != sc.i_symtab
        || sc.syms == NULL) {
      OBJLOAD_ERR ("malformed relocation section %d", ri);
      goto done;
    }
    const Elf64_Rela *ra = (const Elf64_Rela *) (sc.buf + sh[ri].sh_offset);
    size_t n_rel = sh[ri].sh_size / sizeof (Elf64_Rela);
    for (size_t k = 0; k < n_rel; k++) {
      unsigned rtype = (unsigned) ELF64_R_TYPE (ra[k].r_info);
      int kind = rtype == R_X86_64_64      ? MIR_OBJ_RELOC_ABS64
                 : rtype == R_X86_64_PC32 ? MIR_OBJ_RELOC_PC32
                                          : -1;
      if (kind < 0) {
        OBJLOAD_ERR ("unsupported relocation type %u (the emitter's subset is "
                     "R_X86_64_64/R_X86_64_PC32)",
                     rtype);
        goto done;
      }
      size_t si = ELF64_R_SYM (ra[k].r_info);
      if (si >= sc.n_file_syms || map[si] < 0) {
        OBJLOAD_ERR ("relocation %zu in section %d against an unmapped symbol", k, ri);
        goto done;
      }
      int64_t addend = ra[k].r_addend;
      if (map_sec[si] >= 0) addend += (int64_t) base[(int) map_sec[si]];
      MIR_object_add_reloc (obj, tsec, ra[k].r_offset + base[tsec], map[si], addend, kind);
    }
  }
  rc = sc.has_debug_p ? 1 : 0; /* 1: merged, but debug sections were dropped */

done:
  free (idx);
  free (map);
  free (map_sec);
  return rc;
}

const char *MIR_object_undef_name (MIR_object_t obj, size_t idx) {
  if (obj == NULL) return NULL;
  for (size_t i = 0; i < obj->n_syms; i++) {
    objsym_t *s = &obj->syms[i];
    if (s->defined_p || s->name == NULL) continue;
    if (idx == 0) return s->name;
    idx--;
  }
  return NULL;
}
#else
int MIR_debug_emit (MIR_debug_t d MIR_UNUSED, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1; /* no <elf.h> on this host */
}

MIR_object_t MIR_object_create (void) { return NULL; }
void MIR_object_destroy (MIR_object_t obj MIR_UNUSED) {}
size_t MIR_object_text_append (MIR_object_t obj MIR_UNUSED, const void *bytes MIR_UNUSED,
                               size_t len MIR_UNUSED) {
  return 0;
}
size_t MIR_object_data_append (MIR_object_t obj MIR_UNUSED, const void *bytes MIR_UNUSED,
                               size_t len MIR_UNUSED, size_t align MIR_UNUSED) {
  return 0;
}
size_t MIR_object_bss_reserve (MIR_object_t obj MIR_UNUSED, size_t len MIR_UNUSED,
                               size_t align MIR_UNUSED) {
  return 0;
}
size_t MIR_object_addrpool_append (MIR_object_t obj MIR_UNUSED, const void *bytes MIR_UNUSED,
                                   size_t len MIR_UNUSED, size_t align MIR_UNUSED) {
  return 0;
}
int MIR_object_add_symbol (MIR_object_t obj MIR_UNUSED, const char *name MIR_UNUSED,
                           int sec MIR_UNUSED, uint64_t value MIR_UNUSED,
                           uint64_t size MIR_UNUSED, int func_p MIR_UNUSED,
                           int local_p MIR_UNUSED, int weak_p MIR_UNUSED) {
  return -1;
}
void MIR_object_symbol_define (MIR_object_t obj MIR_UNUSED, int sym_id MIR_UNUSED,
                               int sec MIR_UNUSED, uint64_t value MIR_UNUSED,
                               uint64_t size MIR_UNUSED, int func_p MIR_UNUSED,
                               int local_p MIR_UNUSED, int weak_p MIR_UNUSED) {}
int MIR_object_section_symbol (MIR_object_t obj MIR_UNUSED, int sec MIR_UNUSED) { return -1; }
int MIR_object_symbol_defined_p (MIR_object_t obj MIR_UNUSED, int sym_id MIR_UNUSED) { return 0; }
void MIR_object_add_reloc (MIR_object_t obj MIR_UNUSED, int sec MIR_UNUSED,
                           uint64_t offset MIR_UNUSED, int sym_id MIR_UNUSED,
                           int64_t addend MIR_UNUSED, int kind MIR_UNUSED) {}
int MIR_object_find_symbol (MIR_object_t obj MIR_UNUSED, const char *name MIR_UNUSED,
                            int *sec MIR_UNUSED, uint64_t *value MIR_UNUSED,
                            uint64_t *size MIR_UNUSED) {
  return 0;
}
void MIR_object_set_debug (MIR_object_t obj MIR_UNUSED, MIR_debug_t d MIR_UNUSED) {}
int MIR_object_emit (MIR_object_t obj MIR_UNUSED, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1;
}
int MIR_object_emit_executable (MIR_object_t obj MIR_UNUSED,
                                const MIR_object_exec_params *params MIR_UNUSED, void **buf,
                                size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1;
}
MIR_object_loaded_t MIR_object_load (const void *buf MIR_UNUSED, size_t size MIR_UNUSED,
                                     MIR_object_resolver_t resolver MIR_UNUSED,
                                     void *env MIR_UNUSED, char *err_msg, size_t err_len) {
  if (err_msg != NULL && err_len != 0)
    snprintf (err_msg, err_len, "no <elf.h> on this host");
  return NULL;
}
void *MIR_object_loaded_sym (MIR_object_loaded_t lo MIR_UNUSED, const char *name MIR_UNUSED) {
  return NULL;
}
void MIR_object_loaded_unload (MIR_object_loaded_t lo MIR_UNUSED) {}
int MIR_object_read (MIR_object_t obj MIR_UNUSED, const void *vbuf MIR_UNUSED,
                     size_t size MIR_UNUSED, char *err_msg, size_t err_len) {
  if (err_msg != NULL && err_len != 0)
    snprintf (err_msg, err_len, "no <elf.h> on this host");
  return -1;
}
const char *MIR_object_undef_name (MIR_object_t obj MIR_UNUSED, size_t idx MIR_UNUSED) {
  return NULL;
}
#endif

/* The GDB JIT interface (the process-global __jit_debug_descriptor and
   MIR_debug_gdb_register/unregister) lives in mir-debug-gdb.c -- a separate
   translation unit so embedders that own their own descriptor can link this
   builder + MIR_debug_emit without a second one. */
