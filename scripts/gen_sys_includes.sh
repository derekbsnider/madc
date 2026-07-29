#!/bin/bash
# Generate src/sys_include_paths.cpp — the host's system #include <...> search
# list, captured from the configured compiler so madc searches the SAME paths the
# build toolchain does (incl. the C++ paths the hardcoded list lacked).
#
# ONE TABLE PER C++ STANDARD LIBRARY FLAVOR. `-stdlib=` selects a whole search
# list, exactly as clang's driver does — a libc++ compile has no libstdc++
# directories on the path at all, and putting libc++ merely FIRST is a different
# thing (libc++'s <cstdlib> reaches the C library via #include_next, which would
# then land on /usr/include/c++/NN/stdlib.h). The default flavor is whatever
# $(CXX) uses; an alternate is recorded whenever a probe reports a different one.
# No path or library name is hardcoded here: the flavor NAMES come from the
# libraries' own version macros, and the paths from each probe's own search list.
# MADC_STDLIB_ALT_CXX overrides the alternate-flavor probe command.
#
# Host-specific: gitignored, regenerated on build. `make clean` forces a refresh
# if the toolchain changes. Empty output => madc falls back to its hardcoded list.
set -u

CXX="${CXX:-c++}"
OUT="$(dirname "$0")/../src/sys_include_paths.cpp"
# Optional "from=to" path-prefix rewrite for cross-built binaries whose build
# sysroot lives elsewhere on the RUN machine (e.g. hosted-darwin modes map the
# staged SDK to the standard CLT SDK location).
PREFIX_MAP="${MADC_SYS_INCLUDE_PREFIX_MAP:-}"

map_prefix() {
    # rewrite $1 through PREFIX_MAP ("from=to"); echoes the (possibly) mapped path
    local p="$1"
    if [ -n "$PREFIX_MAP" ]; then
        local from="${PREFIX_MAP%%=*}" to="${PREFIX_MAP#*=}"
        case "$p" in "$from"*) p="$to${p#"$from"}";; esac
    fi
    printf '%s' "$p"
}

# `<compiler> -x c++ -E -v` prints the search list between two marker lines;
# each path line is indented by one space. Strip indent + any trailing
# annotation. The probe command is word-split on purpose: cross toolchains and
# flavor probes are multi-word (compiler -target ... --sysroot ... -stdlib=...).
search_list() {
    printf '' | "$@" -x c++ -E -v - 2>&1 \
        | sed -n '/#include <...> search starts here:/,/End of search list/p' \
        | sed -n 's/^ \([^ ]*\).*$/\1/p'
}

# The compiler-owned (bucket-1/2 freestanding) include dir — the one the
# toolchain supplies itself (stddef.h, stdarg.h, limits.h, intrinsics, …). The
# header partition (madc-header-partition-handoff.md) says madc PROVIDES
# equivalents for these, so when bypassing embedded SYSTEM-library shims we must
# still keep them: a header resolving here is freestanding, not a glibc/libstdc++
# twin. It is also the SLOT madc's embedded set occupies in the search order, so
# it is per-flavor (gcc's gcc/.../include vs clang's llvm/.../clang/NN/include).
# Empty string when detection fails (no compiler at build time).
resource_dir() {
    local owned
    owned=$("$@" -print-file-name=include 2>/dev/null)
    case "$owned" in
        include) owned="" ;;   # not found: -print-file-name echoes the bare arg
        */) ;; *) [ -n "$owned" ] && owned="$owned/" ;;
    esac
    [ -n "$owned" ] && owned=$(map_prefix "$owned")
    printf '%s' "$owned"
}

# Which C++ standard library does this probe command actually resolve to? Asked
# of the LIBRARY, via the version macro it defines for exactly this purpose —
# never inferred from a directory name or a compiler name. Echoes nothing when
# the probe cannot compile (an unsupported -stdlib=, a missing library), which
# is how an unavailable flavor drops out.
detect_flavor() {
    printf '#include <cstddef>\n#if defined(_LIBCPP_VERSION)\nMADC_STDLIB_FLAVOR "libc++"\n#elif defined(__GLIBCXX__)\nMADC_STDLIB_FLAVOR "libstdc++"\n#endif\n' \
        | "$@" -x c++ -E - 2>/dev/null \
        | sed -n 's/^MADC_STDLIB_FLAVOR "\([^"]*\)".*/\1/p' \
        | head -1
}

# The flavor's C++ runtime DT_NEEDED set, asked of the toolchain ITSELF: link
# an empty C++ program (-Wl,--no-as-needed so the driver's full runtime list
# survives an --as-needed default) and read the produced binary's NEEDED
# entries, minus the platform base the emitter adds unconditionally
# (libc/libm/the loader). No SONAME is hardcoded per flavor — a toolchain that
# names a different runtime reports it here. Empty on probe failure (no
# linkable runtime, no readelf): the emitter then falls back.
link_libs() {
    command -v readelf >/dev/null 2>&1 || return 0
    local tmp
    tmp=$(mktemp) || return 0
    if echo 'int main(){return 0;}' \
        | "$@" -Wl,--no-as-needed -x c++ - -o "$tmp" 2>/dev/null; then
        readelf -d "$tmp" 2>/dev/null \
            | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p' \
            | grep -v -e '^libc\.' -e '^libm\.' -e '^ld-'
    fi
    rm -f "$tmp"
}

emit_link_libs_array() {
    # $1 = array suffix, rest = probe command
    local idx="$1"; shift
    local libs l
    libs=$(link_libs "$@")
    echo "static const char *madc_stdlib_link_libs_$idx[] = {"
    if [ -n "$libs" ]; then
        while IFS= read -r l; do
            [ -z "$l" ] && continue
            printf '    "%s",\n' "$l"
        done <<< "$libs"
    fi
    echo '    (const char *)0'
    echo '};'
}

emit_paths_array() {
    # $1 = array suffix, rest = probe command
    local idx="$1"; shift
    local paths p
    paths=$(search_list "$@")
    echo "static const char *madc_sys_include_paths_$idx[] = {"
    if [ -n "$paths" ]; then
        while IFS= read -r p; do
            [ -z "$p" ] && continue
            p=$(map_prefix "$p")
            case "$p" in */) ;; *) p="$p/";; esac
            printf '    "%s",\n' "$p"
        done <<< "$paths"
    fi
    echo '    (const char *)0'
    echo '};'
}

DEFAULT_FLAVOR=$(detect_flavor $CXX)

# Find an alternate flavor: a probe that resolves to a DIFFERENT library than
# the default. Candidates are tool spellings, not answers — each one is asked
# what it actually resolves to, and the first that differs wins. A candidate
# whose compiler or library is absent simply reports nothing.
ALT_CXX="${MADC_STDLIB_ALT_CXX:-}"
ALT_FLAVOR=""
if [ -n "$ALT_CXX" ]; then
    # Same two conditions the discovery loop applies, so an override that names
    # a broken or same-flavor probe drops out instead of emitting a nameless
    # entry that -stdlib= could never select.
    ALT_FLAVOR=$(detect_flavor $ALT_CXX)
    if [ -z "$ALT_FLAVOR" ] || [ "$ALT_FLAVOR" = "$DEFAULT_FLAVOR" ]; then
        ALT_FLAVOR=""
        ALT_CXX=""
    fi
elif [ -n "$DEFAULT_FLAVOR" ]; then
    for cand in "$CXX -stdlib=libc++" "$CXX -stdlib=libstdc++" \
                "clang++-18 -stdlib=libc++" "clang++ -stdlib=libc++"; do
        f=$(detect_flavor $cand)
        if [ -n "$f" ] && [ "$f" != "$DEFAULT_FLAVOR" ]; then
            ALT_CXX="$cand"
            ALT_FLAVOR="$f"
            break
        fi
    done
fi

emit() {
    echo '// AUTO-GENERATED by scripts/gen_sys_includes.sh — DO NOT EDIT, DO NOT COMMIT.'
    echo "// Host system #include search paths from \`$CXX -x c++ -E -v\`."
    if [ -n "$ALT_CXX" ]; then
        echo "// Alternate C++ standard library flavor from \`$ALT_CXX\`."
    fi
    echo '#include "madc_sys_includes.h"'
    echo
    emit_paths_array 0 $CXX
    echo
    emit_link_libs_array 0 $CXX
    if [ -n "$ALT_CXX" ]; then
        echo
        emit_paths_array 1 $ALT_CXX
        echo
        emit_link_libs_array 1 $ALT_CXX
    fi
    echo
    echo 'const madc_stdlib_flavor madc_stdlib_flavors[] = {'
    printf '    { "%s", madc_sys_include_paths_0, "%s", madc_stdlib_link_libs_0 },\n' \
           "$DEFAULT_FLAVOR" "$(resource_dir $CXX)"
    if [ -n "$ALT_CXX" ]; then
        printf '    { "%s", madc_sys_include_paths_1, "%s", madc_stdlib_link_libs_1 },\n' \
               "$ALT_FLAVOR" "$(resource_dir $ALT_CXX)"
    fi
    echo '    { (const char *)0, (const char *const *)0, "", (const char *const *)0 }'
    echo '};'
    echo "const char *madc_default_stdlib_flavor = \"$DEFAULT_FLAVOR\";"
}

# Idempotent write: rewrite only on real content change, so the parse-time
# regeneration in src/Makefile never churns mtimes / forces rebuilds.
emit > "$OUT.tmp"
if cmp -s "$OUT.tmp" "$OUT" 2>/dev/null; then
    rm -f "$OUT.tmp"
else
    mv "$OUT.tmp" "$OUT"
fi
