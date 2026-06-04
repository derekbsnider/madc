#ifndef MADC_NS_RUST_H
#define MADC_NS_RUST_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
int64_t __rust_contains(const char *, const char *);
int64_t __rust_starts_with(const char *, const char *);
int64_t __rust_ends_with(const char *, const char *);
std::string *__rust_trim(std::string *);
std::string *__rust_trim_start(std::string *);
std::string *__rust_trim_end(std::string *);
std::string *__rust_replace(std::string *, const char *, const char *);
std::string *__rust_repeat(std::string *, int64_t);
int64_t __rust_len(const char *);
int64_t __rust_is_empty(const char *);
void __rust_split(MadArray *, const char *, const char *);
void __rust_split_whitespace(MadArray *, const char *);
std::string *__rust_join(std::string *, MadArray *, const char *);
std::string *__rust_first(std::string *, MadArray *);
std::string *__rust_last(std::string *, MadArray *);
std::string *__rust_get(std::string *, MadArray *, int64_t);
void __rust_push(MadArray *, const char *);
std::string *__rust_pop(std::string *, MadArray *);
}

namespace rust {
inline int64_t contains(const char *text, const char *needle) { return __rust_contains(text, needle); }
inline int64_t starts_with(const char *text, const char *prefix) { return __rust_starts_with(text, prefix); }
inline int64_t ends_with(const char *text, const char *suffix) { return __rust_ends_with(text, suffix); }
inline std::string &trim(std::string &s) { return *__rust_trim(&s); }
inline std::string &trim_start(std::string &s) { return *__rust_trim_start(&s); }
inline std::string &trim_end(std::string &s) { return *__rust_trim_end(&s); }
inline std::string &replace(std::string &s, const char *from, const char *to) { return *__rust_replace(&s, from, to); }
inline std::string &repeat(std::string &s, int64_t count) { return *__rust_repeat(&s, count); }
inline int64_t len(const char *text) { return __rust_len(text); }
inline int64_t is_empty(const char *text) { return __rust_is_empty(text); }
inline void split(MadArray &out, const char *text, const char *delim) { __rust_split(&out, text, delim); }
inline void split_whitespace(MadArray &out, const char *text) { __rust_split_whitespace(&out, text); }
inline std::string &join(std::string &result, MadArray &values, const char *sep) { return *__rust_join(&result, &values, sep); }
inline std::string &first(std::string &result, MadArray &values) { return *__rust_first(&result, &values); }
inline std::string &last(std::string &result, MadArray &values) { return *__rust_last(&result, &values); }
inline std::string &get(std::string &result, MadArray &values, int64_t idx) { return *__rust_get(&result, &values, idx); }
inline void push(MadArray &values, const char *value) { __rust_push(&values, value); }
inline std::string &pop(std::string &result, MadArray &values) { return *__rust_pop(&result, &values); }
}

#endif

#endif
