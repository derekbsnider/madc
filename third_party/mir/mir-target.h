/* This file is a part of MIR project.
   Copyright (C) 2018-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.

   Build-time TARGET selection (madc fork, Mach-O/ARM64 track axis A).

   Upstream MIR selects its code-generation target by detecting the
   HOST architecture and OS with compiler-predefined macros at every
   arch/OS switch.  This header centralizes that decision and adds a
   cross override with two separate primitive knobs plus pair helpers
   (owner design 2026-07-25):

     arch knob:  define ONE of  MIR_TARGET_X86_64  MIR_TARGET_AARCH64
     OS   knob:  optionally     MIR_TARGET_APPLE or MIR_TARGET_WINDOWS
                                (default: linux/ELF)

     pair helpers (define the knobs together -- the CLI-facing spelling):
       MIR_TARGET_X86_64_LINUX   =  MIR_TARGET_X86_64
       MIR_TARGET_AARCH64_LINUX  =  MIR_TARGET_AARCH64
       MIR_TARGET_ARM64_MACOS    =  MIR_TARGET_AARCH64 + MIR_TARGET_APPLE
       MIR_TARGET_X86_64_MACOS   =  MIR_TARGET_X86_64  + MIR_TARGET_APPLE
       MIR_TARGET_X86_64_WINDOWS =  MIR_TARGET_X86_64  + MIR_TARGET_WINDOWS

   Only validated arch+OS pairs are accepted -- anything else is a
   compile error.  A cross build translates and CAPTURES code for the
   target and emits objects/executables, but never executes target
   code: execution paths (code pages, thunks, ffi) are host-only.

   Code switches on the derived MIR_TARGET_IS_<arch> /
   MIR_TARGET_APPLE_P / MIR_CROSS_P macros below.  With no override
   they all equal host detection, so a default build is
   behavior-identical to upstream. */

#ifndef MIR_TARGET_H
#define MIR_TARGET_H

/* --- pair helpers expand to the primitive knobs --- */

#ifdef MIR_TARGET_X86_64_LINUX
#ifndef MIR_TARGET_X86_64
#define MIR_TARGET_X86_64 1
#endif
#endif

#ifdef MIR_TARGET_AARCH64_LINUX
#ifndef MIR_TARGET_AARCH64
#define MIR_TARGET_AARCH64 1
#endif
#endif

#ifdef MIR_TARGET_ARM64_MACOS
#ifndef MIR_TARGET_AARCH64
#define MIR_TARGET_AARCH64 1
#endif
#ifndef MIR_TARGET_APPLE
#define MIR_TARGET_APPLE 1
#endif
#endif

#ifdef MIR_TARGET_X86_64_MACOS
#ifndef MIR_TARGET_X86_64
#define MIR_TARGET_X86_64 1
#endif
#ifndef MIR_TARGET_APPLE
#define MIR_TARGET_APPLE 1
#endif
#endif

#ifdef MIR_TARGET_X86_64_WINDOWS
#ifndef MIR_TARGET_X86_64
#define MIR_TARGET_X86_64 1
#endif
#ifndef MIR_TARGET_WINDOWS
#define MIR_TARGET_WINDOWS 1
#endif
#endif

/* --- validation: one arch knob at most; only pairs that exist --- */

#if defined(MIR_TARGET_X86_64) && defined(MIR_TARGET_AARCH64)
#error "conflicting MIR_TARGET arch selection -- define exactly one"
#endif

#if defined(MIR_TARGET_APPLE) && defined(MIR_TARGET_WINDOWS)
#error "conflicting MIR_TARGET OS selection -- define at most one of APPLE/WINDOWS"
#endif

#if defined(MIR_TARGET_APPLE) && !defined(MIR_TARGET_AARCH64) && !defined(MIR_TARGET_X86_64)
#error "MIR_TARGET_APPLE requires an arch knob (arm64-macos or x86_64-macos)"
#endif

#if defined(MIR_TARGET_WINDOWS) && !defined(MIR_TARGET_X86_64)
#error "MIR_TARGET_WINDOWS requires the x86-64 arch knob (x86_64-windows is the one pair)"
#endif

#if defined(MIR_TARGET_X86_64) || defined(MIR_TARGET_AARCH64)
#define MIR_TARGET_OVERRIDE_P 1
#else
#define MIR_TARGET_OVERRIDE_P 0
#if defined(MIR_TARGET_APPLE)
#error "MIR_TARGET_APPLE requires an explicit arch knob (it never combines with host detection)"
#endif
#if defined(MIR_TARGET_WINDOWS)
#error "MIR_TARGET_WINDOWS requires an explicit arch knob (it never combines with host detection)"
#endif
#endif

/* --- derived: target architecture --- */

#if defined(MIR_TARGET_X86_64) \
  || (!MIR_TARGET_OVERRIDE_P && (defined(__x86_64__) || defined(_M_AMD64)))
#define MIR_TARGET_IS_X86_64 1
#else
#define MIR_TARGET_IS_X86_64 0
#endif

#if defined(MIR_TARGET_AARCH64) || (!MIR_TARGET_OVERRIDE_P && defined(__aarch64__))
#define MIR_TARGET_IS_AARCH64 1
#else
#define MIR_TARGET_IS_AARCH64 0
#endif

/* --- derived: target OS ABI variant --- */

/* aarch64 has Darwin deviations (all varargs on stack, x18 reserved,
   ...): a native Apple build keeps them via host detection; a cross
   build opts in with the OS knob / the arm64-macos pair helper. */
#if defined(MIR_TARGET_APPLE) || (!MIR_TARGET_OVERRIDE_P && defined(__APPLE__))
#define MIR_TARGET_APPLE_P 1
#else
#define MIR_TARGET_APPLE_P 0
#endif

/* PE/COFF container target (mir-pe.c behind the MIR_object seam): a native
   Windows build keeps it via host detection (the hosted mingw madc.exe); a
   cross build opts in with the OS knob / the x86_64-windows pair helper. */
#if defined(MIR_TARGET_WINDOWS) || (!MIR_TARGET_OVERRIDE_P && defined(_WIN32))
#define MIR_TARGET_WINDOWS_P 1
#else
#define MIR_TARGET_WINDOWS_P 0
#endif

/* --- derived: cross build? (target pair != host pair) --- */

#if defined(__APPLE__)
#define MIR_HOST_APPLE_P_ 1
#else
#define MIR_HOST_APPLE_P_ 0
#endif

#if defined(_WIN32)
#define MIR_HOST_WINDOWS_P_ 1
#else
#define MIR_HOST_WINDOWS_P_ 0
#endif

#if (MIR_TARGET_IS_X86_64 && !(defined(__x86_64__) || defined(_M_AMD64)))          \
  || (MIR_TARGET_IS_AARCH64 && !defined(__aarch64__)) || MIR_TARGET_APPLE_P != MIR_HOST_APPLE_P_ \
  || MIR_TARGET_WINDOWS_P != MIR_HOST_WINDOWS_P_
#define MIR_CROSS_P 1
#else
#define MIR_CROSS_P 0
#endif

#endif /* MIR_TARGET_H */
