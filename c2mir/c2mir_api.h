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
node_t c2mir_op_tail (c2m_ctx_t c2m_ctx, node_t n);
node_t c2mir_op_splice_after (c2m_ctx_t c2m_ctx, node_t dst, node_t after, node_t src);

/* Literal nodes */
node_t c2mir_new_i_node (c2m_ctx_t c2m_ctx, long val, c2mir_pos_t pos);
node_t c2mir_new_l_node (c2m_ctx_t c2m_ctx, long val, c2mir_pos_t pos);
node_t c2mir_new_ll_node (c2m_ctx_t c2m_ctx, long long val, c2mir_pos_t pos);
node_t c2mir_new_u_node (c2m_ctx_t c2m_ctx, unsigned long val, c2mir_pos_t pos);
node_t c2mir_new_d_node (c2m_ctx_t c2m_ctx, double val, c2mir_pos_t pos);
node_t c2mir_new_str_node (c2m_ctx_t c2m_ctx, c2mir_node_code_t nc, const char *s, size_t len, c2mir_pos_t pos);

void c2mir_set_node_pos (c2m_ctx_t c2m_ctx, node_t n, c2mir_pos_t pos);

/* ---- External tree support ---- */

/* Assign the next uid and ensure node_positions has a slot.
   Used by external code that allocates its own node memory. */
unsigned c2mir_next_uid (c2m_ctx_t c2m_ctx);

/* Intern a string in c2mir's string table. Returns the interned pointer.
   The returned pointer is valid until c2mir_finish(). */
const char *c2mir_uniq_str (c2m_ctx_t c2m_ctx, const char *s, size_t len);

/* Node-code -> name (e.g. N_MODULE -> "MODULE"), for external AST
   walkers/dumpers. Matches the names c2m -d prints. */
const char *c2mir_node_code_name (c2mir_node_code_t code);

/* i-th operand of a composite node (NULL past the end), for external
   AST walkers that can't see c2mir's generated DLIST accessors. */
node_t c2mir_node_op (node_t n, int i);

/* O(1) forward operand iteration, for external deep-copy / walkers that
   would pay O(n^2) for a repeated-index c2mir_node_op walk.
   c2mir_node_first_op: first operand, NULL for a leaf or empty interior.
   c2mir_node_next_op:  next sibling, NULL past the end. */
node_t c2mir_node_first_op (node_t n);
node_t c2mir_node_next_op (node_t op);

/* Initialize the DLIST ops field of an externally-allocated node.
   Must be called before appending children. */
void c2mir_init_node_ops (node_t n);

/* ---- Compilation from external AST ---- */

int c2mir_compile_tree (MIR_context_t ctx, c2m_ctx_t c2m_ctx,
                        node_t tree, const char *module_name);

/* Dump the raw AST tree (pre-check, no annotations).
   Output format matches c2m -d for easy diffing. */
void c2mir_dump_tree (c2m_ctx_t c2m_ctx, FILE *f, node_t tree);

/* Run do_context on the (external) tree, then dump it — the POST-check tree,
   matching `c2m -d`'s stage. Mutates the tree; do not compile it afterwards. */
void c2mir_dump_tree_checked (c2m_ctx_t c2m_ctx, FILE *f, node_t tree);

/* Deep-copy an externally-built AST from src_c2m into dst_c2m (fresh uids,
   NULL attrs, positions restamped cross-context). Leaf string payloads stay
   owned by the source context, which must outlive uses of the copy. */
node_t c2mir_copy_tree (c2m_ctx_t dst_c2m, c2m_ctx_t src_c2m, node_t tree);

/* Detach a direct child from a composite node's op list (splice-out). */
void c2mir_op_remove (node_t n, node_t op);

/* Run the context checker over an N_MODULE tree, attributing check errors to
   the module's top-level items: cb fires once per defective item with the item
   node, its 0-based index in the module's list, and its error count (the
   trailing incomplete-decl sweep is attributed too). Returns total new errors
   (0 = clean) or -1 on misuse. Mutates the tree and is not idempotent per
   context — check a c2mir_copy_tree copy in a fresh compile context per round;
   never compile or re-check the same tree. */
int c2mir_check_tree (c2m_ctx_t c2m_ctx, node_t tree,
                      void (*cb) (node_t top_item, int index, unsigned n_errs, void *data),
                      void *data);

#ifdef __cplusplus
}
#endif

#endif /* C2MIR_API_H */
