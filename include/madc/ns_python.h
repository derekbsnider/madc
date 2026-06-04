#ifndef MADC_NS_PYTHON_H
#define MADC_NS_PYTHON_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
std::string *__py_title(std::string *);
std::string *__py_swapcase(std::string *);
std::string *__py_center(std::string *, int64_t, const char *);
std::string *__py_ljust(std::string *, int64_t, const char *);
std::string *__py_rjust(std::string *, int64_t, const char *);
std::string *__py_zfill(std::string *, int64_t);
int64_t __py_count(const char *, const char *);
int64_t __py_startswith(const char *, const char *);
int64_t __py_endswith(const char *, const char *);
int64_t __py_isdigit(const char *);
int64_t __py_isalpha(const char *);
int64_t __py_isalnum(const char *);
int64_t __py_isspace(const char *);
std::string *__py_replace(std::string *, const char *, const char *);
std::string *__py_format(std::string *, const char *, MadArray *);
}

namespace python {
inline std::string &title(std::string &s) { return *__py_title(&s); }
inline std::string &swapcase(std::string &s) { return *__py_swapcase(&s); }
inline std::string &center(std::string &s, int64_t width, const char *fill) { return *__py_center(&s, width, fill); }
inline std::string &ljust(std::string &s, int64_t width, const char *fill) { return *__py_ljust(&s, width, fill); }
inline std::string &rjust(std::string &s, int64_t width, const char *fill) { return *__py_rjust(&s, width, fill); }
inline std::string &zfill(std::string &s, int64_t width) { return *__py_zfill(&s, width); }
inline int64_t count(const char *haystack, const char *needle) { return __py_count(haystack, needle); }
inline int64_t startswith(const char *text, const char *prefix) { return __py_startswith(text, prefix); }
inline int64_t endswith(const char *text, const char *suffix) { return __py_endswith(text, suffix); }
inline int64_t isdigit(const char *text) { return __py_isdigit(text); }
inline int64_t isalpha(const char *text) { return __py_isalpha(text); }
inline int64_t isalnum(const char *text) { return __py_isalnum(text); }
inline int64_t isspace(const char *text) { return __py_isspace(text); }
inline std::string &replace(std::string &s, const char *old_text, const char *new_text) { return *__py_replace(&s, old_text, new_text); }
inline std::string &format(std::string &result, const char *fmt, MadArray &args) { return *__py_format(&result, fmt, &args); }
}

#endif

#endif
