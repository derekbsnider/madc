#!/bin/bash
# selfhost_lane.sh — the self-host lane (ROADMAP 2.11; owner go 2026-09-17).
#
# madc parses madc. Every translation unit src/Makefile compiles into bin/madc
# runs through `bin/madc --emit=c11` with the SAME preprocessor-shaping flags
# the Makefile hands the C++ compiler — derived from `make -n` on the spot,
# never restated here — plus one wrapper TU per madc-own header so every
# header is measured standalone as well. There is no parse-only flag;
# --emit=c11 is the parse + sema + lower driver and a TU without main() emits
# rc=0, so a unit PASSES when madc exits 0.
#
# The failures ARE the empirical gap list. The lane is a RATCHET exactly like
# c_testsuite_lane.sh: known-failing units live in
# docs/parity/selfhost-baseline.txt (one unit key per line, # comments allowed).
# RED when any non-baseline unit fails; a baseline unit that PASSES is reported
# loudly so the baseline only ever shrinks — never silently pads.
#
# Unit key = the source path as the Makefile spells it from src/ (ns_js.cpp,
# rt/rt_task.c, ../obj/<mode>/embedded_headers.cpp) or, for a header wrapper,
# the header's repo-relative path (include/madc.h).
#
# Outputs (default tmp/selfhost/):
#   <unit>.err    the unit's stderr, ANSI stripped
#   results.tsv   key  rc  secs  errors  first-error   (one row per unit)
#   shapes.txt    error-message SHAPE tally over every unit (quoted names and
#                 numbers normalized), then a coarse first-4-words tally
#   files.txt     errors by the FILE they are reported in (TU vs header)
#   failing.txt   every failing key, baseline-file format
#
#   MADC_BIN                binary under test (default bin/madc)
#   MADC_SELFHOST_MODE      the Makefile MODE whose objects define the TU set (develop)
#   MADC_SELFHOST_BASELINE  baseline file (default docs/parity/selfhost-baseline.txt)
#   MADC_SELFHOST_CAP       per-unit wall-clock AND cpu cap, seconds (default 600)
#   MADC_SELFHOST_ERR_CAP   per-unit stderr cap in bytes (default 32M): a unit
#                           whose diagnostics loop (2M errors from one <regex>
#                           include, 2026-09-17) is cut off here, not on disk
#   MADC_SELFHOST_JOBS      units run concurrently (default 4)
#   MADC_SELFHOST_ONLY      space-separated unit keys to run (default: every unit)
#   MADC_SELFHOST_OUT       output directory (default tmp/selfhost)
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)

BIN="${MADC_BIN:-bin/madc}"
case "$BIN" in /*) BIN_ABS="$BIN";; *) BIN_ABS="$ROOT/$BIN";; esac
MODE="${MADC_SELFHOST_MODE:-develop}"
BASE="${MADC_SELFHOST_BASELINE:-docs/parity/selfhost-baseline.txt}"
CAP="${MADC_SELFHOST_CAP:-600}"
ERR_CAP="${MADC_SELFHOST_ERR_CAP:-33554432}"
JOBS="${MADC_SELFHOST_JOBS:-4}"
ONLY="${MADC_SELFHOST_ONLY:-}"
OUT="${MADC_SELFHOST_OUT:-tmp/selfhost}"
case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT";; esac

# madc-own headers measured standalone: every header under include/ except the
# script-facing embedded headers (include/madc/ — their own lanes) and the
# vendored single-header libraries listed here (not madc source).
HEADER_GLOBS="include/*.h include/madcdis/*.h include/madcdat/*.h include/libmadc/*.h"
THIRD_PARTY_HEADERS="include/doctest.h"

if [ ! -x "$BIN_ABS" ]; then
	echo "selfhost_lane: $BIN missing — build first" >&2
	exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/raw" "$OUT/args" "$OUT/rows" "$OUT/hdr"

# --- unit key -> file-name stem ---------------------------------------------
sanitize() {
	local k="$1"
	k="${k#../}"
	printf '%s\n' "${k//\//__}"
}

# --- derive the madc argument list from ONE make -n compile line -------------
# Keeps what shapes the preprocessed text (-I -D -std=); drops toolchain-only
# flags (-c -o -M* -W* -O* -g* -f* -m* -x -pipe). A preprocessor-shaping flag
# madc has no spelling for is reported, never silently dropped. Prints one
# argument per line; the source path is the last line.
derive_args() {
	local line="$1"
	# make -n prints shell-quoted text (-DX='"v"'); the shell is its parser.
	eval "set -- $line"
	shift	# the compiler
	local src=""
	while [ $# -gt 0 ]; do
		case "$1" in
		-o|-MF|-MT|-MQ|-x)
			shift 2; continue ;;
		-I|-D)
			printf '%s\n' "$1$2"; shift 2; continue ;;
		-isystem)
			printf '%s\n' "-I$2"; shift 2; continue ;;
		-I*|-D*)
			printf '%s\n' "$1" ;;
		-std=*)
			printf '%s\n' "--std=${1#-std=}" ;;
		-include|-imacros|-U*|-idirafter|-iquote)
			echo "selfhost_lane: preprocessor flag not carried: $1" >&2
			case "$1" in -include|-imacros|-idirafter|-iquote) shift;; esac ;;
		-*)
			;;	# toolchain-only
		*)
			src="$1" ;;
		esac
		shift
	done
	printf '%s\n' "$src"
}

# --- the TU set: every object the Makefile links into bin/madc ---------------
# --no-print-directory: under `make -C src selfhost` this is a sub-make, and a
# sub-make prints "Entering directory" lines that would join the object list.
objects=$(make -s --no-print-directory -C src MODE="$MODE" print-OBJECTS 2> "$OUT/make_stderr.txt")
if [ -z "$objects" ]; then
	echo "selfhost_lane: make print-OBJECTS produced nothing (MODE=$MODE)" >&2
	exit 2
fi
# shellcheck disable=SC2086
make -n -B --no-print-directory -C src MODE="$MODE" $objects 2>> "$OUT/make_stderr.txt" \
	| grep -- ' -c -o ' > "$OUT/compile_lines.txt"
nlines=$(wc -l < "$OUT/compile_lines.txt")
if [ "$nlines" -eq 0 ]; then
	echo "selfhost_lane: derived NO compile lines from make -n (harness broken)" >&2
	tail -5 "$OUT/make_stderr.txt" >&2
	exit 2
fi

keys=()
ref_args=""	# the flags of the first C++ TU line, for the header wrappers
while IFS= read -r line; do
	mapfile -t a < <(derive_args "$line")
	n=${#a[@]}
	[ "$n" -eq 0 ] && continue
	key="${a[$((n-1))]}"
	san=$(sanitize "$key")
	printf '%s\n' "${a[@]}" > "$OUT/args/$san.args"
	keys+=("$key")
	if [ -z "$ref_args" ]; then
		case "$line" in *" -std=c++"*|*" -std=gnu++"*)
			ref_args="$OUT/args/$san.args" ;;
		esac
	fi
done < "$OUT/compile_lines.txt"

# --- one wrapper TU per madc-own header ---------------------------------------
if [ -n "$ref_args" ]; then
	# shellcheck disable=SC2086
	for hdr in $HEADER_GLOBS; do
		[ -f "$hdr" ] || continue
		skip=0
		for tp in $THIRD_PARTY_HEADERS; do [ "$hdr" = "$tp" ] && skip=1; done
		[ "$skip" -eq 1 ] && continue
		san=$(sanitize "$hdr")
		wrapper="$OUT/hdr/$san.cpp"
		printf '#include "%s/%s"\n' "$ROOT" "$hdr" > "$wrapper"
		# flags of the reference C++ TU, then this wrapper as the source
		head -n -1 "$ref_args" > "$OUT/args/$san.args"
		printf '%s\n' "$wrapper" >> "$OUT/args/$san.args"
		keys+=("$hdr")
	done
else
	echo "selfhost_lane: no C++ compile line to borrow header-wrapper flags from" >&2
fi

if [ -n "$ONLY" ]; then
	sel=()
	for k in "${keys[@]}"; do
		for o in $ONLY; do [ "$k" = "$o" ] && sel+=("$k"); done
	done
	keys=("${sel[@]}")
fi
if [ "${#keys[@]}" -eq 0 ]; then
	echo "selfhost_lane: no units selected" >&2
	exit 2
fi

# --- run one unit (invoked under xargs -P) ------------------------------------
run_unit() {
	local key="$1" san
	san=$(sanitize "$key")
	local -a args
	mapfile -t args < "$OUT/args/$san.args"
	# flags first, then --emit=c11, then the source LAST: madc treats every
	# argument after the source file as the program's argv, so an --emit
	# placed after it is not a flag and the unit would be RUN, not rendered
	# ("madc_cir_execute: main() not found" — the first lane run, 2026-09-17).
	local n=${#args[@]}
	local src="${args[$((n - 1))]}"
	local -a flags=("${args[@]:0:$((n - 1))}")
	local start end rc
	start=$(date +%s)
	# stderr through a byte cap: a diagnostics loop must not fill the disk
	# (head closing the pipe ends the runaway early; its rc stays non-zero).
	# The subshell must exit with madc's status, not `wait`'s (the second
	# lane run read 172/172 green from exactly that mistake).
	( cd src; ulimit -t "$CAP"; timeout "$CAP" "$BIN_ABS" "${flags[@]}" --emit=c11 "$src" \
		> /dev/null 2> >(head -c "$ERR_CAP" > "$OUT/raw/$san.err"); rc=$?; wait; exit $rc )
	rc=$?
	end=$(date +%s)
	sed 's/\x1b\[[0-9;]*m//g' "$OUT/raw/$san.err" > "$OUT/$san.err"
	local nerr first
	nerr=$(grep -cE ': error: |^cir error: ' "$OUT/$san.err")
	first=$(grep -m1 -E ': error: |^cir error: ' "$OUT/$san.err")
	# Defence in depth: diagnostics with a zero exit is an inconsistency
	# (compiler or harness) — never a PASS. Recorded as rc 99.
	if [ "$rc" -eq 0 ] && [ "$nerr" -gt 0 ]; then rc=99; fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$key" "$rc" "$((end - start))" "$nerr" "$first" \
		> "$OUT/rows/$san.row"
}
export -f run_unit sanitize
export OUT CAP ERR_CAP BIN_ABS

lane_start=$(date +%s)
printf '%s\n' "${keys[@]}" | xargs -P "$JOBS" -I{} bash -c 'run_unit "$1"' _ {}
lane_end=$(date +%s)

# --- aggregate ----------------------------------------------------------------
cat "$OUT"/rows/*.row | sort > "$OUT/results.tsv"

declare -A baseline
if [ -f "$BASE" ]; then
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue;; esac
		baseline["$line"]=1
	done < "$BASE"
fi

pass=0; fail=0; newfail=0; fixed=0
newfail_names=""; fixed_names=""
: > "$OUT/failing.txt"
while IFS=$'\t' read -r key rc secs nerr first; do
	if [ "$rc" -eq 0 ]; then
		pass=$((pass + 1))
		if [ -n "${baseline[$key]:-}" ]; then
			fixed=$((fixed + 1)); fixed_names="$fixed_names $key"
		fi
	else
		fail=$((fail + 1))
		printf '%s\n' "$key" >> "$OUT/failing.txt"
		if [ -z "${baseline[$key]:-}" ]; then
			newfail=$((newfail + 1)); newfail_names="$newfail_names $key"
		fi
	fi
done < "$OUT/results.tsv"

# error shapes: quoted names and numbers normalized; then a coarse 4-word tally
{
	echo "# error-message shapes (normalized), every unit"
	cat "$OUT"/*.err | grep -E ': error: |^cir error: ' | sed 's/.*error: //' \
		| sed "s/'[^']*'/'…'/g; s/\"[^\"]*\"/\"…\"/g; s/[0-9][0-9]*/N/g" \
		| sort | uniq -c | sort -rn
	echo
	echo "# coarse shapes (first four words)"
	cat "$OUT"/*.err | grep -E ': error: |^cir error: ' | sed 's/.*error: //' \
		| awk '{print $1, $2, $3, $4}' | sort | uniq -c | sort -rn
} > "$OUT/shapes.txt"

# errors by the file they are reported in
{
	echo "# errors by reporting file (a header's count = the TUs that include it)"
	cat "$OUT"/*.err | grep ': error: ' | sed 's/:[0-9]*:[0-9]*: error: .*//' \
		| sort | uniq -c | sort -rn
} > "$OUT/files.txt"

# --- the coverage table -------------------------------------------------------
total=$((pass + fail))
echo "selfhost coverage — $total units (bin=$BIN, MODE=$MODE, cap=${CAP}s, jobs=$JOBS)"
printf '%-4s %-44s %4s %5s %6s  %s\n' "" "unit" "rc" "secs" "errors" "first error"
while IFS=$'\t' read -r key rc secs nerr first; do
	mark=PASS; [ "$rc" -ne 0 ] && mark=FAIL
	[ "$rc" -eq 124 ] && mark=TIME
	[ "$rc" -eq 99 ] && mark=INCO	# errors printed, exit 0
	printf '%-4s %-44s %4s %5s %6s  %.110s\n' "$mark" "$key" "$rc" "$secs" "$nerr" "$first"
done < "$OUT/results.tsv"
echo
echo "top error shapes:"
sed -n '2,13p' "$OUT/shapes.txt"
echo
echo "errors by reporting file (top 12):"
sed -n '2,13p' "$OUT/files.txt"
echo

echo "selfhost: $pass passed, $fail failed of $total units" \
     "($newfail outside baseline, $fixed baseline units now passing)" \
     "— wall $((lane_end - lane_start))s; details in $OUT/"
if [ "$fixed" -gt 0 ]; then
	echo "selfhost: SHRINK THE BASELINE — now passing:$fixed_names"
fi
if [ "$newfail" -gt 0 ]; then
	echo "selfhost: RED — non-baseline failure(s):$newfail_names" >&2
	exit 1
fi
exit 0
