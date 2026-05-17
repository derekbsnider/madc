// madc embedded stdlib.h — standard library constants
// Functions (malloc, free, exit, atoi, atof, rand, srand, abs, etc.) via dlsym fallback

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647
