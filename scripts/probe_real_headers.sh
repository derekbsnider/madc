#!/bin/bash
# Measure madc's parse coverage of real system C++ headers — the burn-down
# instrument for the real-header-PCH track (docs/plans/2026-06-06-real-header-pch-pipeline.md).
#
# For each header: parse it with madc (--emit=c11) two ways and report the first
# error, which SEPARATES the two gap classes:
#   * "madc"   — madc does its OWN preprocessing + parse of the real header.
#   * "pp"     — feed gcc-PREPROCESSED source (macros expanded, '# line' markers
#                stripped) so madc only PARSES. If "pp" passes but "madc" fails,
#                the gap is in madc's PREPROCESSOR (e.g. _GLIBCXX_* macros), not
#                the parser. If both fail at the same construct, it's the parser.
#
# Usage:  scripts/probe_real_headers.sh [header ...]
#   default header set below. Caveat: the "madc" column's transitive top-level
#   std:: includes hit the embedded stubs (shadowing) until those are retired;
#   the "pp" column is the clean parser signal.
#
# KEY (measured 2026-06-06): the "pp" column is OK for nearly the whole std::
# surface — the C++ PARSER handles the real headers. The remaining "madc"-column
# failures are therefore PREPROCESSOR gaps (madc's own macro/directive handling),
# NOT parser gaps. To diagnose one: compare madc's preprocessing to `gcc -E` and
# find the divergent directive/macro — known culprits: `#include_next` (read as a
# filename: ".../next <stdlib.h>") and `_GLIBCXX_*` macro expansion (a mis-expanded
# macro yields a broken typedef / parameter list / class body). char_traits is the
# lone exception: a genuine `__gnu_cxx::char_traits` name-resolution gap (fails even
# in the "pp" column).
set -u
STD="${MADC_PREDEF_STD:-c++17}"
CXXINC="$(printf '#include <vector>\n' | g++ -std="$STD" -E -x c++ - 2>/dev/null \
          | sed -n 's/^# [0-9]* "\(.*\)\/vector".*/\1/p' | head -1)"
[ -n "$CXXINC" ] || CXXINC=/usr/include/c++/13
MADC="${MADC:-bin/madc}"
TMP="${TMPDIR:-tmp}"; mkdir -p "$TMP"

hdrs=("$@")
[ ${#hdrs[@]} -gt 0 ] || hdrs=(type_traits utility memory string string_view \
    char_traits iosfwd ostream istream iostream sstream fstream \
    vector map set algorithm)

firsterr(){ sed 's/\x1b\[[0-9;]*m//g' | grep -m1 'error:' | sed 's#.*/[^ ]*:\([0-9]*:[0-9]*\): #\1 #'; }

printf '%-14s %-26s %s\n' HEADER 'madc (own preprocess)' 'pp (gcc-preprocessed, parser-only)'
for h in "${hdrs[@]}"; do
    f="$CXXINC/$h"; [ -f "$f" ] || f="$CXXINC/bits/$h.h"
    [ -f "$f" ] || { printf '%-14s %s\n' "$h" '(not found)'; continue; }
    printf '#include "%s"\nint main(){return 0;}\n' "$f" > "$TMP/_probe.mad"
    me=$(timeout 60 "$MADC" --std="$STD" --emit=c11 "$TMP/_probe.mad" 2>&1 >/dev/null | firsterr)
    g++ -std="$STD" -E "$f" 2>/dev/null | grep -v '^#' > "$TMP/_probe_pp.cpp"
    pe=$(timeout 60 "$MADC" --std="$STD" --emit=c11 "$TMP/_probe_pp.cpp" 2>&1 >/dev/null | firsterr)
    printf '%-14s %-26s %s\n' "$h" "${me:-OK}" "${pe:-OK}"
done
