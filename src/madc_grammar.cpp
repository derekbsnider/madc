// madc_grammar.cpp — Gecko GLR grammar for the madc language
//
// Based on the ANSI C grammar from Gecko's test/test_gecko.c, with
// the critical fix: pointer/reference on the DECLARATOR, not the type.
// This correctly handles `int *a, *b;` (two pointer declarators sharing
// type `int`).
//
// C++ extensions (classes, references, try/catch, namespaces, etc.)
// are added incrementally on top of the ANSI C base.
//
// Operator precedence uses Gecko's ambiguous-expression approach:
// all binary operators in one rule, resolved by LEFT/RIGHT declarations.

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
// Structure follows ANSI C (ISO 9899:1999) Appendix A, with:
//   - Correct declarator model (pointer on declarator, not type)
//   - Gecko's ambiguous binary expression approach (precedence-resolved)
//   - Abstract node annotations (# name (children)) for emitter
//   - C++ extensions: class, references, try/catch, new/delete,
//     namespaces, operator overloading, match, defer
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
    "ELLIPSIS = 372\n"
    "THREE_WAY = 373\n"
    "FAT_ARROW = 374\n"
    "COL_ASSIGN = 375\n"
    "EQ3_OP = 376;\n"
    "\n"

    // ================================================================
    // Operator precedence — lowest to highest
    //
    // IMPORTANT: In Gecko, LATER declarations have HIGHER priority
    // (tighter binding).  The ANSI C test grammar (test_gecko.c) has
    // these in the WRONG order — we use the correct C order here.
    //
    // Only binary operators that appear in bin_expr need declarations.
    // Assignment and ternary are handled structurally.
    // ================================================================
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
    // A.2.4  External definitions  (§6.9)
    // ================================================================

    "start : translation_unit                        # 0\n"
    "      ;\n"
    "\n"

    "translation_unit : external_declaration                     # 0\n"
    "                 | translation_unit external_declaration    # tu (0 1)\n"
    "                 ;\n"
    "\n"

    "external_declaration : function_definition   # 0\n"
    "                     | declaration            # 0\n"
    "                     | class_definition       # 0\n"
    "                     | using_declaration      # 0\n"
    "                     | namespace_definition   # 0\n"
    "                     | ';'                    \n"
    "                     ;\n"
    "\n"

    // function_definition — ANSI C structure.
    // The function name and parameters are INSIDE the declarator:
    //   int main(int argc, char **argv) { ... }
    //   → specs=int, decl=main(int argc, char **argv), body={...}
    "function_definition : declaration_specifiers declarator compound_statement\n"
    "                                              # func_def (0 1 2)\n"
    "                    ;\n"
    "\n"

    // ================================================================
    // A.2.2  Declarations  (§6.7)
    // ================================================================

    "declaration : declaration_specifiers init_declarator_list_opt ';'\n"
    "                                              # decl (0 1)\n"
    "            ;\n"
    "\n"

    // declaration_specifiers — recursive list of specifiers.
    // Each specifier is one of: storage class, type, qualifier, function spec.
    // For `static const unsigned long int`:
    //   → qual(static, qual(const, qual(unsigned, qual(long, qual(int, nil)))))
    "declaration_specifiers"
    "  : storage_class_specifier declaration_specifiers_opt  # qual (0 1)\n"
    "  | type_specifier declaration_specifiers_opt           # qual (0 1)\n"
    "  | type_qualifier declaration_specifiers_opt           # qual (0 1)\n"
    "  | function_specifier declaration_specifiers_opt       # qual (0 1)\n"
    "  ;\n"
    "\n"

    "declaration_specifiers_opt :                     \n"
    "                           | declaration_specifiers  # 0\n"
    "                           ;\n"
    "\n"

    "init_declarator_list_opt :                       \n"
    "                         | init_declarator_list  # 0\n"
    "                         ;\n"
    "\n"

    "init_declarator_list : init_declarator                            # 0\n"
    "                     | init_declarator_list ',' init_declarator   # decl_list (0 2)\n"
    "                     ;\n"
    "\n"

    "init_declarator : declarator                     # 0\n"
    "                | declarator '=' initializer      # init_decl (0 2)\n"
    "                ;\n"
    "\n"

    // §6.7.1  Storage class specifiers
    "storage_class_specifier : TYPEDEF    # 0\n"
    "                        | EXTERN     # 0\n"
    "                        | STATIC     # 0\n"
    "                        | AUTO       # 0\n"
    "                        | REGISTER   # 0\n"
    "                        ;\n"
    "\n"

    // §6.7.2  Type specifiers
    "type_specifier : VOID       # 0\n"
    "               | BOOL       # 0\n"
    "               | CHAR       # 0\n"
    "               | SHORT      # 0\n"
    "               | INT        # 0\n"
    "               | LONG       # 0\n"
    "               | FLOAT      # 0\n"
    "               | DOUBLE     # 0\n"
    "               | SIGNED     # 0\n"
    "               | UNSIGNED   # 0\n"
    "               | STRING_T   # 0\n"
    "               | INT8       # 0\n"
    "               | INT16      # 0\n"
    "               | INT32      # 0\n"
    "               | INT64      # 0\n"
    "               | UINT8      # 0\n"
    "               | UINT16     # 0\n"
    "               | UINT32     # 0\n"
    "               | UINT64     # 0\n"
    "               | struct_or_union_specifier  # 0\n"
    "               | enum_specifier             # 0\n"
    "               | container_type             # 0\n"
    "               | stream_type                # 0\n"
    "               | typedef_name               # 0\n"
    "               ;\n"
    "\n"

    // §6.7.3  Type qualifiers
    "type_qualifier : CONST      # 0\n"
    "               | VOLATILE   # 0\n"
    "               | RESTRICT   # 0\n"
    "               ;\n"
    "\n"

    // §6.7.4  Function specifiers (extended with C++ virtual)
    "function_specifier : INLINE   # 0\n"
    "                   | VIRTUAL  # 0\n"
    "                   ;\n"
    "\n"

    // madc container types: vector<T>, map<K,V>, set<T>, list<T>
    "container_type : VECTOR '<' type_name '>'             # vector_type (2)\n"
    "               | MAP '<' type_name ',' type_name '>'  # map_type (2 4)\n"
    "               | SET '<' type_name '>'                # set_type (2)\n"
    "               | LIST '<' type_name '>'               # list_type (2)\n"
    "               ;\n"
    "\n"

    // madc stream types
    "stream_type : IFSTREAM    # 0\n"
    "            | OFSTREAM    # 0\n"
    "            | FSTREAM     # 0\n"
    "            | SSTREAM     # 0\n"
    "            | OSTREAM     # 0\n"
    "            ;\n"
    "\n"

    // §6.7.2.1  Struct/union specifiers
    "struct_or_union_specifier"
    "  : struct_or_union identifier_opt '{' struct_declaration_list '}'\n"
    "                                              # struct_def (0 1 3)\n"
    "  | struct_or_union IDENT                     # struct_ref (0 1)\n"
    "  ;\n"
    "\n"

    "identifier_opt :            \n"
    "               | IDENT  # 0\n"
    "               ;\n"
    "\n"

    "struct_or_union : STRUCT  # 0\n"
    "                | UNION   # 0\n"
    "                ;\n"
    "\n"

    "struct_declaration_list"
    "  : struct_declaration                              # 0\n"
    "  | struct_declaration_list struct_declaration       # struct_body (0 1)\n"
    "  ;\n"
    "\n"

    "struct_declaration"
    "  : specifier_qualifier_list struct_declarator_list ';'\n"
    "                                              # struct_field (0 1)\n"
    "  ;\n"
    "\n"

    // specifier_qualifier_list — like declaration_specifiers but no
    // storage class or function specifiers.
    "specifier_qualifier_list"
    "  : type_specifier specifier_qualifier_list_opt    # qual (0 1)\n"
    "  | type_qualifier specifier_qualifier_list_opt    # qual (0 1)\n"
    "  ;\n"
    "\n"

    "specifier_qualifier_list_opt :                      \n"
    "                             | specifier_qualifier_list  # 0\n"
    "                             ;\n"
    "\n"

    "struct_declarator_list"
    "  : struct_declarator                                  # 0\n"
    "  | struct_declarator_list ',' struct_declarator        # field_list (0 2)\n"
    "  ;\n"
    "\n"

    "struct_declarator : declarator                         # 0\n"
    "                  | declarator_opt ':' constant_expression\n"
    "                                                       # bitfield (0 2)\n"
    "                  ;\n"
    "\n"

    "declarator_opt :             \n"
    "               | declarator  # 0\n"
    "               ;\n"
    "\n"

    // §6.7.2.2  Enum specifiers
    "enum_specifier"
    "  : ENUM identifier_opt '{' enumerator_list '}'        # enum_def (1 3)\n"
    "  | ENUM identifier_opt '{' enumerator_list ',' '}'    # enum_def (1 3)\n"
    "  | ENUM IDENT                                         # enum_ref (1)\n"
    "  ;\n"
    "\n"

    "enumerator_list : enumerator                           # 0\n"
    "                | enumerator_list ',' enumerator        # enum_list (0 2)\n"
    "                ;\n"
    "\n"

    "enumerator : IDENT                                     # 0\n"
    "           | IDENT '=' constant_expression              # enum_assign (0 2)\n"
    "           ;\n"
    "\n"

    // ================================================================
    // §6.7.5  Declarators — THE KEY FIX
    //
    // In C, pointer/reference belongs on the DECLARATOR, not the type.
    //   int *a, *b;   → two declarators, each with its own pointer
    //   int (*fp)();  → function pointer declarator
    // ================================================================

    "declarator : pointer direct_declarator          # ptr_decl (0 1)\n"
    "           | '&' direct_declarator              # ref_decl (1)\n"
    "           | direct_declarator                  # 0\n"
    "           ;\n"
    "\n"

    "direct_declarator"
    "  : IDENT                                              # 0\n"
    "  | '(' declarator ')'                                 # 1\n"
    "  | direct_declarator '[' assignment_expression_opt ']'\n"
    "                                                       # array_decl (0 2)\n"
    "  | direct_declarator '[' '*' ']'                      # vla_decl (0)\n"
    "  | direct_declarator '(' parameter_type_list ')'      # func_decl (0 2)\n"
    "  | direct_declarator '(' identifier_list_opt ')'      # func_decl (0 2)\n"
    "  ;\n"
    "\n"

    "assignment_expression_opt :                      \n"
    "                          | assignment_expression  # 0\n"
    "                          ;\n"
    "\n"

    "identifier_list_opt :                \n"
    "                    | identifier_list  # 0\n"
    "                    ;\n"
    "\n"

    // §6.7.5  Pointer — right-recursive chain of stars
    "pointer : '*' type_qualifier_list_opt              # star (1)\n"
    "        | '*' type_qualifier_list_opt pointer       # stars (1 2)\n"
    "        ;\n"
    "\n"

    "type_qualifier_list_opt :                    \n"
    "                        | type_qualifier_list  # 0\n"
    "                        ;\n"
    "\n"

    "type_qualifier_list : type_qualifier                       # 0\n"
    "                    | type_qualifier_list type_qualifier    # qual_list (0 1)\n"
    "                    ;\n"
    "\n"

    // §6.7.5  Parameter type list
    "parameter_type_list : parameter_list                       # 0\n"
    "                    | parameter_list ',' ELLIPSIS           # param_va (0)\n"
    "                    ;\n"
    "\n"

    "parameter_list : parameter_declaration                              # 0\n"
    "               | parameter_list ',' parameter_declaration           # param_list (0 2)\n"
    "               ;\n"
    "\n"

    "parameter_declaration"
    "  : declaration_specifiers declarator               # param (0 1)\n"
    "  | declaration_specifiers abstract_declarator_opt   # param (0 1)\n"
    "  ;\n"
    "\n"

    "abstract_declarator_opt :                    \n"
    "                        | abstract_declarator  # 0\n"
    "                        ;\n"
    "\n"

    "identifier_list : IDENT                             # 0\n"
    "                | identifier_list ',' IDENT          # ident_list (0 2)\n"
    "                ;\n"
    "\n"

    // §6.7.6  Type names — used in casts and sizeof
    "type_name : specifier_qualifier_list abstract_declarator_opt\n"
    "                                              # type_name (0 1)\n"
    "          ;\n"
    "\n"

    // §6.7.6  Abstract declarator (unnamed — for casts, sizeof)
    "abstract_declarator : pointer                           # 0\n"
    "                    | pointer direct_abstract_declarator\n"
    "                                                        # abs_ptr (0 1)\n"
    "                    | '&'                               # abs_ref\n"
    "                    | direct_abstract_declarator        # 0\n"
    "                    ;\n"
    "\n"

    "direct_abstract_declarator"
    "  : '(' abstract_declarator ')'                         # 1\n"
    "  | direct_abstract_declarator_opt '[' assignment_expression_opt ']'\n"
    "                                                        # abs_array (0 2)\n"
    "  | direct_abstract_declarator_opt '[' '*' ']'          # abs_vla (0)\n"
    "  | direct_abstract_declarator_opt '(' parameter_type_list_opt ')'\n"
    "                                                        # abs_func (0 2)\n"
    "  ;\n"
    "\n"

    "direct_abstract_declarator_opt :                          \n"
    "                               | direct_abstract_declarator  # 0\n"
    "                               ;\n"
    "\n"

    "parameter_type_list_opt :                      \n"
    "                        | parameter_type_list  # 0\n"
    "                        ;\n"
    "\n"

    // typedef_name — any IDENT can be a typedef name.
    // This creates ambiguity (IDENT as type vs variable); Gecko's GLR
    // handles it.  A rule guard can be added later for disambiguation.
    "typedef_name : IDENT  # 0\n"
    "             ;\n"
    "\n"

    // §6.7.8  Initializers
    "initializer : assignment_expression             # 0\n"
    "            | '{' initializer_list '}'           # init_list (1)\n"
    "            | '{' initializer_list ',' '}'       # init_list (1)\n"
    "            | '{' '}'                            # empty_init\n"
    "            ;\n"
    "\n"

    "initializer_list"
    "  : designation_opt initializer                         # desig_init (0 1)\n"
    "  | initializer_list ',' designation_opt initializer    # init_seq (0 2 3)\n"
    "  ;\n"
    "\n"

    "designation_opt :              \n"
    "                | designation  # 0\n"
    "                ;\n"
    "\n"

    "designation : designator_list '='   # 0\n"
    "            ;\n"
    "\n"

    "designator_list : designator                        # 0\n"
    "                | designator_list designator         # desig_chain (0 1)\n"
    "                ;\n"
    "\n"

    "designator : '[' constant_expression ']'   # index_desig (1)\n"
    "           | '.' IDENT                     # member_desig (1)\n"
    "           ;\n"
    "\n"

    // ================================================================
    // A.2.1  Expressions  (§6.5)
    // ================================================================

    // §6.5.1  Primary expressions
    "primary_expression : IDENT        # 0\n"
    "                   | INTEGER      # 0\n"
    "                   | REAL         # 0\n"
    "                   | STRING_LIT   # 0\n"
    "                   | CHAR_LIT     # 0\n"
    "                   | '(' expression ')'  # paren (1)\n"
    "                   ;\n"
    "\n"

    // §6.5.2  Postfix expressions
    // Includes C++ extensions: namespace calls (ns::func), compound literals.
    // Method calls (a.b(args)) are NOT a separate rule — they parse as
    // call(member(a, b), args) via the ANSI C rules, which is correct.
    "postfix_expression"
    "  : primary_expression                                  # 0\n"
    "  | postfix_expression '[' expression ']'               # subscript (0 2)\n"
    "  | postfix_expression '(' argument_expression_list_opt ')'\n"
    "                                                        # call (0 2)\n"
    "  | postfix_expression '.' IDENT                        # member (0 2)\n"
    "  | postfix_expression ARROW IDENT                      # arrow_member (0 2)\n"
    "  | postfix_expression INC_OP                           # post_inc (0)\n"
    "  | postfix_expression DEC_OP                           # post_dec (0)\n"
    "  | '(' type_name ')' '{' initializer_list '}'          # compound_lit (1 4)\n"
    "  | '(' type_name ')' '{' initializer_list ',' '}'      # compound_lit (1 4)\n"
    "  | IDENT SCOPE IDENT                                   # ns_name (0 2)\n"
    "  | IDENT SCOPE IDENT '(' argument_expression_list_opt ')'\n"
    "                                                        # ns_call (0 2 4)\n"
    "  ;\n"
    "\n"

    "argument_expression_list_opt :                       \n"
    "                             | argument_expression_list  # 0\n"
    "                             ;\n"
    "\n"

    "argument_expression_list"
    "  : assignment_expression                                    # 0\n"
    "  | argument_expression_list ',' assignment_expression       # arg_list (0 2)\n"
    "  ;\n"
    "\n"

    // §6.5.3  Unary expressions
    // Unary operators are inlined (not via unary_operator nonterminal)
    // so each produces a distinct anode name for the emitter.
    // C++ extensions: new, alignof.
    "unary_expression"
    "  : postfix_expression                          # 0\n"
    "  | INC_OP unary_expression                     # pre_inc (1)\n"
    "  | DEC_OP unary_expression                     # pre_dec (1)\n"
    "  | '&' cast_expression                         # addrof (1)\n"
    "  | '*' cast_expression                         # deref (1)\n"
    "  | '+' cast_expression                         # pos (1)\n"
    "  | '-' cast_expression                         # neg (1)\n"
    "  | '~' cast_expression                         # bnot (1)\n"
    "  | '!' cast_expression                         # lnot (1)\n"
    "  | SIZEOF unary_expression                     # sizeof_expr (1)\n"
    "  | SIZEOF '(' type_name ')'                    # sizeof_type (2)\n"
    "  | ALIGNOF '(' type_name ')'                   # alignof_type (2)\n"
    "  | NEW IDENT                                   # new_plain (1)\n"
    "  | NEW IDENT '(' argument_expression_list_opt ')'\n"
    "                                                # new_ctor (1 3)\n"
    "  ;\n"
    "\n"

    // §6.5.4  Cast expressions
    "cast_expression : unary_expression                  # 0\n"
    "                | '(' type_name ')' cast_expression  # cast (1 3)\n"
    "                ;\n"
    "\n"

    // §6.5.5–6.5.14  Binary expressions
    // All binary operators in one rule — Gecko resolves precedence via
    // the LEFT/RIGHT declarations above.  Each alternative produces a
    // distinct anode name matching what the emitter expects.
    "bin_expr"
    "  : bin_expr OR_OP bin_expr        # lor (0 2)\n"
    "  | bin_expr AND_OP bin_expr       # land (0 2)\n"
    "  | bin_expr '|' bin_expr          # bitor (0 2)\n"
    "  | bin_expr '^' bin_expr          # bitxor (0 2)\n"
    "  | bin_expr '&' bin_expr          # bitand (0 2)\n"
    "  | bin_expr EQ_OP bin_expr        # eq (0 2)\n"
    "  | bin_expr NE_OP bin_expr        # ne (0 2)\n"
    "  | bin_expr EQ3_OP bin_expr       # eq3 (0 2)\n"
    "  | bin_expr '<' bin_expr          # lt (0 2)\n"
    "  | bin_expr '>' bin_expr          # gt (0 2)\n"
    "  | bin_expr LE_OP bin_expr        # le (0 2)\n"
    "  | bin_expr GE_OP bin_expr        # ge (0 2)\n"
    "  | bin_expr THREE_WAY bin_expr    # three_way (0 2)\n"
    "  | bin_expr BSL_OP bin_expr       # bsl (0 2)\n"
    "  | bin_expr BSR_OP bin_expr       # bsr (0 2)\n"
    "  | bin_expr '+' bin_expr          # add (0 2)\n"
    "  | bin_expr '-' bin_expr          # sub (0 2)\n"
    "  | bin_expr '*' bin_expr          # mul (0 2)\n"
    "  | bin_expr '/' bin_expr          # div (0 2)\n"
    "  | bin_expr '%' bin_expr          # mod (0 2)\n"
    "  | cast_expression                # 0\n"
    "  ;\n"
    "\n"

    // §6.5.15  Conditional (ternary) expression
    "conditional_expression"
    "  : bin_expr                                            # 0\n"
    "  | bin_expr '?' expression ':' conditional_expression  # ternary (0 2 4)\n"
    "  ;\n"
    "\n"

    // §6.5.16  Assignment expression
    "assignment_expression"
    "  : conditional_expression                              # 0\n"
    "  | unary_expression '=' assignment_expression          # assign (0 2)\n"
    "  | unary_expression ADD_ASSIGN assignment_expression   # add_assign (0 2)\n"
    "  | unary_expression SUB_ASSIGN assignment_expression   # sub_assign (0 2)\n"
    "  | unary_expression MUL_ASSIGN assignment_expression   # mul_assign (0 2)\n"
    "  | unary_expression DIV_ASSIGN assignment_expression   # div_assign (0 2)\n"
    "  | unary_expression MOD_ASSIGN assignment_expression   # mod_assign (0 2)\n"
    "  | unary_expression BSL_ASSIGN assignment_expression   # bsl_assign (0 2)\n"
    "  | unary_expression BSR_ASSIGN assignment_expression   # bsr_assign (0 2)\n"
    "  | unary_expression BAND_ASSIGN assignment_expression  # band_assign (0 2)\n"
    "  | unary_expression BOR_ASSIGN assignment_expression   # bor_assign (0 2)\n"
    "  | unary_expression XOR_ASSIGN assignment_expression   # xor_assign (0 2)\n"
    "  | unary_expression COL_ASSIGN assignment_expression   # col_assign (0 2)\n"
    "  ;\n"
    "\n"

    // §6.5.17  Expression (comma)
    "expression : assignment_expression                      # 0\n"
    "           | expression ',' assignment_expression        # comma (0 2)\n"
    "           ;\n"
    "\n"

    // §6.6  Constant expression
    "constant_expression : conditional_expression  # 0\n"
    "                    ;\n"
    "\n"

    // ================================================================
    // A.2.3  Statements  (§6.8)
    // ================================================================

    "statement : labeled_statement     # 0\n"
    "          | compound_statement    # 0\n"
    "          | expression_statement  # 0\n"
    "          | selection_statement   # 0\n"
    "          | iteration_statement   # 0\n"
    "          | jump_statement        # 0\n"
    "          | try_statement         # 0\n"
    "          | throw_statement       # 0\n"
    "          | delete_statement      # 0\n"
    "          | defer_statement       # 0\n"
    "          | match_statement       # 0\n"
    "          ;\n"
    "\n"

    // §6.8.1  Labeled statements
    "labeled_statement"
    "  : IDENT ':' statement                         # label (0 2)\n"
    "  | CASE constant_expression ':' statement       # case (1 3)\n"
    "  | CASE constant_expression ELLIPSIS constant_expression ':' statement\n"
    "                                                 # case_range (1 3 5)\n"
    "  | DEFAULT ':' statement                        # default (2)\n"
    "  ;\n"
    "\n"

    // §6.8.2  Compound statement
    "compound_statement : '{' block_item_list_opt '}'   # block (1)\n"
    "                   ;\n"
    "\n"

    "block_item_list_opt :                  \n"
    "                    | block_item_list  # 0\n"
    "                    ;\n"
    "\n"

    "block_item_list : block_item                    # 0\n"
    "                | block_item_list block_item     # stmt_list (0 1)\n"
    "                ;\n"
    "\n"

    "block_item : declaration  # 0\n"
    "           | statement    # 0\n"
    "           | ctor_call_decl # 0\n"
    "           ;\n"
    "\n"
    // Constructor-call declaration: ClassName var(expr, expr, ...);
    // Parses Foo x(1, 2); as a variable declaration + ctor invocation.
    // Cfront pattern: separate the declaration from the initialization.
    "ctor_call_decl\n"
    "  : declaration_specifiers IDENT '(' argument_expression_list ')' ';'\n"
    "                                              # ctor_decl (0 1 3)\n"
    "  ;\n"
    "\n"

    // §6.8.3  Expression statement
    "expression_statement : expression_opt ';'  # expr_stmt (0)\n"
    "                     ;\n"
    "\n"

    "expression_opt :             \n"
    "               | expression  # 0\n"
    "               ;\n"
    "\n"

    // §6.8.4  Selection statements (if/else, switch)
    "selection_statement"
    "  : IF '(' expression ')' statement                      # if (2 4)\n"
    "  | IF '(' expression ')' statement ELSE statement       # if_else (2 4 6)\n"
    "  | SWITCH '(' expression ')' compound_statement         # switch (2 4)\n"
    "  ;\n"
    "\n"

    // §6.8.5  Iteration statements
    // C++ extension: range-for (for (type id : expr))
    "iteration_statement"
    "  : WHILE '(' expression ')' statement                   # while (2 4)\n"
    "  | DO statement WHILE '(' expression ')' ';'            # do_while (1 4)\n"
    "  | FOR '(' expression_opt ';' expression_opt ';' expression_opt ')' statement\n"
    "                                                         # for (2 4 6 8)\n"
    "  | FOR '(' declaration expression_opt ';' expression_opt ')' statement\n"
    "                                                         # for_decl (2 3 5 7)\n"
    "  | FOR '(' declaration_specifiers declarator ':' expression ')' statement\n"
    "                                                         # for_range (2 3 5 7)\n"
    "  ;\n"
    "\n"

    // §6.8.6  Jump statements
    "jump_statement"
    "  : GOTO IDENT ';'                # goto (1)\n"
    "  | GOTO '*' expression ';'       # goto_indirect (2)\n"
    "  | CONTINUE ';'                  # continue\n"
    "  | BREAK ';'                     # break\n"
    "  | RETURN expression_opt ';'     # return_val (1)\n"
    "  | RETURN expression ',' expression ';'\n"
    "                                  # return_multi (1 3)\n"
    "  ;\n"
    "\n"

    // ================================================================
    // C++ Extensions: try/catch, throw, delete, defer, match
    // ================================================================

    // try / catch
    "try_statement : TRY compound_statement catch_list  # try (1 2)\n"
    "              ;\n"
    "\n"

    "catch_list : catch_clause                   # 0\n"
    "           | catch_list catch_clause         # catch_list (0 1)\n"
    "           ;\n"
    "\n"

    "catch_clause"
    "  : CATCH '(' declaration_specifiers declarator ')' compound_statement\n"
    "                                              # catch (2 3 5)\n"
    "  | CATCH '(' ELLIPSIS ')' compound_statement # catch_all (4)\n"
    "  ;\n"
    "\n"

    // throw
    "throw_statement : THROW ';'              # throw\n"
    "                | THROW expression ';'   # throw_expr (1)\n"
    "                ;\n"
    "\n"

    // delete
    "delete_statement : DELETE expression ';'          # delete (1)\n"
    "                 | DELETE '[' ']' expression ';'  # delete_array (3)\n"
    "                 ;\n"
    "\n"

    // defer (Go/Zig-style deferred execution)
    "defer_statement : DEFER statement   # defer (1)\n"
    "                ;\n"
    "\n"

    // match (Rust-style pattern matching)
    "match_statement"
    "  : MATCH '(' expression ')' '{' match_arm_list '}'\n"
    "                                              # match (2 5)\n"
    "  ;\n"
    "\n"

    "match_arm_list :                                \n"
    "               | match_arm_list match_arm       # match_arms (0 1)\n"
    "               ;\n"
    "\n"

    "match_arm"
    "  : match_patterns FAT_ARROW match_body   # match_arm (0 2)\n"
    "  | IDENT FAT_ARROW match_body            # match_wild (0 2)\n"
    "  ;\n"
    "\n"

    "match_patterns : constant_expression                    # 0\n"
    "               | match_patterns '|' constant_expression # match_pats (0 2)\n"
    "               ;\n"
    "\n"

    "match_body : statement          # 0\n"
    "           | compound_statement # 0\n"
    "           ;\n"
    "\n"

    // ================================================================
    // C++ Extensions: classes
    // ================================================================

    "class_definition"
    "  : CLASS IDENT '{' class_body '}' ';'\n"
    "                                              # class_def (1 3)\n"
    "  | CLASS IDENT ':' access_specifier IDENT '{' class_body '}' ';'\n"
    "                                              # class_inherit (1 3 4 6)\n"
    "  ;\n"
    "\n"

    "access_specifier : PUBLIC     # 0\n"
    "                 | PRIVATE    # 0\n"
    "                 | PROTECTED  # 0\n"
    "                 ;\n"
    "\n"

    "class_body :                                    \n"
    "           | class_body class_member            # class_body (0 1)\n"
    "           ;\n"
    "\n"

    "class_member"
    "  : access_specifier ':'                        # access (0)\n"
    "  | declaration                                 # 0\n"
    "  | method_definition                           # 0\n"
    "  | constructor_definition                      # 0\n"
    "  | destructor_definition                       # 0\n"
    "  | operator_method_definition                  # 0\n"
    "  ;\n"
    "\n"

    "method_definition"
    "  : declaration_specifiers declarator compound_statement\n"
    "                                              # method (0 1 2)\n"
    "  | declaration_specifiers declarator ';'      # method_proto (0 1)\n"
    "  ;\n"
    "\n"

    "constructor_definition"
    "  : IDENT '(' parameter_type_list_opt ')' compound_statement\n"
    "                                              # ctor (0 2 4)\n"
    "  ;\n"
    "\n"

    "destructor_definition"
    "  : '~' IDENT '(' ')' compound_statement      # dtor (1 4)\n"
    "  ;\n"
    "\n"

    "operator_method_definition"
    "  : declaration_specifiers OPERATOR overload_op '(' parameter_type_list_opt ')' compound_statement\n"
    "                                              # oper_method (0 2 4 6)\n"
    "  ;\n"
    "\n"

    "overload_op : '+' # 0 | '-' # 0 | '*' # 0 | '/' # 0 | '%' # 0\n"
    "            | EQ_OP # 0 | NE_OP # 0\n"
    "            | '<' # 0 | '>' # 0 | LE_OP # 0 | GE_OP # 0\n"
    "            | THREE_WAY # 0\n"
    "            | BSL_OP # 0 | BSR_OP # 0\n"
    "            | '(' ')' # funcall_op\n"
    "            | '[' ']' # subscript_op\n"
    "            ;\n"
    "\n"

    // ================================================================
    // C++ Extensions: namespace, using
    // ================================================================

    "namespace_definition"
    "  : NAMESPACE IDENT '{' translation_unit '}'  # namespace_def (1 3)\n"
    "  ;\n"
    "\n"

    "using_declaration"
    "  : USING NAMESPACE IDENT ';'   # using_ns (2)\n"
    "  | USING IDENT ';'             # using_decl (1)\n"
    "  | PREFER IDENT ';'            # prefer (1)\n"
    "  ;\n"
    "\n"
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
                if (ti->str == "sizeof")       return GT_SIZEOF;
                if (ti->str == "inline")       return GT_INLINE;
                if (ti->str == "signed")       return GT_SIGNED;
                if (ti->str == "unsigned")     return GT_UNSIGNED;
                if (ti->str == "string")       return GT_STRING_T;
                if (ti->str == "auto")         return GT_AUTO;
                if (ti->str == "bool")         return GT_BOOL;
                if (ti->str == "_Bool")        return GT_BOOL;
                if (ti->str == "void")         return GT_VOID;
                if (ti->str == "char")         return GT_CHAR;
                if (ti->str == "short")        return GT_SHORT;
                if (ti->str == "int")          return GT_INT;
                if (ti->str == "long")         return GT_LONG;
                if (ti->str == "float")        return GT_FLOAT;
                if (ti->str == "double")       return GT_DOUBLE;
                if (ti->str == "int8_t")       return GT_INT8;
                if (ti->str == "int16_t")      return GT_INT16;
                if (ti->str == "int32_t")      return GT_INT32;
                if (ti->str == "int64_t")      return GT_INT64;
                if (ti->str == "uint8_t")      return GT_UINT8;
                if (ti->str == "uint16_t")     return GT_UINT16;
                if (ti->str == "uint32_t")     return GT_UINT32;
                if (ti->str == "uint64_t")     return GT_UINT64;
                if (ti->str == "stringstream") return GT_SSTREAM;
                if (ti->str == "ifstream")     return GT_IFSTREAM;
                if (ti->str == "ofstream")     return GT_OFSTREAM;
                if (ti->str == "fstream")      return GT_FSTREAM;
                if (ti->str == "ostream")      return GT_OSTREAM;
                // Access specifiers + virtual
                if (ti->str == "public")       return GT_PUBLIC;
                if (ti->str == "private")      return GT_PRIVATE;
                if (ti->str == "protected")    return GT_PROTECTED;
                if (ti->str == "virtual")      return GT_VIRTUAL;
                if (ti->str == "alignof" || ti->str == "_Alignof")
                    return GT_ALIGNOF;
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
    case TokenID::tkMod:       return '%';
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
