// SPDX-License-Identifier: MPL-2.0
///////////////////////////////////////////////////////////////////////////
//                                                                       //
// Win64 POSIX supplement for the platform's real <sys/time.h>.          //
// Additive macros only; the native header is always served first.       //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_POSIX_SYS_TIME_H
#define __MADC_POSIX_SYS_TIME_H 1

#ifndef _WIN32
#error madc POSIX supplements are Win64-only
#endif

#ifndef timeradd
#define timeradd(lhs, rhs, out)                                              \
	do {                                                                  \
		(out)->tv_sec = (lhs)->tv_sec + (rhs)->tv_sec;                  \
		(out)->tv_usec = (lhs)->tv_usec + (rhs)->tv_usec;               \
		if ( (out)->tv_usec >= 1000000L ) {                             \
			(out)->tv_sec += 1;                                       \
			(out)->tv_usec -= 1000000L;                               \
		}                                                                 \
	} while ( 0 )
#endif

#ifndef timersub
#define timersub(lhs, rhs, out)                                              \
	do {                                                                  \
		(out)->tv_sec = (lhs)->tv_sec - (rhs)->tv_sec;                  \
		(out)->tv_usec = (lhs)->tv_usec - (rhs)->tv_usec;               \
		if ( (out)->tv_usec < 0 ) {                                    \
			(out)->tv_sec -= 1;                                       \
			(out)->tv_usec += 1000000L;                               \
		}                                                                 \
	} while ( 0 )
#endif

#endif
