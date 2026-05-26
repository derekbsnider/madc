// madc_grammar.cpp — Gecko GLR grammar for the madc language
//
// This file defines the complete madc grammar as a Gecko grammar string
// and provides the tokenizer-to-Gecko mapping function.
//
// The grammar produces an AST with named abstract nodes.  Semantic
// analysis happens in a separate walk (Phase 2).

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <queue>
#include <stack>
#include <stdint.h>
#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"

extern "C" {
#include "gecko.h"
}

// -----------------------------------------------------------------------
// Gecko terminal codes for madc tokens
//
// Single-char tokens use their ASCII code directly in the grammar
// (e.g., '+' is 43).  Multi-char operators and keywords get explicit
// codes >= 256 so they don't collide with any printable character.
// -----------------------------------------------------------------------

enum GeckoTermCode {
    // Identifiers and literals
    GT_IDENT      = 256,
    GT_INTEGER    = 257,   // integer literal
    GT_REAL       = 258,   // floating point literal
    GT_STRING     = 259,   // string literal "..."
    GT_CHAR_LIT   = 260,   // char literal '.'

    // Keywords
    GT_IF         = 261,
    GT_ELSE       = 262,
    GT_WHILE      = 263,
    GT_FOR        = 264,
    GT_DO         = 265,
    GT_RETURN     = 266,
    GT_BREAK      = 267,
    GT_CONTINUE   = 268,
    GT_SWITCH     = 269,
    GT_CASE       = 270,
    GT_DEFAULT    = 271,
    GT_GOTO       = 272,
    GT_STRUCT     = 273,
    GT_CLASS      = 274,
    GT_ENUM       = 275,
    GT_UNION      = 276,
    GT_TYPEDEF    = 277,
    GT_SIZEOF     = 278,
    GT_STATIC     = 279,
    GT_EXTERN     = 280,
    GT_CONST      = 281,
    GT_VOLATILE   = 282,
    GT_REGISTER   = 283,
    GT_INLINE     = 284,
    GT_SIGNED     = 285,
    GT_UNSIGNED   = 286,
    GT_AUTO       = 287,
    GT_TRY        = 288,
    GT_CATCH      = 289,
    GT_THROW      = 290,
    GT_NEW        = 291,
    GT_DELETE     = 292,
    GT_VIRTUAL    = 293,
    GT_PUBLIC     = 294,
    GT_PRIVATE    = 295,
    GT_PROTECTED  = 296,
    GT_USING      = 297,
    GT_NAMESPACE  = 298,
    GT_MATCH      = 299,   // rust-style match
    GT_DEFER      = 300,
    GT_PREFER     = 301,
    GT_RESTRICT   = 302,
    GT_OPERATOR   = 303,   // operator keyword (overloading)
    GT_ALIGNOF    = 305,

    // Type keywords
    GT_VOID       = 310,
    GT_BOOL       = 311,
    GT_CHAR       = 312,
    GT_SHORT      = 313,
    GT_INT        = 314,
    GT_LONG       = 315,
    GT_FLOAT      = 316,
    GT_DOUBLE     = 317,
    GT_STRING_T   = 318,   // "string" keyword (C++ std::string)
    GT_INT8       = 319,
    GT_INT16      = 320,
    GT_INT32      = 321,
    GT_INT64      = 322,
    GT_UINT8      = 323,
    GT_UINT16     = 324,
    GT_UINT32     = 325,
    GT_UINT64     = 326,

    // Container type keywords
    GT_VECTOR     = 330,
    GT_MAP        = 331,
    GT_SET        = 332,
    GT_LIST       = 333,

    // Stream type keywords
    GT_IFSTREAM   = 340,
    GT_OFSTREAM   = 341,
    GT_FSTREAM    = 342,
    GT_SSTREAM    = 343,   // stringstream
    GT_OSTREAM    = 344,

    // Multi-char operators
    GT_EQ         = 350,   // ==
    GT_NE         = 351,   // !=
    GT_LE         = 352,   // <=
    GT_GE         = 353,   // >=
    GT_AND        = 354,   // &&
    GT_OR         = 355,   // ||
    GT_INC        = 356,   // ++
    GT_DEC        = 357,   // --
    GT_ARROW      = 358,   // ->
    GT_SCOPE      = 359,   // ::
    GT_BSL        = 360,   // <<
    GT_BSR        = 361,   // >>
    GT_ADD_ASSIGN = 362,   // +=
    GT_SUB_ASSIGN = 363,   // -=
    GT_MUL_ASSIGN = 364,   // *=
    GT_DIV_ASSIGN = 365,   // /=
    GT_MOD_ASSIGN = 366,   // %=
    GT_BSL_ASSIGN = 367,   // <<=
    GT_BSR_ASSIGN = 368,   // >>=
    GT_BAND_ASSIGN= 369,   // &=
    GT_BOR_ASSIGN = 370,   // |=
    GT_XOR_ASSIGN = 371,   // ^=
    GT_ELLIPSIS   = 372,   // ...
    GT_THREE_WAY  = 373,   // <=>
    GT_FAT_ARROW  = 374,   // =>
    GT_COL_ASSIGN = 375,   // :=
    GT_3EQ        = 376,   // ===
};

// -----------------------------------------------------------------------
// Grammar string
//
// Convention:
//   # name (child_indices)     — create an abstract node
//   # N                        — pass-through the Nth child (0-based)
//   Empty alternative          — epsilon production (no # annotation)
//
// Operator precedence is declared via LEFT/RIGHT/NONASSOC directives
// at the top.  Gecko resolves shift/reduce conflicts using those
// declarations, just like yacc/bison.
// -----------------------------------------------------------------------

static const char *madc_grammar_str =
    // ================================================================
    // Terminal declarations
    // ================================================================
    "TERM\n"

    // Identifiers and literals
    "IDENT = 256\n"
    "INTEGER = 257\n"
    "REAL = 258\n"
    "STRING_LIT = 259\n"
    "CHAR_LIT = 260\n"

    // Keywords
    "IF = 261\n"
    "ELSE = 262\n"
    "WHILE = 263\n"
    "FOR = 264\n"
    "DO = 265\n"
    "RETURN = 266\n"
    "BREAK = 267\n"
    "CONTINUE = 268\n"
    "SWITCH = 269\n"
    "CASE = 270\n"
    "DEFAULT = 271\n"
    "GOTO = 272\n"
    "STRUCT = 273\n"
    "CLASS = 274\n"
    "ENUM = 275\n"
    "UNION = 276\n"
    "TYPEDEF = 277\n"
    "SIZEOF = 278\n"
    "STATIC = 279\n"
    "EXTERN = 280\n"
    "CONST = 281\n"
    "VOLATILE = 282\n"
    "REGISTER = 283\n"
    "INLINE = 284\n"
    "SIGNED = 285\n"
    "UNSIGNED = 286\n"
    "AUTO = 287\n"
    "TRY = 288\n"
    "CATCH = 289\n"
    "THROW = 290\n"
    "NEW = 291\n"
    "DELETE = 292\n"
    "VIRTUAL = 293\n"
    "PUBLIC = 294\n"
    "PRIVATE = 295\n"
    "PROTECTED = 296\n"
    "USING = 297\n"
    "NAMESPACE = 298\n"
    "MATCH = 299\n"
    "DEFER = 300\n"
    "PREFER = 301\n"
    "RESTRICT = 302\n"
    "OPERATOR = 303\n"
    "ALIGNOF = 305\n"

    // Type keywords
    "VOID = 310\n"
    "BOOL = 311\n"
    "CHAR = 312\n"
    "SHORT = 313\n"
    "INT = 314\n"
    "LONG = 315\n"
    "FLOAT = 316\n"
    "DOUBLE = 317\n"
    "STRING_T = 318\n"
    "INT8 = 319\n"
    "INT16 = 320\n"
    "INT32 = 321\n"
    "INT64 = 322\n"
    "UINT8 = 323\n"
    "UINT16 = 324\n"
    "UINT32 = 325\n"
    "UINT64 = 326\n"

    // Container type keywords
    "VECTOR = 330\n"
    "MAP = 331\n"
    "SET = 332\n"
    "LIST = 333\n"

    // Stream type keywords
    "IFSTREAM = 340\n"
    "OFSTREAM = 341\n"
    "FSTREAM = 342\n"
    "SSTREAM = 343\n"
    "OSTREAM = 344\n"

    // Multi-char operators
    "EQ_OP = 350\n"
    "NE_OP = 351\n"
    "LE_OP = 352\n"
    "GE_OP = 353\n"
    "AND_OP = 354\n"
    "OR_OP = 355\n"
    "INC_OP = 356\n"
    "DEC_OP = 357\n"
    "ARROW = 358\n"
    "SCOPE = 359\n"
    "BSL_OP = 360\n"
    "BSR_OP = 361\n"
    "ADD_ASSIGN = 362\n"
    "SUB_ASSIGN = 363\n"
    "MUL_ASSIGN = 364\n"
    "DIV_ASSIGN = 365\n"
    "MOD_ASSIGN = 366\n"
    "BSL_ASSIGN = 367\n"
    "BSR_ASSIGN = 368\n"
    "BAND_ASSIGN = 369\n"
    "BOR_ASSIGN = 370\n"
    "XOR_ASSIGN = 371\n"
    "ELLIPSIS = 372\n"  // lexer emits 3 dots; mapper synthesizes ELLIPSIS
    "THREE_WAY = 373\n"
    "FAT_ARROW = 374\n"
    "COL_ASSIGN = 375\n"
    "EQ3_OP = 376;\n"
    "\n"

    // ================================================================
    // Operator precedence (lowest to highest)
    //
    // C operator precedence table, adapted for Gecko.  The comma
    // operator is at the very bottom; unary operators are handled
    // structurally via the grammar (not by precedence declarations).
    // ================================================================
    "RIGHT '=' ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN"
    " BSL_ASSIGN BSR_ASSIGN BAND_ASSIGN BOR_ASSIGN XOR_ASSIGN COL_ASSIGN\n"
    "RIGHT '?' ':'\n"
    "LEFT OR_OP\n"
    "LEFT AND_OP\n"
    "LEFT '|'\n"
    "LEFT '^'\n"
    "LEFT '&'\n"
    "LEFT EQ_OP NE_OP EQ3_OP\n"
    "LEFT '<' '>' LE_OP GE_OP THREE_WAY\n"
    "LEFT BSL_OP BSR_OP\n"
    "LEFT '+' '-'\n"
    "LEFT '*' '/' '%'\n"
    "RIGHT ELSE\n"
    "\n"

    // ================================================================
    // Top-level program
    // ================================================================
    "program : translation_unit                     # 0\n"
    "        ;\n"
    "translation_unit : top_level                   # 0\n"
    "                 | translation_unit top_level  # tu (0 1)\n"
    "                 ;\n"
    "top_level : function_def                       # 0\n"
    "          | declaration ';'                    # 0\n"
    "          | class_def                          # 0\n"
    "          | struct_def                         # 0\n"
    "          | union_def                          # 0\n"
    "          | enum_def                           # 0\n"
    "          | typedef_decl                       # 0\n"
    "          | using_decl                         # 0\n"
    "          | namespace_def                      # 0\n"
    "          | ';'                                \n"
    "          ;\n"
    "\n"

    // ================================================================
    // Type specifiers
    //
    // The typedef/identifier ambiguity (user-defined type names are
    // IDENT tokens) is accepted here — both IDENT and primitive type
    // keywords produce type_spec.  A rule guard can be used in Phase 2
    // to disambiguate when an IDENT is a typename vs. a variable name.
    // ================================================================
    "type_spec : VOID                               # 0\n"
    "          | BOOL                               # 0\n"
    "          | CHAR                               # 0\n"
    "          | SHORT                              # 0\n"
    "          | INT                                # 0\n"
    "          | LONG                               # 0\n"
    "          | FLOAT                              # 0\n"
    "          | DOUBLE                             # 0\n"
    "          | SIGNED                             # 0\n"
    "          | UNSIGNED                           # 0\n"
    "          | STRING_T                           # 0\n"
    "          | AUTO                               # 0\n"
    "          | INT8                               # 0\n"
    "          | INT16                              # 0\n"
    "          | INT32                              # 0\n"
    "          | INT64                              # 0\n"
    "          | UINT8                              # 0\n"
    "          | UINT16                             # 0\n"
    "          | UINT32                             # 0\n"
    "          | UINT64                             # 0\n"
    "          | STRUCT IDENT                       # struct_type (1)\n"
    "          | UNION IDENT                        # union_type (1)\n"
    "          | ENUM IDENT                         # enum_type (1)\n"
    "          | CLASS IDENT                        # class_type (1)\n"
    "          | IDENT                              # 0\n"
    "          ;\n"
    "\n"

    // Compound type specifiers (multi-keyword like "unsigned int",
    // "long long", "signed char", etc.)
    "type_prefix : UNSIGNED                         # 0\n"
    "            | SIGNED                           # 0\n"
    "            | LONG                             # 0\n"
    "            | SHORT                            # 0\n"
    "            | CONST                            # 0\n"
    "            | VOLATILE                         # 0\n"
    "            | STATIC                           # 0\n"
    "            | EXTERN                           # 0\n"
    "            | REGISTER                         # 0\n"
    "            | INLINE                           # 0\n"
    "            | RESTRICT                         # 0\n"
    "            ;\n"
    "\n"

    // Full type: optional prefixes + base type + optional pointer/ref
    "qualified_type : type_spec                     # 0\n"
    "               | type_prefix qualified_type    # qual_type (0 1)\n"
    "               ;\n"
    "type : qualified_type                          # 0\n"
    "     | type '*'                                # ptr_type (0)\n"
    "     | type '&'                                # ref_type (0)\n"
    "     | CONST type                              # const_type (1)\n"
    "     | type CONST                              # type_const (0)\n"
    "     ;\n"
    "\n"

    // Container types: vector<T>, map<K,V>, set<T>, list<T>
    "container_type : VECTOR '<' type '>'           # vector_type (2)\n"
    "               | MAP '<' type ',' type '>'     # map_type (2 4)\n"
    "               | SET '<' type '>'              # set_type (2)\n"
    "               | LIST '<' type '>'             # list_type (2)\n"
    "               ;\n"
    "\n"

    // Stream types
    "stream_type : IFSTREAM                         # 0\n"
    "            | OFSTREAM                         # 0\n"
    "            | FSTREAM                          # 0\n"
    "            | SSTREAM                          # 0\n"
    "            | OSTREAM                          # 0\n"
    "            ;\n"
    "\n"

    // Full type including containers and streams
    "full_type : type                               # 0\n"
    "          | container_type                     # 0\n"
    "          | stream_type                        # 0\n"
    "          ;\n"
    "\n"

    // ================================================================
    // Declarations
    // ================================================================

    // Declarator: name, name with array dims, name with initializer
    "declarator : IDENT                             # 0\n"
    "           | IDENT '[' const_expr ']'          # array_decl (0 2)\n"
    "           | IDENT '[' ']'                     # unsized_array (0)\n"
    "           | IDENT '=' initializer             # init_decl (0 2)\n"
    "           | IDENT '[' const_expr ']' '=' initializer\n"
    "                                               # array_init_decl (0 2 5)\n"
    "           | IDENT '[' ']' '=' initializer\n"
    "                                               # unsized_array_init (0 4)\n"
    "           | '(' '*' IDENT ')' '(' param_type_list_opt ')'\n"
    "                                               # fptr_decl (2 5)\n"
    "           ;\n"
    "\n"

    "declaration : full_type declarator_list        # decl (0 1)\n"
    "            ;\n"
    "declarator_list : declarator                   # 0\n"
    "                | declarator_list ',' declarator\n"
    "                                               # decl_list (0 2)\n"
    "                ;\n"
    "\n"

    // Initializers
    "initializer : assign_expr                      # 0\n"
    "            | '{' initializer_list '}'         # init_list (1)\n"
    "            | '{' initializer_list ',' '}'     # init_list (1)\n"
    "            | '{' '}'                          # empty_init\n"
    "            ;\n"
    "initializer_list : initializer_elem            # 0\n"
    "                 | initializer_list ',' initializer_elem\n"
    "                                               # init_seq (0 2)\n"
    "                 ;\n"
    "initializer_elem : initializer                 # 0\n"
    "                 | designator '=' initializer  # desig_init (0 2)\n"
    "                 ;\n"
    "designator : '.' IDENT                         # member_desig (1)\n"
    "           | '[' const_expr ']'                # index_desig (1)\n"
    "           ;\n"
    "\n"

    // ================================================================
    // Typedef
    // ================================================================
    "typedef_decl : TYPEDEF full_type IDENT ';'     # typedef (1 2)\n"
    "             | TYPEDEF full_type '(' '*' IDENT ')' '(' param_type_list_opt ')' ';'\n"
    "                                               # typedef_fptr (1 4 7)\n"
    "             | TYPEDEF STRUCT IDENT IDENT ';'  # typedef_struct (2 3)\n"
    "             | TYPEDEF ENUM IDENT IDENT ';'    # typedef_enum (2 3)\n"
    "             | TYPEDEF UNION IDENT IDENT ';'   # typedef_union (2 3)\n"
    "             ;\n"
    "\n"

    // ================================================================
    // Functions
    // ================================================================
    "function_def : full_type IDENT '(' param_list_opt ')' compound_stmt\n"
    "                                               # func_def (0 1 3 5)\n"
    "             | full_type IDENT '(' param_list_opt ')' ';'\n"
    "                                               # func_proto (0 1 3)\n"
    "             | full_type OPERATOR overload_op '(' param_list_opt ')' compound_stmt\n"
    "                                               # oper_def (0 2 4 6)\n"
    "             ;\n"
    "\n"

    "overload_op : '+' # 0\n"
    "            | '-' # 0\n"
    "            | '*' # 0\n"
    "            | '/' # 0\n"
    "            | '%' # 0\n"
    "            | EQ_OP # 0\n"
    "            | NE_OP # 0\n"
    "            | '<' # 0\n"
    "            | '>' # 0\n"
    "            | LE_OP # 0\n"
    "            | GE_OP # 0\n"
    "            | THREE_WAY # 0\n"
    "            | BSL_OP # 0\n"
    "            | BSR_OP # 0\n"
    "            | '(' ')' # funcall_op\n"
    "            | '[' ']' # subscript_op\n"
    "            ;\n"
    "\n"

    "param_list_opt :                                \n"
    "               | param_list                    # 0\n"
    "               ;\n"
    "param_list : param                             # 0\n"
    "           | param_list ',' param              # param_list (0 2)\n"
    "           ;\n"
    "param : full_type IDENT                        # param (0 1)\n"
    "      | full_type IDENT '[' ']'                # param_array (0 1)\n"
    "      | full_type IDENT '[' const_expr ']'     # param_fixed_array (0 1 3)\n"
    "      | full_type                              # 0\n"
    "      | ELLIPSIS                               # ellipsis_param\n"
    "      ;\n"
    "\n"

    // Parameter type list (for function pointer typedefs — no names)
    "param_type_list_opt :                           \n"
    "                    | param_type_list           # 0\n"
    "                    ;\n"
    "param_type_list : full_type                    # 0\n"
    "                | param_type_list ',' full_type # param_types (0 2)\n"
    "                | param_type_list ',' ELLIPSIS  # param_types_va (0)\n"
    "                ;\n"
    "\n"

    // ================================================================
    // Struct / Union
    // ================================================================
    "struct_def : STRUCT IDENT '{' struct_body '}' ';'\n"
    "                                               # struct_def (1 3)\n"
    "           | STRUCT '{' struct_body '}' ';'    # anon_struct (2)\n"
    "           | STRUCT IDENT ';'                  # struct_fwd (1)\n"
    "           ;\n"
    "union_def : UNION IDENT '{' struct_body '}' ';'\n"
    "                                               # union_def (1 3)\n"
    "          | UNION '{' struct_body '}' ';'      # anon_union (2)\n"
    "          ;\n"
    "struct_body :                                   \n"
    "            | struct_body struct_member         # struct_body (0 1)\n"
    "            ;\n"
    "struct_member : full_type struct_field_list ';' # struct_field (0 1)\n"
    "              | struct_def                      # 0\n"
    "              | union_def                       # 0\n"
    "              | enum_def                        # 0\n"
    "              ;\n"
    "struct_field_list : struct_field                # 0\n"
    "                  | struct_field_list ',' struct_field\n"
    "                                               # field_list (0 2)\n"
    "                  ;\n"
    "struct_field : IDENT                            # 0\n"
    "             | IDENT '[' const_expr ']'         # field_array (0 2)\n"
    "             | '*' IDENT                        # field_ptr (1)\n"
    "             | IDENT ':' INTEGER                # bitfield (0 2)\n"
    "             ;\n"
    "\n"

    // ================================================================
    // Enum
    // ================================================================
    "enum_def : ENUM IDENT '{' enum_body '}' ';'   # enum_def (1 3)\n"
    "         | ENUM IDENT '{' enum_body ',' '}' ';'\n"
    "                                               # enum_def (1 3)\n"
    "         | ENUM '{' enum_body '}' ';'          # anon_enum (2)\n"
    "         | ENUM '{' enum_body ',' '}' ';'      # anon_enum (2)\n"
    "         ;\n"
    "enum_body : enum_val                           # 0\n"
    "          | enum_body ',' enum_val             # enum_list (0 2)\n"
    "          ;\n"
    "enum_val : IDENT                               # 0\n"
    "         | IDENT '=' const_expr                # enum_assign (0 2)\n"
    "         ;\n"
    "\n"

    // ================================================================
    // Class
    // ================================================================
    "class_def : CLASS IDENT '{' class_body '}' ';'\n"
    "                                               # class_def (1 3)\n"
    "          | CLASS IDENT ':' access_spec IDENT '{' class_body '}' ';'\n"
    "                                               # class_inherit (1 3 4 6)\n"
    "          ;\n"
    "access_spec : PUBLIC                           # 0\n"
    "            | PRIVATE                          # 0\n"
    "            | PROTECTED                        # 0\n"
    "            ;\n"
    "class_body :                                    \n"
    "           | class_body class_member            # class_body (0 1)\n"
    "           ;\n"
    "class_member : access_spec ':'                  # access (0)\n"
    "             | declaration ';'                  # 0\n"
    "             | method_def                       # 0\n"
    "             | constructor_def                  # 0\n"
    "             | destructor_def                   # 0\n"
    "             | oper_method_def                  # 0\n"
    "             ;\n"
    "method_def : full_type IDENT '(' param_list_opt ')' compound_stmt\n"
    "                                               # method (0 1 3 5)\n"
    "           | VIRTUAL full_type IDENT '(' param_list_opt ')' compound_stmt\n"
    "                                               # vmethod (1 2 4 6)\n"
    "           | full_type IDENT '(' param_list_opt ')' ';'\n"
    "                                               # method_proto (0 1 3)\n"
    "           | VIRTUAL full_type IDENT '(' param_list_opt ')' ';'\n"
    "                                               # vmethod_proto (1 2 4)\n"
    "           ;\n"
    "constructor_def : IDENT '(' param_list_opt ')' compound_stmt\n"
    "                                               # ctor (0 2 4)\n"
    "                ;\n"
    "destructor_def : '~' IDENT '(' ')' compound_stmt\n"
    "                                               # dtor (1 4)\n"
    "               ;\n"
    "oper_method_def : full_type OPERATOR overload_op '(' param_list_opt ')' compound_stmt\n"
    "                                               # oper_method (0 2 4 6)\n"
    "                ;\n"
    "\n"

    // ================================================================
    // Namespace / Using
    // ================================================================
    "namespace_def : NAMESPACE IDENT '{' translation_unit '}'\n"
    "                                               # namespace_def (1 3)\n"
    "              ;\n"
    "using_decl : USING NAMESPACE IDENT ';'         # using_ns (2)\n"
    "           | USING IDENT ';'                   # using_decl (1)\n"
    "           | PREFER IDENT ';'                  # prefer (1)\n"
    "           ;\n"
    "\n"

    // ================================================================
    // Statements
    // ================================================================
    "compound_stmt : '{' stmt_list_opt '}'          # block (1)\n"
    "              ;\n"
    "stmt_list_opt :                                 \n"
    "              | stmt_list                       # 0\n"
    "              ;\n"
    "stmt_list : stmt                                # 0\n"
    "          | stmt_list stmt                      # stmt_list (0 1)\n"
    "          ;\n"
    "stmt : expr_stmt                               # 0\n"
    "     | declaration ';'                          # 0\n"
    "     | compound_stmt                            # 0\n"
    "     | if_stmt                                  # 0\n"
    "     | while_stmt                               # 0\n"
    "     | do_while_stmt                            # 0\n"
    "     | for_stmt                                 # 0\n"
    "     | switch_stmt                              # 0\n"
    "     | return_stmt                              # 0\n"
    "     | break_stmt                               # 0\n"
    "     | continue_stmt                            # 0\n"
    "     | goto_stmt                                # 0\n"
    "     | label_stmt                               # 0\n"
    "     | try_stmt                                 # 0\n"
    "     | throw_stmt                               # 0\n"
    "     | delete_stmt                              # 0\n"
    "     | defer_stmt                               # 0\n"
    "     | match_stmt                               # 0\n"
    "     | ';'                                      \n"
    "     ;\n"
    "\n"

    "expr_stmt : expr ';'                           # expr_stmt (0)\n"
    "          ;\n"

    // if / else with RIGHT ELSE for dangling-else resolution
    "if_stmt : IF '(' expr ')' stmt                 # if (2 4)\n"
    "        | IF '(' expr ')' stmt ELSE stmt       # if_else (2 4 6)\n"
    "        ;\n"

    "while_stmt : WHILE '(' expr ')' stmt           # while (2 4)\n"
    "           ;\n"

    "do_while_stmt : DO stmt WHILE '(' expr ')' ';' # do_while (1 4)\n"
    "              ;\n"

    // for loop — supports both expression and declaration init
    "for_stmt : FOR '(' for_init for_cond ';' for_iter_opt ')' stmt\n"
    "                                               # for (2 3 5 7)\n"
    "         | FOR '(' full_type IDENT ':' expr ')' stmt\n"
    "                                               # for_range (2 3 5 7)\n"
    "         ;\n"
    "for_init : ';'                                  \n"
    "         | expr ';'                             # 0\n"
    "         | declaration ';'                      # 0\n"
    "         | expr_list ';'                        # 0\n"
    "         ;\n"
    "for_cond :                                      \n"
    "         | expr                                 # 0\n"
    "         ;\n"
    "for_iter_opt :                                   \n"
    "             | expr                             # 0\n"
    "             | expr_list                        # 0\n"
    "             ;\n"
    // Comma-separated expression list (for for-init and for-iter)
    "expr_list : expr ',' expr                      # expr_list (0 2)\n"
    "          | expr_list ',' expr                  # expr_list (0 2)\n"
    "          ;\n"
    "\n"

    // switch / case / default
    "switch_stmt : SWITCH '(' expr ')' '{' case_list '}'\n"
    "                                               # switch (2 5)\n"
    "            ;\n"
    "case_list :                                     \n"
    "          | case_list case_clause               # case_list (0 1)\n"
    "          ;\n"
    "case_clause : CASE const_expr ':' case_stmts   # case (1 3)\n"
    "            | CASE const_expr ELLIPSIS const_expr ':' case_stmts\n"
    "                                               # case_range (1 3 5)\n"
    "            | DEFAULT ':' case_stmts            # default (2)\n"
    "            ;\n"
    "case_stmts :                                    \n"
    "           | case_stmts stmt                    # case_stmts (0 1)\n"
    "           ;\n"
    "\n"

    "return_stmt : RETURN ';'                       # return\n"
    "            | RETURN expr ';'                   # return_val (1)\n"
    "            | RETURN expr ',' expr ';'          # return_multi (1 3)\n"
    "            ;\n"

    "break_stmt : BREAK ';'                         # break\n"
    "           ;\n"
    "continue_stmt : CONTINUE ';'                   # continue\n"
    "              ;\n"

    "goto_stmt : GOTO IDENT ';'                     # goto (1)\n"
    "          | GOTO '*' expr ';'                   # goto_indirect (2)\n"
    "          ;\n"
    "label_stmt : IDENT ':'                          # label (0)\n"
    "           ;\n"
    "\n"

    // try / catch
    "try_stmt : TRY compound_stmt catch_list        # try (1 2)\n"
    "         ;\n"
    "catch_list : catch_clause                       # 0\n"
    "           | catch_list catch_clause            # catch_list (0 1)\n"
    "           ;\n"
    "catch_clause : CATCH '(' full_type IDENT ')' compound_stmt\n"
    "                                               # catch (2 3 5)\n"
    "             | CATCH '(' ELLIPSIS ')' compound_stmt\n"
    "                                               # catch_all (4)\n"
    "             ;\n"

    // throw
    "throw_stmt : THROW ';'                         # throw\n"
    "           | THROW expr ';'                     # throw_expr (1)\n"
    "           ;\n"

    // delete
    "delete_stmt : DELETE expr ';'                  # delete (1)\n"
    "            | DELETE '[' ']' expr ';'           # delete_array (3)\n"
    "            ;\n"

    // defer (Go/Zig-style)
    "defer_stmt : DEFER stmt                        # defer (1)\n"
    "           ;\n"
    "\n"

    // match (Rust-style)
    "match_stmt : MATCH '(' expr ')' '{' match_arms '}'\n"
    "                                               # match (2 5)\n"
    "           ;\n"
    "match_arms :                                    \n"
    "           | match_arms match_arm               # match_arms (0 1)\n"
    "           ;\n"
    "match_arm : match_patterns FAT_ARROW match_body\n"
    "                                               # match_arm (0 2)\n"
    "          | IDENT FAT_ARROW match_body          # match_wild (0 2)\n"
    "          ;\n"
    "match_patterns : const_expr                    # 0\n"
    "               | match_patterns '|' const_expr # match_pats (0 2)\n"
    "               ;\n"
    "match_body : stmt                              # 0\n"
    "           | compound_stmt                     # 0\n"
    "           ;\n"
    "\n"

    // ================================================================
    // Expressions
    //
    // Full C expression grammar with operator precedence handled by
    // Gecko's LEFT/RIGHT declarations.  The grammar is factored into
    // layers: expr (binary + ternary + assignment), unary_expr,
    // postfix_expr, primary_expr.
    // ================================================================

    // Constant expression (used in enum values, array sizes, case labels)
    "const_expr : expr                              # 0\n"
    "           ;\n"
    "\n"

    // Full expression — binary operators resolved by precedence decls
    "expr : expr ',' expr                           # comma (0 2)\n"
    "     | assign_expr                             # 0\n"
    "     ;\n"

    "assign_expr : cond_expr                        # 0\n"
    "            | unary_expr '=' assign_expr        # assign (0 2)\n"
    "            | unary_expr ADD_ASSIGN assign_expr # add_assign (0 2)\n"
    "            | unary_expr SUB_ASSIGN assign_expr # sub_assign (0 2)\n"
    "            | unary_expr MUL_ASSIGN assign_expr # mul_assign (0 2)\n"
    "            | unary_expr DIV_ASSIGN assign_expr # div_assign (0 2)\n"
    "            | unary_expr MOD_ASSIGN assign_expr # mod_assign (0 2)\n"
    "            | unary_expr BSL_ASSIGN assign_expr # bsl_assign (0 2)\n"
    "            | unary_expr BSR_ASSIGN assign_expr # bsr_assign (0 2)\n"
    "            | unary_expr BAND_ASSIGN assign_expr # band_assign (0 2)\n"
    "            | unary_expr BOR_ASSIGN assign_expr # bor_assign (0 2)\n"
    "            | unary_expr XOR_ASSIGN assign_expr # xor_assign (0 2)\n"
    "            | unary_expr COL_ASSIGN assign_expr # col_assign (0 2)\n"
    "            ;\n"
    "\n"

    // Ternary conditional
    "cond_expr : lor_expr                           # 0\n"
    "          | lor_expr '?' expr ':' cond_expr    # ternary (0 2 4)\n"
    "          ;\n"

    // Logical OR
    "lor_expr : land_expr                           # 0\n"
    "         | lor_expr OR_OP land_expr            # lor (0 2)\n"
    "         ;\n"

    // Logical AND
    "land_expr : bor_expr                           # 0\n"
    "          | land_expr AND_OP bor_expr           # land (0 2)\n"
    "          ;\n"

    // Bitwise OR
    "bor_expr : bxor_expr                           # 0\n"
    "         | bor_expr '|' bxor_expr              # bitor (0 2)\n"
    "         ;\n"

    // Bitwise XOR
    "bxor_expr : band_expr                          # 0\n"
    "          | bxor_expr '^' band_expr            # bitxor (0 2)\n"
    "          ;\n"

    // Bitwise AND
    "band_expr : eq_expr                            # 0\n"
    "          | band_expr '&' eq_expr              # bitand (0 2)\n"
    "          ;\n"

    // Equality
    "eq_expr : rel_expr                             # 0\n"
    "        | eq_expr EQ_OP rel_expr               # eq (0 2)\n"
    "        | eq_expr NE_OP rel_expr               # ne (0 2)\n"
    "        | eq_expr EQ3_OP rel_expr              # eq3 (0 2)\n"
    "        ;\n"

    // Relational
    "rel_expr : shift_expr                          # 0\n"
    "         | rel_expr '<' shift_expr             # lt (0 2)\n"
    "         | rel_expr '>' shift_expr             # gt (0 2)\n"
    "         | rel_expr LE_OP shift_expr           # le (0 2)\n"
    "         | rel_expr GE_OP shift_expr           # ge (0 2)\n"
    "         | rel_expr THREE_WAY shift_expr       # three_way (0 2)\n"
    "         ;\n"

    // Shift
    "shift_expr : add_expr                          # 0\n"
    "           | shift_expr BSL_OP add_expr        # bsl (0 2)\n"
    "           | shift_expr BSR_OP add_expr        # bsr (0 2)\n"
    "           ;\n"

    // Additive
    "add_expr : mul_expr                            # 0\n"
    "         | add_expr '+' mul_expr               # add (0 2)\n"
    "         | add_expr '-' mul_expr               # sub (0 2)\n"
    "         ;\n"

    // Multiplicative
    "mul_expr : unary_expr                          # 0\n"
    "         | mul_expr '*' unary_expr             # mul (0 2)\n"
    "         | mul_expr '/' unary_expr             # div (0 2)\n"
    "         | mul_expr '%' unary_expr             # mod (0 2)\n"
    "         ;\n"
    "\n"

    // Unary expressions
    "unary_expr : postfix_expr                      # 0\n"
    "           | '-' unary_expr                    # neg (1)\n"
    "           | '+' unary_expr                    # pos (1)\n"
    "           | '!' unary_expr                    # lnot (1)\n"
    "           | '~' unary_expr                    # bnot (1)\n"
    "           | '*' unary_expr                    # deref (1)\n"
    "           | '&' unary_expr                    # addrof (1)\n"
    "           | INC_OP unary_expr                 # pre_inc (1)\n"
    "           | DEC_OP unary_expr                 # pre_dec (1)\n"
    "           | SIZEOF '(' full_type ')'          # sizeof_type (2)\n"
    "           | SIZEOF unary_expr                 # sizeof_expr (1)\n"
    "           | '(' full_type ')' unary_expr      # cast (1 3)\n"
    "           | NEW IDENT                         # new_plain (1)\n"
    "           | NEW IDENT '(' arg_list_opt ')'    # new_ctor (1 3)\n"
    "           ;\n"
    "\n"

    // Postfix expressions
    "postfix_expr : primary_expr                    # 0\n"
    "             | postfix_expr '(' arg_list_opt ')'\n"
    "                                               # call (0 2)\n"
    "             | postfix_expr '.' IDENT          # member (0 2)\n"
    "             | postfix_expr ARROW IDENT        # arrow_member (0 2)\n"
    "             | postfix_expr '.' IDENT '(' arg_list_opt ')'\n"
    "                                               # method_call (0 2 4)\n"
    "             | postfix_expr ARROW IDENT '(' arg_list_opt ')'\n"
    "                                               # arrow_call (0 2 4)\n"
    "             | postfix_expr '[' expr ']'       # subscript (0 2)\n"
    "             | postfix_expr INC_OP             # post_inc (0)\n"
    "             | postfix_expr DEC_OP             # post_dec (0)\n"
    "             | IDENT SCOPE IDENT               # ns_name (0 2)\n"
    "             | IDENT SCOPE IDENT '(' arg_list_opt ')'\n"
    "                                               # ns_call (0 2 4)\n"
    "             | '(' full_type ')' '{' initializer_list '}'\n"
    "                                               # compound_lit (1 4)\n"
    "             ;\n"
    "\n"

    // Primary expressions
    "primary_expr : IDENT                           # 0\n"
    "             | INTEGER                         # 0\n"
    "             | REAL                            # 0\n"
    "             | STRING_LIT                      # 0\n"
    "             | CHAR_LIT                        # 0\n"
    "             | '(' expr ')'                    # paren (1)\n"
    "             ;\n"
    "\n"

    "arg_list_opt :                                  \n"
    "             | arg_list                         # 0\n"
    "             ;\n"
    "arg_list : assign_expr                          # 0\n"
    "         | arg_list ',' assign_expr             # arg_list (0 2)\n"
    "         ;\n"
;


// -----------------------------------------------------------------------
// madc_create_gecko_grammar — create, configure, and return an
// initialized Gecko grammar for madc.  Returns nullptr on error,
// prints diagnostics to stderr.
// -----------------------------------------------------------------------
struct grammar *madc_create_gecko_grammar()
{
    struct grammar *g = gp_create_grammar();
    if (!g) {
        fprintf(stderr, "madc_create_gecko_grammar: failed to allocate grammar\n");
        return nullptr;
    }

    gp_set_debug_level(g, 0);

    int err = gp_parse_grammar(g, 1, madc_grammar_str);
    if (err) {
        fprintf(stderr, "madc_create_gecko_grammar: grammar error %d: %s\n",
                gp_error_code(g), gp_error_message(g));
        gp_fin(g);
        return nullptr;
    }

    return g;
}


// -----------------------------------------------------------------------
// madc_token_to_gecko — map a madc TokenBase to a Gecko terminal code.
// Returns -1 for tokens that should be skipped (whitespace, comments).
// -----------------------------------------------------------------------
int madc_token_to_gecko(TokenBase *tb)
{
    if (!tb) return -1;

    TokenType tt = tb->type();
    TokenID   tid = tb->id();

    // Skip whitespace and comments
    switch (tt) {
    case TokenType::ttSpace:
    case TokenType::ttTab:
    case TokenType::ttEOL:
    case TokenType::ttComment:
        return -1;
    default:
        break;
    }

    // Literal values
    switch (tt) {
    case TokenType::ttInteger:
        return GT_INTEGER;
    case TokenType::ttReal:
        return GT_REAL;
    case TokenType::ttString:
        return GT_STRING;
    case TokenType::ttChar:
        return GT_CHAR_LIT;
    case TokenType::ttIdentifier:
        // Check for identifier-form keywords that the lexer does not
        // tokenize as keyword tokens.
        {
            TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
            if (ti) {
                if (ti->str == "sizeof")   return GT_SIZEOF;
                if (ti->str == "inline")   return GT_INLINE;
                if (ti->str == "signed")   return GT_SIGNED;
                if (ti->str == "unsigned") return GT_UNSIGNED;
            }
        }
        return GT_IDENT;
    default:
        break;
    }

    // Data type keywords
    switch (tt) {
    case TokenType::ttDataType:
        // Map each data type token to its Gecko terminal
        switch (tid) {
        case TokenID::tkBase:  // use str matching below
            break;
        default:
            break;
        }
        // Fall through to string-based matching for data types
        {
            // Cast to TokenDataType to check the definition
            TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
            if (ti) {
                const std::string &s = ti->str;
                if (s == "void")          return GT_VOID;
                if (s == "bool" || s == "_Bool") return GT_BOOL;
                if (s == "char")          return GT_CHAR;
                if (s == "short")         return GT_SHORT;
                if (s == "int")           return GT_INT;
                if (s == "long")          return GT_LONG;
                if (s == "float")         return GT_FLOAT;
                if (s == "double")        return GT_DOUBLE;
                if (s == "string")        return GT_STRING_T;
                if (s == "auto")          return GT_AUTO;
                if (s == "int8_t")        return GT_INT8;
                if (s == "int16_t")       return GT_INT16;
                if (s == "int32_t")       return GT_INT32;
                if (s == "int64_t")       return GT_INT64;
                if (s == "uint8_t")       return GT_UINT8;
                if (s == "uint16_t")      return GT_UINT16;
                if (s == "uint32_t")      return GT_UINT32;
                if (s == "uint64_t")      return GT_UINT64;
                if (s == "ostream")       return GT_OSTREAM;
                if (s == "stringstream")  return GT_SSTREAM;
                if (s == "ifstream")      return GT_IFSTREAM;
                if (s == "ofstream")      return GT_OFSTREAM;
                if (s == "fstream")       return GT_FSTREAM;
                if (s == "LPSTR")         return GT_INT;  // char* alias
                if (s == "array")         return GT_IDENT; // treated as ident
            }
        }
        return GT_IDENT;  // fallback for unknown data types

    default:
        break;
    }

    // Keywords (ttKeyword type)
    switch (tid) {
    case TokenID::tkIF:        return GT_IF;
    case TokenID::tkELSE:      return GT_ELSE;
    case TokenID::tkWHILE:     return GT_WHILE;
    case TokenID::tkFOR:       return GT_FOR;
    case TokenID::tkDO:        return GT_DO;
    case TokenID::tkRETURN:    return GT_RETURN;
    case TokenID::tkBREAK:     return GT_BREAK;
    case TokenID::tkCONT:      return GT_CONTINUE;
    case TokenID::tkSWITCH:    return GT_SWITCH;
    case TokenID::tkCASE:      return GT_CASE;
    case TokenID::tkDEFAULT:   return GT_DEFAULT;
    case TokenID::tkGOTO:      return GT_GOTO;
    case TokenID::tkSTRUCT:    return GT_STRUCT;
    case TokenID::tkCLASS:     return GT_CLASS;
    case TokenID::tkENUM:      return GT_ENUM;
    case TokenID::tkUNION:     return GT_UNION;
    case TokenID::tkTYPEDEF:   return GT_TYPEDEF;
    case TokenID::tkSTATIC:    return GT_STATIC;
    case TokenID::tkEXTERN:    return GT_EXTERN;
    case TokenID::tkCONST:     return GT_CONST;
    case TokenID::tkVOLATILE:  return GT_VOLATILE;
    case TokenID::tkREGISTER:  return GT_REGISTER;
    case TokenID::tkTRY:       return GT_TRY;
    case TokenID::tkCATCH:     return GT_CATCH;
    case TokenID::tkTHROW:     return GT_THROW;
    case TokenID::tkNEW:       return GT_NEW;
    case TokenID::tkDELETE:    return GT_DELETE;
    case TokenID::tkUSING:     return GT_USING;
    case TokenID::tkNAMESPACE: return GT_NAMESPACE;
    case TokenID::tkMATCH:     return GT_MATCH;
    case TokenID::tkDEFER:     return GT_DEFER;
    case TokenID::tkPREFER:    return GT_PREFER;
    case TokenID::tkRESTRICT:  return GT_RESTRICT;
    case TokenID::tkOPEROVER:  return GT_OPERATOR;
    case TokenID::tkVECTOR:    return GT_VECTOR;
    case TokenID::tkMAP:       return GT_MAP;
    case TokenID::tkSET:       return GT_SET;
    case TokenID::tkLIST:      return GT_LIST;
    default:
        break;
    }

    // Multi-char operators
    switch (tid) {
    case TokenID::tkEquals:    return GT_EQ;
    case TokenID::tk3Eq:       return GT_3EQ;
    case TokenID::tkNotEq:     return GT_NE;
    case TokenID::tkLE:        return GT_LE;
    case TokenID::tkGE:        return GT_GE;
    case TokenID::tkLand:      return GT_AND;
    case TokenID::tkLor:       return GT_OR;
    case TokenID::tkInc:       return GT_INC;
    case TokenID::tkDec:       return GT_DEC;
    case TokenID::tkDeRef:     return GT_ARROW;
    case TokenID::tkNS:        return GT_SCOPE;
    case TokenID::tkBSL:       return GT_BSL;
    case TokenID::tkBSR:       return GT_BSR;
    case TokenID::tkAddEq:     return GT_ADD_ASSIGN;
    case TokenID::tkSubEq:     return GT_SUB_ASSIGN;
    case TokenID::tkMulEq:     return GT_MUL_ASSIGN;
    case TokenID::tkDivEq:     return GT_DIV_ASSIGN;
    case TokenID::tkModEq:     return GT_MOD_ASSIGN;
    case TokenID::tkBSLEq:     return GT_BSL_ASSIGN;
    case TokenID::tkBSREq:     return GT_BSR_ASSIGN;
    case TokenID::tkBandEq:    return GT_BAND_ASSIGN;
    case TokenID::tkBorEq:     return GT_BOR_ASSIGN;
    case TokenID::tkXorEq:     return GT_XOR_ASSIGN;
    case TokenID::tk3Way:      return GT_THREE_WAY;
    case TokenID::tkFatArrow:  return GT_FAT_ARROW;
    case TokenID::tkColEq:     return GT_COL_ASSIGN;
    default:
        break;
    }

    // Single-char operators and symbols — use ASCII code directly.
    // These map 1:1 to the character literal in the grammar ('c').
    switch (tid) {
    case TokenID::tkAdd:       return '+';
    case TokenID::tkSub:       return '-';
    case TokenID::tkMul:       return '*';
    case TokenID::tkDiv:       return '/';
    case TokenID::tkMod:       return '%';  // note: tkMod == tkQmark alias? no, tkMod is 29
    case TokenID::tkAssign:    return '=';
    case TokenID::tkLT:        return '<';
    case TokenID::tkGT:        return '>';
    case TokenID::tkBand:      return '&';
    case TokenID::tkBor:       return '|';
    case TokenID::tkXor:       return '^';
    case TokenID::tkBnot:      return '~';
    case TokenID::tkLnot:      return '!';
    case TokenID::tkNeg:       return '-';
    case TokenID::tkTerQ:      return '?';
    case TokenID::tkTerC:      return ':';
    case TokenID::tkDot:       return '.';
    case TokenID::tkComma:     return ',';
    case TokenID::tkSemi:      return ';';
    case TokenID::tkOpBrc:     return '{';
    case TokenID::tkClBrc:     return '}';
    case TokenID::tkOpBrk:     return '(';
    case TokenID::tkClBrk:     return ')';
    case TokenID::tkOpSqr:     return '[';
    case TokenID::tkClSqr:     return ']';
    case TokenID::tkHash:      return '#';
    default:
        break;
    }

    // Fallback: if the token has a small numeric value, it may be an
    // ASCII-coded single-char token.
    int64_t raw = tb->get();
    if (raw > 0 && raw < 128) {
        return (int)raw;
    }

    // Unknown token — skip
    return -1;
}
