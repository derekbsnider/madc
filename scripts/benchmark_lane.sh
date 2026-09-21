#!/bin/bash
# benchmark_lane.sh — madc vs gcc/g++, EXECUTION time and COMPILE time.
#
# Both halves matter and they move independently: MIR's generator owns the
# execution numbers, madc's front end owns the compile numbers. A change can
# improve one and regress the other, so they are tracked side by side and
# re-measured at every release (docs/benchmarks/README.md).
#
# Corpora:
#   exec-c    third_party/mir/c-benchmarks (MIR's own shootout set, .arg/.expect)
#   exec-cpp  docs/benchmarks/cpp          (container growth, vtable, std::string)
#   exec-fp   donut.c at the repo root IF PRESENT — the call-heavy floating-point
#             sentinel for the v0.93.0 MIR false-dependency fix (pxor before the
#             merging SSE converts). That fix made donut 2.8x faster and BEAT
#             gcc -O0; it is upstream code untouched since 2019, so a silent
#             revert is exactly the regression this lane exists to catch.
#             Not vendored: third-party source, run only if the owner's copy is
#             there.
#   compile   the same sources, timed as `-c` to an object.
#
# METHOD. Best-of-N wall clock, INTERLEAVED (a sequential A-then-B run
# penalises whichever goes second on a cold cache). Output is compared before
# any timing is kept — a wrong answer makes a fast time meaningless. The
# summary line is a GEOMETRIC mean, because these are ratios.
#
# Wall time is noisy; treat a single run as a trend point, not a verdict, and
# read the RATIO rather than the milliseconds (the ratio survives a change of
# host, the absolute numbers do not).
#
#   MADC_BIN        binary under test (default bin/madc)
#   MADC_BENCH_N    repetitions per timing (default 3)
#   MADC_BENCH_TSV  append a machine-readable row per benchmark here
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
N="${MADC_BENCH_N:-3}"
TSV="${MADC_BENCH_TSV:-}"
MIRB=third_party/mir/c-benchmarks
CPPB=docs/benchmarks/cpp
STAMP=$(date -u +%FT%TZ)
REV=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)

[ -x "$BIN" ] || { echo "benchmark_lane: $BIN missing — build first" >&2; exit 1; }
mkdir -p tmp/bench

now(){ date +%s%3N; }
best(){ local b=99999999 i t0 t1 d; for i in $(seq 1 "$N"); do t0=$(now); "$@" >/dev/null 2>&1; t1=$(now); d=$((t1-t0)); [ "$d" -lt "$b" ] && b=$d; done; echo "$b"; }
emit(){ [ -n "$TSV" ] && printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$STAMP" "$REV" "$1" "$2" "$3" "$4" >> "$TSV"; return 0; }

sum=0; n=0
reset_gm(){ sum=0; n=0; }
acc(){ sum=$(awk -v s="$sum" -v a="$1" -v b="$2" 'BEGIN{print s + log(a/b)}'); n=$((n+1)); }
gm(){ awk -v s="$sum" -v k="$n" -v l="$1" 'BEGIN{ if(k>0) printf "  %-18s geometric mean over %d: %.2fx (madc/%s)\n", l, k, exp(s/k), (l ~ /cpp/ ? "g++" : "gcc") }'; }

# run_exec <kind> <label> <src> <std> <cc> <args...>
run_exec(){
	local kind=$1 label=$2 src=$3 std=$4 cc=$5; shift 5
	local args="$*"
	$cc $std -w -O2 -o tmp/bench/g_bin "$src" -lm 2>/dev/null || { printf "  %-18s %9s\n" "$label" "CC-FAIL"; return; }
	( ulimit -t 600; timeout 600 "$BIN" $std -O2 -o tmp/bench/m_bin "$src" ) >/dev/null 2>&1 \
		|| { printf "  %-18s %9s\n" "$label" "MADC-FAIL"; return; }
	local go mo
	go=$(timeout 300 tmp/bench/g_bin $args 2>/dev/null | md5sum)
	mo=$(timeout 300 tmp/bench/m_bin $args 2>/dev/null | md5sum)
	[ "$go" = "$mo" ] || { printf "  %-18s %9s\n" "$label" "MISMATCH"; return; }
	local g m
	g=$(best tmp/bench/g_bin $args); m=$(best tmp/bench/m_bin $args)
	[ "$g" -eq 0 ] && g=1
	printf "  %-18s %8s %8s %8s\n" "$label" "$g" "$m" "$(awk -v a="$m" -v b="$g" 'BEGIN{printf "%.2fx", a/b}')"
	emit "$kind" "$label" "$g" "$m"; acc "$m" "$g"
}

# run_compile <kind> <label> <src> <std> <cc>
run_compile(){
	local kind=$1 label=$2 src=$3 std=$4 cc=$5
	local g m
	$cc $std -w -O0 -c -o tmp/bench/g.o "$src" 2>/dev/null || { printf "  %-18s %9s\n" "$label" "CC-FAIL"; return; }
	( ulimit -t 600; timeout 600 "$BIN" $std -c -o tmp/bench/m.o "$src" ) >/dev/null 2>&1 \
		|| { printf "  %-18s %9s\n" "$label" "MADC-FAIL"; return; }
	g=$(best $cc $std -w -O0 -c -o tmp/bench/g.o "$src")
	m=$(best "$BIN" $std -c -o tmp/bench/m.o "$src")
	[ "$g" -eq 0 ] && g=1
	printf "  %-18s %8s %8s %8s\n" "$label" "$g" "$m" "$(awk -v a="$m" -v b="$g" 'BEGIN{printf "%.2fx", a/b}')"
	emit "$kind" "$label" "$g" "$m"; acc "$m" "$g"
}

echo "benchmark_lane $STAMP  rev=$REV  best-of-$N"
echo "$( $BIN --version 2>&1 | head -1 )  |  gcc $(gcc -dumpversion)  |  $(nproc) cpus"
echo

echo "EXECUTION — C (MIR's own suite, gcc -O2 vs madc -O2 native binaries)"
printf "  %-18s %8s %8s %8s\n" benchmark "gcc" "madc" ratio
reset_gm
for src in "$MIRB"/*.c; do
	nm=$(basename "$src" .c); a=""
	[ -f "$MIRB/$nm.arg" ] && a=$(sh -c "$(cat "$MIRB/$nm.arg")" 2>/dev/null)
	run_exec exec-c "$nm" "$src" "--std=c17" gcc $a
done
gm exec-c
echo

echo "EXECUTION — floating point (the v0.93.0 MIR false-dependency sentinel)"
reset_gm
if [ -f donut.c ]; then
	run_exec exec-fp donut donut.c "--std=c17" gcc
	gm exec-fp
else
	echo "  donut.c not present at the repo root — skipped (not vendored)"
fi
echo

echo "EXECUTION — C++ (g++ -O2 vs madc -O2 native binaries)"
printf "  %-18s %8s %8s %8s\n" benchmark "g++" "madc" ratio
reset_gm
for src in "$CPPB"/*.cpp; do
	[ -e "$src" ] || continue
	run_exec exec-cpp "$(basename "$src" .cpp)" "$src" "--std=c++11" g++ 200
done
gm exec-cpp
echo

echo "COMPILE — TU to object (gcc -O0 vs madc; madc's front end is the subject)"
printf "  %-18s %8s %8s %8s\n" source "gcc" "madc" ratio
reset_gm
for src in "$MIRB"/sieve.c "$MIRB"/binary-trees.c; do
	[ -e "$src" ] && run_compile compile-c "$(basename "$src")" "$src" "--std=c17" gcc
done
for src in "$CPPB"/*.cpp; do
	[ -e "$src" ] && run_compile compile-cpp "$(basename "$src")" "$src" "--std=c++11" g++
done
gm compile
echo
echo "benchmark_lane: done${TSV:+ (rows appended to $TSV)}"
