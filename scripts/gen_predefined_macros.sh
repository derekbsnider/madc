#!/bin/bash
# Generate src/predefined_macros.cpp — the host compiler's predefined macros
# (`<CXX> -dM -E`), so madc presents the SAME predefined-macro environment the
# toolchain does. Real system headers branch heavily on these (__STDC_HOSTED__,
# __cplusplus, __SIZEOF_*, __*_TYPE__, feature-test macros, …).
#
# Emits two C arrays (object-like and function-like); the lexer seeds them into
# define_map / macro_map at init. Programmatic seeding (NOT source #defines) —
# feeding 460 #define lines through the lexer breaks on the macro bodies.
#
# Host/std-specific: gitignored, regenerated each build; `make clean` refreshes.
#
# The output (MADC_PREDEF_MACROS_OUT, src/Makefile): a mode that captures a
# foreign compiler (darwin hosted+cross, windows hosted) writes its own table
# into its obj tree — the SYS_INCLUDES_CPP precedent — since every mode
# regenerates at parse time and two modes' tables differ; the host modes
# share src/predefined_macros.cpp. The temp file is per process, so two
# concurrent writers of one table cannot interleave into it.
set -u

CXX="${CXX:-c++}"
STD="${MADC_PREDEF_STD:-c++17}"
OUT="${MADC_PREDEF_MACROS_OUT:-$(dirname "$0")/../src/predefined_macros.cpp}"
TMP="$OUT.tmp.$$"

# GCC posture (MADC_PREDEF_GCC_POSTURE=<gnuc version, e.g. 13.3.0>): drop the
# __clang__/__llvm__ identity macros and present the given GCC version, so the
# headers madc SERVES take their GCC branches — the posture every host lane
# already runs (the host tables come from g++), under which libc++ keys off
# __GCC_ATOMIC_* instead of __has_feature probes and disables the clang-only
# availability-annotation machinery. Used by the Apple-target modes, whose
# capture compiler is necessarily clang (only clang cross-targets darwin) but
# whose served posture must match what madc actually implements. The version
# floor matters: libc++ rejects _LIBCPP_COMPILER_GCC older than its support
# window, and clang's compat value is a fictional GCC 4.
POSTURE="${MADC_PREDEF_GCC_POSTURE:-}"
posture_filter() {
    # ONE filter, shared with gen_darwin_prelude.sh (whose flattened output
    # keeps clang's identity #defines and would re-poison the posture).
    "$(dirname "$0")/gcc_posture_filter.sh" "$POSTURE"
}

# The posture's ONE ABI consequence, darwin arm64: libc++'s __config selects
# the std::string layout Apple's libc++.dylib was built with —
# _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT on arm64-apple (x86_64 Apple keeps the
# old layout in the same branch) — ONLY under the clang identity
# (_LIBCPP_COMPILER_CLANG_BASED) the GCC posture drops. The dylib's layout is a
# fact of the TARGET, not of the compiler reading the headers, so the table
# restates it exactly where the dropped identity would have implied it: a
# captured table that defines __APPLE__ and __aarch64__ gains the define. It
# is the only _LIBCPP_ABI_* define inside that clang-gated block of the pinned
# 18.1.3 text. Measured on an M-series Mac (darwin D4 re-measure): a string
# built by grove-compiled code carried its size at byte 0 while one returned by
# the dylib (std::to_string) carried it at byte 23 — every std::string crossing
# the dylib/host boundary read as garbage (to_string, operator+, stod, the
# php::/rust:: string publics); Intel unaffected. The baked table is the ONE
# place for it: a define arriving through a served header would land after
# __config had already decided.
target_abi_restatement() {
    local tbl
    tbl=$(cat)
    printf '%s\n' "$tbl"
    if [ -n "$POSTURE" ] \
       && printf '%s\n' "$tbl" | grep -q '^#define __APPLE__ ' \
       && printf '%s\n' "$tbl" | grep -q '^#define __aarch64__ '; then
        echo '#define _LIBCPP_ABI_ALTERNATE_STRING_LAYOUT 1'
    fi
}

# Builtin VALUE macros: clang (15+) implements __FLT_EVAL_METHOD__ inside its
# preprocessor (Preprocessor::RegisterBuiltinMacros, beside __LINE__), so its
# -dM dump omits a macro every translation unit sees (Apple arm64 and x86-64:
# 0); gcc lists it as a #define. madc's float.h spells FLT_EVAL_METHOD through
# it, so a capture whose dump lacks it asks the same compiler to expand it.
# The other builtins (__LINE__, __FILE__, __COUNTER__, ...) are per-location
# values madc implements itself (builtin_macro_text).
builtin_value_macros() {
    local tbl name val
    tbl=$(cat)
    printf '%s\n' "$tbl"
    for name in __FLT_EVAL_METHOD__; do
        printf '%s\n' "$tbl" | grep -q "^#define $name " && continue
        val=$(printf '%s\n' "$name" | $CXX -E -P -std="$STD" -x c++ - 2>/dev/null | tr -d '[:space:]')
        [ -n "$val" ] && [ "$val" != "$name" ] && echo "#define $name $val"
    done
    return 0
}

# C23 <float.h> predefines, per type: __X_NORM_MAX__ and __X_IS_IEC_60559__
# (gcc 13 has both; clang 20 has the NORM_MAX three and no IS_IEC_60559; Apple
# clang 16 and clang 18 have neither), so an older capture compiler leaves
# madc's float.h C23 section undeclared on its target. Both are facts of the
# type's FORMAT, which the dump states (__X_MANT_DIG__ / __X_MAX_EXP__): an
# IEC 60559 binary format -- binary32 (24/128), binary64 (53/1024), x87
# extended (64/16384), binary128 (113/16384) -- has NORM_MAX == MAX and
# IS_IEC_60559 1, gcc's values. Any other format (IBM double-double, 106)
# gets neither, and stays undeclared rather than guessed.
float_format_macros() {
    local tbl t mant maxexp max
    tbl=$(cat)
    printf '%s\n' "$tbl"
    for t in FLT DBL LDBL; do
        mant=$(printf '%s\n' "$tbl" | sed -n "s/^#define __${t}_MANT_DIG__ //p")
        maxexp=$(printf '%s\n' "$tbl" | sed -n "s/^#define __${t}_MAX_EXP__ //p")
        max=$(printf '%s\n' "$tbl" | sed -n "s/^#define __${t}_MAX__ //p")
        case "$mant/$maxexp" in
        24/128|53/1024|64/16384|113/16384) ;;
        *) continue ;;
        esac
        printf '%s\n' "$tbl" | grep -q "^#define __${t}_NORM_MAX__ " \
            || echo "#define __${t}_NORM_MAX__ $max"
        printf '%s\n' "$tbl" | grep -q "^#define __${t}_IS_IEC_60559__ " \
            || echo "#define __${t}_IS_IEC_60559__ 1"
    done
    return 0
}

# $CXX is word-split on purpose: cross toolchains are multi-word commands.
$CXX -dM -E -std="$STD" -x c++ /dev/null 2>/dev/null | posture_filter | target_abi_restatement | builtin_value_macros | float_format_macros | awk '
function esc(s) { gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); return s }
BEGIN {
    print "// AUTO-GENERATED by scripts/gen_predefined_macros.sh — DO NOT EDIT/COMMIT."
    print "struct MadcPredefObj  { const char *name; const char *value; };"
    print "struct MadcPredefFunc { const char *name; const char *params; const char *body; };"
    obj = "const MadcPredefObj madc_predefined_obj[] = {\n"
    fn  = "const MadcPredefFunc madc_predefined_func[] = {\n"
}
/^#define / {
    rest = substr($0, 9)                         # strip "#define "
    # function-like iff "(" immediately follows the name (no space before it)
    sp = index(rest, " ")
    par = index(rest, "(")
    if (par > 0 && (sp == 0 || par < sp)) {
        name = substr(rest, 1, par - 1)
        close_par = index(rest, ")")
        params = substr(rest, par + 1, close_par - par - 1)
        body = substr(rest, close_par + 1)
        sub(/^ /, "", body)
        fn = fn "    {\"" esc(name) "\", \"" esc(params) "\", \"" esc(body) "\"},\n"
    } else {
        if (sp == 0) { name = rest; val = "" }
        else { name = substr(rest, 1, sp - 1); val = substr(rest, sp + 1) }
        obj = obj "    {\"" esc(name) "\", \"" esc(val) "\"},\n"
    }
}
END {
    print obj "    {(const char *)0, (const char *)0}\n};"
    print fn  "    {(const char *)0, (const char *)0, (const char *)0}\n};"
    # Accessor functions (not direct extern data refs) so the lexer reaches the
    # arrays via a PLT-resolved call — avoids text relocations in the PIE binary
    # (same pattern as embedded_headers.cpp / find_embedded_header).
    print "const MadcPredefObj  *madc_predefined_objects()  { return madc_predefined_obj; }"
    print "const MadcPredefFunc *madc_predefined_functions() { return madc_predefined_func; }"
}
' > "$TMP"

# A near-empty capture means the compiler probe or the filter pipeline
# FAILED (a real -dM table has hundreds of entries) — installing it would
# silently strip every predefined macro from the built madc (the exact G2
# silent-degradation class; a non-executable filter script did precisely
# this once). Keep the previous table and fail loudly instead.
if [ "$(grep -c '{"' "$TMP")" -lt 50 ]; then
    rm -f "$TMP"
    echo "gen_predefined_macros: capture came back (near) empty — refusing to install" >&2
    exit 1
fi

# Idempotent write: rewrite only on real content change, so the parse-time
# regeneration in src/Makefile never churns mtimes / forces rebuilds.
if cmp -s "$TMP" "$OUT" 2>/dev/null; then
    rm -f "$TMP"
else
    mv "$TMP" "$OUT"
fi
