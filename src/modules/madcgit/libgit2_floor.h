// modules/madcgit/libgit2_floor.h — THE libgit2 floor (owner 2026-10-04):
// 1.8.7 within 1.8, 1.9.7 within 1.9, or any later minor — the releases
// carrying libgit2's security fixes. One owner for two readers: madcgit.cpp
// includes it, and src/madcgit.mk preprocesses it against a system libgit2's
// pkg-config flags to decide whether that libgit2 may build the module.
#ifndef __LIBGIT2_FLOOR_H
#define __LIBGIT2_FLOOR_H 1

#include <git2.h>

#if LIBGIT2_VER_MAJOR < 1 \
    || (LIBGIT2_VER_MAJOR == 1 && (LIBGIT2_VER_MINOR < 8 \
	|| (LIBGIT2_VER_MINOR == 8 && LIBGIT2_VER_REVISION < 7) \
	|| (LIBGIT2_VER_MINOR == 9 && LIBGIT2_VER_REVISION < 7)))
#error "madcgit needs libgit2 1.8.7, 1.9.7 or newer (src/madcgit.mk pins the tag; scripts/stage_libgit2.sh stages it)"
#endif

#endif
