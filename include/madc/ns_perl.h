#ifndef MADC_NS_PERL_H
#define MADC_NS_PERL_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
int64_t __perl_chop(std::string *);
int64_t __perl_chomp(std::string *);
void __perl_grep(MadArray *, const char *, MadArray *);
void __perl_glob(MadArray *, const char *);
int64_t __perl_scalar(MadArray *);
void __perl_push(MadArray *, const char *);
std::string *__perl_pop(std::string *, MadArray *);
std::string *__perl_shift(std::string *, MadArray *);
void __perl_unshift(MadArray *, const char *);
std::string *__perl_join(std::string *, const char *, MadArray *);
void __perl_split(MadArray *, const char *, const char *);
std::string *__perl_reverse(std::string *);
std::string *__perl_lc(std::string *);
std::string *__perl_uc(std::string *);
std::string *__perl_ucfirst(std::string *);
std::string *__perl_lcfirst(std::string *);
int64_t __perl_index(const char *, const char *);
int64_t __perl_rindex(const char *, const char *);
int64_t __perl_length(const char *);
std::string *__perl_substr(std::string *, const char *, int64_t, int64_t);
}

namespace perl {
inline int64_t chop(std::string &s) { return __perl_chop(&s); }
inline int64_t chomp(std::string &s) { return __perl_chomp(&s); }
inline void grep(MadArray &dest, const char *needle, MadArray &src) { __perl_grep(&dest, needle, &src); }
inline void glob(MadArray &out, const char *pattern) { __perl_glob(&out, pattern); }
inline int64_t scalar(MadArray &values) { return __perl_scalar(&values); }
inline void push(MadArray &values, const char *text) { __perl_push(&values, text); }
inline std::string &pop(std::string &result, MadArray &values) { return *__perl_pop(&result, &values); }
inline std::string &shift(std::string &result, MadArray &values) { return *__perl_shift(&result, &values); }
inline void unshift(MadArray &values, const char *text) { __perl_unshift(&values, text); }
inline std::string &join(std::string &result, const char *separator, MadArray &values) { return *__perl_join(&result, separator, &values); }
inline void split(MadArray &out, const char *pattern, const char *text) { __perl_split(&out, pattern, text); }
inline std::string &reverse(std::string &s) { return *__perl_reverse(&s); }
inline std::string &lc(std::string &s) { return *__perl_lc(&s); }
inline std::string &uc(std::string &s) { return *__perl_uc(&s); }
inline std::string &ucfirst(std::string &s) { return *__perl_ucfirst(&s); }
inline std::string &lcfirst(std::string &s) { return *__perl_lcfirst(&s); }
inline int64_t index(const char *haystack, const char *needle) { return __perl_index(haystack, needle); }
inline int64_t rindex(const char *haystack, const char *needle) { return __perl_rindex(haystack, needle); }
inline int64_t length(const char *text) { return __perl_length(text); }
inline std::string &substr(std::string &result, const char *text, int64_t offset, int64_t length) { return *__perl_substr(&result, text, offset, length); }
}

#endif

#endif
