/* c2mir_api.h — Public API for building c2mir AST nodes externally.
   Part of MIR project (madc fork).

   When included from c2mir.c, internal types are already defined.
   When included from external code, minimal forward declarations
   are provided. External code uses c2mir_pos_t (3 ints) for positions
   and gets node_code_t via c2mir_node_code.h.
*/

#ifndef C2MIR_API_H
#define C2MIR_API_H

#include "c2mir.h"
#include "c2mir_node_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types (forward declarations for external use) ---- */

#ifndef C2MIR_INTERNAL
/* When not inside c2mir.c, provide opaque types */
typedef struct c2m_ctx *c2m_ctx_t;
typedef struct node *node_t;
#endif

/* Position for source locations (matches c2mir.c's pos_t layout) */
typedef struct c2mir_pos {
  const char *fname;
  int lno, ln_pos;
} c2mir_pos_t;

/* ---- Context management ---- */

c2m_ctx_t c2mir_init_compile (MIR_context_t ctx, struct c2mir_options *ops);
void c2mir_finish_compile (c2m_ctx_t c2m_ctx);

/* ---- Node creation ---- */

node_t c2mir_new_node (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc);
node_t c2mir_new_node1 (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, node_t op1);
node_t c2mir_new_node2 (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, node_t op1, node_t op2);
node_t c2mir_new_node3 (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, node_t op1, node_t op2, node_t op3);
node_t c2mir_new_node4 (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, node_t op1, node_t op2, node_t op3, node_t op4);
node_t c2mir_new_node5 (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, node_t op1, node_t op2, node_t op3, node_t op4, node_t op5);
node_t c2mir_op_append (c2m_ctx_t c2m_ctx, node_t n, node_t child);

/* Literal nodes */
node_t c2mir_new_i_node (c2m_ctx_t c2m_ctx, long val, c2mir_pos_t pos);
node_t c2mir_new_l_node (c2m_ctx_t c2m_ctx, long val, c2mir_pos_t pos);
node_t c2mir_new_ll_node (c2m_ctx_t c2m_ctx, long long val, c2mir_pos_t pos);
node_t c2mir_new_u_node (c2m_ctx_t c2m_ctx, unsigned long val, c2mir_pos_t pos);
node_t c2mir_new_d_node (c2m_ctx_t c2m_ctx, double val, c2mir_pos_t pos);
node_t c2mir_new_str_node (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, const char *s, size_t len, c2mir_pos_t pos);

void c2mir_set_node_pos (c2m_ctx_t c2m_ctx, node_t n, c2mir_pos_t pos);

/* ---- Compilation from external AST ---- */

int c2mir_compile_tree (MIR_context_t ctx, c2m_ctx_t c2m_ctx,
                        node_t tree, const char *module_name);

#ifdef __cplusplus
}
#endif

#endif /* C2MIR_API_H */
