/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 9 "sgramm.y"

#include <stdio.h>
#include <ctype.h>
#include <assert.h>

/* The following is necessary if we use Gecko (GLR parser) with byacc/bison/msta parser. */
#define yylval gp_yylval
#define yylex gp_yylex
#define yyerror gp_yyerror
#define yyparse gp_yyparse
#define yychar gp_yychar
#define yynerrs gp_yynerrs
#define yydebug gp_yydebug
#define yyerrflag gp_yyerrflag
#define yyssp gp_yyssp
#define yyval gp_yyval
#define yyvsp gp_yyvsp
#define yylhs gp_yylhs
#define yylen gp_yylen
#define yydefred gp_yydefred
#define yydgoto gp_yydgoto
#define yysindex gp_yysindex
#define yyrindex gp_yyrindex
#define yygindex gp_yygindex
#define yytable gp_yytable
#define yycheck gp_yycheck
#define yyss gp_yyss
#define yyvs gp_yyvs

  /* The following structure describes syntax grammar terminal. */
  struct sterm {
    char *repr; /* terminal representation. */
    int code;   /* terminal code. */
    int num;    /* order number. */
    int priority;
    enum gp_assoc assoc; /* undefined for prioirty < 0 */
  };

  /* The following structure describes syntax grammar terminal. */
  struct sassoc {
    char *repr; /* terminal representation. */
    enum gp_assoc assoc;
    int priority;
    bool used_p;
  };

  /* The following structure describes abstract node. */
  struct sanode {
    char *name; /* anode name. */
    int code;    /* anode code. */
    int num;    /* order number. */
  };

  /* The following structure describes syntax grammar rule. */
  struct srule {
    int guard_num; /* rule guard number */
    /* The following members are left hand side nonterminal
       representation and abstract node name (if any) for the rule. */
    char *lhs, *anode;
    int rhs_len; /* The following is length of right hand side of the rule. */
    /* Terminal/nonterminal representations in RHS of the rule.  The array end marker is NULL. */
    char **rhs;
    int *trans; /* The translations numbers. */
  };

  /* Current priority for terminal associativity */
  static int curr_priority;
  
  /* The following vlos contain all syntax terminal, assoc, anode, and rule structures. */
  static vlo_t sterms, assocs, sanodes, srules;
  static os_t assocs_os; /* container for sassocs */
  
  /* The following contain all right hand sides and translations arrays.
     See members rhs, trans in structure `rule'. */
  static os_t srhs, strans;

  /* This variable is used in yacc action to process alternatives. */
  static char *slhs;

  /* Forward declarations. */
  static void add_assoc (const char *repr, int priority, enum gp_assoc assoc);
  extern int yyerror (void *g, const char *str);
  extern int yylex (void *);
  extern int yyparse (void *);


#line 158 "sgramm.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif


/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    IDENT = 258,                   /* IDENT  */
    SEM_IDENT = 259,               /* SEM_IDENT  */
    CHAR = 260,                    /* CHAR  */
    NUMBER = 261,                  /* NUMBER  */
    TERM = 262,                    /* TERM  */
    LEFT = 263,                    /* LEFT  */
    RIGHT = 264,                   /* RIGHT  */
    NONASSOC = 265,                /* NONASSOC  */
    ANODE = 266                    /* ANODE  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif
/* Token kinds.  */
#define YYEMPTY -2
#define YYEOF 0
#define YYerror 256
#define YYUNDEF 257
#define IDENT 258
#define SEM_IDENT 259
#define CHAR 260
#define NUMBER 261
#define TERM 262
#define LEFT 263
#define RIGHT 264
#define NONASSOC 265
#define ANODE 266

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 99 "sgramm.y"

  void *ref;
  int num;
  enum gp_assoc assoc;

#line 236 "sgramm.c"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void *g);



/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_IDENT = 3,                      /* IDENT  */
  YYSYMBOL_SEM_IDENT = 4,                  /* SEM_IDENT  */
  YYSYMBOL_CHAR = 5,                       /* CHAR  */
  YYSYMBOL_NUMBER = 6,                     /* NUMBER  */
  YYSYMBOL_TERM = 7,                       /* TERM  */
  YYSYMBOL_LEFT = 8,                       /* LEFT  */
  YYSYMBOL_RIGHT = 9,                      /* RIGHT  */
  YYSYMBOL_NONASSOC = 10,                  /* NONASSOC  */
  YYSYMBOL_ANODE = 11,                     /* ANODE  */
  YYSYMBOL_12_ = 12,                       /* ';'  */
  YYSYMBOL_13_ = 13,                       /* '='  */
  YYSYMBOL_14_ = 14,                       /* '|'  */
  YYSYMBOL_15_ = 15,                       /* '?'  */
  YYSYMBOL_16_ = 16,                       /* '#'  */
  YYSYMBOL_17_ = 17,                       /* '-'  */
  YYSYMBOL_18_ = 18,                       /* '('  */
  YYSYMBOL_19_ = 19,                       /* ')'  */
  YYSYMBOL_YYACCEPT = 20,                  /* $accept  */
  YYSYMBOL_file = 21,                      /* file  */
  YYSYMBOL_opt_sem = 22,                   /* opt_sem  */
  YYSYMBOL_terms = 23,                     /* terms  */
  YYSYMBOL_anodes = 24,                    /* anodes  */
  YYSYMBOL_assocs = 25,                    /* assocs  */
  YYSYMBOL_number = 26,                    /* number  */
  YYSYMBOL_rule = 27,                      /* rule  */
  YYSYMBOL_28_1 = 28,                      /* $@1  */
  YYSYMBOL_rhs = 29,                       /* rhs  */
  YYSYMBOL_alt = 30,                       /* alt  */
  YYSYMBOL_seq = 31,                       /* seq  */
  YYSYMBOL_guard = 32,                     /* guard  */
  YYSYMBOL_trans = 33,                     /* trans  */
  YYSYMBOL_numbers = 34                    /* numbers  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  13
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   47

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  20
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  15
/* YYNRULES -- Number of rules.  */
#define YYNRULES  41
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  54

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   266


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,    16,     2,     2,     2,     2,
      18,    19,     2,     2,     2,    17,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    12,
       2,    13,     2,    15,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,    14,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint8 yyrline[] =
{
       0,   114,   114,   114,   114,   114,   115,   115,   115,   115,
     118,   119,   121,   130,   133,   140,   143,   147,   151,   152,
     153,   156,   157,   160,   160,   162,   162,   164,   184,   188,
     198,   201,   202,   205,   206,   207,   212,   217,   218,   221,
     222,   226
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "IDENT", "SEM_IDENT",
  "CHAR", "NUMBER", "TERM", "LEFT", "RIGHT", "NONASSOC", "ANODE", "';'",
  "'='", "'|'", "'?'", "'#'", "'-'", "'('", "')'", "$accept", "file",
  "opt_sem", "terms", "anodes", "assocs", "number", "rule", "$@1", "rhs",
  "alt", "seq", "guard", "trans", "numbers", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-10)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int8 yypact[] =
{
      34,   -10,   -10,   -10,   -10,   -10,   -10,    19,     6,     9,
      28,   -10,   -10,   -10,     6,     9,    28,   -10,     0,   -10,
     -10,     0,   -10,   -10,   -10,   -10,    20,   -10,    -1,   -10,
     -10,   -10,     4,   -10,   -10,   -10,   -10,   -10,   -10,     5,
      10,   -10,   -10,     2,   -10,   -10,    18,   -10,   -10,   -10,
      -3,   -10,   -10,   -10
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,    23,    13,    18,    19,    20,    15,     0,    10,    10,
      10,     9,    30,     1,    10,    10,    10,     5,    21,    11,
       6,    21,     8,    16,    17,     7,    10,    26,    33,     2,
       4,     3,     0,    12,    14,    30,    24,    28,    29,    34,
      31,    22,    25,    38,    35,    36,     0,    27,    39,    32,
       0,    40,    41,    37
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -10,   -10,    -9,    29,    30,    32,    14,    39,   -10,   -10,
      12,   -10,   -10,   -10,   -10
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
       0,     7,    20,     8,     9,    10,    33,    11,    12,    26,
      27,    28,    47,    40,    50
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int8 yytable[] =
{
      22,    25,    37,    51,    38,    29,    30,    31,    43,    18,
      41,    44,    21,    32,    52,    39,    53,    36,    19,    13,
      48,    19,    45,     1,    49,    46,     2,     3,     4,     5,
       6,    23,    19,    24,    35,    34,    14,    15,     1,    16,
      19,     2,     3,     4,     5,     6,    17,    42
};

static const yytype_int8 yycheck[] =
{
       9,    10,     3,     6,     5,    14,    15,    16,     3,     3,
       6,     6,     3,    13,    17,    16,    19,    26,    12,     0,
      18,    12,    17,     4,     6,    15,     7,     8,     9,    10,
      11,     3,    12,     5,    14,    21,     7,     7,     4,     7,
      12,     7,     8,     9,    10,    11,     7,    35
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     4,     7,     8,     9,    10,    11,    21,    23,    24,
      25,    27,    28,     0,    23,    24,    25,    27,     3,    12,
      22,     3,    22,     3,     5,    22,    29,    30,    31,    22,
      22,    22,    13,    26,    26,    14,    22,     3,     5,    16,
      33,     6,    30,     3,     6,    17,    15,    32,    18,     6,
      34,     6,    17,    19
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    20,    21,    21,    21,    21,    21,    21,    21,    21,
      22,    22,    23,    23,    24,    24,    25,    25,    25,    25,
      25,    26,    26,    28,    27,    29,    29,    30,    31,    31,
      31,    32,    32,    33,    33,    33,    33,    33,    33,    34,
      34,    34
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     3,     3,     3,     2,     2,     2,     2,     1,
       0,     1,     3,     1,     3,     1,     2,     2,     1,     1,
       1,     0,     2,     0,     4,     3,     1,     3,     2,     2,
       0,     0,     2,     0,     1,     2,     2,     5,     2,     0,
       2,     2
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (g, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, g); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void *g)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (g);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, void *g)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep, g);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule, void *g)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)], g);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule, g); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, void *g)
{
  YY_USE (yyvaluep);
  YY_USE (g);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void *g)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (g);
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 12: /* terms: terms IDENT number  */
#line 121 "sgramm.y"
                           {
          struct sterm term;
          term.repr = (char *) (yyvsp[-1].ref);
          term.code = (yyvsp[0].num);
	  term.priority = -1;
	  term.assoc = GP_NON_ASSOC;
          term.num = (int) (VLO_LENGTH (sterms) / sizeof (term));
	  VLO_ADD_MEMORY (sterms, &term, sizeof (term));
        }
#line 1286 "sgramm.c"
    break;

  case 14: /* anodes: anodes IDENT number  */
#line 133 "sgramm.y"
                             {
          struct sanode anode;
          anode.name = (char *) (yyvsp[-1].ref);
          anode.code = (yyvsp[0].num);
          anode.num = (int) (VLO_LENGTH (sanodes) / sizeof (anode));
	  VLO_ADD_MEMORY (sanodes, &anode, sizeof (anode));
        }
#line 1298 "sgramm.c"
    break;

  case 16: /* assocs: assocs IDENT  */
#line 143 "sgramm.y"
                      {
           (yyval.assoc) = (yyvsp[-1].assoc);
	   add_assoc ((char *) (yyvsp[0].ref), curr_priority, (yyvsp[-1].assoc));
         }
#line 1307 "sgramm.c"
    break;

  case 17: /* assocs: assocs CHAR  */
#line 147 "sgramm.y"
                     {
           (yyval.assoc) = (yyvsp[-1].assoc);
	   add_assoc ((char *) (yyvsp[0].ref), curr_priority, (yyvsp[-1].assoc));
         }
#line 1316 "sgramm.c"
    break;

  case 18: /* assocs: LEFT  */
#line 151 "sgramm.y"
              {(yyval.assoc) = GP_LEFT_ASSOC; curr_priority++;}
#line 1322 "sgramm.c"
    break;

  case 19: /* assocs: RIGHT  */
#line 152 "sgramm.y"
                {(yyval.assoc) = GP_RIGHT_ASSOC; curr_priority++;}
#line 1328 "sgramm.c"
    break;

  case 20: /* assocs: NONASSOC  */
#line 153 "sgramm.y"
                  {(yyval.assoc) = GP_NON_ASSOC; curr_priority++;}
#line 1334 "sgramm.c"
    break;

  case 21: /* number: %empty  */
#line 156 "sgramm.y"
         { (yyval.num) = -1; }
#line 1340 "sgramm.c"
    break;

  case 22: /* number: '=' NUMBER  */
#line 157 "sgramm.y"
                    { (yyval.num) = (yyvsp[0].num); }
#line 1346 "sgramm.c"
    break;

  case 23: /* $@1: %empty  */
#line 160 "sgramm.y"
                 { slhs = (char *) (yyvsp[0].ref); }
#line 1352 "sgramm.c"
    break;

  case 27: /* alt: seq trans guard  */
#line 165 "sgramm.y"
      {
        struct srule rule;
  	int end_marker = -1;

	OS_TOP_ADD_MEMORY (strans, &end_marker, sizeof (int));
	rule.guard_num = (yyvsp[0].num);
	rule.lhs = slhs;
	rule.anode = (char *) (yyvsp[-1].ref);
	rule.rhs_len = (int) (OS_TOP_LENGTH (srhs) / sizeof (char *));
        OS_TOP_EXPAND (srhs, sizeof (char *));
	rule.rhs = (char **) OS_TOP_BEGIN (srhs);
	rule.rhs[rule.rhs_len] = NULL;
	OS_TOP_FINISH (srhs);
	rule.trans = (int *) OS_TOP_BEGIN (strans);
	OS_TOP_FINISH (strans);
        VLO_ADD_MEMORY (srules, &rule, sizeof (rule));
      }
#line 1374 "sgramm.c"
    break;

  case 28: /* seq: seq IDENT  */
#line 184 "sgramm.y"
                {
        char *repr = (char *) (yyvsp[0].ref);
        OS_TOP_ADD_MEMORY (srhs, &repr, sizeof (repr));
      }
#line 1383 "sgramm.c"
    break;

  case 29: /* seq: seq CHAR  */
#line 188 "sgramm.y"
               {
        struct sterm term;
        term.repr = (char *) (yyvsp[0].ref);
        term.code = term.repr[1];
        term.num = (int) (VLO_LENGTH (sterms) / sizeof (term));
	term.priority = -1;
	term.assoc = GP_NON_ASSOC;
        VLO_ADD_MEMORY (sterms, &term, sizeof (term));
        OS_TOP_ADD_MEMORY (srhs, &term.repr, sizeof (term.repr));
      }
#line 1398 "sgramm.c"
    break;

  case 31: /* guard: %empty  */
#line 201 "sgramm.y"
                   { (yyval.num) = -1; }
#line 1404 "sgramm.c"
    break;

  case 32: /* guard: '?' NUMBER  */
#line 202 "sgramm.y"
                   { (yyval.num) = (yyvsp[0].num); }
#line 1410 "sgramm.c"
    break;

  case 33: /* trans: %empty  */
#line 205 "sgramm.y"
        { (yyval.ref) = NULL; }
#line 1416 "sgramm.c"
    break;

  case 34: /* trans: '#'  */
#line 206 "sgramm.y"
            { (yyval.ref) = NULL; }
#line 1422 "sgramm.c"
    break;

  case 35: /* trans: '#' NUMBER  */
#line 207 "sgramm.y"
                   {
          int symb_num = (yyvsp[0].num);
          (yyval.ref) = NULL;
          OS_TOP_ADD_MEMORY (strans, &symb_num, sizeof (int));
        }
#line 1432 "sgramm.c"
    break;

  case 36: /* trans: '#' '-'  */
#line 212 "sgramm.y"
                {
          int symb_num = GP_NIL_TRANSLATION_NUMBER;
          (yyval.ref) = NULL;
         OS_TOP_ADD_MEMORY (strans, &symb_num, sizeof (int));
        }
#line 1442 "sgramm.c"
    break;

  case 37: /* trans: '#' IDENT '(' numbers ')'  */
#line 217 "sgramm.y"
                                  { (yyval.ref) = (yyvsp[-3].ref); }
#line 1448 "sgramm.c"
    break;

  case 38: /* trans: '#' IDENT  */
#line 218 "sgramm.y"
                  { (yyval.ref) = (yyvsp[0].ref); }
#line 1454 "sgramm.c"
    break;

  case 40: /* numbers: numbers NUMBER  */
#line 222 "sgramm.y"
                         {
            int symb_num = (yyvsp[0].num);
            OS_TOP_ADD_MEMORY (strans, &symb_num, sizeof (int));
          }
#line 1463 "sgramm.c"
    break;

  case 41: /* numbers: numbers '-'  */
#line 226 "sgramm.y"
                      {
            int symb_num = GP_NIL_TRANSLATION_NUMBER;
            OS_TOP_ADD_MEMORY (strans, &symb_num, sizeof (int));
          }
#line 1472 "sgramm.c"
    break;


#line 1476 "sgramm.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (g, YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, g);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, g);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (g, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, g);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, g);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 232 "sgramm.y"

/* The following is current input character of the grammar description. */
static const char *curr_ch;

/* The following is current line number of the grammar description. */
static int ln;

/* The following contains all representation of the syntax tokens. */
static os_t stoks;

/* The following is number of syntax terminal and syntax rules being read. */
static int nsterm, nsrule;

/* The following implements lexical analyzer for yacc code. */
int yylex (void *g) {
  int c;
  int n_errs = 0;

  for (;;) {
    c = *curr_ch++;
    switch (c) {
    case '\0': return 0;
    case '\n': ln++;
    case '\t':
    case ' ': break;
    case '/':
      c = *curr_ch++;
      if (c != '*' && n_errs == 0) {
        n_errs++;
        curr_ch--;
        yyerror (g, "invalid input character /");
      }
      for (;;) {
        c = *curr_ch++;
        if (c == '\0') yyerror (g, "unfinished comment");
        if (c == '\n') ln++;
        if (c == '*') {
          c = *curr_ch++;
          if (c == '/') break;
          curr_ch--;
        }
      }
      break;
    case '=':
    case '#':
    case '?':
    case '|':
    case ';':
    case '-':
    case '(':
    case ')': return c;
    case '\'':
      OS_TOP_ADD_BYTE (stoks, '\'');
      yylval.num = *curr_ch++;
      OS_TOP_ADD_BYTE (stoks, (char) yylval.num);
      if (*curr_ch++ != '\'') yyerror (g, "invalid character");
      OS_TOP_ADD_BYTE (stoks, '\'');
      OS_TOP_ADD_BYTE (stoks, '\0');
      yylval.ref = OS_TOP_BEGIN (stoks);
      OS_TOP_FINISH (stoks);
      return CHAR;
    default:
      if (isalpha (c) || c == '_') {
        OS_TOP_ADD_BYTE (stoks, (char) c);
        while ((c = *curr_ch++) != '\0' && (isalnum (c) || c == '_')) OS_TOP_ADD_BYTE (stoks, (char) c);
        curr_ch--;
        OS_TOP_ADD_BYTE (stoks, '\0');
        yylval.ref = OS_TOP_BEGIN (stoks);
        if (strcmp ((char *) yylval.ref, "TERM") == 0) {
          OS_TOP_NULLIFY (stoks);
          return TERM;
        }
        if (strcmp ((char *) yylval.ref, "LEFT") == 0) {
          OS_TOP_NULLIFY (stoks);
          return LEFT;
        }
        if (strcmp ((char *) yylval.ref, "RIGHT") == 0) {
          OS_TOP_NULLIFY (stoks);
          return RIGHT;
        }
        if (strcmp ((char *) yylval.ref, "NONASSOC") == 0) {
          OS_TOP_NULLIFY (stoks);
          return NONASSOC;
        }
        if (strcmp ((char *) yylval.ref, "ANODE") == 0) {
          OS_TOP_NULLIFY (stoks);
          return ANODE;
        }
        OS_TOP_FINISH (stoks);
        while ((c = *curr_ch++) != '\0')
          if (c == '\n')
            ln++;
          else if (c != '\t' && c != ' ')
            break;
        if (c != ':') curr_ch--;
        return (c == ':' ? SEM_IDENT : IDENT);
      } else if (isdigit (c)) {
        yylval.num = c - '0';
        while ((c = *curr_ch++) != '\0' && isdigit (c)) yylval.num = yylval.num * 10 + (c - '0');
        curr_ch--;
        return NUMBER;
      } else {
        n_errs++;
        if (n_errs == 1) {
          char str[100];

          if (isprint (c)) {
            snprintf (str, sizeof (str), "invalid input character '%c'", c);
            yyerror (g, str);
          } else
            yyerror (g, "invalid input character");
        }
      }
    }
  }
}

/* The following implements syntactic error diagnostic function yacc code. */
int yyerror (void *g, const char *str GP_UNUSED) {
  error (g, GP_DESCRIPTION_SYNTAX_ERROR_CODE, "description syntax error on ln %d", ln);
  return 0;
}

/* The following function is used to sort array of syntax terminals by names. */
static int sterm_name_cmp (const void *t1, const void *t2) {
  return strcmp (((struct sterm *) t1)->repr, ((struct sterm *) t2)->repr);
}

/* The following function is used to sort array of syntax terminals by order number. */
static int sterm_num_cmp (const void *t1, const void *t2) {
  return ((struct sterm *) t1)->num - ((struct sterm *) t2)->num;
}

static void add_assoc (const char *repr, int priority, enum gp_assoc assoc) {
  OS_TOP_EXPAND (assocs_os, sizeof (struct sassoc));
  struct sassoc *sassoc = OS_TOP_BEGIN (assocs_os);
  OS_TOP_FINISH (assocs_os);
  sassoc->repr = (char *) repr;
  sassoc->priority = priority;
  sassoc->assoc = assoc;
  VLO_ADD_MEMORY (assocs, &sassoc, sizeof (sassoc));
}

static hash_table_t assoc_htab;

static uint64_t assoc_hash (hash_table_entry_t s) { /* return hash of assoc */
  const char *str = ((struct sassoc *) s)->repr;
  return hash (str, strlen (str), 42);
}

static bool assoc_eq (hash_table_entry_t s1, hash_table_entry_t s2) { /* Equality of assocs. */
  return strcmp (((struct sassoc *) s1)->repr, ((struct sassoc *) s2)->repr) == 0;
}

static struct sassoc *find_assoc (char *repr) {
  struct sassoc assoc;
  assoc.repr = repr;
  hash_table_entry_t *res = find_hash_table_entry (assoc_htab, &assoc, false);
  return (struct sassoc *) *res;
}

static void insert_assoc (struct sassoc *assoc) {
  hash_table_entry_t *entry = find_hash_table_entry (assoc_htab, assoc, true);
  assert (*entry == NULL);
  *entry = (hash_table_entry_t) assoc;
}

/* The following function is used to sort array of anodes by names. */
static int sanode_name_cmp (const void *t1, const void *t2) {
  return strcmp (((struct sanode *) t1)->name, ((struct sanode *) t2)->name);
}

/* The following function is used to sort array of anodes by order number. */
static int sanode_num_cmp (const void *t1, const void *t2) {
  return ((struct sanode *) t1)->num - ((struct sanode *) t2)->num;
}

static void free_sgrammar (void);

/* The following is major function which parses the description and transforms it into IR. */
static int set_sgrammar (struct grammar *g, const char *grammar_name) {
  int i, j, num;
  struct sterm *term, *prev, *arr;
  int code;

  ln = 1;
  if ((code = setjmp (g->error_longjump_buff)) != 0) {
    free_sgrammar ();
    return code;
  }
  curr_priority = 0;
  OS_CREATE (stoks, g->alloc, 0);
  VLO_CREATE (sterms, g->alloc, 0);
  VLO_CREATE (assocs, g->alloc, 0);
  OS_CREATE (assocs_os, g->alloc, 0);
  VLO_CREATE (sanodes, g->alloc, 0);
  VLO_CREATE (srules, g->alloc, 0);
  OS_CREATE (srhs, g->alloc, 0);
  OS_CREATE (strans, g->alloc, 0);
  assoc_htab = create_hash_table (g->alloc, 80, assoc_hash, assoc_eq);
  curr_ch = grammar_name;
  yyparse (g);
  /* sort array of syntax terminals by names. */
  num = (int) (VLO_LENGTH (sterms) / sizeof (struct sterm));
  arr = (struct sterm *) VLO_BEGIN (sterms);
  qsort (arr, (size_t) num, sizeof (struct sterm), sterm_name_cmp);
  /* Check different codes for the same syntax terminal and remove duplicates. */
  for (i = j = 0, prev = NULL; i < num; i++) {
    term = arr + i;
    if (prev == NULL || strcmp (prev->repr, term->repr) != 0) {
      prev = term;
      arr[j++] = *term;
    } else if (term->code != -1 && prev->code != -1 && prev->code != term->code) {
      char str[GP_MAX_ERROR_MESSAGE_LENGTH / 2];

      strncpy (str, prev->repr, sizeof (str));
      str[sizeof (str) - 1] = '\0';
      error (g, GP_REPEATED_TERM_CODE, "term %s described repeatedly with different code", str);
    } else if (prev->code != -1) {
      prev->code = term->code;
    }
  }
  VLO_SHORTEN (sterms, (size_t) (num - j) * sizeof (struct sterm));
  num = j;
  /* sort array of syntax terminals by order number. */
  qsort (arr, (size_t) num, sizeof (struct sterm), sterm_num_cmp);
  for (i = 0; i < (int) (VLO_LENGTH (assocs) / sizeof (struct sassoc *)); i++) {
    struct sassoc *assoc = ((struct sassoc **)VLO_BEGIN (assocs))[i];
    assoc->used_p = false;
    if (find_assoc (assoc->repr) != NULL) {
      error (g, GP_REPEATED_TERM_ASSOC, "term %s is repeteadly described in an associtivity clause", assoc->repr);
    } else {
      insert_assoc (assoc);
    }
  }
  /* Assign codes and priories */
  code = 256;
  for (i = 0; i < num; i++) {
    term = (struct sterm *) VLO_BEGIN (sterms) + i;
    if (term->code < 0) term->code = code++;
    struct sassoc *assoc = find_assoc (term->repr);
    if (assoc == NULL) continue;
    term->priority = assoc->priority;
    term->assoc = assoc->assoc;
    assoc->used_p = true;
  }
  for (i = 0; i < (int) (VLO_LENGTH (assocs) / sizeof (struct sassoc *)); i++) {
    struct sassoc *assoc = ((struct sassoc **) VLO_BEGIN (assocs))[i];
    if (!assoc->used_p)
      error (g, GP_UNDEFINED_TERM_ASSOC, "term %s described in associtivity clause is not defined", assoc->repr);
  }
  nsterm = nsrule = 0;

  /* sort array of syntax anodes by names. */
  struct sanode *anode, *prev_anode;
  num = (int) (VLO_LENGTH (sanodes) / sizeof (struct sanode));
  qsort (VLO_BEGIN (sanodes), (size_t) num, sizeof (struct sanode), sanode_name_cmp);
  /* Check different codes for the same anodes and remove duplicates. */
  for (i = j = 0, prev_anode = NULL; i < num; i++) {
    anode = (struct sanode *) VLO_BEGIN (sanodes) + i;
    if (prev_anode == NULL || strcmp (prev_anode->name, anode->name) != 0) {
      prev_anode = anode;
      ((struct sanode *) VLO_BEGIN (sanodes))[j++] = *anode;
    } else if (anode->code != -1 && prev_anode->code != -1 && prev_anode->code != anode->code) {
      char str[GP_MAX_ERROR_MESSAGE_LENGTH / 2];
      strncpy (str, prev_anode->name, sizeof (str));
      str[sizeof (str) - 1] = '\0';
      error (g, GP_REPEATED_ANODE_CODE, "anode %s described repeatedly with different code", str);
    } else if (prev_anode->code != -1) {
      prev_anode->code = anode->code;
    }
  }
  VLO_SHORTEN (sanodes, (size_t) (num - j) * sizeof (struct sanode));
  num = j;
  /* sort array of anodes by order number. */
  int anode_code = 0;
  qsort (VLO_BEGIN (sanodes), (size_t) num, sizeof (struct sanode), sanode_num_cmp);
  /* Assign anode codes */
  for (i = 0; i < num; i++) {
    anode = (struct sanode *) VLO_BEGIN (sanodes) + i;
    if (anode->code < 0) anode->code = anode_code++;
  }
  return 0;
}

/* The following frees IR. */
static void free_sgrammar (void) {
  OS_DELETE (strans);
  OS_DELETE (srhs);
  VLO_DELETE (srules);
  VLO_DELETE (assocs);
  OS_DELETE (assocs_os);
  delete_hash_table (assoc_htab);
  VLO_DELETE (sterms);
  VLO_DELETE (sanodes);
  OS_DELETE (stoks);
}

/* The following two functions implements functions used by YAEP. */
static const char *sread_terminal (int *code, int *priority, enum gp_assoc *assoc) {
  struct sterm *term;
  const char *name;

  term = &((struct sterm *) VLO_BEGIN (sterms))[nsterm];
  if ((char *) term >= (char *) VLO_BOUND (sterms)) return NULL;
  *code = term->code;
  *priority = term->priority;
  *assoc = term->assoc;
  name = term->repr;
  nsterm++;
  return name;
}

static const char *sread_rule (const char ***rhs, const char **abs_node, int **transl, int *guard_num) {
  struct srule *rule;
  const char *lhs;

  rule = &((struct srule *) VLO_BEGIN (srules))[nsrule];
  if ((char *) rule >= (char *) VLO_BOUND (srules)) return NULL;
  *guard_num = rule->guard_num;
  lhs = rule->lhs;
  *rhs = (const char **) rule->rhs;
  *abs_node = rule->anode;
  *transl = rule->trans;
  nsrule++;
  return lhs;
}

/* The following function parses grammar desrciption. */
int gp_parse_grammar (struct grammar *g, bool strict_p, const char *description)
{
  int code;

  assert (g != NULL);
  if ((code = set_sgrammar (g, description)) != 0) return code;
  code = gp_read_grammar (g, strict_p, sread_terminal, sread_rule);
  int num = (int) (VLO_LENGTH (sanodes) / sizeof (struct sanode));
  for (int i = 0; i < num; i++) {
    struct sanode *anode = (struct sanode *) VLO_BEGIN (sanodes) + i;
    gp_set_anode_code (g, anode->name, anode->code);
  }
  free_sgrammar ();
  return code;
}
