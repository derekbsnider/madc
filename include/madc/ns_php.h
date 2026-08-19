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
void __php_explode(madc::value *, const char *, const char *);
std::string *__php_implode(std::string *, const char *, madc::value *);
int64_t __php_count(madc::value *);
int64_t __php_array_push(madc::value *, const char *);
int64_t __php_array_push_int(madc::value *, int64_t);
int64_t __php_array_push_real(madc::value *, double);
int64_t __php_array_push_bool(madc::value *, bool);
int64_t __php_array_push_array(madc::value *, madc::value *);
int64_t __php_array_push_value(madc::value *, madc::value *);
std::string *__php_array_pop(std::string *, madc::value *);
std::string *__php_array_get(std::string *, madc::value *, int64_t);
int64_t __php_array_get_int(madc::value *, int64_t);
const char *__php_array_get_cstr(madc::value *, int64_t);
void __php_array_reverse(madc::value *);
int64_t __php_in_array(const char *, madc::value *);
int64_t __php_array_search(const char *, madc::value *);
void __php_array_unique(madc::value *);
std::string *__php_array_shift(std::string *, madc::value *);
void __php_array_unshift(madc::value *, const char *);
void __php_sort(madc::value *);
void __php_rsort(madc::value *);
void __php_array_slice(madc::value *, madc::value *, int64_t, int64_t);
void __php_array_merge(madc::value *, madc::value *);
void __php_array_column(madc::value *, madc::value *, int64_t);
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
inline void explode(madc::value &out, const char *delim, const char *text) { __php_explode(&out, delim, text); }
inline std::string &implode(std::string &result, const char *glue, madc::value &values) { return *__php_implode(&result, glue, &values); }
inline int64_t count(madc::value &values) { return __php_count(&values); }
// array_push is ONE overloaded name, PHP parity: it accepts any value type
// and RETURNS the array's new element count. The madc::value& overload is a
// kind-preserving deep copy (an array-kind carrier nests). The plain-int
// overload exists to keep `array_push(a, 7)` unambiguous under ISO C++
// (int->int64_t, int->double and int->bool are all "conversion" rank).
inline int64_t array_push(madc::value &values, const char *text) { return __php_array_push(&values, text); }
inline int64_t array_push(madc::value &values, int64_t v) { return __php_array_push_int(&values, v); }
inline int64_t array_push(madc::value &values, int v) { return __php_array_push_int(&values, v); }
inline int64_t array_push(madc::value &values, double v) { return __php_array_push_real(&values, v); }
inline int64_t array_push(madc::value &values, bool v) { return __php_array_push_bool(&values, v); }
inline int64_t array_push(madc::value &values, madc::value &v) { return __php_array_push_value(&values, &v); }
inline int64_t array_push(madc::value &values, std::string &text) { return __php_array_push(&values, text.c_str()); }
inline std::string &array_pop(std::string &result, madc::value &values) { return *__php_array_pop(&result, &values); }
inline std::string &array_get(std::string &result, madc::value &values, int64_t index) { return *__php_array_get(&result, &values, index); }
inline int64_t array_get_int(madc::value &values, int64_t index) { return __php_array_get_int(&values, index); }
inline const char *array_get_cstr(madc::value &values, int64_t index) { return __php_array_get_cstr(&values, index); }
inline void array_reverse(madc::value &values) { __php_array_reverse(&values); }
inline int64_t in_array(const char *needle, madc::value &values) { return __php_in_array(needle, &values); }
inline int64_t array_search(const char *needle, madc::value &values) { return __php_array_search(needle, &values); }
inline void array_unique(madc::value &values) { __php_array_unique(&values); }
inline std::string &array_shift(std::string &result, madc::value &values) { return *__php_array_shift(&result, &values); }
inline void array_unshift(madc::value &values, const char *text) { __php_array_unshift(&values, text); }
inline void sort(madc::value &values) { __php_sort(&values); }
inline void rsort(madc::value &values) { __php_rsort(&values); }
inline void array_slice(madc::value &dest, madc::value &src, int64_t offset, int64_t length) { __php_array_slice(&dest, &src, offset, length); }
inline void array_merge(madc::value &dest, madc::value &src) { __php_array_merge(&dest, &src); }
inline void array_column(madc::value &dest, madc::value &src, int64_t column_index) { __php_array_column(&dest, &src, column_index); }
}

#endif

#endif
