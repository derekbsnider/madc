// madc embedded ctype.h — character classification and conversion.
// The implementations come from libc via dlsym fallback, but the prototypes
// must be DECLARED here: real <cctype> does `namespace std { using ::isalnum;
// ... }`, and a using-declaration needs a global-scope declaration to bind to
// (otherwise "'isalnum' is not a declaration in '::'"). All take and return a
// signed int (declare the real return type — see embedded-headers rule).
int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);
int toascii(int c);
