#ifndef MADC_NS_RUBY_H
#define MADC_NS_RUBY_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
std::string *__rb_squeeze(std::string *);
std::string *__rb_tr(std::string *, const char *, const char *);
void __rb_chars(MadArray *, const char *);
std::string *__rb_capitalize(std::string *);
std::string *__rb_delete(std::string *, const char *);
int64_t __rb_count(const char *, const char *);
int64_t __rb_include(const char *, const char *);
std::string *__rb_gsub(std::string *, const char *, const char *);
std::string *__rb_sub(std::string *, const char *, const char *);
void __rb_rotate(MadArray *, int64_t);
void __rb_compact(MadArray *);
void __rb_flatten(MadArray *, const char *);
}

namespace ruby {
inline std::string &squeeze(std::string &s) { return *__rb_squeeze(&s); }
inline std::string &tr(std::string &s, const char *from, const char *to) { return *__rb_tr(&s, from, to); }
inline void chars(MadArray &out, const char *text) { __rb_chars(&out, text); }
inline std::string &capitalize(std::string &s) { return *__rb_capitalize(&s); }
inline std::string &delete_chars(std::string &s, const char *chars) { return *__rb_delete(&s, chars); }
inline int64_t count(const char *text, const char *chars) { return __rb_count(text, chars); }
inline int64_t include(const char *text, const char *substr) { return __rb_include(text, substr); }
inline std::string &gsub(std::string &s, const char *pattern, const char *replacement) { return *__rb_gsub(&s, pattern, replacement); }
inline std::string &sub(std::string &s, const char *pattern, const char *replacement) { return *__rb_sub(&s, pattern, replacement); }
inline void rotate(MadArray &values, int64_t n) { __rb_rotate(&values, n); }
inline void compact(MadArray &values) { __rb_compact(&values); }
inline void flatten(MadArray &values, const char *text) { __rb_flatten(&values, text); }
}

#endif

#endif
