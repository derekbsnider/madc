// madc embedded string.h — C string functions
// Most functions resolve through the dlsym fallback at parse time
// (which registers them with a generic int64 return signature). The
// extern declarations below give the parser proper return types so
// `*(strchr(...)) = 0` works without explicit user-side `extern`.

extern char *strchr(char *s, int c);
