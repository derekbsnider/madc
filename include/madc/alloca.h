// madc embedded alloca.h
// alloca() is handled as a compiler intrinsic — bump-allocates from
// a per-function stack pool managed by cc.newStack().
void *alloca(unsigned long size);
