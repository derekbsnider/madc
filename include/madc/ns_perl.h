#ifndef MADC_NS_PERL_H
#define MADC_NS_PERL_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
int64_t __perl_chop(std::string *);
int64_t __perl_chomp(std::string *);
void __perl_grep(madc::value *, const char *, madc::value *);
void __perl_glob(madc::value *, const char *);
int64_t __perl_scalar(madc::value *);
void __perl_push(madc::value *, const char *);
std::string *__perl_pop(std::string *, madc::value *);
std::string *__perl_shift(std::string *, madc::value *);
void __perl_unshift(madc::value *, const char *);
std::string *__perl_join(std::string *, const char *, madc::value *);
void __perl_split(madc::value *, const char *, const char *);
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
inline void grep(madc::value &dest, const char *needle, madc::value &src) { __perl_grep(&dest, needle, &src); }
inline void glob(madc::value &out, const char *pattern) { __perl_glob(&out, pattern); }
inline int64_t scalar(madc::value &values) { return __perl_scalar(&values); }
inline void push(madc::value &values, const char *text) { __perl_push(&values, text); }
inline std::string &pop(std::string &result, madc::value &values) { return *__perl_pop(&result, &values); }
inline std::string &shift(std::string &result, madc::value &values) { return *__perl_shift(&result, &values); }
inline void unshift(madc::value &values, const char *text) { __perl_unshift(&values, text); }
inline std::string &join(std::string &result, const char *separator, madc::value &values) { return *__perl_join(&result, separator, &values); }
inline void split(madc::value &out, const char *pattern, const char *text) { __perl_split(&out, pattern, text); }
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
