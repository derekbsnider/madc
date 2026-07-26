///////////////////////////////////////////////////////////////////////////
//                                                                       //
// VLA scope-exit runtime.                                               //
//                                                                       //
// A DUAL-BUILD source (forest-carriers S5): compiled into libmadc by the //
// host build AND into an AOT ledger module by madc itself at pack time,  //
// so a `-static-libmadc` binary carries it inside its own image. Strict  //
// C11, no compiler builtins.                                            //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stdlib.h>

// A C99 variable-length-array local is lowered to a `T *` backed by malloc;
// the cir_builder attaches __attribute__((cleanup(__madc_vla_free))) to it, so
// c2mir calls this with &a (a `T **`) on scope exit. Free the pointed-to
// storage. NULL-safe (free(NULL) is a no-op).
void __madc_vla_free(void *pp)
{
    if (pp) free(*(void **)pp);
}
