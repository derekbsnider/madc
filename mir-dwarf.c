/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Generic GDB-JIT DWARF debug-object emitter -- see mir-dwarf.h. */

#include "mir-dwarf.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<elf.h>)
#include <elf.h>
#define MIR_DWARF_HAVE_ELF 1
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
#define MIR_DWARF_FP_OP DW_OP_reg29
#define MIR_DWARF_EM EM_AARCH64
#elif defined(__x86_64__)
#define MIR_DWARF_FP_OP DW_OP_reg6
#define MIR_DWARF_EM EM_X86_64
#elif defined(__riscv) && __riscv_xlen == 64
#define MIR_DWARF_FP_OP DW_OP_reg6 /* not validated; see frontend note */
#define MIR_DWARF_EM EM_RISCV
#elif defined(__powerpc64__)
#define MIR_DWARF_FP_OP DW_OP_reg6
#define MIR_DWARF_EM EM_PPC64
#elif defined(__s390x__)
#define MIR_DWARF_FP_OP DW_OP_reg6
#define MIR_DWARF_EM EM_S390
#else
#define MIR_DWARF_FP_OP DW_OP_reg6
#define MIR_DWARF_EM 0
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
struct MIR_dwarf {
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

MIR_dwarf_t MIR_dwarf_init (void) {
  if (MIR_DWARF_EM == 0) return NULL;
  MIR_dwarf_t d = calloc (1, sizeof (struct MIR_dwarf));
  if (d == NULL) return NULL;
  /* type 0 == void */
  dwtype_t v = {.kind = DT_VOID, .ref = 0, .count = -1, .first_member = -1, .last_member = -1,
                .name = dw_strdup ("void")};
  VEC_PUSH (d, types, v);
  return d;
}

void MIR_dwarf_destroy (MIR_dwarf_t d) {
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

uint32_t MIR_dwarf_add_file (MIR_dwarf_t d, const char *path) {
  for (int i = 0; i < d->n_files; i++)
    if (strcmp (d->files[i], path) == 0) return (uint32_t) (i + 1);
  char *s = dw_strdup (path);
  VEC_PUSH (d, files, s);
  return (uint32_t) d->n_files;
}

static uint32_t new_type (MIR_dwarf_t d, int kind) {
  dwtype_t t = {.kind = kind, .ref = 0, .count = -1, .first_member = -1, .last_member = -1};
  VEC_PUSH (d, types, t);
  return (uint32_t) (d->n_types - 1);
}

MIR_dwarf_type_t MIR_dwarf_base_type (MIR_dwarf_t d, const char *name, MIR_dwarf_encoding_t enc,
                                      int byte_size) {
  static const int map[] = {0, DW_ATE_signed, DW_ATE_unsigned, DW_ATE_float, DW_ATE_boolean,
                            DW_ATE_signed_char, DW_ATE_unsigned_char};
  uint32_t i = new_type (d, DT_BASE);
  d->types[i].enc = (enc >= 1 && enc <= 6) ? map[enc] : DW_ATE_signed;
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_dwarf_type_t MIR_dwarf_pointer_type (MIR_dwarf_t d, MIR_dwarf_type_t pointee) {
  uint32_t i = new_type (d, DT_PTR);
  d->types[i].ref = pointee;
  d->types[i].size = sizeof (void *);
  return i;
}
MIR_dwarf_type_t MIR_dwarf_array_type (MIR_dwarf_t d, MIR_dwarf_type_t elem, int64_t count) {
  uint32_t i = new_type (d, DT_ARRAY);
  d->types[i].ref = elem;
  d->types[i].count = count;
  return i;
}
MIR_dwarf_type_t MIR_dwarf_typedef_type (MIR_dwarf_t d, const char *name, MIR_dwarf_type_t ref) {
  uint32_t i = new_type (d, DT_TYPEDEF);
  d->types[i].ref = ref;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_dwarf_type_t MIR_dwarf_struct_type (MIR_dwarf_t d, const char *name, int64_t byte_size,
                                        int is_union) {
  uint32_t i = new_type (d, is_union ? DT_UNION : DT_STRUCT);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_dwarf_type_t MIR_dwarf_enum_type (MIR_dwarf_t d, const char *name, int64_t byte_size) {
  uint32_t i = new_type (d, DT_ENUM);
  d->types[i].size = byte_size;
  d->types[i].name = dw_strdup (name);
  return i;
}
MIR_dwarf_type_t MIR_dwarf_func_type (MIR_dwarf_t d, MIR_dwarf_type_t ret_type) {
  uint32_t i = new_type (d, DT_FUNC);
  d->types[i].ref = ret_type;
  return i;
}

static void append_member (MIR_dwarf_t d, MIR_dwarf_type_t agg, dwmember_t m) {
  m.next = -1;
  VEC_PUSH (d, members, m);
  int idx = d->n_members - 1;
  if (d->types[agg].first_member < 0)
    d->types[agg].first_member = idx;
  else
    d->members[d->types[agg].last_member].next = idx;
  d->types[agg].last_member = idx;
}

void MIR_dwarf_add_member (MIR_dwarf_t d, MIR_dwarf_type_t agg, const char *name,
                           MIR_dwarf_type_t type, int64_t byte_offset) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .off = byte_offset};
  append_member (d, agg, m);
}
void MIR_dwarf_add_bitfield (MIR_dwarf_t d, MIR_dwarf_type_t agg, const char *name,
                             MIR_dwarf_type_t type, int64_t bit_offset, int bit_size) {
  dwmember_t m = {.name = dw_strdup (name), .type = type, .bit_off = (int) bit_offset,
                  .bit_size = bit_size};
  append_member (d, agg, m);
}
void MIR_dwarf_add_enumerator (MIR_dwarf_t d, MIR_dwarf_type_t en, const char *name, int64_t value) {
  dwmember_t m = {.name = dw_strdup (name), .off = value, .is_enumerator = 1};
  append_member (d, en, m);
}
void MIR_dwarf_add_param_type (MIR_dwarf_t d, MIR_dwarf_type_t fn, MIR_dwarf_type_t type) {
  dwmember_t m = {.type = type};
  append_member (d, fn, m);
}

void MIR_dwarf_add_func (MIR_dwarf_t d, const char *name, const void *addr, size_t size,
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

void MIR_dwarf_add_var (MIR_dwarf_t d, const char *name, int is_param, MIR_dwarf_type_t type,
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

static void emit_info (MIR_dwarf_t d, dwbuf_t *b, uint64_t text_base, uint64_t text_size,
                       const char *cu_name) {
  size_t unit_len_pos = b->len;
  buf_u32 (b, 0);
  size_t after_len = b->len;
  buf_u16 (b, 4);
  buf_u32 (b, 0);
  buf_u8 (b, 8);
  buf_uleb (b, A_CU);
  buf_str (b, "mir-dwarf");
  buf_u16 (b, DW_LANG_C99);
  buf_str (b, cu_name ? cu_name : "");
  buf_str (b, "");
  buf_u64 (b, text_base);
  buf_u64 (b, text_base + text_size);
  buf_u32 (b, 0); /* stmt_list */

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
    if (fn->addr == NULL) continue;
    buf_uleb (b, A_SUBPROG);
    buf_str (b, fn->name);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr + fn->size);
    buf_u8 (b, 1); buf_u8 (b, MIR_DWARF_FP_OP);
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

static void emit_line (MIR_dwarf_t d, dwbuf_t *b) {
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
    if (fn->addr == NULL || fn->line_map == NULL || fn->line_map_len == 0) continue;
    buf_u8 (b, 0); buf_uleb (b, 9); buf_u8 (b, DW_LNE_set_address);
    buf_u64 (b, (uint64_t) (uintptr_t) fn->addr);
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

#ifdef MIR_DWARF_HAVE_ELF
/* One ELF section descriptor for the output object assembler below.  A named
   type (rather than an anonymous struct + typeof) so the file stays compilable
   by C frontends without the GNU typeof extension -- e.g. MIR's own c2m, which
   builds mir-dwarf.c as part of its self-bootstrap. */
typedef struct {
  const char *name;
  uint32_t type, link, info, align;
  uint64_t flags, addr, entsize;
  const dwbuf_t *body;
  uint64_t size;
} dwsec_t;

int MIR_dwarf_emit (MIR_dwarf_t d, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  if (d == NULL || buf == NULL || size == NULL || MIR_DWARF_EM == 0) return -1;

  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (int i = 0; i < d->n_funcs; i++) {
    if (d->funcs[i].addr == NULL) continue;
    uintptr_t a = (uintptr_t) d->funcs[i].addr, e = a + (d->funcs[i].size ? d->funcs[i].size : 1);
    if (a < lo) lo = a;
    if (e > hi) hi = e;
  }
  if (lo == UINTPTR_MAX) lo = hi = 0;
  uint64_t text_base = lo, text_size = hi > lo ? (uint64_t) (hi - lo) : 0;

  /* section bodies */
  dwbuf_t strtab = {0}, symtab = {0}, abbrev = {0}, info = {0}, line = {0};
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
  emit_info (d, &info, text_base, text_size, cu_name);
  emit_line (d, &line);

  dwsec_t S[16];
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
  int i_shstr = ns;
  dwbuf_t shstr = {0};
  buf_u8 (&shstr, 0);
  uint32_t name_off[16];
  name_off[0] = 0;
  for (int i = 1; i < ns; i++) { name_off[i] = (uint32_t) shstr.len; buf_str (&shstr, S[i].name); }
  uint32_t name_shstr = (uint32_t) shstr.len;
  buf_str (&shstr, ".shstrtab");
  S[ns++] = (dwsec_t){".shstrtab", SHT_STRTAB, 0, 0, 1, 0, 0, 0, &shstr, shstr.len};
  name_off[i_shstr] = name_shstr;

  size_t off = sizeof (Elf64_Ehdr);
  uint64_t sec_off[16] = {0};
  for (int i = 1; i < ns; i++) {
    if (S[i].type == SHT_NOBITS) { sec_off[i] = off; continue; }
    off = (off + 7) & ~(size_t) 7;
    sec_off[i] = off;
    off += S[i].size;
  }
  off = (off + 7) & ~(size_t) 7;
  size_t shoff = off, total = shoff + (size_t) ns * sizeof (Elf64_Shdr);

  unsigned char *p = calloc (1, total);
  if (p == NULL) {
    free (strtab.p); free (symtab.p); free (abbrev.p); free (info.p); free (line.p); free (shstr.p);
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
  eh->e_machine = MIR_DWARF_EM;
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
  free (strtab.p); free (symtab.p); free (abbrev.p); free (info.p); free (line.p); free (shstr.p);
  *buf = p;
  *size = total;
  return 0;
}
#else
int MIR_dwarf_emit (MIR_dwarf_t d MIR_UNUSED, void **buf, size_t *size) {
  if (buf != NULL) *buf = NULL;
  if (size != NULL) *size = 0;
  return -1; /* no <elf.h> on this host */
}
#endif

/* The GDB JIT interface (the process-global __jit_debug_descriptor and
   MIR_dwarf_gdb_register/unregister) lives in mir-dwarf-gdb.c -- a separate
   translation unit so embedders that own their own descriptor can link this
   builder + MIR_dwarf_emit without a second one. */
