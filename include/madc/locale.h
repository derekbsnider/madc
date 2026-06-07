// madc embedded locale.h — locale constants + struct lconv
//
// The real <clocale> does `#include <locale.h>` then `namespace std { using
// ::lconv; using ::setlocale; using ::localeconv; }`. For the using-declaration
// to resolve, `struct lconv` and the two functions must be declared at global
// scope here. Layout is glibc's struct lconv verbatim (bits/types/struct_lconv.h)
// — declare real return types (localeconv returns `struct lconv *`, setlocale
// `char *`) so ternaries/derefs don't mismatch the generic dlsym `long` fallback.

#define LC_ALL       6
#define LC_COLLATE   3
#define LC_CTYPE     0
#define LC_MESSAGES  5
#define LC_MONETARY  4
#define LC_NUMERIC   1
#define LC_TIME      2

struct lconv
{
	char *decimal_point;
	char *thousands_sep;
	char *grouping;
	char *int_curr_symbol;
	char *currency_symbol;
	char *mon_decimal_point;
	char *mon_thousands_sep;
	char *mon_grouping;
	char *positive_sign;
	char *negative_sign;
	char int_frac_digits;
	char frac_digits;
	char p_cs_precedes;
	char p_sep_by_space;
	char n_cs_precedes;
	char n_sep_by_space;
	char p_sign_posn;
	char n_sign_posn;
	char int_p_cs_precedes;
	char int_p_sep_by_space;
	char int_n_cs_precedes;
	char int_n_sep_by_space;
	char int_p_sign_posn;
	char int_n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);
