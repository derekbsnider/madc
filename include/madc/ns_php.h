#ifndef MADC_NS_PHP_H
#define MADC_NS_PHP_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
std::string *__php_trim(std::string *);
std::string *__php_ltrim(std::string *);
std::string *__php_rtrim(std::string *);
std::string *__php_chop(std::string *);
std::string *__php_ucfirst(std::string *);
std::string *__php_lcfirst(std::string *);
std::string *__php_str_repeat(std::string *, int64_t);
std::string *__php_str_replace(std::string *, std::string *, std::string *);
std::string *__php_str_pad(std::string *, int64_t, std::string *);
int64_t __php_str_word_count(std::string *);
std::string *__php_nl2br(std::string *);
std::string *__php_str_rot13(std::string *);
std::string *__php_chunk_split(std::string *, int64_t, std::string *);
std::string *__php_number_format(std::string *, int64_t, std::string *);
std::string *__php_wordwrap(std::string *, int64_t, std::string *);
void __php_explode(MadArray *, const char *, const char *);
std::string *__php_implode(std::string *, const char *, MadArray *);
int64_t __php_count(MadArray *);
void __php_array_push(MadArray *, const char *);
void __php_array_push_int(MadArray *, int64_t);
void __php_array_push_array(MadArray *, MadArray *);
std::string *__php_array_pop(std::string *, MadArray *);
std::string *__php_array_get(std::string *, MadArray *, int64_t);
int64_t __php_array_get_int(MadArray *, int64_t);
const char *__php_array_get_cstr(MadArray *, int64_t);
void __php_array_reverse(MadArray *);
int64_t __php_in_array(const char *, MadArray *);
int64_t __php_array_search(const char *, MadArray *);
void __php_array_unique(MadArray *);
std::string *__php_array_shift(std::string *, MadArray *);
void __php_array_unshift(MadArray *, const char *);
void __php_sort(MadArray *);
void __php_rsort(MadArray *);
void __php_array_slice(MadArray *, MadArray *, int64_t, int64_t);
void __php_array_merge(MadArray *, MadArray *);
void __php_array_column(MadArray *, MadArray *, int64_t);
}

namespace php {
inline std::string &trim(std::string &s) { return *__php_trim(&s); }
inline std::string &ltrim(std::string &s) { return *__php_ltrim(&s); }
inline std::string &rtrim(std::string &s) { return *__php_rtrim(&s); }
inline std::string &chop(std::string &s) { return *__php_chop(&s); }
inline std::string &ucfirst(std::string &s) { return *__php_ucfirst(&s); }
inline std::string &lcfirst(std::string &s) { return *__php_lcfirst(&s); }
inline std::string &str_repeat(std::string &s, int64_t count) { return *__php_str_repeat(&s, count); }
inline std::string &str_replace(std::string &search, std::string &replace, std::string &subject) { return *__php_str_replace(&search, &replace, &subject); }
inline std::string &str_pad(std::string &s, int64_t length, std::string &pad) { return *__php_str_pad(&s, length, &pad); }
inline int64_t str_word_count(std::string &s) { return __php_str_word_count(&s); }
inline std::string &nl2br(std::string &s) { return *__php_nl2br(&s); }
inline std::string &str_rot13(std::string &s) { return *__php_str_rot13(&s); }
inline std::string &chunk_split(std::string &s, int64_t chunklen, std::string &separator) { return *__php_chunk_split(&s, chunklen, &separator); }
inline std::string &number_format(std::string &result, int64_t number, std::string &separator) { return *__php_number_format(&result, number, &separator); }
inline std::string &wordwrap(std::string &s, int64_t width, std::string &separator) { return *__php_wordwrap(&s, width, &separator); }
inline void explode(MadArray &out, const char *delim, const char *text) { __php_explode(&out, delim, text); }
inline std::string &implode(std::string &result, const char *glue, MadArray &values) { return *__php_implode(&result, glue, &values); }
inline int64_t count(MadArray &values) { return __php_count(&values); }
inline void array_push(MadArray &values, const char *text) { __php_array_push(&values, text); }
inline void array_push_int(MadArray &values, int64_t value) { __php_array_push_int(&values, value); }
inline void array_push_array(MadArray &values, MadArray &nested) { __php_array_push_array(&values, &nested); }
inline std::string &array_pop(std::string &result, MadArray &values) { return *__php_array_pop(&result, &values); }
inline std::string &array_get(std::string &result, MadArray &values, int64_t index) { return *__php_array_get(&result, &values, index); }
inline int64_t array_get_int(MadArray &values, int64_t index) { return __php_array_get_int(&values, index); }
inline const char *array_get_cstr(MadArray &values, int64_t index) { return __php_array_get_cstr(&values, index); }
inline void array_reverse(MadArray &values) { __php_array_reverse(&values); }
inline int64_t in_array(const char *needle, MadArray &values) { return __php_in_array(needle, &values); }
inline int64_t array_search(const char *needle, MadArray &values) { return __php_array_search(needle, &values); }
inline void array_unique(MadArray &values) { __php_array_unique(&values); }
inline std::string &array_shift(std::string &result, MadArray &values) { return *__php_array_shift(&result, &values); }
inline void array_unshift(MadArray &values, const char *text) { __php_array_unshift(&values, text); }
inline void sort(MadArray &values) { __php_sort(&values); }
inline void rsort(MadArray &values) { __php_rsort(&values); }
inline void array_slice(MadArray &dest, MadArray &src, int64_t offset, int64_t length) { __php_array_slice(&dest, &src, offset, length); }
inline void array_merge(MadArray &dest, MadArray &src) { __php_array_merge(&dest, &src); }
inline void array_column(MadArray &dest, MadArray &src, int64_t column_index) { __php_array_column(&dest, &src, column_index); }
}

#endif

#endif
