#ifndef __MADC_REGEX_H
#define __MADC_REGEX_H 1

#include <sys/types.h>

/*
 * Minimal POSIX regex declarations for source compatibility.
 * Extend this if upstream code starts using regex_t/regcomp/regexec directly.
 */

typedef long regoff_t;

typedef struct
{
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

typedef struct
{
    void *buffer;
    size_t allocated;
    size_t used;
    uint32_t re_nsub;
} regex_t;

#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NEWLINE 8

#define REG_NOTBOL 1
#define REG_NOTEOL 2

#define REG_NOERROR 0
#define REG_NOMATCH 1

int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void regfree(regex_t *preg);

#endif
