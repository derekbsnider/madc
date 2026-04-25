// madc embedded crypt.h — POSIX password crypt
// libcrypt.so isn't part of glibc's RTLD_DEFAULT search, so #load it
// explicitly. Once loaded with RTLD_GLOBAL, dlsym(RTLD_DEFAULT, "crypt")
// resolves and the typed extern decl below routes the call through the
// existing dlsym late-bind path with proper char* return typing.

#load "libcrypt.so" as crypt_lib;

extern char *crypt(char *key, char *salt);
