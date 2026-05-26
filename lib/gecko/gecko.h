/* This file is a part of Gecko Parser (GLR parser) project.
   Copyright (C) 2026 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* This is the interface file of a general (working on any CFG) syntax parser with minimal error recovery
   and syntax-directed translation. The algorithm is described in README.md file.  The
   algorithm is sufficiently fast to be used in serious language processors. */

#ifndef __GECKO__
#define __GECKO__

#include <stdbool.h>
#include <limits.h>

#ifdef __GNUC__
#define GP_UNUSED __attribute__ ((unused))
#else
#define GP_UNUSED
#endif

/* The following is a forward declaration of grammar formed by function gp_read_grammar. */
struct grammar;

/* The following value is reserved as a designation of an empty node for translation.  It should be
   a positive number that does not intersect with symbol numbers. */
#define GP_NIL_TRANSLATION_NUMBER INT_MAX

/* The following values are Gecko Parser error codes: */
#define GP_NO_MEMORY 1
#define GP_UNDEFINED_OR_BAD_GRAMMAR 2
#define GP_WRONG_ARG 3
#define GP_DESCRIPTION_SYNTAX_ERROR_CODE 4
#define GP_FIXED_NAME_USAGE 5
#define GP_REPEATED_TERM_DECL 6
#define GP_NEGATIVE_TERM_CODE 7
#define GP_TOO_WIDE_TERM_RANGE_CODE 8
#define GP_REPEATED_TERM_CODE 9
#define GP_REPEATED_TERM_ASSOC 10
#define GP_UNDEFINED_TERM_ASSOC 11
#define GP_WRONG_TERM_ASSOC 12
#define GP_REPEATED_ANODE_CODE 13
#define GP_NO_RULES 14
#define GP_TERM_IN_RULE_LHS 15
#define GP_INCORRECT_TRANSLATION 16
#define GP_INCORRECT_SYMBOL_NUMBER 17
#define GP_REPEATED_SYMBOL_NUMBER 18
#define GP_UNACCESSIBLE_NONTERM 19
#define GP_NONTERM_DERIVATION 20
#define GP_LOOP_NONTERM 21
#define GP_INVALID_TOKEN_CODE 22
#define GP_BAD_RULE_GUARDS 23

enum gp_assoc { GP_NON_ASSOC, GP_LEFT_ASSOC, GP_RIGHT_ASSOC };

enum gp_tree_node_type { /* the parse tree node: */
                         GP_NIL,
                         GP_TERM,
                         GP_ANODE,
                         GP_ALT, /* alternative */
                         GP_OPT, /* context-dependent alternative */
};

struct gp_nil { /* The node exists in one instance. See comment to read_rule. */
};

struct gp_term { /* the terminal node: */
  int code;      /* the terminal code */
  void *attr;    /* the terminal attributes */
};

struct gp_anode {   /* the abstract node: */
  int children_num; /* elements in the next array */
  const char *name; /* the abstract node name */
  /* References to nodes to which the abstract node refers.  The array end marker is NULL. */
  struct gp_tree_node **children;
};

struct gp_alt { /* alternative translations: */
  struct gp_tree_node *first, *second;
};

struct gp_opt {       /* context-dependent alternative translations: */
  size_t context_num; /* the option context number */
  struct gp_tree_node *first, *second;
};

struct gp_tree_node {          /* the generalized node of the parse tree: */
  enum gp_tree_node_type type; /* the type of node */
  int aux;                     /* for anode it is number of anode name */
  size_t num;                  /* node number */
  union {                      /* the node itself */
    struct gp_nil nil;
    struct gp_term term;
    struct gp_anode anode;
    struct gp_alt alt; /* alternative */
    struct gp_opt opt; /* context-dependent alternative */
  } val;
};

/* Create undefined grammar.  The function returns NULL if there is no memory.
   This function should be called first. */
extern struct grammar *gp_create_grammar (void);

/* Return the last occurred error code for given grammar. */
extern int gp_error_code (struct grammar *g);

/* Return the error message corresponding to the last occurred error code. */
extern const char *gp_error_message (struct grammar *g);

/* Read terminals/rules into grammar G, check it depending on STRICT_P, and build SLR sets.
   Returns zero if it is all ok.  Otherwise, returns the error code (also available
   via gp_error_code and gp_error_message).

   READ_TERMINAL is a function for reading terminals.  This function is called before function read_rule.
   The function should return the name and the code of the next terminal.  If all terminals have been
   read the function returns NULL.  The return code should be nonnegative.  Priority and associativity
   of the terminal are passed through the rest of args.  They are used to resolve conflicts in LR-sets
   as for YACC.  If you don't want to resolve the conflicts, use GP_NON_ASSOC.

   READ_RULE is a function called to read the next rule.  This function is called after function
   read_terminal.  The function should return the name of the LHS nonterminal and array of names of
   symbols in RHS of the rule (the array end marker should be NULL).  If all rules have been read the
   function returns NULL.  All symbols with names not provided by the previous function are considered
   to be nonterminal.  The function also returns a translation given by an abstract node name and its
   fields which will be translations of symbols (with indexes given in array *TRANSL) in the RHS of
   the rule.  All indexes in TRANSL should be different (so the translation of a symbol cannot be
   represented twice).  The end marker of the array should be a negative value.  There is a reserved
   value of the translation symbol number denoting an empty node.  It is the value defined by macro
   GP_NIL_TRANSLATION_NUMBER.  If *TRANSL is NULL or contains only the end marker, translations of
   the rule will be a nil node.  If ABS_NODE is NULL, an abstract node is not created.  In this case
   *TRANSL should be NULL or contain at most one element which means that the translation of the
   rule will be a nil node or the translation of the symbol in RHS given by the single array element.
   The function returns the rule guard number through GUARD_NUM (see comments for gp_set_rule_guard). */
extern int gp_read_grammar (struct grammar *g, bool strict_p,
                            const char *(*read_terminal) (int *code, int *priority, enum gp_assoc *assoc),
                            const char *(*read_rule) (const char ***rhs, const char **abs_node, int **transl,
                                                      int *guard_num));

/* Set code (see aux node of gp_tree_node) for anode with NAME.  By default the code is -1.  */
extern void gp_set_anode_code (struct grammar *g, const char *name, int code);

/* Analogous to the previous one but it parses grammar description. */
extern int gp_parse_grammar (struct grammar *g, bool strict_p, const char *description);

/* The following functions set up different parameters which affect parser work.  The functions
   return the previous parameter value.

   * parse_alloc is used to allocate memory for parse tree representation.  By default it is malloc.
     It should always be non-NULL.

   * parse_free is used to free memory for parse tree representation.  By default it is free.
     NULL value means no freeing.  It is not safe to change parse_alloc and parse_free functions after
     gp_parse and before gp_fin as some data allocated by parse_alloc in gp_parse can be freed in gp_fin
     and reading a new grammar.

   * syntax error function is used to print an error message about a syntax error on a token with
     representation ERR_TOK_REPR and attribute ERR_TOK_ATTR (see type gp_syntax_error_func_t).
     If AFTER_P is true then ERR_NONTERM_REPR describes grammar construction after which the error occur.
     In this case you can print syntax error as "error after ERR_NONTERM_REPR".  Otherwise, ERR_NONTERM_REPR
     is the nonterminal minimally covering the error position. In this case you can print syntax error as
     "error in ERR_NONTERM_REPR". The next two parameters describe the recovery stop token.  The default
     function prints the error nonterminal and token representations.  You should set up a new function to
     print positions which can be passed through the token attributes and you can translate ERR_NONTERM_REPR
     into a more readable name, e.g. "stmt" used in a grammar into "statement".

   * non-NULL rule guard function is used to reject some rules or do some other actions.  It is called for
     each reduction of a rule having a guard number.  The function gets the guard number and argument passed
     to gp_parse.  If the function returns false, the rule reduction is rejected.

   * debug_level says what debugging information to output (it works only if compiled without
     defining macro NO_GP_DEBUG_PRINT):

     * 0 (default value) means print nothing
     * 1 results in printing statistics
     * 2 results in additionally printing the result translation
     * 3 results in printing read tokens, actions (conflicts marked by '!') for dynamically generated
       SLR sets, and high-level error recovery info
     * 4 means printing rules, first/follow nonterminal sets, dynamically generated SLR sets
     * 5 results in printing stacks during parsing and stack merging
     * 6 results in even more detailed info about processing stacks during parsing

   * recovery_match means how many subsequent tokens should be successfully shifted to finish error
     recovery.  The default value is 3.

   * gp_set_node_merge_func is used during stack merging.  It gets two parse tree nodes of a symbol
     (terminal or nonterminal) from stacks being merged and returns the result node.  NULL FUNC
     sets up the default function which always returns the first node.  The default function is set
     up in GP_CREATE_GRAMMAR.  Using the default function results in returning only one translation
     by GP_PARSE. */

typedef void *(*gp_parse_alloc_func_t) (size_t);
typedef void (*gp_parse_free_func_t) (void *);
extern gp_parse_alloc_func_t gp_set_parse_alloc (struct grammar *g, gp_parse_alloc_func_t fn);
extern gp_parse_free_func_t gp_set_parse_free (struct grammar *g, gp_parse_free_func_t fn);

/* The syntax error reporting function type. */
typedef void (*gp_syntax_error_func_t) (const char *err_nonterm_repr, bool after_p, const char *err_tok_repr,
                                        void *err_tok_attr, const char *stop_tok_repr, void *stop_tok_attr);
extern gp_syntax_error_func_t gp_set_syntax_error (struct grammar *g, gp_syntax_error_func_t fn);

typedef bool (*gp_rule_guard_func_t) (int num, void *arg);
extern gp_rule_guard_func_t gp_set_rule_guard (struct grammar *g, gp_rule_guard_func_t fn);

extern int gp_set_debug_level (struct grammar *grammar, int level);
extern int gp_set_recovery_match (struct grammar *grammar, int n_toks);

typedef void *(*gp_node_merge_func_t) (struct grammar *grammar, struct gp_tree_node *node1,
                                       struct gp_tree_node *node2, size_t context_num);
extern gp_node_merge_func_t gp_set_node_merge_func (struct grammar *grammar, gp_node_merge_func_t func);

/* Parse input according to the read grammar.  Returns the error code (which is also available via
   gp_error_code).  If the code is zero, also puts the parse result into *root (it will never be NULL).
   Sets *AMBIGUITY to 1 if the grammar is ambiguous on the input.

   *AMBIGUITY is set to 2 if the final stack is produced by merging stacks and two or more
   terminal attributes/abstract nodes were different.

   Consider merging two stacks with two different translations for two stack elements:
   `...a...b...` and `...c...d...`.  We could use the result stack `...alt(a,c)...alt(b,d)...`
   but the alternative semantics means 4 possible choices for the translation: ac, ad, bc, bd instead
   of the two correct ones: ac, bd.  Therefore for such a case we should use option (or context-dependent
   alternative) nodes for the merged stack: `...opt(a,c)...opt(b,d)...`.  To generate the alternative with
   the right semantics (context-dependent or -independent), the merge function has an argument `context_num`
   which defines the corresponding context.  For a context-independent alternative, this number will be
   zero.  To correctly traverse the parse tree, you should always choose the same alternative (`first`
   or `second`) for options with the same `context_num`.  By correctly traversing the tree you can get rid
   of option nodes by transforming the parse tree to contain only alternative nodes.  Fortunately, options
   in the parse tree are impossible for grammars of most programming languages.

   The function READ_TOKEN provides input tokens.  It returns the code of the next input token and its
   attribute.  If the function returns a negative value, all tokens have been read.

   ARG is passed to the rule guard function. */
extern int gp_parse (struct grammar *grammar, int (*read_token) (void **attr), struct gp_tree_node **root,
                     int *ambiguity, void *arg);

/* Return GP_ALT node with given FIRST and SECOND.  Use it in the node merge function if you want to keep all
   possible alternatives during stack merging. */
extern struct gp_tree_node *gp_get_alt_node (struct grammar *g, struct gp_tree_node *first,
                                             struct gp_tree_node *second);
/* Analogous to the previous function but for GP_OPT node. */
extern struct gp_tree_node *gp_get_opt_node (struct grammar *g, struct gp_tree_node *first,
                                             struct gp_tree_node *second, size_t context_num);

#ifndef NO_GP_DEBUG_PRINT
/* Print translation of ROOT parsed for GRAMMAR. */
extern void gp_print_translation (struct grammar *grammar, FILE *f, struct gp_tree_node *root);
#endif

typedef bool (*gp_preorder_func_t) (struct gp_tree_node *node, struct gp_tree_node *father, void *arg);
typedef void (*gp_postorder_func_t) (struct gp_tree_node *node, struct gp_tree_node *father, void *arg);

/* Traverse the parse tree given by ROOT and call functions (if they are non-NULL) PREORDER and POSTORDER
   respectively before and after processing all children.  Don't process children of node if PREORDER for
   the node returns false.  ARG will be passed to PREORDER and POSTORDER.  */
extern void gp_traverse_tree (struct grammar *grammar, struct gp_tree_node *root, gp_preorder_func_t preorder,
                              gp_postorder_func_t postorder, void *arg);

/* Free memory allocated for the parse tree. ROOT must be the root of the parse tree as returned by
   gp_parse(). If ROOT is a null pointer, no operation is performed. */
extern void gp_free_tree (struct grammar *grammar, struct gp_tree_node *root);

/* Finish work with the grammar.  It should be called last. */
extern void gp_fin (struct grammar *grammar);

#endif /* #ifndef __GECKO__ */
