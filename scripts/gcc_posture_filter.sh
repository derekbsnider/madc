#!/bin/bash
# gcc_posture_filter.sh — the ONE GCC-posture filter, shared by
# gen_predefined_macros.sh (the baked table) and gen_darwin_prelude.sh (the
# flattened prelude's kept -dD defines — clang dumps its identity macros
# into that output too, so an unfiltered prelude would re-poison the posture
# the moment a TU includes any prelude name).
#
# Drops the __clang__/__llvm__ identity #defines and rewrites __GNUC__* to
# the given version, so headers madc serves take their GCC branches — the
# posture every host lane runs and the one the libc++ parity campaign
# proved. See gen_predefined_macros.sh for the full rationale.
# __VERSION__ is part of the identity too: GCC spells it as the bare version
# ("13.3.0"); left alone it names the CAPTURE compiler ("Ubuntu Clang 18.1.3
# (1ubuntu1)" on the container, "Homebrew Clang 18.1.8" on a mac runner) —
# a served-world difference between two builds of the same tree.
#
# usage: gcc_posture_filter.sh [<gnuc-version like 13.3.0>]   (stdin -> stdout)
# An empty/absent version means no posture: pass through unchanged.
POSTURE="${1:-}"
if [ -z "$POSTURE" ]; then
    exec cat
fi
maj="${POSTURE%%.*}"; rest="${POSTURE#*.}"
min="${rest%%.*}"; pat="${rest##*.}"
exec sed -e '/^#define __clang/d' \
         -e '/^#define __llvm__/d' \
         -e "s/^#define __GNUC__ .*/#define __GNUC__ $maj/" \
         -e "s/^#define __GNUC_MINOR__ .*/#define __GNUC_MINOR__ $min/" \
         -e "s/^#define __GNUC_PATCHLEVEL__ .*/#define __GNUC_PATCHLEVEL__ $pat/" \
         -e "s/^#define __VERSION__ .*/#define __VERSION__ \"$POSTURE\"/"
