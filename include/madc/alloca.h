// madc embedded alloca.h
// alloca() allocates on the stack — madc maps it to malloc for now.
// True stack allocation would need compiler intrinsic support.
#define alloca(size) malloc(size)
