// madc embedded dlfcn.h — dynamic linking constants
// Note: dlopen/dlsym/dlclose/dlerror are first-class in madc via #load
// These constants are for use with explicit dlopen() calls

#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0x00000
#define RTLD_NOLOAD   0x00004
#define RTLD_DEEPBIND 0x00008
#define RTLD_NODELETE 0x01000
