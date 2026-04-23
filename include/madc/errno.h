// madc embedded errno.h — POSIX error constants
// glibc exposes errno via __errno_location(), so mirror the usual macro.
// Keep the function undeclared here and let madc resolve it through the
// normal external-call path when the macro expands.
#define errno (*(__errno_location()))

// Base POSIX errors
#define EPERM             1
#define ENOENT            2
#define ESRCH             3
#define EINTR             4
#define EIO               5
#define ENXIO             6
#define E2BIG             7
#define ENOEXEC           8
#define EBADF             9
#define ECHILD            10
#define EAGAIN            11
#define EWOULDBLOCK       11
#define ENOMEM            12
#define EACCES            13
#define EFAULT            14
#define EBUSY             16
#define EEXIST            17
#define EXDEV             18
#define ENODEV            19
#define ENOTDIR           20
#define EISDIR            21
#define EINVAL            22
#define ENFILE            23
#define EMFILE            24
#define ENOTTY            25
#define ETXTBSY           26
#define EFBIG             27
#define ENOSPC            28
#define ESPIPE            29
#define EROFS             30
#define EMLINK            31
#define EPIPE             32
#define EDOM              33
#define ERANGE            34

// System V / extended POSIX errors
#define EDEADLK           35
#define ENAMETOOLONG      36
#define ENOLCK            37
#define ENOSYS            38
#define ENOTEMPTY         39
#define ELOOP             40
#define ENOMSG            42
#define ENODATA           61
#define EOVERFLOW         75
#define EILSEQ            84

// Socket / network errors (Linux x86-64 values)
#define EUSERS            87
#define ENOTSOCK          88
#define EDESTADDRREQ      89
#define EMSGSIZE          90
#define EPROTOTYPE        91
#define ENOPROTOOPT       92
#define EPROTONOSUPPORT   93
#define ESOCKTNOSUPPORT   94
#define EOPNOTSUPP        95
#define EPFNOSUPPORT      96
#define EAFNOSUPPORT      97
#define EADDRINUSE        98
#define EADDRNOTAVAIL     99
#define ENETDOWN          100
#define ENETUNREACH       101
#define ENETRESET         102
#define ECONNABORTED      103
#define ECONNRESET        104
#define ENOBUFS           105
#define EISCONN           106
#define ENOTCONN          107
#define ESHUTDOWN         108
#define ETIMEDOUT         110
#define ECONNREFUSED      111
#define EHOSTDOWN         112
#define EHOSTUNREACH      113
#define EALREADY          114
#define EINPROGRESS       115
#define ESTALE            116
#define EDQUOT            122
