/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
   aarch64 call ABI target specific code.
*/

typedef int target_arg_info_t;

static void target_init_arg_vars (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                  target_arg_info_t *arg_info MIR_UNUSED) {}

static int target_return_by_addr_p (c2m_ctx_t c2m_ctx, struct type *ret_type) {
  return ((ret_type->mode == TM_STRUCT || ret_type->mode == TM_UNION)
          && type_size (c2m_ctx, ret_type) > 2 * 8);
}

static int reg_aggregate_size (c2m_ctx_t c2m_ctx, struct type *type) {
  size_t size;

  /* __int128 is memory-shaped in c2mir (memory_value_type_p) and rides the
     two-GPR lane a 16-byte aggregate uses: x0:x1 as a result, two GPRs (or
     a BLK on the stack) as an argument -- AAPCS64 C.9 for a 16-byte integer.
     Without this arm simple_add_res_proto asked get_mir_type for a MIR type
     __int128 does not have ("wrong result type in proto", testint128 on
     arm64).  AAPCS64's even-numbered-pair rule for a 16-byte fundamental
     argument (NGRN rounded up to even) is a recorded refinement: c2mir's BLK
     lane places it in the next two GPRs. */
  if (int128_type_p (type)) return 16;
  if (type->mode != TM_STRUCT && type->mode != TM_UNION) return -1;
  return (size = type_size (c2m_ctx, type)) <= 2 * 8 ? (int) size : -1;
}

static void target_add_res_proto (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                  target_arg_info_t *arg_info, VARR (MIR_type_t) * res_types,
                                  VARR (MIR_var_t) * arg_vars) {
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_res_proto (c2m_ctx, ret_type, arg_info, res_types, arg_vars);
    return;
  }
  if (size == 0) return;
  VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
  if (size > 8) VARR_PUSH (MIR_type_t, res_types, MIR_T_I64);
}

static int target_add_call_res_op (c2m_ctx_t c2m_ctx, struct type *ret_type,
                                   target_arg_info_t *arg_info, size_t call_arg_area_offset) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_add_call_res_op (c2m_ctx, ret_type, arg_info, call_arg_area_offset);
  if (size == 0) return -1;
  VARR_PUSH (MIR_op_t, call_ops,
             MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  if (size > 8)
    VARR_PUSH (MIR_op_t, call_ops,
               MIR_new_reg_op (ctx, get_new_temp (c2m_ctx, MIR_T_I64).mir_op.u.reg));
  return size <= 8 ? 1 : 2;
}

static op_t target_gen_post_call_res_code (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res,
                                           MIR_insn_t call, size_t call_ops_start) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0)
    return simple_gen_post_call_res_code (c2m_ctx, ret_type, res, call, call_ops_start);
  if (size != 0)
    gen_multiple_load_store (c2m_ctx, ret_type, &VARR_ADDR (MIR_op_t, call_ops)[call_ops_start + 2],
                             res.mir_op, FALSE);
  return res;
}

static void target_add_ret_ops (c2m_ctx_t c2m_ctx, struct type *ret_type, op_t res) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  int i, size;

  if ((size = reg_aggregate_size (c2m_ctx, ret_type)) < 0) {
    simple_add_ret_ops (c2m_ctx, ret_type, res);
    return;
  }
  assert (res.mir_op.mode == MIR_OP_MEM && VARR_LENGTH (MIR_op_t, ret_ops) == 0 && size <= 2 * 8);
  for (i = 0; size > 0; size -= 8, i++)
    VARR_PUSH (MIR_op_t, ret_ops, get_new_temp (c2m_ctx, MIR_T_I64).mir_op);
  gen_multiple_load_store (c2m_ctx, ret_type, VARR_ADDR (MIR_op_t, ret_ops), res.mir_op, TRUE);
}

static MIR_type_t target_get_blk_type (c2m_ctx_t c2m_ctx MIR_UNUSED,
                                       struct type *arg_type MIR_UNUSED) {
  return MIR_T_BLK; /* one BLK is enough */
}

/* _Complex T is a Homogeneous Floating-point Aggregate (AAPCS64 C.1, C.2):
   its two components travel in two consecutive SIMD/FP registers -- s0,s1 /
   d0,d1 / q0,q1 -- never in GPRs.  c2mir keeps complex values memory-shaped
   (memory_value_type_p), so the argument is split into two scalar FP args at
   the call and gathered back into the parameter's frame home in the callee's
   prologue; the return side already travels as two FP results.  The BLK lane
   the simple_* helpers take put the 8 / 16 bytes in x0(:x1) instead: a libm
   callee compiled by clang (conjf, crealf, cimag) read s0/s1 -- garbage --
   and the tests' own abort() on the wrong value read as a MIR crash
   (dispatch #9: testbuiltinconjf, testbuiltincomplexparts).  Fidelity note:
   when the FP registers are exhausted AAPCS64 moves the WHOLE HFA to the
   stack; two independent scalar args may split -- both MIR sides agree, and
   no libm entry takes nine FP arguments. */
static MIR_type_t complex_component_mir_type (struct type *type) {
  return (type->u.basic_type == TP_CFLOAT    ? MIR_T_F
          : type->u.basic_type == TP_CDOUBLE ? MIR_T_D
                                             : MIR_T_LD);
}

static int complex_imag_offset (struct type *type) {
  return (type->u.basic_type == TP_CFLOAT    ? (int) sizeof (mir_float)
          : type->u.basic_type == TP_CDOUBLE ? (int) sizeof (mir_double)
                                             : (int) sizeof (mir_ldouble));
}

static void target_add_arg_proto (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  target_arg_info_t *arg_info, VARR (MIR_var_t) * arg_vars) {
  MIR_var_t var;

  if (complex_type_p (arg_type)) {
    var.type = complex_component_mir_type (arg_type);
    var.name = gen_get_indexed_name (c2m_ctx, name, 0);
    VARR_PUSH (MIR_var_t, arg_vars, var);
    var.name = gen_get_indexed_name (c2m_ctx, name, 1);
    VARR_PUSH (MIR_var_t, arg_vars, var);
    return;
  }
  simple_add_arg_proto (c2m_ctx, name, arg_type, arg_info, arg_vars);
}

static void target_add_call_arg_op (c2m_ctx_t c2m_ctx, struct type *arg_type,
                                    target_arg_info_t *arg_info, op_t arg) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;

  /* Declared parameters only: a _Complex VARARG stays a block -- MIR's aarch64
     va_block_arg reads the GP/stack area, so the two sides of a MIR<->MIR
     variadic call keep agreeing (AAPCS64 conformance for variadic HFAs is a
     recorded refinement, together with the va_arg side). */
  if (complex_type_p (arg_type) && !call_arg_vararg_p) {
    MIR_type_t ct = complex_component_mir_type (arg_type);

    assert (arg.mir_op.mode == MIR_OP_MEM);
    VARR_PUSH (MIR_op_t, call_ops, complex_load (c2m_ctx, arg, ct, 0).mir_op);
    VARR_PUSH (MIR_op_t, call_ops,
               complex_load (c2m_ctx, arg, ct, complex_imag_offset (arg_type)).mir_op);
    return;
  }
  simple_add_call_arg_op (c2m_ctx, arg_type, arg_info, arg);
}

static int target_gen_gather_arg (c2m_ctx_t c2m_ctx, const char *name, struct type *arg_type,
                                  decl_t param_decl, target_arg_info_t *arg_info MIR_UNUSED) {
  gen_ctx_t gen_ctx = c2m_ctx->gen_ctx;
  MIR_context_t ctx = c2m_ctx->ctx;
  MIR_type_t ct;
  MIR_reg_t fp_reg;
  MIR_alias_t alias;
  op_t home;

  if (!complex_type_p (arg_type)) return FALSE;
  ct = complex_component_mir_type (arg_type);
  fp_reg = MIR_reg (ctx, FP_NAME, curr_func->u.func);
  alias = get_type_alias (c2m_ctx, arg_type);
  home = new_op (param_decl,
                 MIR_new_alias_mem_op (ctx, ct, param_decl->offset, fp_reg, 0, 1, alias, 0));
  emit2 (c2m_ctx, tp_mov (ct), home.mir_op,
         MIR_new_reg_op (ctx, get_reg_var (c2m_ctx, MIR_T_UNDEF,
                                           gen_get_indexed_name (c2m_ctx, name, 0), NULL)
                                .reg));
  home.mir_op = MIR_new_alias_mem_op (ctx, ct, param_decl->offset + complex_imag_offset (arg_type),
                                      fp_reg, 0, 1, alias, 0);
  emit2 (c2m_ctx, tp_mov (ct), home.mir_op,
         MIR_new_reg_op (ctx, get_reg_var (c2m_ctx, MIR_T_UNDEF,
                                           gen_get_indexed_name (c2m_ctx, name, 1), NULL)
                                .reg));
  return TRUE;
}
