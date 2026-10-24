/* aarch64_predef.c — the TARGET's predefined macros and header world. The
   aarch64-linux cross madc captured both tables from the x86-64 HOST's g++:
   it predefined __x86_64__ and not __aarch64__, __LDBL_MANT_DIG__ 64 (x87)
   where aarch64's long double is binary128 (113), no __CHAR_UNSIGNED__ where
   plain char is unsigned, and searched the host's x86-64 glibc headers, whose
   struct stat is x86-64's (144 bytes; aarch64's is 128). */
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
int main(void) {
#if defined(__aarch64__) && !defined(__x86_64__)
    printf("arch: aarch64\n");
#else
    printf("arch: other\n");
#endif
#ifdef __CHAR_UNSIGNED__
    printf("char: unsigned\n");
#else
    printf("char: signed\n");
#endif
    printf("ldbl: %d %d\n", __LDBL_MANT_DIG__, (int)sizeof(long double));
    printf("stat: %d %d\n", (int)sizeof(struct stat), (int)offsetof(struct stat, st_size));
    return 0;
}
