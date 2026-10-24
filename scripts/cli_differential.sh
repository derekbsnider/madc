#!/usr/bin/env bash
# cli_differential.sh OLD_BIN NEW_BIN [CORPUS] [OUTDIR] — madc's command line,
# old binary against new: every corpus line (scripts/cli_differential.corpus)
# runs under both, each in a fresh scratch directory with programs that print
# their argv, and the lines whose exit status, stdout, stderr or created files
# differ are reported (install paths, addresses, timings and rates normalized).
#
# A change meant to keep the command line's behaviour (a refactor of the
# option reader or madc's table) is proven with this BEFORE it commits; every
# reported difference is classified as intended or fixed. Build both binaries
# with the same configure (a scratch worktree), on the build container.
set -u
[ $# -ge 2 ] || { echo "usage: $0 OLD_BIN NEW_BIN [CORPUS] [OUTDIR]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Every run cds into its scratch directory: both binaries by absolute path,
# and a binary that cannot run refuses the comparison (it would make every
# line "identical").
absolute() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
for b in "$1" "$2"; do
	[ -f "$b" ] && [ -x "$b" ] || { echo "$0: not an executable: $b" >&2; exit 2; }
done
OLD="$(absolute "$1")" NEW="$(absolute "$2")"
CORPUS="${3:-$ROOT/scripts/cli_differential.corpus}"
OUT="${4:-$ROOT/tmp/cli_differential}"
OLD_ROOT="$(cd "$(dirname "$OLD")/.." && pwd)" NEW_ROOT="$(cd "$(dirname "$NEW")/.." && pwd)"
rm -rf "$OUT"; mkdir -p "$OUT/run"
report="$OUT/report.txt"; : > "$report"
n=0 same=0 diff=0

setup()
{
	local d="$1"
	mkdir -p "$d/inc"
	cat > "$d/hello.c" <<'C'
#include <stdio.h>
int main(int argc, char **argv)
{
	printf("argc=%d", argc);
	for (int i = 1; i < argc; i++)
		printf(" [%s]", argv[i]);
	printf("\n");
	return 0;
}
C
	printf 'int two(void) { return 2; }\n' > "$d/two.c"
	sed 's/<stdio.h>/<cstdio>/' "$d/hello.c" > "$d/hello.cpp"
	cat > "$d/hello.mad" <<'M'
int main(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		println(argv[i]);
	return 0;
}
M
	printf '#define H 1\n' > "$d/inc/h.h"
	printf -- '-v hello.c r1 r2\n' > "$d/rsp.txt"
	ls -1A "$d" > "$d.inputs"
}

norm()
{
	sed -E -e "s|$OLD|madc|g" -e "s|$NEW|madc|g" -e "s|$OLD_ROOT|{ROOT}|g" \
	    -e "s|$NEW_ROOT|{ROOT}|g" -e "s#$OUT/run/[0-9]+\.(old|new)#{D}#g" \
	    -e 's/0x[0-9a-f]+/0xADDR/g' -e 's/[0-9]+\.[0-9]+/N.N/g' \
	    -e 's/madc_frozen_[A-Za-z0-9]+/madc_frozen_X/g' -e 's/"[0-9]{2}:[0-9]{2}:[0-9]{2}"/"HH:MM:SS"/g' \
	    -e 's/\b[0-9]{12,}\b/ADDR/g' -e 's/elapsed time: [0-9]+/elapsed time: N/g' \
	    -e 's|\([0-9]+ tok/s\)|(N tok/s)|g' "$1"
}

while IFS= read -r line; do
	case "$line" in ''|'#'*) continue;; esac
	n=$((n + 1))
	args="$line" stdin=/dev/null
	[ "$args" = "(no arguments)" ] && args=""
	case "$args" in *" < "*) stdin="${args##* < }"; args="${args% < *}";; esac
	[ "${args:0:2}" = "< " ] && { stdin="${args#< }"; args=""; }
	for side in old new; do
		bin="$OLD"; [ "$side" = new ] && bin="$NEW"
		d="$OUT/run/$n.$side"
		setup "$d"
		a=$(printf '%s' "$args" | sed -e 's/{c}/hello.c/g' -e 's/{c2}/two.c/g' \
			-e 's/{cpp}/hello.cpp/g' -e 's/{mad}/hello.mad/g' -e 's/{o}/out.bin/g' \
			-e 's/{inc}/inc/g' -e 's/{rsp}/rsp.txt/g')
		s=$(printf '%s' "$stdin" | sed -e 's/{c}/hello.c/g' -e 's/{mad}/hello.mad/g')
		read -ra argv <<< "$a"
		( cd "$d" && ulimit -t 60 && timeout 20 "$bin" "${argv[@]}" < "$s" \
			> "$d.stdout" 2> "$d.stderr"; echo $? > "$d.rc" )
		ls -1A "$d" | grep -vxF -f "$d.inputs" > "$d.files"
	done
	o="$OUT/run/$n.old" w="$OUT/run/$n.new" what=""
	cmp -s "$o.rc" "$w.rc" || what="$what rc($(cat "$o.rc")->$(cat "$w.rc"))"
	cmp -s <(norm "$o.stdout") <(norm "$w.stdout") || what="$what stdout"
	cmp -s <(norm "$o.stderr") <(norm "$w.stderr") || what="$what stderr"
	cmp -s "$o.files" "$w.files" || what="$what files"
	if [ -z "$what" ]; then
		same=$((same + 1))
		continue
	fi
	diff=$((diff + 1))
	{
		echo "=== [$n] madc $line —$what"
		echo "--- old rc=$(cat "$o.rc") files: $(tr '\n' ' ' < "$o.files")"
		echo "+++ new rc=$(cat "$w.rc") files: $(tr '\n' ' ' < "$w.files")"
		diff <(norm "$o.stdout") <(norm "$w.stdout") | head -8 | sed 's/^/  out| /'
		diff <(norm "$o.stderr") <(norm "$w.stderr") | head -8 | sed 's/^/  err| /'
	} >> "$report"
done < "$CORPUS"
echo "cli_diff: $n lines, $same identical, $diff differ — $report"
