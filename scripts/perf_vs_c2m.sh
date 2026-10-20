#!/usr/bin/env bash
# perf_vs_c2m.sh — madc against c2m, MIR's own C compiler (owner 2026-10-20:
# madc must compile faster than c2m). Both sit on the same back end (the
# in-tree libmir + c2mir) at the same -O level (madc's default -O1, c2m -O1)
# with the same command line, so the difference is the front end. Plan:
# docs/plans/2026-10-20-front-end-speed-plan.md (item G1).
#
# Per workload: the callgrind instruction count (Ir) of each compile — the
# verdict, deterministic — interleaved wall times (best of N), peak RSS, the
# madc/c2m ratios, tcc beside them as the floor when installed, and the
# sha256 of madc's object: the plan's behaviour oracle (a cost change leaves
# the object byte-identical).
#
#   bash scripts/perf_vs_c2m.sh                  # report + check (exit 1 on red)
#   bash scripts/perf_vs_c2m.sh --same-objects   # also red when madc's object
#                                                #   differs from the ledger's
#   bash scripts/perf_vs_c2m.sh --record         # write the ledger rows
#   --runs=N (wall rounds, default 5)  --only=NAME  --no-callgrind
#   MADC_BIN (default bin/madc-release), C2M_BIN (default obj/mir/host/c2m,
#   built by `make -C src`)
#
# Workloads live in scripts/perf_vs_c2m/: <name>.c, a symlink to a real
# codebase on the build host (never copied into the repo; a missing source is
# RED, never skipped), and optionally <name>.flags, given to BOTH compilers
# ({srcdir} = the source's real directory, for its quoted headers). The ledger,
# scripts/perf_vs_c2m/ledger.tsv, holds the last recorded row per workload.
# RED: a compile fails; a workload madc led on (recorded Ir ratio < 1) is
# behind again; with --same-objects, madc's object changed.
# Every compile is capped (timeout + ulimit -t).
set -u
cd "$(dirname "$0")/.." || exit 9
DIR=scripts/perf_vs_c2m
LEDGER=$DIR/ledger.tsv
MADC=${MADC_BIN:-bin/madc-release}
C2M=${C2M_BIN:-obj/mir/host/c2m}
runs=5; only=""; record=0; same=0; cg=1
for a in "$@"; do
	case "$a" in
	--record) record=1 ;;
	--same-objects) same=1 ;;
	--no-callgrind) cg=0 ;;
	--runs=*) runs="${a#--runs=}" ;;
	--only=*) only="${a#--only=}" ;;
	*) echo "perf_vs_c2m: unknown argument '$a'" >&2; exit 2 ;;
	esac
done
[ -x "$MADC" ] || { echo "perf_vs_c2m: no $MADC (make -C src release)" >&2; exit 2; }
[ -x "$C2M" ] || { echo "perf_vs_c2m: no $C2M (make -C src builds it)" >&2; exit 2; }
[ "$cg" = 0 ] || command -v valgrind > /dev/null || { echo "perf_vs_c2m: valgrind not installed" >&2; exit 2; }
[ "$record" = 0 ] || [ "$cg" = 1 ] || { echo "perf_vs_c2m: --record needs the callgrind counts" >&2; exit 2; }
TCC=$(command -v tcc)

SP=$(mktemp -d "${TMPDIR:-/tmp}/perf_vs_c2m.XXXXXX")
trap 'rm -rf "$SP"' EXIT

# The command line of each compiler: cmd_<who> <out.o> <src> <flags...>
cmd_madc() { local o="$1" s="$2"; shift 2; echo "$MADC $* -c -o $o $s"; }
cmd_c2m() { local o="$1" s="$2"; shift 2; echo "$C2M -w -O1 $* -fobject -o $o $s"; }
cmd_tcc() { local o="$1" s="$2"; shift 2; echo "$TCC -w $* -c -o $o $s"; }

# run_capped <log> cmd... -> the command's status (output to <log>)
run_capped() {
	local log="$1"; shift
	( ulimit -t 900; timeout 900 "$@" > "$log" 2>&1 )
}
# ir_of <cg-out> -> the callgrind total
ir_of() { grep -m1 '^summary:' "$1" | awk '{print $2}'; }
# usec -> the wall clock in microseconds
usec() { local t=$EPOCHREALTIME; echo "${t/./}"; }

ledger_get() { # name column(2..)
	[ -f "$LEDGER" ] || return 1
	awk -F'\t' -v n="$1" -v c="$2" '$1 == n { print $c; f = 1 } END { exit !f }' "$LEDGER"
}

red=0
declare -a rows=()
for src in "$DIR"/*.c; do
	name=$(basename "$src" .c)
	[ -z "$only" ] || [ "$only" = "$name" ] || continue
	if [ ! -e "$src" ]; then
		echo "RED  $name: its source $(readlink "$src") is not on this host"
		red=$((red + 1)); continue
	fi
	real=$(readlink -f "$src")
	flags=""
	[ -f "$DIR/$name.flags" ] && flags=$(sed "s|{srcdir}|$(dirname "$real")|g" "$DIR/$name.flags")
	who="madc c2m"; [ -n "$TCC" ] && who="madc c2m tcc"
	declare -A cmd=() ms=() rss=() ir=()
	failed=0
	for w in $who; do
		# shellcheck disable=SC2086
		cmd[$w]=$(cmd_$w "$SP/$name.$w.o" "$real" $flags)
		# The first run checks the compile and measures peak RSS.
		# shellcheck disable=SC2086
		if ! run_capped "$SP/$name.$w.log" /usr/bin/time -f '%M' -o "$SP/$name.$w.rss" ${cmd[$w]} \
			|| [ ! -s "$SP/$name.$w.o" ]; then
			echo "RED  $name: $w failed: ${cmd[$w]}"; head -5 "$SP/$name.$w.log" | sed 's/^/     /'
			[ "$w" = tcc ] || failed=1
			[ "$w" = tcc ] && who="madc c2m"
			continue
		fi
		rss[$w]=$(( $(cat "$SP/$name.$w.rss") / 1024 ))
		ms[$w]=999999999
	done
	[ "$failed" = 0 ] || { red=$((red + 1)); continue; }
	# Interleaved rounds: each round times every compiler once.
	for _ in $(seq "$runs"); do
		for w in $who; do
			t0=$(usec)
			# shellcheck disable=SC2086
			run_capped /dev/null ${cmd[$w]}
			t1=$(usec)
			[ $((t1 - t0)) -lt "${ms[$w]}" ] && ms[$w]=$((t1 - t0))
		done
	done
	sha=$(sha256sum "$SP/$name.madc.o" | awk '{print $1}')
	echo "== $name  ($real${flags:+ $flags})"
	for w in $who; do
		printf '   %-5s %9.1f ms  %5d MB' "$w" "$(awk -v u="${ms[$w]}" 'BEGIN { print u / 1000 }')" "${rss[$w]}"
		if [ "$cg" = 1 ] && [ "$w" != tcc ]; then
			# shellcheck disable=SC2086
			run_capped "$SP/$name.$w.cglog" valgrind --tool=callgrind \
				--callgrind-out-file="$SP/$name.$w.cg" ${cmd[$w]}
			ir[$w]=$(ir_of "$SP/$name.$w.cg")
			printf '  %16s Ir' "$(printf "%'d" "${ir[$w]:-0}")"
		fi
		echo
	done
	wall_ratio=$(awk -v a="${ms[madc]}" -v b="${ms[c2m]}" 'BEGIN { printf "%.3f", a / b }')
	ir_ratio=""
	if [ "$cg" = 1 ]; then
		[ -n "${ir[madc]:-}" ] && [ -n "${ir[c2m]:-}" ] \
			|| { echo "RED  $name: no callgrind total"; red=$((red + 1)); continue; }
		ir_ratio=$(awk -v a="${ir[madc]}" -v b="${ir[c2m]}" 'BEGIN { printf "%.3f", a / b }')
	fi
	echo "   madc/c2m: ${ir_ratio:+Ir ${ir_ratio}x  }wall ${wall_ratio}x"
	echo "   madc object sha256 $sha"
	# The verdict against the ledger.
	if old_ratio=$(ledger_get "$name" 4); then
		if [ -n "$ir_ratio" ] && awk -v o="$old_ratio" -v n="$ir_ratio" 'BEGIN { exit !(o < 1 && n >= 1) }'; then
			echo "RED  $name: madc led c2m (Ir ${old_ratio}x recorded) and is behind again (${ir_ratio}x)"
			red=$((red + 1))
		fi
		old_sha=$(ledger_get "$name" 7)
		if [ "$old_sha" != "$sha" ]; then
			if [ "$same" = 1 ]; then
				echo "RED  $name: madc's object differs from the ledger's ($old_sha)"
				red=$((red + 1))
			else
				echo "   (madc's object differs from the ledger's)"
			fi
		fi
		[ -n "$ir_ratio" ] && echo "   ledger: Ir ${old_ratio}x  madc $(printf "%'d" "$(ledger_get "$name" 2)") Ir"
	fi
	rows+=("$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' "$name" "${ir[madc]:-}" "${ir[c2m]:-}" \
		"$ir_ratio" "${ms[madc]}" "${ms[c2m]}" "$sha" "$(date +%Y-%m-%d)")")
	unset cmd ms rss ir
done

if [ "$record" = 1 ]; then
	[ "$red" = 0 ] || { echo "perf_vs_c2m: not recording over $red red row(s)" >&2; exit 1; }
	{
		echo "# perf_vs_c2m ledger (scripts/perf_vs_c2m.sh --record): one row per workload"
		printf '# %s\n' "name	madc_Ir	c2m_Ir	Ir_ratio	madc_us	c2m_us	madc_obj_sha256	recorded"
		[ -f "$LEDGER" ] && grep -v '^#' "$LEDGER" | while IFS= read -r l; do
			n=${l%%	*}
			printf '%s\n' "${rows[@]}" | cut -f1 | grep -qx "$n" || echo "$l"
		done
		printf '%s\n' "${rows[@]}"
	} > "$SP/ledger" && mv "$SP/ledger" "$LEDGER"
	echo "perf_vs_c2m: recorded ${#rows[@]} row(s) in $LEDGER"
fi
[ "$red" = 0 ] || { echo "perf_vs_c2m: $red red"; exit 1; }
echo "perf_vs_c2m: 0 red"
