/* This file is a major part of Gecko Parser (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "allocate.h"
#include "hash.h"
#include "hashtab.h"
#include "vlobject.h"
#include "objstack.h"
#include "bitmap.h"
#include "gecko.h"

#ifdef __GNUC__
#define FORCE_INLINE inline __attribute__ ((always_inline))
#define NO_INLINE __attribute__ ((noinline))
#define LIKELY(c) __builtin_expect (c, 1)
#define UNLIKELY(c) __builtin_expect (c, 0)
#else
#define FORCE_INLINE inline
#define NO_INLINE
#define LIKELY(c) c
#define UNLIKELY(c) c
#endif

#ifndef GP_MAX_ERROR_MESSAGE_LENGTH
#define GP_MAX_ERROR_MESSAGE_LENGTH 200
#endif

struct symbs {             /* information about grammar vocabulary: */
  int n_terms, n_nonterms; /* number of all terminals and non-terminals */
  os_t symbs_os;           /* all symbols are placed in this object */
  /* References to the symbols, terminals, nonterminals are stored in the following vlos. The
     indexes in the arrays are the same as corresponding symbol, terminal, and nonterm numbers. */
  vlo_t symbs_vlo, terms_vlo, nonterms_vlo;
  hash_table_t repr_to_symb_tab;      /* table to find symbol by its representation */
  hash_table_t code_to_term_tab;      /* table to find term symbol by its code */
  struct symb **term_code_trans_vect; /* terminal code to terminal symbol vector */
  int term_code_trans_vect_start, term_code_trans_vect_end;
};

struct sit;
struct set;
struct action_desc;

struct grammar {    /* major structure which stores information about grammar: */
  bool undefined_p; /* true for undefined or erroneous grammar */
  int error_code;   /* the last occurred error code for given grammar */
  char error_message[GP_MAX_ERROR_MESSAGE_LENGTH + 1]; /* the last error message */
  struct symb *axiom;         /* grammar axiom (there is only one rule with axiom in lhs) */
  struct symb *start;         /* nonterminal with grammar first rule */
  struct symb *end_marker;    /* auxiliary symbol denoting EOF */
  int recovery_token_matches; /* number of subsequent tokens should be successfully shifted to finish
                                 error recovery */
  int debug_level;
  struct symbs *symbs;         /* vocabulary used for this grammar */
  struct rules *rules;         /* rules used for this grammar */
  struct term_sets *term_sets; /* terminal sets used for this grammar */
  gp_allocator_t *alloc;       /* allocator of internal parser data (grammar, stacks, etc) */

  jmp_buf error_longjump_buff; /* jump buffer for processing errors */

  gp_node_merge_func_t node_merge; /* function for merging stack elements attributes */

  size_t read_tokens;                  /* read tokens so far */
  int (*read_token) (void **attr);     /* function for reading tokens */
  gp_parse_alloc_func_t parse_alloc;   /* function to allocate parse tree nodes */
  gp_parse_free_func_t parse_free;     /* function to free parse tree nodes */
  gp_syntax_error_func_t syntax_error; /* function to print syntax error */
  gp_rule_guard_func_t rule_guard;     /* function to guard rules */

  vlo_t caller_anode_names; /* Pointers to allocated names of anodes */

  /* statistic numbers for hash tables updated at the end of gp_parse and gp_free_tree: */
  int all_searches, all_collisions;

  vlo_t temp_vlo;

  /* vlo is array which is indexed by situation number (sit->rule->rule_start_offset + sit->pos): */
  vlo_t sit_table_vlo;
  struct sit **sit_table; /* the above vlo as array: */
  os_t sits_os;           /* all situations are placed in the object */
  int n_all_sits;         /* current number of unique situations */

  vlo_t sets_vlo; /* used to build sets */

  struct set *eof_set; /* set with situation axiom: start . $eof */
  struct set *new_set; /* set being created: defined only when new_set_ready_p is true */
  /* The following says that new_set and its members are defined. Before this the access
     to data of the set being formed are possible only through the following variables. */
  bool new_set_ready_p;
  /* To optimize code we use the following variables to access data of new set. They are always defined
     and correspondingly situations and the current number of start situations of the set being formed. */
  struct sit **new_sits;
  size_t new_n_start_sits;
  size_t n_sets, n_sets_start_sits;         /* # of unique sets and their start situations */
  size_t n_goto_vects, n_goto_vect_len;     /* goto vects and their length */
  size_t n_actions;                         /* actions number */
  size_t n_action_vects, n_action_vect_len; /* action vects and their length */
  os_t set_sits_os;                         /* container of situations of being formed sets */
  os_t sets_os;                             /* container of sets */
  hash_table_t set_tab;                     /* set table: key is only start situations */

  vlo_t symb_sits;   /* container for symb_sits */
  vlo_t actions_vlo; /* container for set actions  */

  struct action_desc *empty_action_map; /* no actions for each terminal */

  struct set *start_set; /* start set of the grammar */

  os_t anode_name_code_os;          /* container for anode_name_code structures  */
  hash_table_t anode_name_code_tab; /* table of anode_name_code structures  */

  void *rule_guard_arg; /* arg passed to rule guards */

  size_t contexts_num; /* last context number */

  vlo_t all_nodes;       /* all parse tree nodes */
  bitmap_t marked_nodes; /* it is used in GC to find nodes reached from curr stacks */

  struct gp_tree_node temp_node; /* used for insertion of node into the table */

  /* Statistic numbers: all parse tree nodes, terminal nodes, abstract and alternative nodes: */
  size_t n_parse_nodes, n_parse_term_nodes;
  size_t n_parse_abstract_nodes, n_parse_alt_nodes, n_parse_opt_nodes;
  /* Parse tree node representing empty node.  It exists in one instance. */
  struct gp_tree_node *empty_node;

  /* internal data used for error recovery */
  os_t recovery_infos;                       /* container for recovery_info structures */
  struct recovery_info *free_recovery_infos; /* infos already allocated and can be reused */

  vlo_t free_stacks;    /* pointers to stack structs already allocated and can be reused */
  vlo_t temp_nodes_vlo; /* temp vlo containing pointers to parse tree nodes and used for translation and
                           freeing */

  size_t n_stacks; /* all allocated stacks */
#ifndef NO_GP_DEBUG_PRINT
  size_t n_peak_stack_els, n_curr_stack_els; /* peak allocated and currently allocated stacks */
#endif

  /* Token buffers used mostly during error recovery: */
  size_t curr_buff_token_ind; /* current position in the token buffer */
  vlo_t token_buff;           /* container of token_buff_el structs */

  /* The current stacks and the stacks which will be current after reading and processing current
     terminal, in other words stacks containing situation with position after the terminal.  */
  vlo_t curr_stacks, new_stacks;
  /* Current stacks which had no actions on the current term.  They are start stacks for error recovery. Error
     recovery starts when there are no actions on all current stacks.  They are also used during error
     recovery.  */
  vlo_t failed_stacks;

  hash_table_t stack_htab; /* internal htab used for stack merging */

#ifndef NO_GP_DEBUG_PRINT
  int n_single_stack_actions, n_multi_stack_actions; /* actions taken for single and multiple stacks */
  bool *visits_p;                                    /* temporary array used for printing parse trees */
#endif
};

/* Forward declarations: */
static void error (struct grammar *g, int code, const char *format, ...);

/* The default number of tokens successfully matched to stop error recovery alternative (state). */
#define DEFAULT_RECOVERY_TOKEN_MATCHES 5

/* This page is abstract data `grammar symbols'. */

typedef unsigned long int term_set_el_t; /* type of element of array representing set of terminals */

struct rule;

struct symb {       /* symbol of the grammar: */
  const char *repr; /* external representation of the symbol */
  union {
    struct {
      int code;            /* code of the terminal symbol */
      int term_num;        /* order number of the terminal */
      int priority;        /* < 0 no priority */
      enum gp_assoc assoc; /* undefined for priority < 0 */
    } term;
    struct {
      struct rule *rules;            /* all rules with the nonterminal in the rule LHS */
      int nonterm_num;               /* order number of the nonterminal */
      bool loop_p;                   /* flag that nonterminal may derive itself */
      term_set_el_t *first, *follow; /* FIRST and FOLLOW sets of the nonterminal */
    } nonterm;
  } u;
  bool term_p;       /* true if it is a terminal */
  bool access_p;     /* true if the symbol is accessible (derived) from the axiom */
  bool derivation_p; /* true if it is a term or it is a nonterm which derives a term string */
  bool empty_p;      /* true if it is nonterminal which may derive empty string */
  int num;           /* order number of the symbol */
};

/* delete_hash_table plus accumulate all_searches and all_collisions. */
static inline void delete_htab_update_statistics (struct grammar *g, hash_table_t htab) {
  g->all_searches += get_searches (htab);
  g->all_collisions += get_collisions (htab);
  delete_hash_table (htab);
}

static uint64_t symb_repr_hash (hash_table_entry_t s) { /* return hash of symbol representation */
  const char *str = ((struct symb *) s)->repr;
  return hash (str, strlen (str), 42);
}

/* Equality of symbol representations. */
static bool symb_repr_eq (hash_table_entry_t s1, hash_table_entry_t s2) {
  return strcmp (((struct symb *) s1)->repr, ((struct symb *) s2)->repr) == 0;
}

static uint64_t symb_code_hash (hash_table_entry_t s) { /* Hash of terminal code. */
  struct symb *symb = ((struct symb *) s);
  assert (symb->term_p);
  return (uint64_t) symb->u.term.code;
}

/* Equality of terminal codes. */
static bool symb_code_eq (hash_table_entry_t s1, hash_table_entry_t s2) {
  struct symb *symb1 = ((struct symb *) s1);
  struct symb *symb2 = ((struct symb *) s2);
  assert (symb1->term_p && symb2->term_p);
  return symb1->u.term.code == symb2->u.term.code;
}

/* Initialize work with symbols and return storage for the symbols. */
static struct symbs *symb_init (struct grammar *g) {
  void *mem = gp_malloc (g->alloc, sizeof (struct symbs));
  struct symbs *result = (struct symbs *) mem;
  OS_CREATE (result->symbs_os, g->alloc, 0);
  VLO_CREATE (result->symbs_vlo, g->alloc, 1024);
  VLO_CREATE (result->terms_vlo, g->alloc, 512);
  VLO_CREATE (result->nonterms_vlo, g->alloc, 512);
  result->repr_to_symb_tab = create_hash_table (g->alloc, 300, symb_repr_hash, symb_repr_eq);
  result->code_to_term_tab = create_hash_table (g->alloc, 200, symb_code_hash, symb_code_eq);
  result->term_code_trans_vect = NULL;
  result->n_nonterms = result->n_terms = 0;
  return result;
}

/* Return symbol (or NULL if it does not exist) whose representation is REPR. */
static struct symb *symb_find_by_repr (struct grammar *g, const char *repr) {
  struct symb symb;
  symb.repr = repr;
  return (struct symb *) *find_hash_table_entry (g->symbs->repr_to_symb_tab, &symb, false);
}

/* Return symbol (or NULL if it does not exist) which is terminal with CODE. */
static struct symb *term_tab_find_by_code (struct grammar *g, int code) {
  struct symbs *symbs = g->symbs;
  struct symb symb;
  symb.term_p = true;
  symb.u.term.code = code;
  return (struct symb *) *find_hash_table_entry (symbs->code_to_term_tab, &symb, false);
}

/* Return symbol (or NULL if it does not exist) which is terminal with CODE. */
static FORCE_INLINE struct symb *term_find_by_code (struct grammar *g, int code) {
  struct symbs *symbs = g->symbs;
  struct symb *symb;
  assert (symbs->term_code_trans_vect != NULL);
  if (code < symbs->term_code_trans_vect_start || code >= symbs->term_code_trans_vect_end
      || (symb = symbs->term_code_trans_vect[code - symbs->term_code_trans_vect_start]) == NULL) {
    error (g, GP_INVALID_TOKEN_CODE, "invalid token code %d", code);
    return NULL;
  }
  return symb;
}

/* Create new terminal symbol and return reference for it. The symbol should not be in the tables.
   The function should create own copy of name for the new symbol. */
static struct symb *symb_add_term (struct grammar *g, const char *name, int code, int priority,
                                   enum gp_assoc assoc) {
  struct symb symb, *result;
  hash_table_entry_t *repr_entry, *code_entry;

  symb.repr = name;
  symb.term_p = true;
  symb.num = g->symbs->n_nonterms + g->symbs->n_terms;
  symb.u.term.code = code;
  symb.u.term.term_num = g->symbs->n_terms++;
  symb.u.term.priority = priority;
  symb.u.term.assoc = assoc;
  symb.empty_p = false;
  repr_entry = find_hash_table_entry (g->symbs->repr_to_symb_tab, &symb, true);
  assert (*repr_entry == NULL);
  code_entry = find_hash_table_entry (g->symbs->code_to_term_tab, &symb, true);
  assert (*code_entry == NULL);
  OS_TOP_ADD_STRING (g->symbs->symbs_os, name);
  symb.repr = (char *) OS_TOP_BEGIN (g->symbs->symbs_os);
  OS_TOP_FINISH (g->symbs->symbs_os);
  OS_TOP_ADD_MEMORY (g->symbs->symbs_os, &symb, sizeof (struct symb));
  result = (struct symb *) OS_TOP_BEGIN (g->symbs->symbs_os);
  OS_TOP_FINISH (g->symbs->symbs_os);
  *repr_entry = (hash_table_entry_t) result;
  *code_entry = (hash_table_entry_t) result;
  VLO_ADD_MEMORY (g->symbs->symbs_vlo, &result, sizeof (struct symb *));
  VLO_ADD_MEMORY (g->symbs->terms_vlo, &result, sizeof (struct symb *));
  return result;
}

/* Create new nonterminal symbol and return reference for it. The symbol should not be
   in the table. The function should create own copy of name for the new symbol. */
static struct symb *symb_add_nonterm (struct grammar *g, const char *name) {
  struct symb symb;
  symb.repr = name;
  symb.term_p = false;
  symb.num = g->symbs->n_nonterms + g->symbs->n_terms;
  symb.u.nonterm.rules = NULL;
  symb.u.nonterm.loop_p = 0;
  symb.u.nonterm.nonterm_num = g->symbs->n_nonterms++;
  hash_table_entry_t *entry = find_hash_table_entry (g->symbs->repr_to_symb_tab, &symb, true);
  assert (*entry == NULL);
  OS_TOP_ADD_STRING (g->symbs->symbs_os, name);
  symb.repr = (char *) OS_TOP_BEGIN (g->symbs->symbs_os);
  OS_TOP_FINISH (g->symbs->symbs_os);
  OS_TOP_ADD_MEMORY (g->symbs->symbs_os, &symb, sizeof (struct symb));
  struct symb *result = (struct symb *) OS_TOP_BEGIN (g->symbs->symbs_os);
  OS_TOP_FINISH (g->symbs->symbs_os);
  *entry = (hash_table_entry_t) result;
  VLO_ADD_MEMORY (g->symbs->symbs_vlo, &result, sizeof (struct symb *));
  VLO_ADD_MEMORY (g->symbs->nonterms_vlo, &result, sizeof (struct symb *));
  return result;
}

static struct symb *symb_get (struct grammar *g, int n) { /* return N-th symbol (if any) or NULL otherwise */
  if (n < 0 || (VLO_LENGTH (g->symbs->symbs_vlo) / sizeof (struct symb *) <= (size_t) n)) return NULL;
  struct symb *symb = ((struct symb **) VLO_BEGIN (g->symbs->symbs_vlo))[n];
  assert (symb->num == n);
  return symb;
}

static struct symb *term_get (struct grammar *g, int n) { /* return N-th term (if any) or NULL otherwise */
  if (n < 0 || (VLO_LENGTH (g->symbs->terms_vlo) / sizeof (struct symb *) <= (size_t) n)) return NULL;
  struct symb *symb = ((struct symb **) VLO_BEGIN (g->symbs->terms_vlo))[n];
  assert (symb->term_p && symb->u.term.term_num == n);
  return symb;
}

/* Return N-th nonterm (if any) or NULL otherwise. */
static struct symb *nonterm_get (struct grammar *g, int n) {
  if (n < 0 || (VLO_LENGTH (g->symbs->nonterms_vlo) / sizeof (struct symb *) <= (size_t) n)) return NULL;
  struct symb *symb = ((struct symb **) VLO_BEGIN (g->symbs->nonterms_vlo))[n];
  assert (!symb->term_p && symb->u.nonterm.nonterm_num == n);
  return symb;
}

#ifndef NO_GP_DEBUG_PRINT

/* Print symbol SYMB to file F. Terminal is printed with its code if CODE_P. */
static void symb_print (FILE *f, struct symb *symb, int code_p) {
  fprintf (f, "%s", symb->repr);
  if (code_p && symb->term_p) fprintf (f, "(%d)", symb->u.term.code);
}

#endif /* #ifndef NO_GP_DEBUG_PRINT */

#define TERM_CODE_TRANS_VECT_SIZE 10000 /* Max length of vector: terminal code->terminal symbol */

static void symb_finish_adding_terms (struct grammar *g) { /* Set up term_code_trans_vect */
  int i, max_code, min_code;
  struct symb *symb;
  for (min_code = max_code = i = 0; (symb = term_get (g, i)) != NULL; i++) {
    if (i == 0 || min_code > symb->u.term.code) min_code = symb->u.term.code;
    if (i == 0 || max_code < symb->u.term.code) max_code = symb->u.term.code;
  }
  assert (i != 0);
  if (max_code - min_code >= TERM_CODE_TRANS_VECT_SIZE) {
    error (g, GP_TOO_WIDE_TERM_RANGE_CODE, "term code range is more %d", TERM_CODE_TRANS_VECT_SIZE);
  } else {
    g->symbs->term_code_trans_vect_start = min_code;
    g->symbs->term_code_trans_vect_end = max_code + 1;
    void *mem = gp_calloc (g->alloc, (size_t) (max_code - min_code + 1), sizeof (struct symb *));
    g->symbs->term_code_trans_vect = (struct symb **) mem;
    for (i = 0; (symb = term_get (g, i)) != NULL; i++)
      g->symbs->term_code_trans_vect[symb->u.term.code - min_code] = symb;
  }
}

static void symb_fin (struct grammar *g, struct symbs *symbs) { /* Finalize work with symbols. */
  if (symbs == NULL) return;
  if (g->symbs->term_code_trans_vect != NULL) gp_free (g->alloc, g->symbs->term_code_trans_vect);
  delete_hash_table (g->symbs->repr_to_symb_tab);
  delete_hash_table (g->symbs->code_to_term_tab);
  VLO_DELETE (g->symbs->nonterms_vlo);
  VLO_DELETE (g->symbs->terms_vlo);
  VLO_DELETE (g->symbs->symbs_vlo);
  OS_DELETE (g->symbs->symbs_os);
  gp_free (g->alloc, symbs);
  symbs = NULL;
}

/* This page contains abstract data set of terminals. */

#define TERM_SET_EL_BITS (CHAR_BIT * (int) sizeof (term_set_el_t))

struct tab_term_set { /* element of term set hash table: */
  int num;            /* number of set in the table */
  term_set_el_t *set; /* terminal set itself */
};

struct term_sets {                      /* container for the abstract data: */
  os_t term_set_os;                     /* all terminal sets are stored in the os */
  size_t n_term_sets, n_term_sets_size; /* number of terminal sets and their overall size */
  vlo_t tab_term_set_vlo;               /* refs to all struct tab_term_set are stored in the vlo */
};

/* Initialize work with terminal sets and returns storage for terminal sets. */
static struct term_sets *term_set_init (struct grammar *g) {
  void *mem = gp_malloc (g->alloc, sizeof (struct term_sets));
  struct term_sets *result = (struct term_sets *) mem;
  OS_CREATE (result->term_set_os, g->alloc, 0);
  VLO_CREATE (result->tab_term_set_vlo, g->alloc, 4096);
  result->n_term_sets = result->n_term_sets_size = 0;
  return result;
}

/* Return new terminal SET. Its value is undefined. */
static term_set_el_t *term_set_create (struct grammar *g) {
  assert (sizeof (term_set_el_t) <= 8);
  /* Make it 64 bit multiple to have the same statistics for 64 bit machines. */
  size_t size = (((size_t) g->symbs->n_terms + CHAR_BIT * 8 - 1) / (CHAR_BIT * 8)) * 8;
  OS_TOP_EXPAND (g->term_sets->term_set_os, size);
  term_set_el_t *result = (term_set_el_t *) OS_TOP_BEGIN (g->term_sets->term_set_os);
  OS_TOP_FINISH (g->term_sets->term_set_os);
  g->term_sets->n_term_sets++;
  g->term_sets->n_term_sets_size += size;
  return result;
}

static inline void term_set_clear (struct grammar *g, term_set_el_t *set) { /* make terminal SET empty: */
  int size = (g->symbs->n_terms + TERM_SET_EL_BITS - 1) / (TERM_SET_EL_BITS);
  term_set_el_t *bound = set + size;
  while (set < bound) *set++ = 0;
}

/* Copy SRC into DEST */
static inline GP_UNUSED void term_set_copy (struct grammar *g, term_set_el_t *dest, term_set_el_t *src) {
  int size = (g->symbs->n_terms + TERM_SET_EL_BITS - 1) / (TERM_SET_EL_BITS);
  term_set_el_t *bound = dest + size;
  while (dest < bound) *dest++ = *src++;
}

/* Add all terminals from set OP to SET. Return true if SET has been changed. */
static inline bool term_set_or (struct grammar *g, term_set_el_t *set, term_set_el_t *op) {
  term_set_el_t *bound;
  int size = (g->symbs->n_terms + TERM_SET_EL_BITS - 1) / (TERM_SET_EL_BITS);
  bound = set + size;
  bool changed_p = false;
  while (set < bound) {
    if ((*set | *op) != *set) changed_p = true;
    *set++ |= *op++;
  }
  return changed_p;
}

/* Add terminal with number NUM to SET. Return true if SET has been changed. */
static inline bool term_set_up (struct grammar *g, term_set_el_t *set, int num) {
  assert (num < g->symbs->n_terms);
  int ind = num / TERM_SET_EL_BITS;
  term_set_el_t bit = ((term_set_el_t) 1) << (num % TERM_SET_EL_BITS);
  bool changed_p = (set[ind] & bit) == 0;
  set[ind] |= bit;
  return changed_p;
}

/* Return true if terminal with number NUM is in SET. */
static inline int term_set_test (struct grammar *g, term_set_el_t *set, int num) {
  assert (num >= 0 && num < g->symbs->n_terms);
  int ind = num / TERM_SET_EL_BITS;
  term_set_el_t bit = ((term_set_el_t) 1) << (num % TERM_SET_EL_BITS);
  return (set[ind] & bit) != 0;
}

/* Return set which is in the table with number NUM. */
static inline GP_UNUSED term_set_el_t *term_set_from_table (struct grammar *g, int num) {
  assert ((size_t) num < VLO_LENGTH (g->term_sets->tab_term_set_vlo) / sizeof (struct tab_term_set *));
  return ((struct tab_term_set **) VLO_BEGIN (g->term_sets->tab_term_set_vlo))[num]->set;
}

#ifndef NO_GP_DEBUG_PRINT
/* print terminal SET into file F */
static void term_set_print (struct grammar *g, FILE *f, term_set_el_t *set) {
  for (int i = 0; i < g->symbs->n_terms; i++)
    if (term_set_test (g, set, i)) {
      fprintf (f, " ");
      symb_print (f, term_get (g, i), false);
    }
}
#endif

/* Finalize work with terminal sets. */
static void term_set_fin (struct grammar *g, struct term_sets *term_sets) {
  if (term_sets == NULL) return;
  VLO_DELETE (term_sets->tab_term_set_vlo);
  OS_DELETE (term_sets->term_set_os);
  gp_free (g->alloc, term_sets);
  term_sets = NULL;
}

/* This page is abstract data `grammar rules'. */

struct rule {                 /* rule of the grammar: */
  int num;                    /* order number of rule */
  int guard_num;              /* guard number, < 0 means no guard calls */
  int rhs_len;                /* length of rhs */
  int lhs_nonterm_num;        /* = lhs->u.nonterm.nonterm_num */
  struct rule *next;          /* the next grammar rule */
  struct rule *lhs_next;      /* the next grammar rule with the same nonterminal in the rule lhs */
  struct symb *lhs;           /* nonterminal in the left hand side of the rule */
  struct symb **rhs;          /* symbols in the right hand side of the rule */
  struct symb *priority_term; /* last priority terminal, NULL otherwise */
  /* The following three members define rule translation: */
  const char *anode; /* abstract node name if any */
  int trans_len;     /* number of symbol translations in the rule translation */
  /* Array elements correspond to symbol of rhs with the same index. The element value is order
     number of the corresponding symbol in the rule translation. If the symbol translation is rejected,
     the corresponding element value is negative. */
  int *order;
  /* Size of all previous rule lengths + number of the previous rules. Imagine that all left hand
     symbols and right hand side symbols of the rules are stored in array. Then the following member
     is index of the rule lhs in the array. */
  int rule_start_offset;
  char *caller_anode;    /* the same string as anode but memory allocated in parse_alloc */
  int caller_anode_code; /* corresponding code */
};

struct rules {             /* container for the abstract data */
  int n_rules, n_rhs_lens; /* number of all rules and their total rhs length */
  struct rule *first_rule; /* the first rule */
  struct rule *curr_rule;  /* rule being formed */
  vlo_t rules_vlo;         /* all references to rules are placed in this object: */
  os_t rules_os;           /* all rules are placed in this object: */
};

/* Initialize work with rules and returns pointer to rules storage. */
static struct rules *rule_init (struct grammar *g) {
  void *mem;
  struct rules *result;

  mem = gp_malloc (g->alloc, sizeof (struct rules));
  result = (struct rules *) mem;
  VLO_CREATE (result->rules_vlo, g->alloc, 0);
  OS_CREATE (result->rules_os, g->alloc, 0);
  result->first_rule = result->curr_rule = NULL;
  result->n_rules = result->n_rhs_lens = 0;
  return result;
}

/* Create new rule with LHS, empty rhs, and abstract node name ANODE, and GUARD_NUM. */
static struct rule *rule_new_start (struct grammar *g, struct symb *lhs, const char *anode, int guard_num) {
  struct rule *rule;
  struct symb *empty;

  assert (!lhs->term_p);
  OS_TOP_EXPAND (g->rules->rules_os, sizeof (struct rule));
  rule = (struct rule *) OS_TOP_BEGIN (g->rules->rules_os);
  OS_TOP_FINISH (g->rules->rules_os);
  rule->guard_num = guard_num;
  rule->lhs = lhs;
  rule->lhs_nonterm_num = lhs->u.nonterm.nonterm_num;
  rule->priority_term = NULL;
  if (anode == NULL) {
    rule->anode = NULL;
  } else {
    OS_TOP_ADD_STRING (g->rules->rules_os, anode);
    rule->anode = (char *) OS_TOP_BEGIN (g->rules->rules_os);
    OS_TOP_FINISH (g->rules->rules_os);
  }
  rule->trans_len = 0;
  rule->order = NULL;
  rule->next = NULL;
  if (g->rules->curr_rule != NULL) g->rules->curr_rule->next = rule;
  rule->lhs_next = lhs->u.nonterm.rules;
  lhs->u.nonterm.rules = rule;
  rule->rhs_len = 0;
  empty = NULL;
  OS_TOP_ADD_MEMORY (g->rules->rules_os, &empty, sizeof (struct symb *));
  rule->rhs = (struct symb **) OS_TOP_BEGIN (g->rules->rules_os);
  g->rules->curr_rule = rule;
  if (g->rules->first_rule == NULL) g->rules->first_rule = rule;
  rule->rule_start_offset = g->rules->n_rhs_lens + g->rules->n_rules;
  rule->num = g->rules->n_rules++;
  assert (VLO_LENGTH (g->rules->rules_vlo) / sizeof (struct rule *) == (size_t) rule->num);
  VLO_ADD_MEMORY (g->rules->rules_vlo, &rule, sizeof (rule));
  return rule;
}

/* Add SYMB at the end of current rule rhs. */
static void rule_new_symb_add (struct grammar *g, struct symb *symb) {
  struct symb *empty;

  empty = NULL;
  OS_TOP_ADD_MEMORY (g->rules->rules_os, &empty, sizeof (struct symb *));
  g->rules->curr_rule->rhs = (struct symb **) OS_TOP_BEGIN (g->rules->rules_os);
  g->rules->curr_rule->rhs[g->rules->curr_rule->rhs_len] = symb;
  if (symb->term_p && symb->u.term.priority >= 0) g->rules->curr_rule->priority_term = symb;
  g->rules->curr_rule->rhs_len++;
  g->rules->n_rhs_lens++;
}

/* Create and initialize situation cache (it should be called at the end of forming each rule).  */
static void rule_new_stop (struct grammar *g) {
  int i;

  OS_TOP_FINISH (g->rules->rules_os);
  OS_TOP_EXPAND (g->rules->rules_os, (size_t) g->rules->curr_rule->rhs_len * sizeof (int));
  g->rules->curr_rule->order = (int *) OS_TOP_BEGIN (g->rules->rules_os);
  OS_TOP_FINISH (g->rules->rules_os);
  for (i = 0; i < g->rules->curr_rule->rhs_len; i++) g->rules->curr_rule->order[i] = -1;
}

#ifndef NO_GP_DEBUG_PRINT
static struct rule *rule_get (struct grammar *g, int num) { /* return NUM-th rule */
  assert (VLO_LENGTH (g->rules->rules_vlo) > sizeof (struct rule *) * (size_t) num);
  return ((struct rule **) VLO_BEGIN (g->rules->rules_vlo))[num];
}

/* Print RULE with its translation (if TRANS_P) to file F. */
static void rule_print (FILE *f, struct rule *rule, bool trans_p, bool newln_p) {
  int i, j;

  symb_print (f, rule->lhs, false);
  fprintf (f, " :");
  for (i = 0; i < rule->rhs_len; i++) {
    fprintf (f, " ");
    symb_print (f, rule->rhs[i], false);
  }
  if (trans_p) {
    fprintf (f, " # ");
    if (rule->anode != NULL) {
      fprintf (f, "%s (", rule->anode);
    }
    for (i = 0; i < rule->trans_len; i++) {
      for (j = 0; j < rule->rhs_len; j++)
        if (rule->order[j] == i) {
          fprintf (f, " %d:", j);
          symb_print (f, rule->rhs[j], false);
          break;
        }
      if (j >= rule->rhs_len) fprintf (f, " nil");
    }
    if (rule->anode != NULL) fprintf (f, " )");
  }
  if (newln_p) fprintf (f, "\n");
}

/* Print RULE to file F with dot in position POS. */
static void rule_dot_print (FILE *f, struct rule *rule, int pos) {
  assert (pos >= 0 && pos <= rule->rhs_len);
  symb_print (f, rule->lhs, false);
  fprintf (f, " :");
  for (int i = 0; i < rule->rhs_len; i++) {
    fprintf (f, i == pos ? " ." : " ");
    symb_print (f, rule->rhs[i], false);
  }
  if (rule->rhs_len == pos) fprintf (f, ".");
}

#endif /* #ifndef NO_GP_DEBUG_PRINT */

static void rule_fin (struct grammar *g, struct rules *rules) { /* Finalize work with rules. */
  if (rules == NULL) return;
  VLO_DELETE (rules->rules_vlo);
  OS_DELETE (rules->rules_os);
  gp_free (g->alloc, rules);
  rules = NULL;
}

/* This page is abstract data `situations'. */

struct sit {         /* the situation: */
  bool empty_tail_p; /* true if the tail can derive empty string */
  int pos;           /* position of dot in rhs of the situation rule */
  int sit_number;    /* unique situation number */
  struct rule *rule; /* the situation rule */
  /* the situation lookahead = FIRST (the situation tail & FOLLOW (lhs)) */
  term_set_el_t *lookahead;
};

static void sit_init (struct grammar *g) { /* Initialize work with situations: */
  g->n_all_sits = 0;
  OS_CREATE (g->sits_os, g->alloc, 0);
  VLO_CREATE (g->sit_table_vlo, g->alloc, 4096);
  g->sit_table = (struct sit **) VLO_BEGIN (g->sit_table_vlo);
}

/* Set up lookahead of situation SIT. Returns true if the situation tail may derive empty string. */
static bool sit_set_lookahead (struct grammar *g, struct sit *sit) {
  struct symb *symb, **symb_ptr;

  sit->lookahead = term_set_create (g);
  term_set_clear (g, sit->lookahead);
  symb_ptr = &sit->rule->rhs[sit->pos];
  while ((symb = *symb_ptr) != NULL) {
    if (symb->term_p)
      term_set_up (g, sit->lookahead, symb->u.term.term_num);
    else
      term_set_or (g, sit->lookahead, symb->u.nonterm.first);
    if (!symb->empty_p) break;
    symb_ptr++;
  }
  if (symb != NULL) return false;
  term_set_or (g, sit->lookahead, sit->rule->lhs->u.nonterm.follow);
  return true;
}

/* Return situations with given characteristics. Remember that sits are stored in one instance. */
static inline struct sit *sit_create (struct grammar *g, struct rule *rule, int pos) {
  struct sit *sit;
  ptrdiff_t diff
    = (char *) (g->sit_table + rule->rule_start_offset + pos) - (char *) VLO_BOUND (g->sit_table_vlo);

  if (diff >= 0) {
    diff += (ptrdiff_t) sizeof (struct sit *);
    VLO_EXPAND (g->sit_table_vlo, (size_t) diff);
    g->sit_table = (struct sit **) VLO_BEGIN (g->sit_table_vlo);
    struct sit **bound = (struct sit **) VLO_BOUND (g->sit_table_vlo);
    for (struct sit **ptr = bound - (size_t) diff / sizeof (struct sit *); ptr < bound; ptr++) *ptr = NULL;
  }
  if ((sit = g->sit_table[rule->rule_start_offset + pos]) != NULL) return sit;
  OS_TOP_EXPAND (g->sits_os, sizeof (struct sit));
  sit = (struct sit *) OS_TOP_BEGIN (g->sits_os);
  OS_TOP_FINISH (g->sits_os);
  g->n_all_sits++;
  sit->rule = rule;
  sit->pos = pos;
  sit->sit_number = g->n_all_sits;
  sit->empty_tail_p = sit_set_lookahead (g, sit);
  g->sit_table[rule->rule_start_offset + pos] = sit;
  return sit;
}

#ifndef NO_GP_DEBUG_PRINT

static void sit_print (FILE *f, struct sit *sit) { /* print situation SIT to file F: */
  fprintf (f, "%3d ", sit->sit_number);
  rule_dot_print (f, sit->rule, sit->pos);
}

#endif /* #ifndef NO_GP_DEBUG_PRINT */

/* Return hash of sequence of N_SITS situations in array SITS. */
static uint64_t sits_hash (size_t n_sits, struct sit **sits) {
  uint64_t result = hash_init (24);
  for (size_t i = 0; i < n_sits; i++) result = hash_step (result, (uint64_t) sits[i]->sit_number);
  return hash_finish (result);
}

static void sit_fin (struct grammar *g) { /* Finalize work with situations. */
  VLO_DELETE (g->sit_table_vlo);
  OS_DELETE (g->sits_os);
}

struct action_desc {      /* description of LR set actions on some term: */
  unsigned actions_num;   /* number of actions for given term */
  unsigned actions_start; /* index of first term action, defined for actions_num != 0 */
};

struct action { /* a LR set action: */
  bool shift_p; /* true if action is a shift, otherwise it is a reduce */
  int term_num; /* action on given term */
  union {
    struct set *set;   /* result set on shift */
    struct rule *rule; /* rule for reduce */
  } u;
};

/* This page is abstract data `sets'. */

struct set {                      /* the grammar state: */
  size_t num;                     /* unique number of the state */
  size_t n_start_sits, n_sits;    /* numbers of (start) situations in the following array */
  size_t n_actions;               /* len of array actions */
  struct symb *symb;              /* symb shifting which resulted into this state */
  struct sit **sits;              /* array of situation */
  struct set **goto_map;          /* map nonterm -> goto set */
  struct action_desc *action_map; /* map term -> action desc */
  struct action *actions;         /* action number -> action */
};

static uint64_t set_hash (hash_table_entry_t s) { /* hash of set */
  struct set *set = (struct set *) s;
  return sits_hash (set->n_start_sits, set->sits);
}

static bool set_eq (hash_table_entry_t s1, hash_table_entry_t s2) { /* equality of sets: */
  struct set *set1 = (struct set *) s1;
  struct set *set2 = (struct set *) s2;
  struct sit **sit_ptr1, **sit_ptr2, **sit_bound1;
  if (set1->n_start_sits != set2->n_start_sits) return false;
  sit_ptr1 = set1->sits;
  sit_bound1 = sit_ptr1 + set1->n_start_sits;
  sit_ptr2 = set2->sits;
  while (sit_ptr1 < sit_bound1)
    if (*sit_ptr1++ != *sit_ptr2++) return false;
  return true;
}

static void set_init (struct grammar *g) { /* initialize work with sets: */
  OS_CREATE (g->set_sits_os, g->alloc, 2048);
  OS_CREATE (g->sets_os, g->alloc, 0);
  g->set_tab = create_hash_table (g->alloc, 8192, set_hash, set_eq);
  g->eof_set = NULL;
  g->n_sets = g->n_sets_start_sits = 0;
  g->n_goto_vects = g->n_goto_vect_len = 0;
  g->n_actions = g->n_action_vects = g->n_action_vect_len = 0;
}

/* Return vector of actions for SET on TERM, set up actions number via ACTIONS_NUM. */
static FORCE_INLINE struct action *get_actions (struct grammar *g, struct set *set, int term,
                                                unsigned *actions_num) {
  assert (term >= 0 && term < g->symbs->n_terms);
  *actions_num = set->action_map[term].actions_num;
  assert (*actions_num == 0 || set->actions != NULL);
  return &set->actions[set->action_map[term].actions_start];
}

static inline void set_new_set_start (struct grammar *g) { /* start forming of new set: */
  g->new_set = NULL;
  g->new_set_ready_p = false;
  g->new_n_start_sits = 0;
  g->new_sits = NULL;
}

/* Add start SIT at the end of the situation array of the set being formed: */
static inline void set_new_add_start_sit (struct grammar *g, struct sit *sit) {
  assert (!g->new_set_ready_p);
  OS_TOP_EXPAND (g->set_sits_os, sizeof (struct sit *));
  g->new_sits = (struct sit **) OS_TOP_BEGIN (g->set_sits_os);
  g->new_sits[g->new_n_start_sits] = sit;
  g->new_n_start_sits++;
}

/* Add nonstart SIT (if it is not there yet) at the end of array of the new situations. */
static inline void set_new_add_nonstart_sit (struct grammar *g, struct sit *sit) {
  assert (g->new_set_ready_p);
  /* When we add non-start situations we need to have situations w/o duplicates. */
  for (size_t i = g->new_n_start_sits; i < g->new_set->n_sits; i++)
    if (g->new_sits[i] == sit) return;
  OS_TOP_EXPAND (g->set_sits_os, sizeof (struct sit *));
  g->new_sits = g->new_set->sits = (struct sit **) OS_TOP_BEGIN (g->set_sits_os);
  g->new_sits[g->new_set->n_sits++] = sit;
}

/* The new set should contain only start situations.  Insert set into the set table.  new_set will be
   set to the table set.  If the function returns true then there was no such table set yet. */
static bool set_insert (struct grammar *g) {
  OS_TOP_EXPAND (g->sets_os, sizeof (struct set));
  g->new_set = (struct set *) OS_TOP_BEGIN (g->sets_os);
  g->new_set->n_start_sits = g->new_n_start_sits;
  g->new_set->sits = g->new_sits;
  g->new_set_ready_p = true;
  /* Insert set into table: */
  hash_table_entry_t *entry = find_hash_table_entry (g->set_tab, g->new_set, true);
  if (*entry != NULL) {
    OS_TOP_NULLIFY (g->sets_os);
    g->new_set = (struct set *) *entry;
    g->new_sits = g->new_set->sits;
    OS_TOP_NULLIFY (g->set_sits_os);
    return false;
  }
  OS_TOP_FINISH (g->sets_os);
  g->new_set->num = g->n_sets++;
  g->new_set->goto_map = NULL;
  g->new_set->action_map = NULL;
  g->new_set->actions = NULL;
  g->new_set->n_actions = 0;
  g->new_set->n_sits = g->new_n_start_sits;
  *entry = (hash_table_entry_t) g->new_set;
  g->n_sets_start_sits += g->new_n_start_sits;
  return true;
}

static inline void set_new_set_stop (struct grammar *g) { /* finish work with set being formed: */
  OS_TOP_FINISH (g->set_sits_os);
}

static void *set_calloc (struct grammar *g, size_t size) { /* allocate and store data in set os */
  OS_TOP_EXPAND (g->sets_os, size);
  void *res = (struct set *) OS_TOP_BEGIN (g->sets_os);
  OS_TOP_FINISH (g->sets_os);
  memset (res, 0, size);
  return res;
}

#ifndef NO_GP_DEBUG_PRINT
/* Print SET to file F. If NONSTART_P is true then print all situations. */
static void set_print (struct grammar *g, FILE *f, struct set *set, bool nonstart_p) {
  ptrdiff_t num;
  size_t n_start_sits, n_sits;
  struct sit **sits;

  if (set == NULL && !g->new_set_ready_p) {
    /* The following is necessary if we call the function from a debugger. In this case new_set,
       and their members may be not set up yet. */
    num = -1;
    n_start_sits = n_sits = g->new_n_start_sits;
    sits = g->new_sits;
  } else {
    num = (ptrdiff_t) set->num;
    n_sits = set->n_sits;
    sits = set->sits;
    n_start_sits = set->n_start_sits;
  }
  fprintf (f, "  Set = %lld\n", (long long) num);
  for (size_t i = 0; i < n_sits; i++) {
    fprintf (f, "    ");
    sit_print (f, sits[i]);
    if (i == n_start_sits - 1) {
      if (!nonstart_p) break;
      fprintf (f, "\n    -----------\n");
    }
  }
  fprintf (f, "\n");
}
#endif /* #ifndef NO_GP_DEBUG_PRINT */

static void set_fin (struct grammar *g) { /* finalize work with sets: */
  delete_htab_update_statistics (g, g->set_tab);
  OS_DELETE (g->sets_os);
  OS_DELETE (g->set_sits_os);
}

/* Store error CODE and message. The function makes long jump after that. */
static void error (struct grammar *g, int code, const char *format, ...) {
  va_list arguments;
  g->error_code = code;
  va_start (arguments, format);
  vsnprintf (g->error_message, sizeof (g->error_message), format, arguments);
  va_end (arguments);
  longjmp (g->error_longjump_buff, code);
}

static void error_func_for_allocate (void *g) { /* Process allocation errors. */
  error (g, GP_NO_MEMORY, "no memory");
}

static struct gp_tree_node *get_alt_opt_node (struct grammar *, struct gp_tree_node *, struct gp_tree_node *,
                                              size_t);

static void *default_node_merge (struct grammar *g GP_UNUSED, struct gp_tree_node *node1,
                                 struct gp_tree_node *node2 GP_UNUSED, size_t context_num GP_UNUSED) {
  return node1;
}

static void *parse_alloc_default (size_t size) { return malloc (size); }

static FORCE_INLINE void *parse_alloc (struct grammar *g, size_t size) { /* a wrapper with check */
  void *result = g->parse_alloc (size);
  if (result == NULL) error (g, GP_NO_MEMORY, "no memory for parse tree");
  return result;
}

static void parse_free_default (void *mem) { free (mem); }

static FORCE_INLINE void parse_free (struct grammar *g, void *mem) { /* a wrapper with check */
  if (mem != NULL && g->parse_free != NULL) g->parse_free (mem);
}

static void syntax_error_default (const char *err_nonterm_repr, bool after_p, const char *err_tok_repr,
                                  void *err_tok_attr GP_UNUSED, const char *stop_tok_repr,
                                  void *stop_tok_attr GP_UNUSED) {
  assert (err_nonterm_repr != NULL);
  if (stop_tok_repr == NULL)
    fprintf (stderr, "Syntax error %s %s on token %s\n", after_p ? "after" : "in", err_nonterm_repr,
             err_tok_repr);
  else
    fprintf (stderr, "Syntax error %s %s on token %s and stopping on token %s\n", after_p ? "after" : "in",
             err_nonterm_repr, err_tok_repr, stop_tok_repr);
}

int gp_error_code (struct grammar *g) { /* Return the last occurred error code for given grammar. */
  assert (g != NULL);
  return g->error_code;
}

/* Return the error message corresponding to the last occurred error code.  */
const char *gp_error_message (struct grammar *g) {
  assert (g != NULL);
  return g->error_message;
}

/* Create sets FIRST and FOLLOW for all grammar nonterminals. */
static void create_first_follow_sets (struct grammar *g) {
  struct symb *symb;
  for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++) {
    symb->u.nonterm.first = term_set_create (g);
    term_set_clear (g, symb->u.nonterm.first);
    symb->u.nonterm.follow = term_set_create (g);
    term_set_clear (g, symb->u.nonterm.follow);
  }
  bool changed_p;
  do {
    changed_p = false;
    for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++)
      for (struct rule *rule = symb->u.nonterm.rules; rule != NULL; rule = rule->lhs_next) {
        bool first_continue_p = true;
        struct symb **rhs = rule->rhs;
        int rhs_len = rule->rhs_len;
        for (int j = 0; j < rhs_len; j++) {
          struct symb *rhs_symb = rhs[j];
          if (rhs_symb->term_p) {
            if (first_continue_p && term_set_up (g, symb->u.nonterm.first, rhs_symb->u.term.term_num))
              changed_p = true;
          } else {
            if (first_continue_p && term_set_or (g, symb->u.nonterm.first, rhs_symb->u.nonterm.first))
              changed_p = true;
            int k;
            for (k = j + 1; k < rhs_len; k++) {
              struct symb *next_rhs_symb = rhs[k];
              if (next_rhs_symb->term_p
                  && term_set_up (g, rhs_symb->u.nonterm.follow, next_rhs_symb->u.term.term_num))
                changed_p = true;
              else if (!next_rhs_symb->term_p
                       && term_set_or (g, rhs_symb->u.nonterm.follow, next_rhs_symb->u.nonterm.first))
                changed_p = true;
              if (!next_rhs_symb->empty_p) break;
            }
            if (k == rhs_len && term_set_or (g, rhs_symb->u.nonterm.follow, symb->u.nonterm.follow))
              changed_p = true;
          }
          if (!rhs_symb->empty_p) first_continue_p = false;
        }
      }
  } while (changed_p);
}

/* Set up flags empty_p, access_p and derivation_p for all grammar symbols. */
static void set_empty_access_derives (struct grammar *g) {
  struct symb *symb;
  for (int i = 0; (symb = symb_get (g, i)) != NULL; i++) {
    symb->empty_p = false;
    symb->derivation_p = symb->term_p;
    symb->access_p = false;
  }
  g->axiom->access_p = true;
  bool empty_changed_p, derivation_changed_p, accessibility_change_p;
  do {
    empty_changed_p = derivation_changed_p = accessibility_change_p = false;
    for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++)
      for (struct rule *rule = symb->u.nonterm.rules; rule != NULL; rule = rule->lhs_next) {
        bool empty_p = true, derivation_p = true;
        for (int j = 0; j < rule->rhs_len; j++) {
          struct symb *rhs_symb = rule->rhs[j];
          if (symb->access_p) {
            accessibility_change_p |= rhs_symb->access_p ^ true;
            rhs_symb->access_p = 1;
          }
          empty_p &= rhs_symb->empty_p;
          derivation_p &= rhs_symb->derivation_p;
        }
        if (empty_p) {
          empty_changed_p |= symb->empty_p ^ empty_p;
          symb->empty_p = empty_p;
        }
        if (derivation_p) {
          derivation_changed_p |= symb->derivation_p ^ derivation_p;
          symb->derivation_p = derivation_p;
        }
      }
  } while (empty_changed_p || derivation_changed_p || accessibility_change_p);
}

static void set_loop_p (struct grammar *g) { /* set up flags loop_p for nonterminals: */
  struct symb *symb;
  /* Initialize according to minimal criteria: There is a rule in which the nonterminal stands and
     all the rest symbols can derive empty strings. */
  for (struct rule *rule = g->rules->first_rule; rule != NULL; rule = rule->next)
    for (int i = 0; i < rule->rhs_len; i++)
      if (!(symb = rule->rhs[i])->term_p) {
        int j;
        for (j = 0; j < rule->rhs_len; j++)
          if (i == j)
            continue;
          else if (!rule->rhs[j]->empty_p)
            break;
        if (j >= rule->rhs_len) symb->u.nonterm.loop_p = 1;
      }
  /* Major cycle: Check looped nonterminal that there is a rule with the nonterminal in lhs with a
     looped nonterminal in rhs and all the rest rhs symbols deriving empty string. */
  bool changed_p;
  do {
    struct symb *lhs;
    changed_p = false;
    for (int i = 0; (lhs = nonterm_get (g, i)) != NULL; i++)
      if (lhs->u.nonterm.loop_p) {
        bool loop_p = false;
        for (struct rule *rule = lhs->u.nonterm.rules; rule != NULL; rule = rule->lhs_next)
          for (int j = 0; j < rule->rhs_len; j++)
            if (!(symb = rule->rhs[j])->term_p && symb->u.nonterm.loop_p) {
              int k;
              for (k = 0; k < rule->rhs_len; k++)
                if (j == k)
                  continue;
                else if (!rule->rhs[k]->empty_p)
                  break;
              if (k >= rule->rhs_len) loop_p = true;
            }
        if (!loop_p) changed_p = true;
        lhs->u.nonterm.loop_p = loop_p;
      }
  } while (changed_p);
}

/* Evaluate different sets and flags for grammar and checks the grammar on correctness. */
static void check_grammar (struct grammar *g, int strict_p) {
  struct symb *symb;
  set_empty_access_derives (g);
  set_loop_p (g);
  if (strict_p) {
    for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++) {
      if (!symb->derivation_p)
        error (g, GP_NONTERM_DERIVATION, "nonterm `%s' does not derive any term string", symb->repr);
      else if (!symb->access_p)
        error (g, GP_UNACCESSIBLE_NONTERM, "nonterm `%s' is not accessible from axiom", symb->repr);
    }
  } else if (!g->axiom->derivation_p)
    error (g, GP_NONTERM_DERIVATION, "nonterm `%s' does not derive any term string", g->axiom->repr);
  for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++)
    if (symb->u.nonterm.loop_p)
      error (g, GP_LOOP_NONTERM, "nonterm `%s' can derive only itself (grammar with loops)", symb->repr);
  /* We should have correct flags empty_p here. */
  create_first_follow_sets (g);
}

/* Names of additional symbols. Don't use them in grammars. */
#define AXIOM_NAME "$S"
#define END_MARKER_NAME "$eof"

#define END_MARKER_CODE (-1) /* Should be negative. */

/* Forward declaration: */
static void empty_grammar (struct grammar *g);
static struct set *build_sets (struct grammar *g);

/* Read terminals/rules. Return error code or 0. Return pointer in G to the grammar. */
int gp_read_grammar (struct grammar *g, bool strict_p,
                     const char *(*read_terminal) (int *code, int *priority, enum gp_assoc *assoc),
                     const char *(*read_rule) (const char ***rhs, const char **abs_node, int **transl,
                                               int *guard_num)) {
  struct symb *symb;
  assert (g != NULL);
  int code;
  if ((code = setjmp (g->error_longjump_buff)) != 0) return code;
  if (!g->undefined_p) empty_grammar (g);
  const char *name;
  int priority;
  enum gp_assoc assoc;
  while ((name = (*read_terminal) (&code, &priority, &assoc)) != NULL) {
    if (code < 0) error (g, GP_NEGATIVE_TERM_CODE, "term `%s' has negative code", name);
    if (assoc != GP_NON_ASSOC && assoc != GP_LEFT_ASSOC && assoc != GP_RIGHT_ASSOC)
      error (g, GP_WRONG_TERM_ASSOC, "term `%s' has wrong associativity %d", name, assoc);
    symb = symb_find_by_repr (g, name);
    if (symb != NULL) error (g, GP_REPEATED_TERM_DECL, "repeated declaration of term `%s'", name);
    if (term_tab_find_by_code (g, code) != NULL)
      error (g, GP_REPEATED_TERM_CODE, "repeated code %d in term `%s'", code, name);
    symb_add_term (g, name, code, priority, assoc);
  }
  g->axiom = g->start = g->end_marker = NULL;
  const char *lhs, **rhs, *anode;
  int *transl;
  struct rule *rule;
  int guard_num;
  while ((lhs = (*read_rule) (&rhs, &anode, &transl, &guard_num)) != NULL) {
    symb = symb_find_by_repr (g, lhs);
    if (symb == NULL)
      symb = symb_add_nonterm (g, lhs);
    else if (symb->term_p)
      error (g, GP_TERM_IN_RULE_LHS, "term `%s' in the left hand side of rule", lhs);
    if (anode == NULL && transl != NULL && *transl >= 0 && transl[1] >= 0)
      error (g, GP_INCORRECT_TRANSLATION, "rule for `%s' has incorrect translation", lhs);
    if (g->axiom == NULL) {
      /* We made this here because we want that the start rule has number 0. */
      /* Add axiom and end marker. */
      g->start = symb;
      g->axiom = symb_find_by_repr (g, AXIOM_NAME);
      if (g->axiom != NULL) error (g, GP_FIXED_NAME_USAGE, "do not use fixed name `%s'", AXIOM_NAME);
      g->axiom = symb_add_nonterm (g, AXIOM_NAME);
      g->end_marker = symb_find_by_repr (g, END_MARKER_NAME);
      if (g->end_marker != NULL)
        error (g, GP_FIXED_NAME_USAGE, "do not use fixed name `%s'", END_MARKER_NAME);
      if (term_tab_find_by_code (g, END_MARKER_CODE) != NULL) abort ();
      g->end_marker = symb_add_term (g, END_MARKER_NAME, END_MARKER_CODE, -1, GP_NON_ASSOC);
      /* Add rules for start */
      rule = rule_new_start (g, g->axiom, NULL, -1);
      rule_new_symb_add (g, g->start);
      rule_new_symb_add (g, g->end_marker);
      rule_new_stop (g);
      rule->order[0] = 0;
      rule->trans_len = 1;
    }
    rule = rule_new_start (g, symb, anode, guard_num);
    while (*rhs != NULL) {
      symb = symb_find_by_repr (g, *rhs);
      if (symb == NULL) symb = symb_add_nonterm (g, *rhs);
      rule_new_symb_add (g, symb);
      rhs++;
    }
    rule_new_stop (g);
    if (transl != NULL) {
      int i, el;
      for (i = 0; (el = transl[i]) >= 0; i++)
        if (el >= rule->rhs_len) {
          if (el != GP_NIL_TRANSLATION_NUMBER)
            error (g, GP_INCORRECT_SYMBOL_NUMBER,
                   "translation symbol number %d in rule for `%s' is out of range", el, lhs);
          else
            rule->trans_len++;
        } else if (rule->order[el] >= 0)
          error (g, GP_REPEATED_SYMBOL_NUMBER, "repeated translation symbol number %d in rule for `%s'", el,
                 lhs);
        else {
          rule->order[el] = i;
          rule->trans_len++;
        }
      assert (i < rule->rhs_len || transl[i] < 0);
    }
  }
  if (g->axiom == NULL) error (g, GP_NO_RULES, "grammar does not contain rules");
  assert (g->start != NULL);
  check_grammar (g, strict_p);
  symb_finish_adding_terms (g);
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 3) {
    /* Print rules. */
    fprintf (stderr, "Rules:\n");
    for (rule = g->rules->first_rule; rule != NULL; rule = rule->next) {
      fprintf (stderr, "  ");
      rule_print (stderr, rule, true, true);
    }
    fprintf (stderr, "\n");
    /* Print symbol sets. */
    for (int i = 0; (symb = nonterm_get (g, i)) != NULL; i++) {
      fprintf (stderr, "Nonterm %s:  Empty=%s , Access=%s, Derive=%s\n", symb->repr,
               (symb->empty_p ? "Yes" : "No"), (symb->access_p ? "Yes" : "No"),
               (symb->derivation_p ? "Yes" : "No"));
      if (g->debug_level > 3) {
        fprintf (stderr, "  First: ");
        term_set_print (g, stderr, symb->u.nonterm.first);
        fprintf (stderr, "\n  Follow: ");
        term_set_print (g, stderr, symb->u.nonterm.follow);
        fprintf (stderr, "\n\n");
      }
    }
  }
#endif
  g->start_set = build_sets (g);
  g->undefined_p = false;
  return 0;
}

struct anode_name_code {
  const char *name; /* key */
  int code;
};

static uint64_t anode_name_code_hash (hash_table_entry_t a) { /* return hash of anode code */
  const char *str = ((struct anode_name_code *) a)->name;
  return hash (str, strlen (str), 42);
}

static bool anode_name_code_eq (hash_table_entry_t a1, hash_table_entry_t a2) { /* Equality of anode code. */
  return strcmp (((struct anode_name_code *) a1)->name, ((struct anode_name_code *) a2)->name) == 0;
}

/* Get code (used aux node of gp_tree_node) for anode with NAME.  By default the code is -1.  */
static int get_anode_code (struct grammar *g, const char *name) {
  struct anode_name_code anode_name_code, *tab_el;
  anode_name_code.name = name;
  tab_el
    = (struct anode_name_code *) *find_hash_table_entry (g->anode_name_code_tab, &anode_name_code, false);
  return tab_el == NULL ? -1 : tab_el->code;
}

/* Set code (used aux node of gp_tree_node) for anode with NAME.  By default the code is -1.  */
void gp_set_anode_code (struct grammar *g, const char *name, int code) {
  assert (g != NULL);
  struct anode_name_code anode_name_code;
  anode_name_code.name = name;
  hash_table_entry_t *entry = find_hash_table_entry (g->anode_name_code_tab, &anode_name_code, true);
  if (*entry != NULL) { /* redefine code */
    ((struct anode_name_code *) *entry)->code = code;
  } else {
    OS_TOP_ADD_MEMORY (g->anode_name_code_os, name, strlen (name) + 1);
    anode_name_code.name = OS_TOP_BEGIN (g->anode_name_code_os);
    OS_TOP_FINISH (g->anode_name_code_os);
    anode_name_code.code = code;
    OS_TOP_ADD_MEMORY (g->anode_name_code_os, &anode_name_code, sizeof (struct anode_name_code));
    *entry = OS_TOP_BEGIN (g->anode_name_code_os);
    OS_TOP_FINISH (g->anode_name_code_os);
  }
}

static void anode_name_code_tab_init (struct grammar *g) {
  OS_CREATE (g->anode_name_code_os, g->alloc, 0);
  g->anode_name_code_tab = create_hash_table (g->alloc, 300, anode_name_code_hash, anode_name_code_eq);
}

static void anode_name_code_tab_fin (struct grammar *g) {
  OS_DELETE (g->anode_name_code_os);
  delete_hash_table (g->anode_name_code_tab);
}

#include "sgramm.c"

/* Some Gecko API functions: */

gp_parse_alloc_func_t gp_set_parse_alloc (struct grammar *g, gp_parse_alloc_func_t fn) {
  assert (g != NULL);
  if (fn == NULL) error (g, GP_WRONG_ARG, "null parse_alloc func");
  gp_parse_alloc_func_t old = g->parse_alloc;
  g->parse_alloc = fn;
  return old;
}

gp_parse_free_func_t gp_set_parse_free (struct grammar *g, gp_parse_free_func_t fn) {
  assert (g != NULL);
  gp_parse_free_func_t old = g->parse_free;
  g->parse_free = fn;
  return old;
}

gp_syntax_error_func_t gp_set_syntax_error (struct grammar *g, gp_syntax_error_func_t fn) {
  assert (g != NULL);
  gp_syntax_error_func_t old = g->syntax_error;
  g->syntax_error = fn;
  return old;
}

gp_rule_guard_func_t gp_set_rule_guard (struct grammar *g, gp_rule_guard_func_t fn) {
  assert (g != NULL);
  gp_rule_guard_func_t old = g->rule_guard;
  g->rule_guard = fn;
  return old;
}

int gp_set_debug_level (struct grammar *g, int level) {
  assert (g != NULL);
  int old = g->debug_level;
  g->debug_level = level;
  return old;
}

int gp_set_recovery_match (struct grammar *g, int n_toks) {
  assert (g != NULL);
  int old = g->recovery_token_matches;
  g->recovery_token_matches = n_toks;
  return old;
}

gp_node_merge_func_t gp_set_node_merge_func (struct grammar *g, gp_node_merge_func_t func) {
  gp_node_merge_func_t res = g->node_merge;
  g->node_merge = func == NULL ? default_node_merge : func;
  return res;
}

/* Add the rest (non-start) situations to the new set. */
static inline void expand_new_start_set (struct grammar *g) {
  for (size_t i = 0; i < g->new_set->n_sits; i++) {
    struct sit *sit = g->new_sits[i];
    if (sit->pos >= sit->rule->rhs_len) continue;
    /* There is a symbol after dot in the situation. */
    struct symb *symb = sit->rule->rhs[sit->pos];
    if (!symb->term_p)
      for (struct rule *rule = symb->u.nonterm.rules; rule != NULL; rule = rule->lhs_next)
        set_new_add_nonstart_sit (g, sit_create (g, rule, 0));
  }
  set_new_set_stop (g);
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 3) set_print (g, stderr, g->new_set, g->debug_level > 3);
#endif
}

struct symb_sit {    /* tuple (symb, SLR-situation): */
  struct symb *symb; /* symbol after dot in the situation */
  size_t sit_num;    /* the situation number */
};

static int symb_sit_cmp (const void *el1, const void *el2) { /* used to sort the tuples by their symbols */
  const struct symb_sit *e1 = (const struct symb_sit *) el1, *e2 = (const struct symb_sit *) el2;
  if (e1->symb == e2->symb) return 0;
  return e1->symb->num - e2->symb->num;
}

static int action_cmp (const void *el1, const void *el2) { /* used to sort actions */
  const struct action *e1 = (const struct action *) el1, *e2 = (const struct action *) el2;
  int diff = e1->term_num - e2->term_num;
  if (diff != 0) return diff;
  if (e1->shift_p) return -1; /* put shift first */
  if (e2->shift_p) return 1;
  return e1->u.rule->num - e2->u.rule->num;
}

#ifndef NO_GP_DEBUG_PRINT
static void print_action (struct grammar *g, FILE *f, struct action *a) { /* Print action A into F */
  struct symb *term = ((struct symb **) VLO_BEGIN (g->symbs->terms_vlo))[a->term_num];
  fprintf (f, "%s : ", term->repr);
  if (a->shift_p) {
    fprintf (f, "shift to S%lld", (long long) a->u.set->num);
  } else {
    struct rule *rule = rule_get (g, a->u.rule->num);
    fprintf (f, "reduce \"");
    rule_print (f, rule, false, false);
    fprintf (f, "\"");
  }
}
#endif

/* Remove conflicts in ACTIONS whose initial and final number passed via ACTIONS_NUM. */
static void remove_priority_conflict_actions (struct grammar *g, struct action *actions,
                                              size_t *actions_num) {
  struct symb *term = term_get (g, actions[0].term_num);
  assert (term->u.term.term_num == actions[0].term_num);
  size_t num = *actions_num, new_num = 1;
  if (term->u.term.priority < 0 || num <= 1 || !actions[0].shift_p) return;
  bool remove_shift_p = false;
  for (size_t i = 1; i < num; i++) {
    struct action *action = &actions[i];
    assert (!action->shift_p);
    struct symb *reduce_term = action->u.rule->priority_term;
    if (reduce_term == NULL) continue;
    if (term->u.term.priority < reduce_term->u.term.priority) { /* remove shift */
      remove_shift_p = true;
      actions[new_num++] = *action;
    } else if (term->u.term.priority > reduce_term->u.term.priority) { /* remove reduce */
    } else if (term->u.term.assoc == GP_LEFT_ASSOC) {                  /* remove shift */
      remove_shift_p = true;
      actions[new_num++] = *action;
    } else if (term->u.term.assoc == GP_RIGHT_ASSOC) { /* remove reduce */
    } else {                                           /* nonassoc: remove both */
      remove_shift_p = true;
    }
  }
  if (remove_shift_p) {
    for (size_t i = 1; i < new_num; i++) actions[i - 1] = actions[i];
    new_num--;
  }
  *actions_num = new_num;
}

/* Build SET actions and goto map. */
static void build_goto_map_and_actions (struct grammar *g, struct set *set) {
  VLO_NULLIFY (g->symb_sits);
  VLO_NULLIFY (g->actions_vlo);
  for (size_t i = 0; i < set->n_sits; i++) {
    struct sit *sit = set->sits[i];
    if (sit->pos >= sit->rule->rhs_len) {
      for (int j = 0; j < g->symbs->n_terms; j++)
        if (term_set_test (g, sit->lookahead, j)) {
          struct action action;
          action.shift_p = false;
          action.term_num = j;
          action.u.rule = sit->rule;
          VLO_ADD_MEMORY (g->actions_vlo, &action, sizeof (action));
        }
      continue;
    }
    /* There is a symbol after dot in the situation. */
    struct symb *symb = sit->rule->rhs[sit->pos];
    struct symb_sit symb_sit = {symb, i};
    VLO_ADD_MEMORY (g->symb_sits, &symb_sit, sizeof (symb_sit));
  }
  size_t n = VLO_LENGTH (g->symb_sits) / sizeof (struct symb_sit);
  struct symb_sit *symb_sit_addr = (struct symb_sit *) VLO_BEGIN (g->symb_sits);
  qsort (symb_sit_addr, n, sizeof (struct symb_sit), symb_sit_cmp);
  set_new_set_start (g);
  for (size_t i = 0; i < n; i++) { /* build derived sets, goto map, and collect actions: */
    struct symb_sit *symb_sit = &symb_sit_addr[i];
    struct sit *sit = set->sits[symb_sit->sit_num];
    assert (sit->pos < sit->rule->rhs_len);
    struct symb_sit *next_symb_sit = i + 1 >= n ? NULL : &symb_sit_addr[i + 1];
    set_new_add_start_sit (g, sit_create (g, sit->rule, sit->pos + 1));
    if (next_symb_sit == NULL || symb_sit->symb != next_symb_sit->symb) { /* the last symb sit: */
      if (set_insert (g)) {
        g->new_set->symb = symb_sit->symb;
        expand_new_start_set (g);
        VLO_ADD_MEMORY (g->sets_vlo, &g->new_set, sizeof (struct set *));
      }
      struct set *trans_set = g->new_set;
      if (!symb_sit->symb->term_p) { /* goto */
        if (set->goto_map == NULL) {
          set->goto_map = set_calloc (g, sizeof (struct set *) * (size_t) g->symbs->n_nonterms);
          g->n_goto_vects++;
          g->n_goto_vect_len += (size_t) g->symbs->n_nonterms;
        }
        set->goto_map[symb_sit->symb->u.nonterm.nonterm_num] = trans_set;
      } else { /* shift */
        struct action action = {true, symb_sit->symb->u.term.term_num, {.set = trans_set}};
        VLO_ADD_MEMORY (g->actions_vlo, &action, sizeof (action));
      }
      set_new_set_start (g);
    }
  }
  set_new_set_stop (g);
  set->action_map = g->empty_action_map;
  size_t nta = VLO_LENGTH (g->actions_vlo) / sizeof (struct action);
  if (nta == 0) return;
  /* build action descs: */
  struct action *action_addr = (struct action *) VLO_BEGIN (g->actions_vlo);
  qsort (action_addr, nta, sizeof (struct action), action_cmp);
  size_t new_nta = 0;
  for (size_t i = 0, term_actions_num = 0, start = 0; i < nta; i++) { /* apply priorities: */
    struct action *action = &action_addr[i];
    struct action *prev_action = i == 0 ? NULL : &action_addr[i - 1];
    struct action *next_action = i + 1 >= nta ? NULL : &action_addr[i + 1];
    if (prev_action != NULL && action->term_num == prev_action->term_num) {
      term_actions_num++;
    } else {
      start = new_nta;
      term_actions_num = 1;
    }
    action_addr[new_nta++] = *action;
    if (next_action == NULL || action->term_num != next_action->term_num) {
      remove_priority_conflict_actions (g, &action_addr[start], &term_actions_num);
      new_nta = start + term_actions_num;
    }
  }
  nta = new_nta;
  assert (set->action_map == g->empty_action_map);
  set->action_map = set_calloc (g, sizeof (struct action_desc) * (size_t) g->symbs->n_terms);
  g->n_action_vects++;
  g->n_action_vect_len += (size_t) g->symbs->n_terms;
  set->actions = set_calloc (g, sizeof (struct action) * nta);
  set->n_actions = nta;
  g->n_actions += nta;
  unsigned actions_num = 0;
  for (size_t i = 0; i < nta; i++) { /* set up set actions, actions_start, and actions_num: */
    struct action *action = &action_addr[i];
    struct action *prev_action = i == 0 ? NULL : &action_addr[i - 1];
    struct action *next_action = i + 1 >= nta ? NULL : &action_addr[i + 1];
    set->actions[i] = *action;
    if (prev_action != NULL && action->term_num == prev_action->term_num) {
      actions_num++;
    } else {
      set->action_map[action->term_num].actions_start = (unsigned) i;
      actions_num = 1;
    }
    if (next_action == NULL || action->term_num != next_action->term_num)
      set->action_map[action->term_num].actions_num = actions_num;
  }
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 2) {
    fprintf (stderr, "  Actions for");
    set_print (g, stderr, set, false);
    bool conflict_p = false;
    for (size_t i = 0; i < nta; i++) {
      if (i == 0 || set->actions[i - 1].term_num != set->actions[i].term_num)
        conflict_p = i + 1 < nta && set->actions[i].term_num == set->actions[i + 1].term_num;
      fprintf (stderr, "           %c ", conflict_p ? '!' : ' ');
      print_action (g, stderr, &set->actions[i]);
      fprintf (stderr, "\n");
    }
  }
#endif
}

static void build_empty_action_map (struct grammar *g) { /* initialize empty_action_map: */
  g->empty_action_map = set_calloc (g, sizeof (struct action_desc) * (size_t) g->symbs->n_terms);
  for (int i = 0; i < g->symbs->n_terms; i++) {
    g->empty_action_map[i].actions_num = 0;
    g->empty_action_map[i].actions_start = 0;
  }
  g->n_action_vects++;
  g->n_action_vect_len += (size_t) g->symbs->n_terms;
}

static struct set *build_sets (struct grammar *g) { /* build grammar SLR-sets and return the 1st set: */
  build_empty_action_map (g);
  VLO_CREATE (g->sets_vlo, g->alloc, 0);
  set_new_set_start (g);
  for (struct rule *rule = g->axiom->u.nonterm.rules; rule != NULL; rule = rule->lhs_next) {
    struct sit *sit = sit_create (g, rule, 0);
    set_new_add_start_sit (g, sit);
  }
  if (!set_insert (g)) assert (false);
  g->new_set->symb = g->axiom;
  expand_new_start_set (g);
  struct set *start_set = g->new_set;
  VLO_ADD_MEMORY (g->sets_vlo, &start_set, sizeof (struct set *));
  while (VLO_LENGTH (g->sets_vlo) != 0) {
    struct set *set = ((struct set **) VLO_BOUND (g->sets_vlo))[-1];
    VLO_SHORTEN (g->sets_vlo, sizeof (struct set *));
    build_goto_map_and_actions (g, set);
    unsigned int actions_num;
    struct action *actions = get_actions (g, set, g->end_marker->u.term.term_num, &actions_num);
    if (actions_num == 1 && actions[0].shift_p) g->eof_set = set;
  }
  VLO_DELETE (g->sets_vlo);
  return start_set;
}

#define SWAP(a, b, t) \
  do {                \
    t = a;            \
    a = b;            \
    b = t;            \
  } while (false)

struct recovery_info { /* stack data used for syntax error recovery: */
  union {
    struct {
      bool after_p;             /* true if error occurred after nonterm */
      int n_matched_toks;       /* number of last matched toks */
      ptrdiff_t buff_token_ind; /* index of the current input token in the input buffer for the stack */
      ptrdiff_t cost;           /* cost of the recovery for given stack */
      ptrdiff_t err_ntoks;      /* how many tokens were read before the error token */
      /* the topmost nonterm on the stack when error occurred (if after_p) or the most nested nonterm covering
         the error: */
      struct symb *nonterm;
    } info;
    struct recovery_info *next; /* used for freed infos */
  } u;
};

static void recovery_info_init (struct grammar *g) { /* Init work with recovery data: */
  OS_CREATE (g->recovery_infos, g->alloc, 0);
  g->free_recovery_infos = NULL;
}

static struct recovery_info *recovery_info_get_free (struct grammar *g) { /* get free error recovery data: */
  struct recovery_info *info = g->free_recovery_infos;
  if (info != NULL) {
    g->free_recovery_infos = info->u.next;
    return info;
  }
  OS_TOP_EXPAND (g->recovery_infos, sizeof (struct recovery_info));
  info = (struct recovery_info *) OS_TOP_BEGIN (g->recovery_infos);
  OS_TOP_FINISH (g->recovery_infos);
  return info;
}

static void recovery_info_free (struct grammar *g, struct recovery_info *info) { /* free recovery INFO: */
  info->u.next = g->free_recovery_infos;
  g->free_recovery_infos = info;
}

/* Return new recovery data which is copy of recovery data INFO: */
static struct recovery_info *recovery_info_copy (struct grammar *g, struct recovery_info *info) {
  struct recovery_info *res = recovery_info_get_free (g);
  *res = *info;
  return res;
}

/* Finalize work with error recovery data. */
static void recovery_info_finish (struct grammar *g) { OS_DELETE (g->recovery_infos); }

#define TOKS_BIT_NUM (sizeof (size_t) * 8 - 1) /* bit field width for ntoks */

typedef struct {               /* the stack element: */
  bool attr_p : 1;             /* true if ANODE_ATTR refers to the term attribute instead of node */
  size_t ntoks : TOKS_BIT_NUM; /* read tokens before the state */
  struct set *set;             /* the corresponding SLR-set */
  void *anode_attr;            /* abstract node or term attr if attr_p */
} stack_el_t;

struct stack {                    /* a parse stack: */
  int ambiguity;                  /* ambiguity found on the stack */
  size_t num;                     /* unique stack number */
  struct recovery_info *recovery; /* stack recovery data */
  vlo_t els;                      /* container of the stack elements */
};

static void stack_init (struct grammar *g) { /* initialize work with the stacks: */
  static_assert (TOKS_BIT_NUM < sizeof (size_t) * 8, "wrong value of TOKS_BIT_NUM");
  VLO_CREATE (g->free_stacks, g->alloc, 16);
  g->n_stacks = 0;
#ifndef NO_GP_DEBUG_PRINT
  g->n_peak_stack_els = g->n_curr_stack_els = 0;
#endif
  recovery_info_init (g);
}

static void stack_vlo_free (struct grammar *g, vlo_t *stack_vlo) { /* free all stacks in STACK_VLO: */
  VLO_ADD_MEMORY (g->free_stacks, VLO_BEGIN (*stack_vlo), VLO_LENGTH (*stack_vlo));
}

static void stack_finish (struct grammar *g) { /* finish all work with the stacks: */
  for (size_t i = 0; i < VLO_LENGTH (g->free_stacks) / sizeof (struct stack *); i++) {
    struct stack *stack = ((struct stack **) VLO_BEGIN (g->free_stacks))[i];
    VLO_DELETE (stack->els);
    gp_free (g->alloc, stack);
  }
  VLO_DELETE (g->free_stacks);
  recovery_info_finish (g);
}

/* Create and return new stack.  For non-null stack BASE, copy all stack data into the new stack. */
static struct stack *stack_create (struct grammar *g, struct stack *base) {
  struct stack *stack;
  if (VLO_LENGTH (g->free_stacks) == 0) {
    stack = gp_malloc (g->alloc, sizeof (struct stack));
    stack->num = g->n_stacks++;
    VLO_CREATE (stack->els, g->alloc, (base == NULL ? 0 : VLO_LENGTH (base->els)) + 4 * sizeof (stack_el_t));
  } else {
    stack = ((struct stack **) VLO_BOUND (g->free_stacks))[-1];
    VLO_SHORTEN (g->free_stacks, sizeof (struct stack *));
    VLO_NULLIFY (stack->els);
  }
#ifndef NO_GP_DEBUG_PRINT
  if (base != NULL) {
    g->n_curr_stack_els += VLO_LENGTH (base->els) / sizeof (stack_el_t);
    if (g->n_peak_stack_els < g->n_curr_stack_els) g->n_peak_stack_els = g->n_curr_stack_els;
  }
#endif
  stack->ambiguity = 0;
  if (base != NULL) {
    stack->ambiguity = base->ambiguity;
    VLO_ADD_MEMORY (stack->els, VLO_BEGIN (base->els), VLO_LENGTH (base->els));
  }
  if (base == NULL || base->recovery == NULL) {
    stack->recovery = NULL;
  } else {
    stack->recovery = recovery_info_copy (g, base->recovery);
  }
  return stack;
}

#define MAX_FREE_STACKS 200 /* Max free stacks to speedup stack allocation */

static void stack_free (struct grammar *g, struct stack *stack) { /* free STACK: */
  if (stack->recovery != NULL) recovery_info_free (g, stack->recovery);
#ifndef NO_GP_DEBUG_PRINT
  g->n_curr_stack_els -= VLO_LENGTH (stack->els) / sizeof (stack_el_t);
#endif
  if (VLO_LENGTH (g->free_stacks) / sizeof (struct stack *) < MAX_FREE_STACKS) {
    VLO_ADD_MEMORY (g->free_stacks, &stack, sizeof (stack)); /* keep free stack in cache */
  } else {
    VLO_DELETE (stack->els);
    gp_free (g->alloc, stack);
  }
}

struct token_buff_el { /* element of the input token buffer: */
  int code;            /* token code */
  void *attr;          /* token attribute */
};

/* Allocate and initialize STACK recovery data, use BUFF_TOKEN_IND as index of the current input token. */
static void stack_init_recovery (struct grammar *g, struct stack *stack, ptrdiff_t buff_token_ind) {
  assert (stack->recovery == NULL);
  stack->recovery = recovery_info_get_free (g);
  stack->recovery->u.info.buff_token_ind = buff_token_ind;
  stack->recovery->u.info.n_matched_toks = 0;
  stack->recovery->u.info.cost = 0;
  stack->recovery->u.info.after_p = false;
  stack->recovery->u.info.nonterm = NULL;
  stack->recovery->u.info.err_ntoks = (ptrdiff_t) g->read_tokens - 1;
  ptrdiff_t diff = buff_token_ind - (ptrdiff_t) (VLO_LENGTH (g->token_buff) / sizeof (struct token_buff_el));
  if (UNLIKELY (diff < 0)) stack->recovery->u.info.err_ntoks += diff + 1;
}

static void stack_free_recovery (struct grammar *g, struct stack *stack) { /* free recovery data of STACK: */
  assert (stack->recovery != NULL);
  recovery_info_free (g, stack->recovery);
  stack->recovery = NULL;
}

/* Return the number of input tokens in the buffer. */
static FORCE_INLINE size_t token_buff_len (struct grammar *g) {
  return VLO_LENGTH (g->token_buff) / sizeof (struct token_buff_el);
}

/* Return token (given by index in the buffer) from the buffer. */
static int token_buff_get (struct grammar *g, ptrdiff_t ind, void **attr) {
  assert (ind >= 0 && (size_t) ind * sizeof (struct token_buff_el) < VLO_LENGTH (g->token_buff));
  struct token_buff_el *el = &((struct token_buff_el *) VLO_BEGIN (g->token_buff))[ind];
  *attr = el->attr;
  return el->code;
}

/* Add token to token buffer. */
static void token_buff_add (struct grammar *g, int code, void *attr, bool new_p) {
  if (!new_p && token_buff_len (g) == 0 && g->curr_buff_token_ind > 0) {
    void *attr2;
    int code2 = token_buff_get (g, (ptrdiff_t) g->curr_buff_token_ind - 1, &attr2);
    assert (code2 == code && attr == attr2);
    g->curr_buff_token_ind--;
    return;
  }
  VLO_EXPAND (g->token_buff, sizeof (struct token_buff_el));
  struct token_buff_el *el = &((struct token_buff_el *) VLO_BOUND (g->token_buff))[-1];
  el->code = code;
  el->attr = attr;
}

/* Read a token and save it in the buffer. */
static int token_buff_read (struct grammar *g, void **attr) {
  int code = g->read_token (attr);
  if (code < 0) code = END_MARKER_CODE;
  g->read_tokens++;
  token_buff_add (g, code, *attr, true);
  return code;
}

/* Return the next token.  Take it from the buffer if the buffer is not fully read. */
static int token_read (struct grammar *g, void **attr) {
  size_t size = g->curr_buff_token_ind * sizeof (struct token_buff_el);
  if (UNLIKELY (size > 0 && size <= VLO_LENGTH (g->token_buff))) {
    if (size == VLO_LENGTH (g->token_buff)) { /* buffer was read fully: nullify it */
      g->curr_buff_token_ind = 0;
      VLO_NULLIFY (g->token_buff);
    } else {
      struct token_buff_el *el
        = &((struct token_buff_el *) VLO_BEGIN (g->token_buff))[g->curr_buff_token_ind++];
      *attr = el->attr;
      return el->code;
    }
  }
  int code = g->read_token (attr);
  if (code < 0) code = END_MARKER_CODE;
  g->read_tokens++;
  return code;
}

#ifndef NO_GP_DEBUG_PRINT
static void token_buff_print (struct grammar *g, FILE *f) { /* print the input buffer: */
  for (size_t i = 0; i < VLO_LENGTH (g->token_buff) / sizeof (struct token_buff_el); i++) {
    struct token_buff_el *el = &((struct token_buff_el *) VLO_BEGIN (g->token_buff))[i];
    struct symb *term = term_find_by_code (g, el->code);
    fprintf (f, " %llu:%s", (unsigned long long) i, term->repr);
  }
}

/* Print debug info after reading TERM where the number of current stack is STACKS_NUM. */
static void print_read (struct grammar *g, FILE *f, struct symb *term, size_t stacks_num) {
  fprintf (f, "--Read %s (%llu, #stacks: %llu, #nodes: all=%llu)", term->repr,
           (unsigned long long) g->read_tokens, (unsigned long long) stacks_num,
           (unsigned long long) g->n_parse_nodes);
  if (g->debug_level < 3) {
    fprintf (f, "\n");
  } else {
    fprintf (f, ": buff (%llu) =", (unsigned long long) g->curr_buff_token_ind);
    token_buff_print (g, f);
    fprintf (f, "\n");
  }
}
#endif

/* Read input tokens and store them in the input buffer until the buffer has MAX_BUFF_IND+1 tokens. */
static void token_buff_expand (struct grammar *g, size_t max_buff_ind) {
  for (size_t len = token_buff_len (g); len <= max_buff_ind; len++) {
    void *attr;
    int new_code GP_UNUSED = token_buff_read (g, &attr);
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 2))
      print_read (g, stderr, term_find_by_code (g, new_code),
                  VLO_LENGTH (g->new_stacks) / sizeof (struct stack *));
#endif
  }
}

static void token_buff_init (struct grammar *g) { /* init work with the input buffer: */
  g->curr_buff_token_ind = 0;
  VLO_CREATE (g->token_buff, g->alloc, 0);
}

static void token_buff_reset (struct grammar *g) { /* clear the input buffer: */
  g->curr_buff_token_ind = 0;
  VLO_NULLIFY (g->token_buff);
}

/* Finish work with the input buffer. */
static void token_buff_finish (struct grammar *g) { VLO_DELETE (g->token_buff); }

/* Push element with the grammar start SET to STACK. */
static void push_init_set (struct grammar *g GP_UNUSED, struct stack *stack, struct set *set) {
  VLO_EXPAND (stack->els, sizeof (stack_el_t));
  stack_el_t *el = &((stack_el_t *) VLO_BOUND (stack->els))[-1];
  el->set = set;
  el->attr_p = false;
  el->ntoks = 0;
  el->anode_attr = NULL;
#ifndef NO_GP_DEBUG_PRINT
  g->n_curr_stack_els++;
  if (g->n_peak_stack_els < g->n_curr_stack_els) g->n_peak_stack_els = g->n_curr_stack_els;
#endif
}

/* Return pointer to the top STACK element. */
static FORCE_INLINE stack_el_t *stack_get_top_el (struct stack *stack) {
  assert (VLO_LENGTH (stack->els) != 0);
  return &((stack_el_t *) VLO_BOUND (stack->els))[-1];
}

/* Return pointer to the top STACK element set. */
static FORCE_INLINE struct set *stack_get_top_set (struct stack *stack) {
  return stack_get_top_el (stack)->set;
}

/* Push an element to the STACK after shift with result SET.  ATTR is attribute of the shift token. */
static FORCE_INLINE struct set *stack_shift (struct grammar *g, struct stack *stack, struct set *set,
                                             void *attr) {
  assert (set->symb->term_p);
  VLO_EXPAND (stack->els, sizeof (stack_el_t));
  stack_el_t *el = &((stack_el_t *) VLO_BOUND (stack->els))[-1];
  el->set = set;
  el->attr_p = true;
  el->ntoks = (g->read_tokens - 1) & ~((size_t) 1 << TOKS_BIT_NUM);
  el->anode_attr = attr;
  ptrdiff_t diff
    = (ptrdiff_t) (g->curr_buff_token_ind - VLO_LENGTH (g->token_buff) / sizeof (struct token_buff_el));
  if (UNLIKELY (diff < 0))
    el->ntoks = (size_t) ((ptrdiff_t) el->ntoks + diff + 1) & ~((size_t) 1 << TOKS_BIT_NUM);
#ifndef NO_GP_DEBUG_PRINT
  g->n_curr_stack_els++;
  if (g->n_peak_stack_els < g->n_curr_stack_els) g->n_peak_stack_els = g->n_curr_stack_els;
#endif
  return set;
}

static FORCE_INLINE struct gp_tree_node *get_term_node (struct grammar *g, int code, void *attr) {
  struct gp_tree_node *term_node = parse_alloc (g, sizeof (struct gp_tree_node));
  term_node->type = GP_TERM;
  term_node->aux = -1;
  term_node->val.term.code = code;
  term_node->val.term.attr = attr;
  term_node->num = g->n_parse_nodes++;
  g->n_parse_term_nodes++;
  VLO_ADD_MEMORY (g->all_nodes, &term_node, sizeof (struct gp_tree_node *));
  return term_node;
}

/* Return empty node (GP_NIL), create it if there is no such node yet. */
static FORCE_INLINE struct gp_tree_node *get_empty_node (struct grammar *g) {
  if (g->empty_node == NULL) {
    g->empty_node = (struct gp_tree_node *) parse_alloc (g, sizeof (struct gp_tree_node));
    g->empty_node->type = GP_NIL;
    g->empty_node->aux = -1;
    g->empty_node->num = 0; /* always zero */
    VLO_ADD_MEMORY (g->all_nodes, &g->empty_node, sizeof (struct gp_tree_node *));
  }
  return g->empty_node;
}

/* Return term node (GP_TERM) for stack element EL referring to the token attribute. */
static FORCE_INLINE struct gp_tree_node *get_stack_term_node (struct grammar *g, stack_el_t *el) {
  assert (el->attr_p && el->set->symb->term_p);
  return get_term_node (g, el->set->symb->u.term.code, el->anode_attr);
}

/* Return abstract node (GP_ANODE) with given NAME, CODE, and CHILDREN. Create it if necessary.  */
static struct gp_tree_node *get_anode (struct grammar *g, const char *name, int code, int children_num,
                                       struct gp_tree_node **children) {
  struct gp_tree_node *anode = parse_alloc (g, sizeof (struct gp_tree_node));
#if !defined(NO_GP_DEBUG_PRINT)
  g->n_parse_abstract_nodes++;
#endif
  anode->type = GP_ANODE;
  anode->aux = code;
  anode->val.anode.name = name;
  anode->val.anode.children_num = children_num;
  anode->num = g->n_parse_nodes++;
  anode->val.anode.children = parse_alloc (g, (size_t) children_num * sizeof (struct gp_tree_node *));
  memcpy (anode->val.anode.children, children, (size_t) children_num * sizeof (struct gp_tree_node *));
  VLO_ADD_MEMORY (g->all_nodes, &anode, sizeof (struct gp_tree_node *));
  return anode;
}

/* Return alternative node (GP_ALT) if CONTEXT_NUM == 0 or context-dependent alternative node (GP_OPT) with
   given fields. Create it if necessary. */
static struct gp_tree_node *get_alt_opt_node (struct grammar *g, struct gp_tree_node *first,
                                              struct gp_tree_node *second, size_t context_num) {
  struct gp_tree_node *res = parse_alloc (g, sizeof (struct gp_tree_node));
  res->aux = -1;
#ifndef NO_GP_DEBUG_PRINT
  context_num > 0 ? g->n_parse_opt_nodes++ : g->n_parse_alt_nodes++;
#endif
  if (context_num == 0) {
    res->type = GP_ALT;
    res->val.alt.first = first;
    res->val.alt.second = second;
  } else {
    res->val.opt.context_num = (size_t) context_num;
    res->type = GP_OPT;
    res->val.opt.first = first;
    res->val.opt.second = second;
  }
  res->num = g->n_parse_nodes++;
  VLO_ADD_MEMORY (g->all_nodes, &res, sizeof (struct gp_tree_node *));
  return res;
}

/* API: Return alternative node (GP_ALT) with given fields.  Create it if necessary. */
struct gp_tree_node *gp_get_alt_node (struct grammar *g, struct gp_tree_node *first,
                                      struct gp_tree_node *second) {
  return get_alt_opt_node (g, first, second, 0);
}

/* API: Return context-dependent alternative node (GP_OPT) with given fields.  Create it if necessary. */
struct gp_tree_node *gp_get_opt_node (struct grammar *g, struct gp_tree_node *first,
                                      struct gp_tree_node *second, size_t context_num) {
  assert (context_num > 0);
  return get_alt_opt_node (g, first, second, context_num);
}

/* Return parse tree node when reducing stack with STACK_ADDR and STACK_LEN for RULE. Allocate and create
   abstract node, abstract node name, and children term nodes if necessary. */
static NO_INLINE struct gp_tree_node *get_reduce_node (struct grammar *g, stack_el_t *stack_addr,
                                                       size_t stack_len, struct rule *rule) {
  int rhs_len = rule->rhs_len;
  if (rule->anode == NULL) {
    assert (rule->trans_len == 1);
    for (size_t i = 0, start = stack_len - (size_t) rhs_len; i < (size_t) rhs_len; i++) {
      int disp = rule->order[i];
      if (disp < 0) continue;
      stack_el_t *el = &stack_addr[start + i];
      if (el->attr_p) return get_stack_term_node (g, el);
      return el->anode_attr;
    }
    return get_empty_node (g);
  }
  if (rule->caller_anode == NULL) {
    rule->caller_anode = ((char *) parse_alloc (g, strlen (rule->anode) + 1));
    VLO_ADD_MEMORY (g->caller_anode_names, &rule->caller_anode, sizeof (rule->caller_anode));
    strcpy (rule->caller_anode, rule->anode);
    rule->caller_anode_code = get_anode_code (g, rule->anode);
  }
  VLO_NULLIFY (g->temp_nodes_vlo);
  VLO_EXPAND (g->temp_nodes_vlo, sizeof (struct gp_tree_node *) * (size_t) rule->trans_len);
  struct gp_tree_node **children = VLO_BEGIN (g->temp_nodes_vlo);
  for (int i = 0; i < rule->trans_len; i++) children[i] = get_empty_node (g);
  for (size_t i = 0, start = stack_len - (size_t) rhs_len; i < (size_t) rhs_len; i++) {
    int disp = rule->order[i];
    if (disp < 0) continue;
    stack_el_t *el = &stack_addr[start + i];
    struct gp_tree_node *anode = (struct gp_tree_node *) el->anode_attr;
    if (el->attr_p) anode = get_stack_term_node (g, el);
    children[disp] = anode;
  }
  return get_anode (g, rule->caller_anode, rule->caller_anode_code, rule->trans_len, children);
}

/* Reduce STACK by RULE and create node representing the rule translation if necessary and return it. */
static FORCE_INLINE struct set *stack_reduce (struct grammar *g, struct stack *stack, struct rule *rule) {
  int rhs_len = rule->rhs_len;
  int nonterm_num = rule->lhs_nonterm_num;
  size_t stack_len = VLO_LENGTH (stack->els) / sizeof (stack_el_t);
  assert ((size_t) rhs_len < stack_len);
  stack_el_t *stack_addr = (stack_el_t *) VLO_BEGIN (stack->els);
  struct set *set = stack_addr[stack_len - 1 - (size_t) rhs_len].set;
  size_t ntoks;
  if (rhs_len != 0) {
    ntoks = stack_addr[stack_len - (size_t) rhs_len].ntoks;
  } else {
    ntoks = g->read_tokens - 1;
    ptrdiff_t diff
      = (ptrdiff_t) (g->curr_buff_token_ind - VLO_LENGTH (g->token_buff) / sizeof (struct token_buff_el));
    if (UNLIKELY (diff < 0)) ntoks = (size_t) ((ptrdiff_t) ntoks + diff + 1);
  }
  VLO_SHORTEN (stack->els, sizeof (stack_el_t) * (size_t) rhs_len);
  void *anode_attr = get_empty_node (g);
  if (rule->anode != NULL || rule->trans_len != 0)
    anode_attr = get_reduce_node (g, stack_addr, stack_len, rule);
  assert (anode_attr != NULL);
  struct set *goto_set = set->goto_map[nonterm_num];
  VLO_EXPAND (stack->els, sizeof (stack_el_t));
  stack_el_t *el = &((stack_el_t *) VLO_BOUND (stack->els))[-1];
  el->set = goto_set;
  el->attr_p = false;
  el->ntoks = ntoks & ~((size_t) 1 << TOKS_BIT_NUM);
  el->anode_attr = anode_attr;
#ifndef NO_GP_DEBUG_PRINT
  g->n_curr_stack_els += (size_t) (1 - rhs_len);
  if (g->n_peak_stack_els < g->n_curr_stack_els) g->n_peak_stack_els = g->n_curr_stack_els;
#endif
  return goto_set;
}

#define MERGE_HTAB_THRESHOLD 10 /* # stacks threshold to use htab for merging stacks */

/* Stack hash table: */

static uint64_t stack_hash (hash_table_entry_t s) { /* Hash of the stack. */
  struct stack *stack = ((struct stack *) s);
  uint64_t result = hash_init (88);
  if (stack->recovery != NULL) {
    result = hash_step (result, (uint64_t) stack->recovery->u.info.after_p);
    result = hash_step (result, (uint64_t) stack->recovery->u.info.n_matched_toks);
    result = hash_step (result, (uint64_t) stack->recovery->u.info.buff_token_ind);
    result = hash_step (result, (uint64_t) stack->recovery->u.info.cost);
    result = hash_step (result, (uint64_t) stack->recovery->u.info.err_ntoks);
    result = hash_step (result, (uint64_t) stack->recovery->u.info.nonterm);
  }
  for (size_t i = 0; i < VLO_LENGTH (stack->els) / sizeof (stack_el_t); i++) {
    stack_el_t *el = &((stack_el_t *) VLO_BEGIN (stack->els))[i];
    result = hash_step (result, (uint64_t) (ptrdiff_t) el->set);
  }
  return hash_finish (result);
}

static bool stack_eq (hash_table_entry_t s1, hash_table_entry_t s2) { /* Equality of the stacks. */
  struct stack *stack1 = ((struct stack *) s1);
  struct stack *stack2 = ((struct stack *) s2);
  if (VLO_LENGTH (stack1->els) != VLO_LENGTH (stack2->els)) return false;
  if ((stack1->recovery == NULL && stack2->recovery != NULL)
      || (stack1->recovery != NULL && stack2->recovery == NULL))
    return false;
  if (stack1->recovery != NULL) {
    if (stack1->recovery->u.info.after_p != stack2->recovery->u.info.after_p) return false;
    if (stack1->recovery->u.info.n_matched_toks != stack2->recovery->u.info.n_matched_toks) return false;
    if (stack1->recovery->u.info.buff_token_ind != stack2->recovery->u.info.buff_token_ind) return false;
    if (stack1->recovery->u.info.cost != stack2->recovery->u.info.cost) return false;
    if (stack1->recovery->u.info.err_ntoks != stack2->recovery->u.info.err_ntoks) return false;
    if (stack1->recovery->u.info.nonterm != stack2->recovery->u.info.nonterm) return false;
  }
  for (size_t i = 0; i < VLO_LENGTH (stack1->els) / sizeof (stack_el_t); i++) {
    stack_el_t *el1 = &((stack_el_t *) VLO_BEGIN (stack1->els))[i];
    stack_el_t *el2 = &((stack_el_t *) VLO_BEGIN (stack2->els))[i];
    if (el1->set != el2->set) return false;
  }
  return true;
}

/* Insert STACK into htab if it is not there yet.  Return the final stack in the htab.  */
static struct stack *stack_tab_insert (struct grammar *g, struct stack *stack) {
  struct stack *tab_stack;
  struct stack **stack_entry = (struct stack **) find_hash_table_entry (g->stack_htab, stack, true);
  if ((tab_stack = *stack_entry) != NULL) return tab_stack;
  *stack_entry = stack;
  return stack;
}

static void stack_tab_init (struct grammar *g) { /* Initialize work with the stack table. */
  g->stack_htab = create_hash_table (g->alloc, 2 * MERGE_HTAB_THRESHOLD, stack_hash, stack_eq);
}

static void stack_tab_fin (struct grammar *g) { /* Finalize work with the stack table. */
  delete_htab_update_statistics (g, g->stack_htab);
}

/* Return true if stacks with set points of view are equal.  Return different node/attributes of the
   corresponding stack els through N_DIFF_ATTR. */
static bool stack_els_eq_p (struct stack *stack1, struct stack *stack2, int *n_diff_attr) {
  if (VLO_LENGTH (stack1->els) != VLO_LENGTH (stack2->els)) return false;
  stack_el_t *stack_addr1 = (stack_el_t *) VLO_BEGIN (stack1->els);
  stack_el_t *stack_addr2 = (stack_el_t *) VLO_BEGIN (stack2->els);
  int n = 0;
  for (ptrdiff_t i = (ptrdiff_t) (VLO_LENGTH (stack1->els) / sizeof (stack_el_t)) - 1; i >= 0; i--) {
    stack_el_t *el1 = &stack_addr1[i], *el2 = &stack_addr2[i];
    if (el1->set != el2->set) return false;
    if (el1->anode_attr != el2->anode_attr) n++;
  }
  *n_diff_attr = n;
  return true;
}

/* Merge nodes/attrs stack FROM to stack TO.  Setup TO ambiguity. */
static FORCE_INLINE void merge_nodes (struct grammar *g, struct stack *to, struct stack *from,
                                      int n_diff_attr) {
  if (n_diff_attr <= 0) return;
  size_t context_num = 0;
  if (n_diff_attr > 1) context_num = ++g->contexts_num;
  stack_el_t *to_addr = (stack_el_t *) VLO_BEGIN (to->els);
  stack_el_t *from_addr = (stack_el_t *) VLO_BEGIN (from->els);
  for (ptrdiff_t k = (ptrdiff_t) (VLO_LENGTH (to->els) / sizeof (stack_el_t)) - 1; k >= 0; k--) {
    stack_el_t *to_el = &to_addr[k], *from_el = &from_addr[k];
    if (to_el->anode_attr == from_el->anode_attr) continue;
    if (to_el->attr_p) {
      to_el->anode_attr = get_stack_term_node (g, to_el);
      to_el->attr_p = false;
    }
    if (from_el->attr_p) {
      from_el->anode_attr = get_stack_term_node (g, from_el);
      from_el->attr_p = false;
    }
    to_el->anode_attr = g->node_merge (g, to_el->anode_attr, from_el->anode_attr, context_num);
  }
  if (context_num > 0) to->ambiguity = 2;
}

static FORCE_INLINE bool merge_stacks (struct grammar *g, vlo_t *stacks) { /* merge identical STACKS */
  bool merge_p = false;
  size_t last = 0;
  size_t len = VLO_LENGTH (*stacks) / sizeof (struct stack *);
  bool htab_p = len > MERGE_HTAB_THRESHOLD;
  if (htab_p) empty_hash_table (g->stack_htab);
  for (size_t i = 0; i < len; i++) {
    struct stack *curr = ((struct stack **) VLO_BEGIN (*stacks))[i];
    if (htab_p) {
      struct stack *tab_stack;
      if ((tab_stack = stack_tab_insert (g, curr)) == curr) {
        ((struct stack **) VLO_BEGIN (*stacks))[last++] = curr;
      } else {
        int n_diff_attr;
        bool eq_p = stack_els_eq_p (curr, tab_stack, &n_diff_attr);
        assert (eq_p);
        if (tab_stack->ambiguity == 0) tab_stack->ambiguity = 1;
        merge_p = true;
        merge_nodes (g, tab_stack, curr, n_diff_attr);
        stack_free (g, curr);
      }
      continue;
    }
    if (curr == NULL) continue;
    ((struct stack **) VLO_BEGIN (*stacks))[last++] = curr;
    for (size_t j = i + 1; j < len; j++) {
      struct stack *curr2 = ((struct stack **) VLO_BEGIN (*stacks))[j];
      if (curr2 == NULL) continue;
      int n_diff_attr;
      if (!stack_els_eq_p (curr, curr2, &n_diff_attr)) continue;
      if (curr->ambiguity == 0) curr->ambiguity = 1;
      merge_p = true;
      ((struct stack **) VLO_BEGIN (*stacks))[j] = NULL;
      merge_nodes (g, curr, curr2, n_diff_attr);
      stack_free (g, curr2);
    }
  }
  VLO_SHORTEN (*stacks, VLO_LENGTH (*stacks) - last * sizeof (struct stack *));
  return merge_p;
}

#ifndef NO_GP_DEBUG_PRINT

static void print_stack_els (FILE *f, struct stack *stack) { /* Print stack elements into F: */
  for (size_t i = 0; i < VLO_LENGTH (stack->els) / sizeof (stack_el_t); i++) {
    stack_el_t *el = &((stack_el_t *) VLO_BEGIN (stack->els))[i];
    struct symb *symb = el->set->symb;
    fprintf (f, " [%lld]s%lld:%s", (long long) el->ntoks, (long long) el->set->num, symb->repr);
    if (el->anode_attr == NULL) continue;
    if (!el->attr_p) {
      struct gp_tree_node *anode = (struct gp_tree_node *) el->anode_attr;
      fprintf (f, ":n%llu", (unsigned long long) anode->num);
    } else if (strcmp (symb->repr, END_MARKER_NAME) != 0) {
      assert (symb->term_p);
      fprintf (f, ":a%llx", (unsigned long long) el->anode_attr);
    }
  }
  fprintf (f, "\n");
}

static void print_stack (FILE *f, struct stack *stack) { /* Print all info about STACK into F: */
  fprintf (f, "      {#%llu}", (unsigned long long) stack->num);
  if (stack->recovery != NULL) {
    struct symb *nonterm = stack->recovery->u.info.nonterm;
    if (nonterm != NULL)
      fprintf (f, "nonterm=%s%s ", nonterm->repr, stack->recovery->u.info.after_p ? "+" : "");
    fprintf (f, "token #%lld (matched=%d, cost=%lld):", (long long) stack->recovery->u.info.buff_token_ind,
             stack->recovery->u.info.n_matched_toks, (long long) stack->recovery->u.info.cost);
  }
  print_stack_els (f, stack);
}

/* Print single STACK elements and ACTION (it is never called in recovery mode). */
static void print_single_stack (struct grammar *g, FILE *f, struct stack *stack, struct action *action) {
  assert (stack->recovery == NULL);
  fprintf (f, "  Single stack after [");
  print_action (g, f, action);
  fprintf (f, "]: {#%llu}", (unsigned long long) stack->num);
  print_stack_els (f, stack);
}

/* Print all stacks starting with index START in STACKS. Print TITLE before. */
static void print_stacks (FILE *f, const char *title, vlo_t *stacks, size_t start) {
  fprintf (f, "%s:\n", title);
  for (size_t i = start; i < VLO_LENGTH (*stacks) / sizeof (struct stack *); i++)
    print_stack (f, ((struct stack **) VLO_BEGIN (*stacks))[i]);
}
#endif

/* Update recovery nonterm of STACK after a reduce (if REDUCE_P) or a shift.  */
static void update_recovery_nonterm (struct stack *stack, bool reduce_p) {
  assert (stack->recovery != NULL);
  stack_el_t *top_el = stack_get_top_el (stack);
  if (reduce_p && (stack->recovery->u.info.nonterm == NULL || stack->recovery->u.info.after_p)
      && top_el->ntoks <= (size_t) stack->recovery->u.info.err_ntoks) {
    stack->recovery->u.info.after_p = false;
    stack->recovery->u.info.nonterm = top_el->set->symb;
    return;
  }
  if (stack->recovery->u.info.nonterm != NULL) return;
  for (ptrdiff_t i = (ptrdiff_t) (VLO_LENGTH (stack->els) / sizeof (stack_el_t)) - 1; i >= 0; i--) {
    stack_el_t *el = &((stack_el_t *) VLO_BEGIN (stack->els))[i];
    if (!el->set->symb->term_p && el->ntoks <= (size_t) stack->recovery->u.info.err_ntoks) {
      stack->recovery->u.info.after_p = true;
      stack->recovery->u.info.nonterm = el->set->symb;
      break;
    }
  }
}

/* Process START_STACK on term.  Add stacks produced by shifting to new_stacks and failed new stacks
   (reduced but not shifted) to failed stacks. Current stacks are the same after the return.  Return true if
   there is any shifted stack, in other words a new stack.  */
static bool process_term_for_stack (struct grammar *g, struct stack *start_stack, int term, void *attr) {
  bool shift_p = false;
  size_t len = VLO_LENGTH (g->curr_stacks);
  size_t new_els_num GP_UNUSED = VLO_LENGTH (g->new_stacks) / sizeof (struct stack *);
  VLO_ADD_MEMORY (g->curr_stacks, &start_stack, sizeof (start_stack));
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 5)) {
    struct symb *symb = ((struct symb **) VLO_BEGIN (g->symbs->terms_vlo))[term];
    fprintf (stderr, "    Processing stack on term %s:", symb->repr);
    print_stack (stderr, start_stack);
  }
#endif
  while (VLO_LENGTH (g->curr_stacks) > len) {
    struct stack *curr_stack = ((struct stack **) VLO_BOUND (g->curr_stacks))[-1];
    VLO_SHORTEN (g->curr_stacks, sizeof (struct stack *));
    stack_el_t *el = &((stack_el_t *) VLO_BOUND (curr_stack->els))[-1];
    struct set *set = el->set;
    unsigned int actions_num;
    struct action *actions = get_actions (g, set, term, &actions_num);
    if (actions_num == 0) {
      VLO_ADD_MEMORY (g->failed_stacks, &curr_stack, sizeof (curr_stack));
      continue;
    }
#ifndef NO_GP_DEBUG_PRINT
    g->n_multi_stack_actions++;
#endif
    for (unsigned i = 0; i < actions_num; i++) {
      struct action *action = &actions[i];
      struct stack *stack = i == actions_num - 1 ? curr_stack : stack_create (g, curr_stack);
#ifndef NO_GP_DEBUG_PRINT
      if (UNLIKELY (g->debug_level > 5)) {
        fprintf (stderr, "      Apply action ");
        print_action (g, stderr, action);
      }
#endif
      if (LIKELY (!action->shift_p)) { /* reduce */
        struct rule *r = action->u.rule;
        if (r->guard_num >= 0 && g->rule_guard != NULL && !g->rule_guard (r->guard_num, g->rule_guard_arg)) {
          stack_free (g, stack);
          continue;
        }
        set = stack_reduce (g, stack, action->u.rule);
        if (stack->recovery != NULL) {
          assert (!set->symb->term_p);
          update_recovery_nonterm (stack, true);
        }
        VLO_ADD_MEMORY (g->curr_stacks, &stack, sizeof (stack));
      } else { /* shift */
        struct set *shifted_set = action->u.set;
        assert (shifted_set != NULL);
        stack_shift (g, stack, shifted_set, attr);
        VLO_ADD_MEMORY (g->new_stacks, &stack, sizeof (stack));
        shift_p = true;
        if (stack->recovery != NULL) {
          stack->recovery->u.info.n_matched_toks++;
          stack->recovery->u.info.buff_token_ind++;
          update_recovery_nonterm (stack, false);
        }
      }
    }
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 5)) fprintf (stderr, "\n");
#endif
  }
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 5))
    print_stacks (stderr, "+++Result stacks after processing the stack", &g->new_stacks, new_els_num);
#endif
  return shift_p;
}

/* Finish error recovery: Keep new stacks only with at least one matched token. Return the single stack
   if there is only one stack, otherwise return NULL.  Report the syntax error. */
static struct stack *recovery_stop (struct grammar *g, struct symb *error_term, void *error_attr) {
  for (size_t i = 0; i < VLO_LENGTH (g->failed_stacks) / sizeof (struct stack *); i++) {
    struct stack *stack = ((struct stack **) VLO_BEGIN (g->failed_stacks))[i];
    stack_free (g, stack);
  }
  VLO_NULLIFY (g->failed_stacks);
  ptrdiff_t min_cost = -1, buff_token_ind = -1;
  bool eof_stack_p = false;
  for (size_t i = 0; i < VLO_LENGTH (g->new_stacks) / sizeof (struct stack *); i++) {
    struct stack *stack = ((struct stack **) VLO_BEGIN (g->new_stacks))[i];
    struct set *set = stack_get_top_set (stack);
    struct symb *symb = set->symb;
    bool eof_p = symb == g->end_marker;
    if ((eof_p || stack->recovery->u.info.n_matched_toks >= g->recovery_token_matches)
        && (min_cost < 0 || stack->recovery->u.info.cost < min_cost)) {
      min_cost = stack->recovery->u.info.cost;
      buff_token_ind = stack->recovery->u.info.buff_token_ind;
      eof_stack_p = eof_p;
    }
  }
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 3))
    print_stacks (stderr, "+++Stacks before error recovery stop", &g->new_stacks, 0);
#endif
  size_t n = 0;
  bool error_after_p = false;
  const char *error_nonterm = NULL;
  struct stack *single_stack = NULL;
  for (size_t i = 0; i < VLO_LENGTH (g->new_stacks) / sizeof (struct stack *); i++) {
    struct stack *stack = ((struct stack **) VLO_BEGIN (g->new_stacks))[i];
    struct set *set = stack_get_top_set (stack);
    struct symb *symb = set->symb;
    bool eof_p = symb == g->end_marker;
    assert (stack->recovery->u.info.nonterm != NULL);
    if (stack->recovery->u.info.buff_token_ind != buff_token_ind || eof_p != eof_stack_p
        || (!eof_p && stack->recovery->u.info.n_matched_toks == 0)) {
      stack_free (g, stack);
    } else {
      single_stack = stack;
      error_after_p = stack->recovery->u.info.after_p;
      error_nonterm = stack->recovery->u.info.nonterm->repr;
      stack_free_recovery (g, stack);
      ((struct stack **) VLO_BEGIN (g->new_stacks))[n++] = stack;
    }
  }
  assert (error_nonterm != NULL);
  VLO_SHORTEN (g->new_stacks, VLO_LENGTH (g->new_stacks) - n * sizeof (struct stack *));
  if (VLO_LENGTH (g->new_stacks) > sizeof (struct stack *)) single_stack = NULL;
  assert (buff_token_ind >= 0);
  g->curr_buff_token_ind = (size_t) buff_token_ind;
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 2)) {
    fprintf (stderr, "<<<%s error recovery stop (buff token ind =%lld, error nonterm =%s%s)>>>\n",
             single_stack != NULL ? "Single stack" : "Multi-stack", (long long) buff_token_ind, error_nonterm,
             error_after_p ? "+" : "");
    if (g->debug_level > 3) print_stacks (stderr, "+++Result stacks after error recovery", &g->new_stacks, 0);
  }
#endif
  buff_token_ind -= g->recovery_token_matches;
  if (buff_token_ind < 0) buff_token_ind = 0; /* we can finish at the end marker */
  void *stop_token_attr;
  int stop_code = token_buff_get (g, buff_token_ind, &stop_token_attr);
  struct symb *stop_token_term = term_find_by_code (g, stop_code);
  g->syntax_error (error_nonterm, error_after_p, error_term->repr, error_attr, stop_token_term->repr,
                   stop_token_attr);
  return single_stack;
}

#define MAX_MERGE_AND_TRUNC_STACKS 200 /* maximal stacks number after merge_and_trunc_stacks */

/* Merge stacks with truncation to MAX_MERGE_AND_TRUNC_STACKS. */
static void merge_and_trunc_stacks (struct grammar *g, vlo_t *stacks) {
  merge_stacks (g, stacks);
  while (VLO_LENGTH (*stacks) > MAX_MERGE_AND_TRUNC_STACKS * sizeof (struct stack *)) {
    struct stack *stack = ((struct stack **) VLO_BOUND (*stacks))[-1];
    VLO_SHORTEN (*stacks, sizeof (struct stack *));
    stack_free (g, stack);
  }
}

/* Make syntax error recovery and set up final new_stacks and return the final single stack (if there is only
   one stack).

   Error recovery algorithm in brief: for each failed stack, we reject current stack token and add new stack
   derived from the stack by popping the top elements until the new stack has an action on the current token
   of the original stack.  We stop error recovery when we have a stack which is a minimal cost stack which
   successfully consumed recovery_token_matches tokens without a gap or which is a final stack which
   consumed EOF.  The stacks which matched at least one token are start stacks after the recovery.

   The error recovery algorithm guarantees that Gecko always produces parse trees corresponding to
   syntactically correct inputs.  Simply some tokens before and after error token are ignored. */
static struct stack *recovery (struct grammar *g, int code, void *attr) {
  assert (VLO_LENGTH (g->failed_stacks) != 0 && VLO_LENGTH (g->new_stacks) == 0
          && VLO_LENGTH (g->curr_stacks) == 0);
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 2)) fprintf (stderr, "<<<Error recovery start>>>\n");
#endif
  struct symb *error_term = term_find_by_code (g, code);
  void *error_attr = attr;
  token_buff_add (g, code, attr, false);
  bool stop_p = false;
  do {
    merge_and_trunc_stacks (g, &g->failed_stacks);
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 3))
      print_stacks (stderr, "+++Failed recovery stacks", &g->failed_stacks, 0);
#endif
    ptrdiff_t max_buff_ind = 0;
    struct stack *eof_stack = NULL;
    for (size_t i = 0; i < VLO_LENGTH (g->failed_stacks) / sizeof (struct stack *); i++) {
      struct stack *stack = ((struct stack **) VLO_BEGIN (g->failed_stacks))[i];
      if (stack->recovery == NULL) stack_init_recovery (g, stack, (ptrdiff_t) g->curr_buff_token_ind);
      int curr_code = token_buff_get (g, stack->recovery->u.info.buff_token_ind, &attr);
      struct symb *term = term_find_by_code (g, curr_code);
      ptrdiff_t len = (ptrdiff_t) (VLO_LENGTH (stack->els) / sizeof (stack_el_t));
      for (ptrdiff_t j = len - 2; j >= 0; j--) {
        stack_el_t *el = &((stack_el_t *) VLO_BEGIN (stack->els))[j];
        unsigned actions_num;
        get_actions (g, el->set, term->u.term.term_num, &actions_num);
        if (actions_num != 0) {
          struct stack *new_stack = stack_create (g, stack);
          new_stack->recovery->u.info.n_matched_toks = 0;
          ptrdiff_t ntoks1 = ((stack_el_t *) VLO_BEGIN (stack->els))[j + 1].ntoks;
          ptrdiff_t ntoks2 = ((stack_el_t *) VLO_BEGIN (stack->els))[len - 1].ntoks;
          new_stack->recovery->u.info.cost += ntoks2 - ntoks1 + 1;
          VLO_SHORTEN (new_stack->els, (size_t) (len - j - 1) * sizeof (stack_el_t));
          VLO_ADD_MEMORY (g->new_stacks, &new_stack, sizeof (new_stack));
          break;
        }
      }
      if (curr_code == END_MARKER_CODE) {
        if (eof_stack == NULL)
          eof_stack = stack;
        else
          stack_free (g, stack);
      } else { /* skip curr stack term: */
        stack->recovery->u.info.buff_token_ind++;
        stack->recovery->u.info.cost++;
        stack->recovery->u.info.n_matched_toks = 0;
        stack->recovery->u.info.after_p = false;
        stack->recovery->u.info.nonterm = NULL;
        VLO_ADD_MEMORY (g->new_stacks, &stack, sizeof (stack));
        if (max_buff_ind < stack->recovery->u.info.buff_token_ind)
          max_buff_ind = stack->recovery->u.info.buff_token_ind;
      }
    }
    token_buff_expand (g, (size_t) max_buff_ind);
    VLO_NULLIFY (g->failed_stacks);
    vlo_t temp_vlo;
    VLO_NULLIFY (g->curr_stacks);
    SWAP (g->curr_stacks, g->new_stacks, temp_vlo);
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 3))
      print_stacks (stderr, "+++Current recovery stacks", &g->curr_stacks, 0);
#endif
    for (size_t i = 0; i < VLO_LENGTH (g->curr_stacks) / sizeof (struct stack *); i++) {
      struct stack *stack = ((struct stack **) VLO_BEGIN (g->curr_stacks))[i];
      int curr_code = token_buff_get (g, stack->recovery->u.info.buff_token_ind, &attr);
      struct symb *term = term_find_by_code (g, curr_code);
      bool shift_p = process_term_for_stack (g, stack, term->u.term.term_num, attr);
      if (shift_p && max_buff_ind < stack->recovery->u.info.buff_token_ind)
        max_buff_ind = stack->recovery->u.info.buff_token_ind;
    }
    VLO_NULLIFY (g->curr_stacks);
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 3)) print_stacks (stderr, "+++New recovery stacks", &g->new_stacks, 0);
#endif
    struct stack *min_cost_stack = NULL;
    if (VLO_LENGTH (g->new_stacks) == 0 && VLO_LENGTH (g->failed_stacks) == 0) {
      if (eof_stack == NULL) {
        error (g, GP_BAD_RULE_GUARDS, "grammar is overconstrained by rule guards -- can do nothing");
      } else {
        assert (VLO_LENGTH (eof_stack->els) >= 2 * sizeof (stack_el_t));
        VLO_SHORTEN (eof_stack->els, VLO_LENGTH (eof_stack->els) - 2 * sizeof (stack_el_t));
        stack_el_t *el = &((stack_el_t *) VLO_BOUND (eof_stack->els))[-1];
        el->attr_p = false;
        el->ntoks = 0;
        el->set = g->eof_set;
        el->anode_attr = get_empty_node (g);
        VLO_ADD_MEMORY (g->new_stacks, &eof_stack, sizeof (eof_stack));
        eof_stack = NULL;
      }
    }
    if (eof_stack != NULL) stack_free (g, eof_stack);
    merge_and_trunc_stacks (g, &g->new_stacks);
    bool expand_p = true;
    /* True when eof was reached or a minimal cost stack matched at least recovery_token_matches.  */
    for (size_t i = 0; i < VLO_LENGTH (g->new_stacks) / sizeof (struct stack *); i++) {
      struct stack *stack = ((struct stack **) VLO_BEGIN (g->new_stacks))[i];
      stack_el_t *el = &((stack_el_t *) VLO_BOUND (stack->els))[-1];
      if (el->set->symb == g->end_marker) {
        expand_p = false;
        stop_p = true;
        break;
      } else if (min_cost_stack == NULL
                 || stack->recovery->u.info.cost < min_cost_stack->recovery->u.info.cost) {
        min_cost_stack = stack;
        stop_p = stack->recovery->u.info.n_matched_toks >= g->recovery_token_matches;
      }
    }
    if (expand_p) token_buff_expand (g, (size_t) max_buff_ind);
  } while (!stop_p);
  return recovery_stop (g, error_term, error_attr);
}

/* Garbage collection of parse tree nodes: */

/* Mark parse node ANODE. */
static FORCE_INLINE void gc_mark_anode (struct grammar *g, struct gp_tree_node *anode) {
  if (!bitmap_set_bit_p (&g->marked_nodes, anode->num)
      || (anode->type != GP_ANODE && anode->type != GP_ALT && anode->type != GP_OPT))
    return;
  VLO_ADD_MEMORY (g->temp_vlo, &anode, sizeof (struct gp_tree_node *));
}

/* Mark parse all nodes reachable from ANODE. */
static void gc_mark_parse_tree (struct grammar *g, struct gp_tree_node *anode) {
  if (!bitmap_set_bit_p (&g->marked_nodes, anode->num)
      || (anode->type != GP_ANODE && anode->type != GP_ALT && anode->type != GP_OPT))
    return;
  VLO_ADD_MEMORY (g->temp_vlo, &anode, sizeof (struct gp_tree_node *));
  while (VLO_LENGTH (g->temp_vlo) != 0) {
    anode = ((struct gp_tree_node **) VLO_BOUND (g->temp_vlo))[-1];
    VLO_SHORTEN (g->temp_vlo, sizeof (struct gp_tree_node *));
    if (anode->type == GP_ANODE) {
      for (int i = anode->val.anode.children_num - 1; i >= 0; i--)
        gc_mark_anode (g, anode->val.anode.children[i]);
    } else if (anode->type == GP_ALT) {
      gc_mark_anode (g, anode->val.alt.first);
      gc_mark_anode (g, anode->val.alt.second);
    } else {
      assert (anode->type == GP_OPT);
      gc_mark_anode (g, anode->val.opt.first);
      gc_mark_anode (g, anode->val.opt.second);
    }
  }
}

/* Mark parse nodes referenced from STACK. */
static void gc_mark_stack (struct grammar *g, struct stack *stack) {
  for (size_t i = 0; i < VLO_LENGTH (stack->els) / sizeof (stack_el_t); i++) {
    stack_el_t *el = &((stack_el_t *) VLO_BEGIN (stack->els))[i];
    if (!el->attr_p && el->anode_attr != NULL) gc_mark_parse_tree (g, el->anode_attr);
  }
}

/* Free all parse nodes not reachable from STACKS. */
static void gc (struct grammar *g, vlo_t *stacks) {
  assert (g->parse_free != NULL);
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 2) fprintf (stderr, "++++GC start\n");
#endif
  /* Mark: */
  bitmap_clear (&g->marked_nodes);
  for (size_t i = 0; i < VLO_LENGTH (*stacks) / sizeof (struct stack *); i++) {
    struct stack *stack = ((struct stack **) VLO_BEGIN (*stacks))[i];
    gc_mark_stack (g, stack);
  }
  /* Sweep */
  size_t last = 0;
#ifndef NO_GP_DEBUG_PRINT
  int n = 0, removed = 0;
  if (g->debug_level > 3) fprintf (stderr, "GC: Removed nodes:\n");
#endif
  size_t nodes_num = VLO_LENGTH (g->all_nodes) / sizeof (struct gp_tree_node *);
  for (size_t i = 0; i < nodes_num; i++) {
    struct gp_tree_node *node = ((struct gp_tree_node **) VLO_BEGIN (g->all_nodes))[i];
    if (bitmap_bit_p (&g->marked_nodes, node->num)) {
      ((struct gp_tree_node **) VLO_BEGIN (g->all_nodes))[last++] = node;
    } else {
#ifndef NO_GP_DEBUG_PRINT
      removed++;
      if (g->debug_level > 3) {
        fprintf (stderr, " n%llu", (unsigned long long) node->num);
        n++;
        if (n >= 16) {
          fprintf (stderr, "\n");
          n = 0;
        }
      }
#endif
      if (node->type == GP_ANODE) parse_free (g, node->val.anode.children);
      parse_free (g, node);
    }
  }
  VLO_SHORTEN (g->all_nodes, VLO_LENGTH (g->all_nodes) - last * sizeof (struct gp_tree_node *));
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 3 && n != 0) fprintf (stderr, "\n");
  if (g->debug_level > 2)
    fprintf (stderr, "++++GC finish: before nodes %llu, kept %llu, removed %d\n",
             (unsigned long long) nodes_num, (unsigned long long) last, removed);
#endif
}

#define GC_START_NODES_THRESHOLD 1000 /* Start threshold to initiate parse tree nodes GC */

/* Major function to make parsing. Return true if we parsed successfully. */
static bool parse (struct grammar *g, int *ambiguity, struct gp_tree_node **transl) {
  g->read_tokens = 0;
  g->n_parse_nodes = 1; /* fix one for empty node */
  g->empty_node = NULL;
  struct stack *single_stack = stack_create (g, NULL);
  VLO_ADD_MEMORY (g->curr_stacks, &single_stack, sizeof (single_stack));
  push_init_set (g, single_stack, g->start_set);
  g->n_parse_term_nodes = g->n_parse_abstract_nodes = g->n_parse_alt_nodes = g->n_parse_opt_nodes = 0;
#ifndef NO_GP_DEBUG_PRINT
  g->n_single_stack_actions = g->n_multi_stack_actions = 0;
#endif
  void *attr;
  int code = g->read_token (&attr);
  if (code < 0) code = END_MARKER_CODE;
  g->read_tokens++;
  struct symb *term_symb = term_find_by_code (g, code);
  int term = term_symb->u.term.term_num;
#ifndef NO_GP_DEBUG_PRINT
  if (UNLIKELY (g->debug_level > 2)) print_read (g, stderr, term_symb, 1);
#endif
  size_t gc_nodes_threshold = GC_START_NODES_THRESHOLD;
  for (;;) {
    if (single_stack != NULL) {
      stack_el_t *el = &((stack_el_t *) VLO_BOUND (single_stack->els))[-1];
      struct set *set = el->set;
      for (;;) {
        unsigned actions_num;
        struct action *actions = get_actions (g, set, term, &actions_num);
        if (actions_num != 1) {
          single_stack = NULL;
          goto multi_stack;
        }
#ifndef NO_GP_DEBUG_PRINT
        g->n_single_stack_actions++;
#endif
        if (LIKELY (!actions[0].shift_p)) { /* reduce */
          struct rule *r = actions[0].u.rule;
          if (r->guard_num >= 0 && g->rule_guard != NULL
              && !g->rule_guard (r->guard_num, g->rule_guard_arg)) {
            goto multi_stack; /* no actions: goto error recovery */
          }
          set = stack_reduce (g, single_stack, r);
#ifndef NO_GP_DEBUG_PRINT
          if (UNLIKELY (g->debug_level > 4)) print_single_stack (g, stderr, single_stack, &actions[0]);
#endif
        } else { /* shift */
          struct set *shifted_set = actions[0].u.set;
          assert (shifted_set != NULL);
          set = stack_shift (g, single_stack, shifted_set, attr);
#ifndef NO_GP_DEBUG_PRINT
          if (UNLIKELY (g->debug_level > 4)) print_single_stack (g, stderr, single_stack, &actions[0]);
#endif
          if (code == END_MARKER_CODE) goto finish;
          code = token_read (g, &attr);
          term_symb = term_find_by_code (g, code);
          term = term_symb->u.term.term_num;
#ifndef NO_GP_DEBUG_PRINT
          if (UNLIKELY (g->debug_level > 2)) print_read (g, stderr, term_symb, 1);
#endif
        }
      }
    }
  multi_stack: /* error recovery start through this too */
    assert (VLO_LENGTH (g->new_stacks) == 0);
    while (VLO_LENGTH (g->failed_stacks) != 0) {
      struct stack *failed_stack = ((struct stack **) VLO_BOUND (g->failed_stacks))[-1];
      VLO_SHORTEN (g->failed_stacks, sizeof (struct stack *));
      stack_free (g, failed_stack);
    }
    bool shift_p = false;
    while (VLO_LENGTH (g->curr_stacks) != 0) {
      struct stack *curr_stack = ((struct stack **) VLO_BOUND (g->curr_stacks))[-1];
      VLO_SHORTEN (g->curr_stacks, sizeof (struct stack *));
      if (process_term_for_stack (g, curr_stack, term, attr)) shift_p = true;
    }
    if (!shift_p) { /* error: */
      if (VLO_LENGTH (g->failed_stacks) == 0)
        error (g, GP_BAD_RULE_GUARDS, "grammar is overconstrained by rule guards -- can do nothing");
      single_stack = recovery (g, code, attr);
      code = token_buff_get (g, (ptrdiff_t) g->curr_buff_token_ind - 1, &attr); /* last read token */
    }
    assert (VLO_LENGTH (g->new_stacks) != 0 && VLO_LENGTH (g->curr_stacks) == 0);
    vlo_t temp_vlo;
    SWAP (g->curr_stacks, g->new_stacks, temp_vlo);
    if (merge_stacks (g, &g->curr_stacks)) {
#ifndef NO_GP_DEBUG_PRINT
      if (UNLIKELY (g->debug_level > 4))
        print_stacks (stderr, "+++Parsing stacks after node merging", &g->curr_stacks, 0);
#endif
    } else {
#ifndef NO_GP_DEBUG_PRINT
      if (UNLIKELY (g->debug_level > 4)) print_stacks (stderr, "+++New parsing stacks", &g->curr_stacks, 0);
#endif
    }
    if (code == END_MARKER_CODE) {
      assert (VLO_LENGTH (g->curr_stacks) == sizeof (struct stack *));
      break;
    }
    if (VLO_LENGTH (g->curr_stacks) == sizeof (struct stack *)) {
      single_stack = ((struct stack **) VLO_BEGIN (g->curr_stacks))[0];
    } else if (g->parse_free != NULL && VLO_LENGTH (g->all_nodes) >= (size_t) gc_nodes_threshold) {
      gc (g, &g->curr_stacks);
      gc_nodes_threshold = 2 * VLO_LENGTH (g->all_nodes);
    }
    code = token_read (g, &attr);
    term_symb = term_find_by_code (g, code);
    term = term_symb->u.term.term_num;
#ifndef NO_GP_DEBUG_PRINT
    if (UNLIKELY (g->debug_level > 2))
      print_read (g, stderr, term_symb, VLO_LENGTH (g->curr_stacks) / sizeof (struct stack *));
#endif
  }
finish:
  assert (VLO_LENGTH (g->curr_stacks) == sizeof (struct stack *));
  struct stack *final_stack = ((struct stack **) VLO_BEGIN (g->curr_stacks))[0];
  struct set *set = stack_get_top_set (final_stack);
  struct symb *symb = set->symb;
  assert (strcmp (symb->repr, END_MARKER_NAME) == 0);
  assert (VLO_LENGTH (final_stack->els) == 3 * sizeof (stack_el_t));
  stack_el_t *el = &((stack_el_t *) VLO_BEGIN (final_stack->els))[1];
  assert (!el->attr_p);
  *transl = (struct gp_tree_node *) el->anode_attr;
  *ambiguity = final_stack->ambiguity;
  if (g->parse_free != NULL) gc (g, &g->curr_stacks); /* free all unused nodes */
  VLO_NULLIFY (g->all_nodes);
  stack_vlo_free (g, &g->failed_stacks);
  VLO_NULLIFY (g->failed_stacks);
  stack_vlo_free (g, &g->curr_stacks);
  VLO_NULLIFY (g->new_stacks);
  VLO_NULLIFY (g->curr_stacks);
  token_buff_reset (g);
  return true;
}

#ifndef NO_GP_DEBUG_PRINT

struct print_arg { /* parse traverse arg used to print parse tree: */
  struct grammar *g;
  FILE *f;
};

/* Prints NODE into file given in A. */
static bool print_node (struct gp_tree_node *node, struct gp_tree_node *father GP_UNUSED, void *a) {
  struct print_arg *arg = a;
  struct grammar *g = arg->g;
  FILE *f = arg->f;
  assert (node != NULL);
  fprintf (f, "%7llu: ", (unsigned long long) node->num);
  switch (node->type) {
  case GP_NIL: fprintf (f, "EMPTY\n"); break;
  case GP_TERM:
    fprintf (f, "TERMINAL: code=%d, repr=%s\n", node->val.term.code,
             term_find_by_code (g, node->val.term.code)->repr);
    break;
  case GP_ANODE:
    fprintf (f, "ABSTRACT: %s (", node->val.anode.name);
    for (int i = 0; i < node->val.anode.children_num; i++)
      fprintf (f, " %llu", (unsigned long long) node->val.anode.children[i]->num);
    fprintf (f, " )\n");
    break;
  case GP_ALT:
    fprintf (f, "ALTERNATIVE:");
    fprintf (f, "%llu %llu\n", (unsigned long long) node->val.alt.first->num,
             (unsigned long long) node->val.alt.second->num);
    break;
  case GP_OPT:
    fprintf (f, "OPTION (%llu):", (unsigned long long) node->val.opt.context_num);
    fprintf (f, "%llu %llu\n", (unsigned long long) node->val.opt.first->num,
             (unsigned long long) node->val.opt.second->num);
    break;
  default: assert (false);
  }
  return true;
}

/* API: Print parse tree with ROOT. */
void gp_print_translation (struct grammar *g, FILE *f, struct gp_tree_node *root) {
  struct print_arg arg = {g, f};
  gp_traverse_tree (g, root, print_node, NULL, &arg);
  fprintf (f, "\n");
}

#endif

/* API function used for parsing.  It also prints debug info. */
int gp_parse (struct grammar *g, int (*read) (void **attr), struct gp_tree_node **root, int *ambiguity,
              void *arg) {
  assert (g != NULL);
  g->all_searches = g->all_collisions = 0;
  g->rule_guard_arg = arg;
  g->read_token = read;
  *root = NULL;
  *ambiguity = 0;
  int code;
  if ((code = setjmp (g->error_longjump_buff)) != 0) return code;
  if (g->undefined_p) error (g, GP_UNDEFINED_OR_BAD_GRAMMAR, "undefined or bad grammar");
  for (struct rule *rule = g->rules->first_rule; rule != NULL; rule = rule->next) rule->caller_anode = NULL;
  struct gp_tree_node *result;
  bool ok_p = parse (g, ambiguity, &result);
  if (ok_p) *root = result;
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 0) {
    if (ok_p && g->debug_level > 1) {
      fprintf (stderr, "Translation:\n");
      gp_print_translation (g, stderr, result);
    }
    fprintf (stderr, "%sGrammar: #terms = %d, #nonterms = %d, ", *ambiguity != 0 ? "AMBIGUOUS " : "",
             g->symbs->n_terms, g->symbs->n_nonterms);
    fprintf (stderr, "#rules = %d, rules size = %d\n", g->rules->n_rules,
             g->rules->n_rhs_lens + g->rules->n_rules);
    fprintf (stderr, "Input: #tokens = %llu, #all situations = %d\n", (unsigned long long) g->read_tokens,
             g->n_all_sits);
    fprintf (stderr, "       #terminal sets = %llu, their size = %llu\n",
             (unsigned long long) g->term_sets->n_term_sets,
             (unsigned long long) g->term_sets->n_term_sets_size);
    fprintf (stderr, "       #sets = %llu, #their start situations = %llu\n", (unsigned long long) g->n_sets,
             (unsigned long long) g->n_sets_start_sits);
    fprintf (stderr, "       #goto vectors = %llu, their length = %llu\n",
             (unsigned long long) g->n_goto_vects, (unsigned long long) g->n_goto_vect_len);
    fprintf (stderr, "       #actions = %llu, #action vectors = %llu, their length = %llu\n",
             (unsigned long long) g->n_actions, (unsigned long long) g->n_action_vects,
             (unsigned long long) g->n_action_vect_len);
    fprintf (stderr, "       max #stacks = %llu, max #stack els = %llu\n", (unsigned long long) g->n_stacks,
             (unsigned long long) g->n_peak_stack_els);
    fprintf (stderr, "       #single stack actions = %d, #multi stack actions = %d\n",
             g->n_single_stack_actions, g->n_multi_stack_actions);
    fprintf (stderr, "       #term nodes = %llu, #abstract nodes = %llu\n",
             (unsigned long long) g->n_parse_term_nodes, (unsigned long long) g->n_parse_abstract_nodes);
    fprintf (stderr, "       #alternative nodes = %llu, #option nodes = %llu, #all nodes = %llu\n",
             (unsigned long long) g->n_parse_alt_nodes, (unsigned long long) g->n_parse_opt_nodes,
             (unsigned long long) (g->n_parse_term_nodes + g->n_parse_abstract_nodes + g->n_parse_alt_nodes
                                   + g->n_parse_opt_nodes));
  }
#endif
#ifndef NO_GP_DEBUG_PRINT
  if (g->debug_level > 0) { /* do it after deleting hash tables */
    if (g->all_searches == 0) g->all_searches++;
    fprintf (stderr, "       #table collisions = %.2g%% (%d out of %d)\n",
             g->all_collisions * 100.0 / g->all_searches, g->all_collisions, g->all_searches);
  }
#endif
  return ok_p ? 0 : g->error_code;
}

struct traverse_el {                  /* element of stack used for traversing: */
  bool post_p;                        /* the element corresponds to node post-processing */
  struct gp_tree_node *node, *father; /* parse node and its father */
};

/* Push on the traverse stack traverse element with NODE, FATHER, and POST_P if necessary.  */
static FORCE_INLINE void consider_traverse (struct grammar *g, struct gp_tree_node *node,
                                            struct gp_tree_node *father, bool post_p) {
  if (!bitmap_set_bit_p (&g->marked_nodes, node->num)) return; /* already pushed */
  struct traverse_el el;
  el.node = node;
  el.father = father;
  if (post_p) {
    el.post_p = true;
    VLO_ADD_MEMORY (g->temp_vlo, &el, sizeof (el)); /* for postorder */
  }
  el.post_p = false;                              /* put it after the post element for the right traversal */
  VLO_ADD_MEMORY (g->temp_vlo, &el, sizeof (el)); /* for preorder */
}

/* API: parse tree traverse function. */
void gp_traverse_tree (struct grammar *g, struct gp_tree_node *root, gp_preorder_func_t preorder,
                       gp_postorder_func_t postorder, void *arg) {
  if (root == NULL) return;
  bool pre_p = preorder != NULL, post_p = postorder != NULL;
  if (!post_p && !pre_p) return;
  VLO_NULLIFY (g->temp_vlo);
  bitmap_clear (&g->marked_nodes);
  consider_traverse (g, root, NULL, post_p);
  while (VLO_LENGTH (g->temp_vlo) != 0) {
    struct traverse_el el = ((struct traverse_el *) VLO_BOUND (g->temp_vlo))[-1];
    VLO_SHORTEN (g->temp_vlo, sizeof (struct traverse_el));
    if (el.post_p) {
      postorder (el.node, el.father, arg);
      continue;
    }
    if (preorder != NULL && !preorder (el.node, el.father, arg)) continue;
    switch (el.node->type) {
    case GP_NIL:
    case GP_TERM: break;
    case GP_ANODE:
      for (int i = el.node->val.anode.children_num - 1; i >= 0; i--)
        consider_traverse (g, el.node->val.anode.children[i], el.node, post_p);
      break;
    case GP_ALT:
      consider_traverse (g, el.node->val.alt.second, el.node, post_p);
      consider_traverse (g, el.node->val.alt.first, el.node, post_p);
      break;
    case GP_OPT:
      consider_traverse (g, el.node->val.opt.second, el.node, post_p);
      consider_traverse (g, el.node->val.opt.first, el.node, post_p);
      break;
    default: assert (false);
    }
  }
}

static void grammar_init (struct grammar *g) { /* initialize grammar G: */
  g->undefined_p = true;
  g->error_code = 0;
  *g->error_message = '\0';
  g->parse_alloc = parse_alloc_default;
  g->parse_free = parse_free_default;
  g->syntax_error = syntax_error_default;
  g->debug_level = 0;
  g->recovery_token_matches = DEFAULT_RECOVERY_TOKEN_MATCHES;
  g->symbs = NULL;
  g->term_sets = NULL;
  g->rules = NULL;
  g->symbs = symb_init (g);
  g->term_sets = term_set_init (g);
  g->rules = rule_init (g);
  g->contexts_num = 0;
  g->node_merge = default_node_merge;
  VLO_CREATE (g->caller_anode_names, g->alloc, 0);
  VLO_CREATE (g->temp_vlo, g->alloc, 0);
  VLO_CREATE (g->all_nodes, g->alloc, 0);
  bitmap_create (&g->marked_nodes, g->alloc);
  sit_init (g);
  set_init (g);
  VLO_CREATE (g->symb_sits, g->alloc, 16);
  VLO_CREATE (g->actions_vlo, g->alloc, 16);
  VLO_CREATE (g->temp_nodes_vlo, g->alloc, 16);
  g->start_set = NULL;
  anode_name_code_tab_init (g);
  stack_init (g);
  token_buff_init (g);
  VLO_CREATE (g->curr_stacks, g->alloc, 2 * sizeof (vlo_t));
  VLO_CREATE (g->new_stacks, g->alloc, 2 * sizeof (vlo_t));
  VLO_CREATE (g->failed_stacks, g->alloc, 0);
  stack_tab_init (g);
}

struct grammar *gp_create_grammar (void) { /* API: Allocate memory for new grammar. */
  gp_allocator_t *allocator;

  allocator = gp_alloc_new (NULL, NULL, NULL, NULL);
  if (allocator == NULL) {
    return NULL;
  }
  struct grammar *g = (struct grammar *) gp_malloc (allocator, sizeof (struct grammar));
  if (g == NULL) {
    gp_alloc_del (allocator);
    return NULL;
  }
  g->alloc = allocator;
  gp_alloc_seterr (allocator, error_func_for_allocate, g);
  if (setjmp (g->error_longjump_buff) != 0) {
    gp_free (allocator, g);
    gp_alloc_del (allocator);
    return NULL;
  }
  grammar_init (g);
  return g;
}

static void grammar_finish (struct grammar *g) { /* Free memory allocated for the grammar data */
  bitmap_destroy (&g->marked_nodes);
  size_t nodes_num = VLO_LENGTH (g->all_nodes) / sizeof (struct gp_tree_node *);
  if (g->parse_free != NULL) {
    for (size_t i = 0; i < nodes_num; i++) { /* nodes existing in case of an error */
      struct gp_tree_node *node = ((struct gp_tree_node **) VLO_BEGIN (g->all_nodes))[i];
      if (node->type == GP_ANODE) parse_free (g, node->val.anode.children);
      parse_free (g, node);
    }
  }
  VLO_DELETE (g->all_nodes);
  VLO_DELETE (g->temp_vlo);
  if (g->parse_free != NULL) {
    for (size_t i = 0; i < VLO_LENGTH (g->caller_anode_names) / sizeof (char *); i++) {
      char *name = ((char **) VLO_BEGIN (g->caller_anode_names))[i];
      parse_free (g, name);
    }
  }
  VLO_DELETE (g->caller_anode_names);
  rule_fin (g, g->rules);
  term_set_fin (g, g->term_sets);
  symb_fin (g, g->symbs);
  set_fin (g);
  sit_fin (g);
  anode_name_code_tab_fin (g);
  stack_tab_fin (g);
  VLO_DELETE (g->symb_sits);
  VLO_DELETE (g->actions_vlo);
  VLO_DELETE (g->temp_nodes_vlo);
  VLO_DELETE (g->failed_stacks);
  stack_vlo_free (g, &g->curr_stacks);
  VLO_DELETE (g->new_stacks);
  VLO_DELETE (g->curr_stacks);
  stack_finish (g);
  token_buff_finish (g);
}

void gp_fin (struct grammar *g) { /* API: free memory allocated for the grammar. */
  if (g != NULL) {
    grammar_finish (g);
    gp_allocator_t *allocator = g->alloc;
    gp_free (allocator, g);
    gp_alloc_del (allocator);
  }
}

static void empty_grammar (struct grammar *g) { /* make grammar G empty: */
  grammar_finish (g);
  grammar_init (g);
}

/* Parse tree traverse pre function used to collect all nodes of the parse tree. */
static bool collect_nodes (struct gp_tree_node *node, struct gp_tree_node *father GP_UNUSED, void *arg) {
  struct grammar *g = arg;
  VLO_ADD_MEMORY (g->temp_nodes_vlo, &node, sizeof (node));
  return true;
}

void gp_free_tree (struct grammar *g, struct gp_tree_node *root) { /* API: free parse tree */
  if (g->parse_free == NULL) return;
  VLO_NULLIFY (g->temp_nodes_vlo);
  gp_traverse_tree (g, root, collect_nodes, NULL, g);
  while (VLO_LENGTH (g->temp_nodes_vlo) != 0) {
    struct gp_tree_node *node = ((struct gp_tree_node **) VLO_BOUND (g->temp_nodes_vlo))[-1];
    VLO_SHORTEN (g->temp_nodes_vlo, sizeof (struct gp_tree_node *));
    if (node->type == GP_ANODE) parse_free (g, node->val.anode.children);
    parse_free (g, node);
  }
}

/* This page contains a test code for Gecko. To use it, define macro GP_TEST during compilation. */

#ifdef GP_TEST

static int nterm; /* the current number of next input grammar terminal */

/* The function imported by Gecko (see comments in the interface file). */
const char *read_terminal (int *code, int *priority, enum gp_assoc *assoc) {
  nterm++;
  *priority = -1;
  *assoc = GP_NON_ASSOC;
  switch (nterm) {
  case 1: *code = 'a'; return "a";
  case 2: *code = '+'; return "+";
  case 3: *code = '*'; return "*";
  case 4: *code = '('; return "(";
  case 5: *code = ')'; return ")";
  default: return NULL;
  }
}

static int nrule; /* the current number of next rule grammar terminal */

/* The function imported by Gecko (see comments in the interface file). */
const char *read_rule (const char ***rhs, const char **anode, int **transl, int *guard_num) {
  static const char *rhs_1[] = {"T", NULL};
  static int tr_1[] = {0, -1};
  static const char *rhs_2[] = {"E", "+", "T", NULL};
  static int tr_2[] = {0, 2, -1};
  static const char *rhs_3[] = {"F", NULL};
  static int tr_3[] = {0, -1};
  static const char *rhs_4[] = {"T", "*", "F", NULL};
  static int tr_4[] = {0, 2, -1};
  static const char *rhs_5[] = {"a", NULL};
  static int tr_5[] = {0, -1};
  static const char *rhs_6[] = {"(", "E", ")", NULL};
  static int tr_6[] = {1, -1};

  *guard_num = -1;
  nrule++;
  switch (nrule) {
  case 1:
    *rhs = rhs_1;
    *anode = NULL;
    *transl = tr_1;
    return "E";
  case 2:
    *rhs = rhs_2;
    *anode = "plus";
    *transl = tr_2;
    return "E";
  case 3:
    *rhs = rhs_3;
    *anode = NULL;
    *transl = tr_3;
    return "T";
  case 4:
    *rhs = rhs_4;
    *anode = "mult";
    *transl = tr_4;
    return "T";
  case 5:
    *rhs = rhs_5;
    *anode = NULL;
    *transl = tr_5;
    return "F";
  case 6:
    *rhs = rhs_6;
    *anode = NULL;
    *transl = tr_6;
    return "F";
  default: return NULL;
  }
}

static int ntok; /* the current number of next input token */

/* The function imported by Gecko (see comments in the interface file). */
static int test_read_token (void **attr) {
  const char input[] = "a+a*(a*a+a)";
  ntok++;
  *attr = NULL;
  if ((size_t) ntok < sizeof (input)) return input[ntok - 1];
  return -1;
}

/* The following two functions calls Gecko with two different ways of forming grammars. */
static void use_functions (int argc, char **argv) {
  struct grammar *g;
  struct gp_tree_node *root;
  int ambiguity;

  nterm = nrule = 0;
  fprintf (stderr, "Use functions\n");
  if ((g = gp_create_grammar ()) == NULL) exit (1);
  gp_set_debug_level (g, argc > 2 ? atoi (argv[2]) : 3);
  if (gp_read_grammar (g, true, read_terminal, read_rule) != 0) exit (1);
  ntok = 0;
  if (gp_parse (g, test_read_token, &root, &ambiguity, NULL) != 0) exit (1);
  gp_free_tree (g, root);
  gp_fin (g);
}

/* The function imported by Gecko (see comments in the interface file). */
static int test_read_wrong_token (void **attr) {
  const char input[] = "b";
  ntok++;
  *attr = NULL;
  if ((size_t) ntok < sizeof (input)) return input[ntok - 1];
  return -1;
}

#define ANODES X (plus) X (mult)

#define X(a) " " #a
static const char *description /* the test grammar */
  = "\n"
    "TERM;\n"
    "ANODE" ANODES
    ";\n"
    "E : T         # 0\n"
    "  | E '+' T   # plus (0 2)\n"
    "  ;\n"
    "T : F         # 0\n"
    "  | T '*' F   # mult (0 2)\n"
    "  ;\n"
    "F : 'a'       # 0\n"
    "  | '(' E ')' # 1\n"
    "  ;\n";

#undef X

#define X(a) AN_##a,
enum anode_code { ANODES };

/* Run test for the above grammar */
static void use_description (int argc, char **argv, int (*read_fn) (void **)) {
  struct grammar *g;
  struct gp_tree_node *root1, *root2;
  int ambiguity;

  fprintf (stderr, "Use description\n");
  if ((g = gp_create_grammar ()) == NULL) exit (1);
  gp_set_debug_level (g, argc > 2 ? atoi (argv[2]) : 3);
  if (gp_parse_grammar (g, true, description) != 0) exit (1);
  if (gp_parse_grammar (g, true, description) != 0) exit (1);
  ntok = 0;
  if (gp_parse (g, read_fn, &root1, &ambiguity, NULL) == 0) {
    ntok = 0;
    gp_set_debug_level (g, 0);
    if (gp_parse (g, read_fn, &root2, &ambiguity, NULL) != 0) exit (1);
    gp_free_tree (g, root2);
    gp_free_tree (g, root1);
  }
  gp_fin (g);
}

static int test_read_ambig (void **attr) { /* function to read token for ambiguous grammar test: */
  const char input[] = "a+a+a+a";
  ntok++;
  *attr = NULL;  // (void *) (ptrdiff_t) ntok;
  if ((size_t) ntok < sizeof (input)) return input[ntok - 1];
  return -1;
}

static const char *ambig /* ambiguous grammar */
  = "\n"
    "TERM;\n"
    "E : E '+' E   # plus (0 2)\n"
    "  | 'a'       # 0\n"
    "  ;\n";

static void *node_merge (struct grammar *g, struct gp_tree_node *node1, struct gp_tree_node *node2,
                         size_t context_num) { /* node merge for ambiguous grammar: */
  if (context_num == 0) return gp_get_alt_node (g, node2, node1);
  return gp_get_opt_node (g, node2, node1, context_num);
}

static void use_ambig (int argc, char **argv) { /* run test using the ambiguous grammar: */
  struct grammar *g;
  struct gp_tree_node *root;
  int ambiguity;

  fprintf (stderr, "Use ambig grammar\n");
  if ((g = gp_create_grammar ()) == NULL) exit (1);
  gp_set_debug_level (g, argc > 2 ? atoi (argv[2]) : 3);
  if (gp_parse_grammar (g, true, ambig) != 0) exit (1);
  ntok = 0;
  gp_set_node_merge_func (g, node_merge);
  if (gp_parse (g, test_read_ambig, &root, &ambiguity, NULL) != 0) exit (1);
  gp_free_tree (g, root);
  gp_fin (g);
}

int main (int argc, char **argv) { /* test driver: */
  if (argc <= 1 || atoi (argv[1]) == 1)
    use_description (argc, argv, test_read_token);
  else if (atoi (argv[1]) == 0)
    use_functions (argc, argv);
  else if (atoi (argv[1]) == 2)
    use_description (argc, argv, test_read_wrong_token);
  else if (atoi (argv[1]) == 3)
    use_ambig (argc, argv);
  exit (0);
}

#endif /* #ifdef GP_TEST */
