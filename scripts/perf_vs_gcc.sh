#!/usr/bin/env bash
# Perf-parity probe — GCC is the PERFORMANCE baseline, not just the codegen one.
#
# If madc's front-end (parse + c2mir) takes longer than gcc compiles the same
# translation unit, that is a signal of an algorithmic pathology (e.g. an
# unbounded-lookahead O(n^2) — see docs/plans/2026-06-23-parser-lookahead-audit.md).
# The standing rule: when a test is slower than gcc, callgrind it and find the
# top self-cost madc function.
#
# FRAME OF REFERENCE IS RECORDED ONCE. gcc (and tinycc, if built) are timed only
# the first time a (file, std) is seen — results are stored in
# docs/parity/perf-baseline.tsv and reused on every later run. Only madc is
# re-timed each run (it is what changes). Re-measure the reference with --refresh
# (e.g. after a host/toolchain change). This keeps the loop fast and the
# comparison stable.
#
# Usage:
#   scripts/perf_vs_gcc.sh <file.c> [--std=STD] [--threshold=N] [--callgrind] [--refresh]
#   scripts/perf_vs_gcc.sh <file> --pipeline [--runs=N] [--std=STD]
#
#   --pipeline      WHOLE-PIPELINE comparison, apples to apples (owner 2026-10-08,
#                   benchmark tests/testsubscript.mad): gcc/g++ compile + link +
#                   run against madc compile + run, on the forest (default) and
#                   --no-forest-bind. Timed fresh every call, INTERLEAVED per round
#                   (gcc, madc, madc live), median of N rounds (default 5); every
#                   run's exit status and stdout are checked against the gcc-built
#                   program's. A .c file goes to gcc, anything else to g++ -x c++.
#                   madc is $MADC_BIN (default bin/madc-release, the shipped -O2
#                   binary). Plan: docs/plans/2026-10-08-compile-time-vs-gxx.md.
#
#   --std=STD       pass -std=STD to gcc/tinycc and --std=STD to madc (default: none).
#   --threshold=N   flag when madc/gcc ratio exceeds N (default 2.0).
#   --callgrind     always run callgrind (else: auto when over threshold).
#   --refresh       re-time gcc/tinycc and overwrite their recorded baseline.
#
# Caps every heavy invocation (timeout + ulimit -t); one job at a time.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MADC="$ROOT/bin/madc"
TCC="/workspace/tinycc/tcc"
BASELINE="$ROOT/docs/parity/perf-baseline.tsv"
SP="${TMPDIR:-/tmp}/perf_vs_gcc.$$"
mkdir -p "$SP"
trap 'rm -rf "$SP"' EXIT

file=""; std=""; threshold="2.0"; force_cg=0; refresh=0; pipeline=0; runs=5
for a in "$@"; do
    case "$a" in
        --std=*)       std="${a#--std=}" ;;
        --threshold=*) threshold="${a#--threshold=}" ;;
        --callgrind)   force_cg=1 ;;
        --refresh)     refresh=1 ;;
        --pipeline)    pipeline=1 ;;
        --runs=*)      runs="${a#--runs=}" ;;
        -*)            echo "unknown option: $a" >&2; exit 2 ;;
        *)             file="$a" ;;
    esac
done
[ -n "$file" ] && [ -f "$file" ] || { echo "usage: $0 <file.c> [--std=STD] [--threshold=N] [--callgrind] [--refresh]" >&2; exit 2; }

# --- Whole pipeline: compile (+ link) + run, interleaved, median of N ---
if [ "$pipeline" = 1 ]; then
    PBIN="${MADC_BIN:-$ROOT/bin/madc-release}"
    [ -x "$PBIN" ] || { echo "no $PBIN (make -C src release, or set MADC_BIN)" >&2; exit 2; }
    pstd_c=(); pstd_m=()
    [ -n "$std" ] && { pstd_c=(-std="$std"); pstd_m=(--std="$std"); }
    case "$file" in
        *.c) cc=(gcc "${pstd_c[@]}") ;;
        *)   cc=(g++ -x c++ "${pstd_c[@]}") ;;
    esac
    pdir="$(dirname "$file")"
    # The oracle: what the gcc-built program prints and exits with.
    ( ulimit -t 300; timeout 300 "${cc[@]}" -I"$pdir" "$file" -o "$SP/a.out" ) > /dev/null 2>&1 \
        || { echo "pipeline: ${cc[*]} cannot build $file" >&2; exit 2; }
    ( ulimit -t 60; timeout 60 "$SP/a.out" ) > "$SP/oracle.out" 2> /dev/null
    oracle_rc=$?
    # one_run <label> cmd... -> appends "<seconds>" to $SP/<label>.t; exits on a mismatch
    one_run() {
        local label="$1" t rc; shift
        t="$( { TIMEFORMAT='%R'; time ( ulimit -t 300; timeout 300 "$@" > "$SP/$label.out" 2> /dev/null; \
                echo $? > "$SP/$label.rc" ); } 2>&1 )"
        rc="$(cat "$SP/$label.rc")"
        if [ "$rc" != "$oracle_rc" ] || ! cmp -s "$SP/$label.out" "$SP/oracle.out"; then
            echo "pipeline: $label run differs from the gcc-built program (rc $rc, oracle rc $oracle_rc)" >&2
            exit 1
        fi
        echo "$t" >> "$SP/$label.t"
    }
    # timeout runs a program, not a shell function: the gcc leg is a script.
    printf '#!/bin/sh\n"$@" -o "%s/b.out" || exit 125\nexec "%s/b.out"\n' "$SP" "$SP" > "$SP/gcc_run"
    chmod +x "$SP/gcc_run"
    for _ in $(seq "$runs"); do
        one_run gcc "$SP/gcc_run" "${cc[@]}" -I"$pdir" "$file"
        one_run forest "$PBIN" "${pstd_m[@]}" -I"$pdir" "$file"
        one_run live "$PBIN" "${pstd_m[@]}" --no-forest-bind -I"$pdir" "$file"
    done
    median() { sort -n "$SP/$1.t" | awk '{v[NR]=$1} END{print (NR%2 ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2)}'; }
    g="$(median gcc)"; f="$(median forest)"; l="$(median live)"
    printf '%-34s %s\n' "file:" "${file#$ROOT/} ${std:+(--std=$std)}  [median of $runs, interleaved]"
    printf '%-34s %ss\n' "${cc[0]} compile+link+run:" "$g"
    printf '%-34s %ss  (%sx %s)\n' "madc forest compile+run:" "$f" \
        "$(awk -v a="$f" -v b="$g" 'BEGIN{printf "%.2f", a/b}')" "${cc[0]}"
    printf '%-34s %ss  (%sx %s)\n' "madc --no-forest-bind:" "$l" \
        "$(awk -v a="$l" -v b="$g" 'BEGIN{printf "%.2f", a/b}')" "${cc[0]}"
    exit 0
fi

# Key the recorded reference by repo-relative path + std.
relfile="${file#$ROOT/}"
key="$relfile|$std"
incdir="$(dirname "$file")"
gcc_std=(); madc_std=(); tcc_std=()
[ -n "$std" ] && { gcc_std=(-std="$std"); madc_std=(--std="$std"); tcc_std=(-std="$std"); }

baseline_get() { # key col(2=gcc,3=tinycc)
    [ -f "$BASELINE" ] || return 1
    awk -F'\t' -v k="$1" -v c="$2" '$1==k {print $c; found=1} END{exit !found}' "$BASELINE"
}
baseline_set() { # key gcc tinycc
    mkdir -p "$(dirname "$BASELINE")"
    if [ ! -f "$BASELINE" ]; then
        {
            echo "# madc perf frame-of-reference — recorded once, reused every run."
            echo "# GCC = must-beat bar (gcc -O0 -c); tinycc = floor (tcc -c). HOST-SPECIFIC"
            echo "# (madc-devbox); re-measure with: scripts/perf_vs_gcc.sh <file> --refresh."
            printf '# %s\t%s\t%s\t%s\n' "key(relpath|std)" "gcc_O0_c_s" "tinycc_c_s" "recorded"
        } > "$BASELINE"
    fi
    grep -v -F "$(printf '%s\t' "$1")" "$BASELINE" > "$SP/bl" 2>/dev/null || true
    mv "$SP/bl" "$BASELINE"
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(env -i date +%Y-%m-%d 2>/dev/null || echo recorded)" >> "$BASELINE"
}
time_real() { # cmd... -> wall seconds (or "" on failure)
    local t
    t="$(TIMEFORMAT='%R'; { time ( ulimit -t 300; timeout 300 "$@" ) >/dev/null 2>&1; } 2>&1)"
    printf '%s' "$t"
}

# --- Reference (recorded once) ---
gcc_real="$(baseline_get "$key" 2)"; tcc_real="$(baseline_get "$key" 3)"
if [ "$refresh" = 1 ] || [ -z "${gcc_real:-}" ]; then
    echo "(measuring reference for $key — recorded once)"
    gcc_real="$(time_real gcc -O0 "${gcc_std[@]}" -I"$incdir" -c "$file" -o "$SP/gcc.o")"
    if [ -x "$TCC" ]; then
        tcc_real="$(time_real "$TCC" "${tcc_std[@]}" -I"$incdir" -c "$file" -o "$SP/tcc.o")"
    else
        tcc_real="${tcc_real:-na}"
    fi
    baseline_set "$key" "${gcc_real:-na}" "${tcc_real:-na}"
fi

# --- madc front-end (always re-timed): parse + c2mir from --show-stats ---
stats="$( ( ulimit -t 300; timeout 300 "$MADC" "${madc_std[@]}" --show-stats -I"$incdir" "$file" ) 2>&1 )"
# The stats lines have a dotted leader ("parse time ...... 0.598 s"), so pick the
# FIRST numeric field, not a fixed column.
firstnum='{for(i=1;i<=NF;i++) if($i ~ /^[0-9]/){print $i; exit}}'
parse="$(printf '%s\n' "$stats" | awk "/parse time/    $firstnum")"
c2mir="$(printf '%s\n' "$stats" | awk "/c2mir compile/ $firstnum")"
toks="$(printf '%s\n'  "$stats" | awk "/tokens produced/$firstnum")"
parse="${parse:-0}"; c2mir="${c2mir:-0}"
madc_fe="$(awk -v p="$parse" -v c="$c2mir" 'BEGIN{printf "%.3f", p + c}')"
ratio="$(awk -v m="$madc_fe" -v g="${gcc_real:-0}" 'BEGIN{ if (g+0==0) print "inf"; else printf "%.2f", m/g }')"

printf '%-22s %s\n'  "file:"            "$relfile ${std:+(--std=$std)}"
printf '%-22s %s tok\n' "tokens:"       "${toks:-?}"
printf '%-22s %ss (recorded)\n' "gcc -O0 -c:" "${gcc_real:-?}"
[ "${tcc_real:-na}" != "na" ] && printf '%-22s %ss (recorded, floor)\n' "tinycc -c:" "$tcc_real"
printf '%-22s %ss (parse %ss + c2mir %ss)\n' "madc front-end:" "$madc_fe" "$parse" "$c2mir"
printf '%-22s %sx gcc\n' "ratio:"       "$ratio"

over="$(awk -v r="$ratio" -v t="$threshold" 'BEGIN{ if (r=="inf"){print 1;exit} print (r>t)?1:0 }')"
if [ "$over" = "1" ]; then
    echo ">> madc is SLOWER than ${threshold}x gcc — perf-parity FAIL. Callgrind candidate."
else
    echo "OK: within ${threshold}x gcc."
fi

if [ "$force_cg" = "1" ] || [ "$over" = "1" ]; then
    command -v callgrind_annotate >/dev/null 2>&1 || { echo "(valgrind not installed — skipping deep dive)"; exit 0; }
    echo
    echo "=== callgrind: top self-cost madc functions (the culprit list) ==="
    ( ulimit -t 600; timeout 600 valgrind --tool=callgrind --callgrind-out-file="$SP/cg.out" \
        "$MADC" "${madc_std[@]}" -I"$incdir" "$file" ) >/dev/null 2>&1
    callgrind_annotate --threshold=90 --inclusive=no "$SP/cg.out" 2>/dev/null \
        | awk '/file:function/{p=1} p' \
        | grep -E '/workspace/madc|\?\?\?:' | grep -viE 'libc|libstdc|/mir/' | head -15
fi
