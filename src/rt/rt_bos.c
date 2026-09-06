///////////////////////////////////////////////////////////////////////////
//                                                                       //
// __builtin_object_size runtime fallback.                               //
//                                                                       //
// A DUAL-BUILD source (forest-carriers S5): compiled into libmadc by the //
// host build AND into an AOT ledger module by madc itself at pack time,  //
// so a `-static-libmadc` binary carries it inside its own image. Strict  //
// C11, no compiler builtins.                                            //
//                                                                       //
// madc's lexer maps `__builtin_object_size` to this symbol (fortify      //
// headers reach it on every platform: glibc _FORTIFY_SOURCE, mingw       //
// __mingw_bos, darwin _chk indirection). madc does no compile-time       //
// object-size analysis, so the answer is always "unknown" — which per    //
// gcc's documented contract is (size_t)-1 for modes 0/1 (maximum) and    //
// 0 for modes 2/3 (minimum). The old C++-side copy returned -1 for ALL   //
// modes — wrong for 2/3, where -1 defeats the fortify check entirely.    //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#include <stddef.h>

// Signature stays exactly the shipped one (libc_signatures.cpp declares
// LibcRet::ULong) — this move changes the HOME and the mode-2/3 arm, not
// the ABI.
unsigned long __madc_builtin_object_size(void *ptr, int mode)
{
    (void)ptr;
    return (mode & 2) ? 0 : (unsigned long)-1;
}
