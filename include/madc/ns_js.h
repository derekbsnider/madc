#ifndef MADC_NS_JS_H
#define MADC_NS_JS_H

#ifdef __cplusplus

#include <cstdint>
#include <string>
#include "datadef.h"

extern "C" {
std::string *__js_btoa(std::string *, const char *);
std::string *__js_atob(std::string *, const char *);
std::string *__js_encodeURIComponent(std::string *, const char *);
std::string *__js_decodeURIComponent(std::string *, const char *);
int64_t __js_parseInt(const char *, int64_t);
std::string *__js_stringify(std::string *, madc::value *);
}

namespace js {
inline std::string &btoa(std::string &result, const char *input) { return *__js_btoa(&result, input); }
inline std::string &atob(std::string &result, const char *input) { return *__js_atob(&result, input); }
inline std::string &encodeURIComponent(std::string &result, const char *input) { return *__js_encodeURIComponent(&result, input); }
inline std::string &decodeURIComponent(std::string &result, const char *input) { return *__js_decodeURIComponent(&result, input); }
inline int64_t parseInt(const char *text, int64_t radix) { return __js_parseInt(text, radix); }
inline std::string &stringify(std::string &result, madc::value &values) { return *__js_stringify(&result, &values); }
}

#endif

#endif
